# sched_hint 重构 TODO：从「pin 用户 TLS 页」到「内核拥有页 + prctl 回填地址」

> 这份文档是重构的单一事实来源。上下文/会话中断后，从这里继续即可。
> 记录了**为什么这样设计**（决策依据）和**要做什么**（分阶段任务）。

## 0. 背景：现在的实现和它的问题

当前机制（已在 repo 里跑通、boot 验证过）：
- 用户态在 TLS 里放一个 `struct sched_hint __sched_hint_data`（initial-exec，64B 对齐）。
- 每个线程通过 `prctl(PR_SET_SCHED_HINT_OFFSET, offset, vaddr)` 注册；主线程注册 TLS 相对偏移，
  子线程在 `clone`（`CLONE_SETTLS`）时由内核用 `new_tls + mm->sched_hint_offset` 反推地址。
- 内核对每个线程的那一页做 `pin_user_pages_fast(FOLL_LONGTERM)`，把 `page_address()` 直接映射
  指针存进 `task->sched_hint_kaddr`，调度器（sched_ext BPF）直接解引用读取。
- `free_task` 里 `unpin_user_page`。

**问题（这次重构要根除的）：**
1. **每线程 pin 一整页**：只用 64B，却钉住 4KiB，且 `FOLL_LONGTERM` 使该页**不可迁移**，
   破坏内存规整（compaction）/CMA/内存热插拔。线程越多越糟。
2. **fork/exit 热路径开销**：每次 `pthread_create` 走 GUP pin，`free_task` unpin。
3. **悬挂/陈旧**：用户 `munmap`/`mremap`/`MADV_DONTNEED` 那段 TLS 后，内核仍指着旧 pin 页；
   longterm pin 持 refcount 不会 UAF，但会**语义陈旧**（用户重映射后写新页，内核读旧页）+ **pin 泄漏**。
4. **粒度粗**：per-thread 一页，浪费 + 无缓存局部性。

## 1. 关键设计决策（已拍板，附依据）

- **模型 B：内核拥有 hint 页，通过接口暴露给用户**，取代「用户拥有 TLS + 内核 pin」。
  - 依据：pin 的四个问题一次性消除（无 pin→无 LONGTERM 不可迁移、无悬挂、无换出问题、可省内存）。
- **暴露面：`prctl` 回填「带页内偏移的裸用户地址」**，不用 fd+mmap，也不用 /dev、/proc。
  - 依据：mmap 家族返回值必然**页对齐**，无法把「共享页里第 N 个 64B 槽的页内偏移」暴露掉；
    而 prctl 返回一个 `long`（经 `put_user` 回填 `void**`），可以直接给出 `page_base + slot%64*64`
    这个**精确地址**，达成「共享页省内存 + 缓存局部性 + 用户侧零索引/零 TLS-slot」三者兼得。
  - 代价：VMA 生命周期要**自己管**（fd/inode 框架免费的清理，这里手工做）。已接受。
- **多线程共享页，按 per-mm slot 位图密排**。64B 对齐 → 每页 `PAGE_SIZE/64 = 64` 槽。
  - 依据：省内存 + 同进程线程 hint 挤在同几页 → 调度器批量读多个任务 hint 时缓存友好。
  - 注意：**没有「进程内稠密线程序号」这种东西**（tid 全局稀疏、会回收），必须用位图分配器；
    位图还比「tid 递进」更省（退出的槽立即复用，无空洞）。
- **预留一个大 VMA（`SCHED_HINT_MAX_SLOTS`，建议 `1<<20` 槽 = 64MiB 虚拟区），物理页全惰性**。
  - 依据：预留 VMA ≠ 建页表；常驻成本 ≈ 一个 `vm_area_struct`（几百字节）+ 廉价虚拟地址；
    物理页/页表只跟**实际 fault 到的页**走。
  - **预留必须足够大的真正理由**：避免运行期 `mremap` 扩 VMA 导致**已发给用户的地址搬迁失效**。
    不是为了省页表。
  - **不要用 `threads-max` 当预留依据**：它是可写 sysctl，运行期会变。用固定慷慨常量。
- **内核只读 hint，用户全权写**。移除内核回写用户页的能力。
  - 影响：删除 `scx_bpf_clear_sched_hint` kfunc。与「hint 尽量 level-triggered、当前值即真相」方向一致。
- **必须支持读「非 current 任务」的 hint**（wakeup/select_cpu 看目标任务 p）。
  - 这条需求正是**必须要内核侧常驻映射指针**（`p->sched_hint_kaddr`）的根本原因；
    per-read `copy_from_user_nofault` 只能读 current，服务不了跨任务读，故排除 per-read copy 方案。
  - 内核侧指针现在指向**内核自有页**（`page_address` 永久有效），跨任务读天然成立。
- **子线程不自动分配**：每线程各自调一次 `prctl` 拿自己的地址（因为要 `put_user` 回填，只能在该线程
  自身上下文做）。取代旧的「clone 时自动 pin」。

## 2. 目标架构

### 用户侧 ABI（极简）
```c
struct sched_hint *h;
prctl(PR_SET_SCHED_HINT, &h, 0, 0, 0);   /* 内核回填本线程专属、已含页内偏移的地址 */
h->exec_dense = SCHED_EXEC_INT;          /* 直接写自己那 64 字节 */
```
- `arg2` = `struct sched_hint **`（用户提供，内核 `put_user` 回填）。
- 每线程各调一次（含主线程）。
- PR 号：`PR_SET_SCHED_HINT_OFFSET`(83) 语义已变，**改名 `PR_SET_SCHED_HINT`**，值沿用 83（实验期无历史包袱）。

### 数据结构
```c
/* mm_struct 里，替换现有 sched_hint_offset / has_sched_hint */
struct sched_hint_area {
    unsigned long   uaddr_base;   /* 预留 VMA 的用户态基址 */
    struct page   **pages;        /* 惰性分配，pages[i]==NULL 表示未分配 */
    unsigned long  *slot_bitmap;  /* 占用位图，密排复用 */
    unsigned int    nr_slots_used;
    struct mutex    lock;
};
struct sched_hint_area *sched_hint_area;  /* mm 里；NULL = 本进程未启用 */

/* task_struct 里，替换现有 sched_hint_kaddr / sched_hint_page */
struct sched_hint *sched_hint_kaddr;  /* = page_address(pages[slot/64]) + (slot%64)*64 */
int                sched_hint_slot;   /* -1 = 无 */
```
常量：`SCHED_HINT_MAX_SLOTS = 1<<20`；`SLOTS_PER_PAGE = PAGE_SIZE/64`（=64）。

### 关键流程
1. **首个注册线程**：分配 `sched_hint_area`，`vm_mmap` 建 `SCHED_HINT_MAX_SLOTS*64` 的特殊 VMA
   （自定义 `vm_operations_struct`：`.fault` 惰性填页、`.close` 清理），存 `uaddr_base`。
2. **每次 prctl**：位图取空闲 slot `i` → 若 `pages[i/64]` 空则 `alloc_page` 并写好 magic/version →
   算内核侧 `task->sched_hint_kaddr` 与用户侧 `uaddr = uaddr_base + i*64` → `put_user(uaddr, arg2)` →
   记 `task->sched_hint_slot = i`。
3. **`.fault`**：用户访问某页 → 找/分配对应内核页 → `vm_insert_page`（保证内核侧与用户侧同一张物理页）。
4. **线程退出（free_task/exit_thread 附近）**：归还 slot 到位图，清 `task->sched_hint_kaddr/slot`；**不释放页**。
5. **进程退出 / VMA `.close`**：释放所有 `pages[]`、位图、area。
6. **跨任务读**：`p->sched_hint_kaddr`（判 NULL 后）直接读，`p != current` 成立。

## 3. 分阶段任务清单（TODO）

> 环境注意：本开发环境**不能 boot 内核**，只能逐文件编译检查（`.config` 在，可 `make kernel/sched/hint.o` 等）。
> 用户（YaQia）会在自己笔记本上 boot 实测。建议先把内核骨架编译稳定，再动用户态 Pass。

### 阶段 A：内核数据结构与分配器
- [ ] `include/uapi/linux/sched_hint.h`：加 `SCHED_HINT_MAX_SLOTS`、`SLOTS_PER_PAGE`（若适合放 uapi），
      或放内核内部头。struct sched_hint 本身不变。
- [ ] `include/linux/sched/hint.h`（内核内部头）：声明 `struct sched_hint_area`、
      新 prctl 原型 `set_sched_hint_prctl(task, uptr)`、slot 分配/释放/area 销毁的函数原型。
- [ ] `include/linux/mm_types.h`：把 `sched_hint_offset`/`has_sched_hint` 换成
      `struct sched_hint_area *sched_hint_area;`。
- [ ] `include/linux/sched.h`：`task_struct` 把 `sched_hint_page` 换成 `int sched_hint_slot;`，
      保留 `struct sched_hint *sched_hint_kaddr;`。
- [ ] `kernel/sched/hint.c`：实现 area 分配、slot 位图 alloc/free、内核页惰性 alloc、
      内核侧 kaddr 计算。保留 magic/version 写入（内核首次建页时写）。

### 阶段 B：prctl 接口
- [ ] `include/uapi/linux/prctl.h`：`PR_SET_SCHED_HINT_OFFSET` → `PR_SET_SCHED_HINT`（值 83）。
- [ ] `kernel/sys.c`：dispatch 改名，调用新签名。
- [ ] `kernel/sched/hint.c` `set_sched_hint_prctl(task, uptr)`：首次建 area+VMA；分配 slot；
      `put_user` 回填带偏移用户地址。错误路径清理。

### 阶段 C：VMA 与惰性映射
- [ ] `kernel/sched/hint.c`（或新文件）：`vm_operations_struct { .fault, .close }`。
      `.fault`：按 vmf->pgoff 找/分配内核页，`vm_insert_page`。`.close`：触发 area 释放（或最后一个 VMA 关闭时）。
- [ ] 用 `vm_mmap` + 手动装 vm_ops 建预留 VMA（参考 mmap 特殊映射的做法，如 `install_special_mapping` 可能更合适——待评估）。
      **评估点**：`install_special_mapping` vs 自定义 `.mmap`——前者更省事且专为「内核提供页」设计。

### 阶段 D：fork / exit 清理
- [ ] `kernel/fork.c`：删掉 `copy_thread` 附近的 clone-pin 段（2247-2267 区域）与 free_task 的 unpin 段（558-561）。
      换成：free_task 时若 `sched_hint_slot >= 0` 则归还 slot。
- [ ] mm 销毁路径（`__mmput`/`exit_mmap` 附近）：释放 `sched_hint_area`（页、位图、结构体）。
      注意与 VMA `.close` 的职责划分，别 double-free。

### 阶段 E：sched_ext / BPF 侧
- [ ] `kernel/sched/ext.c`：删除 `scx_bpf_clear_sched_hint` kfunc + BTF 注册。
- [ ] `tools/sched_ext/include/scx/common.bpf.h`：删除该 kfunc 声明。
- [ ] 确认跨任务读点仍用 `p->sched_hint_kaddr`（语义不变，只是指针来源变了）。

### 阶段 F：用户态 Pass 与 header-only（sched_tags repo，另一个 repo）
- [ ] `sched_tags/include/sched_tag.h`：从「定义 TLS 变量 `__sched_hint_data`」改为
      「`__thread struct sched_hint *`（缓存指针）+ 线程首次使用时 `prctl(PR_SET_SCHED_HINT, &ptr)` 惰性注册」。
- [ ] LLVM Pass（`sched_tags/pass/`）：插桩形态从「写 `__sched_hint.data`」改为
      「确保本线程已注册、经缓存指针写 `hint->field`」。这是 Pass 大改，需重新设计插桩点
      （可能在函数入口/首次标注前插入注册检查）。
- [ ] `sched_tags` 的 prctl 构造器逻辑（原 `emitPrctlConstructor`）：语义完全变了，重做或删除。
- [ ] 测试（`sched_tags/test/`）：readers 从直接引用 TLS 变量改为经 prctl 拿指针；
      更新 reader_dynamic.h 等。

### 阶段 G：文档与收尾
- [ ] `sched_tags/README.zh.md`、`AGENTS.md`：更新使用方式（每线程 prctl 注册模型）。
- [ ] 更新本 TODO 完成状态。

## 4. 待评估 / 风险点（实现时注意）
- **`install_special_mapping` vs 自定义 `.mmap`/`vm_mmap`**：优先评估 `install_special_mapping`
  （专为内核页映射设计，vm_ops 挂 `.fault`，清理更省心）。
- **`.close` 与 mm 销毁的清理职责划分**：避免 double-free；建议 area 释放集中在一处，另一处只解引用置空。
- **并发**：`sched_hint_area->lock`（mutex）保护 slot 位图与 pages[]；注意 prctl 上下文可睡眠，
  但调度器读 `sched_hint_kaddr` 在原子上下文——**读侧不加锁**，靠「指针一旦设好就稳定、页永不释放（进程存活期间）」保证。
  线程退出归还 slot 时，要保证调度器不会再读到该 task 的 kaddr（task 已 dead，天然成立）。
- **跨任务读的内存序**：`p->sched_hint_kaddr` 设置（prctl）与调度器读之间，靠常规发布语义；
  内核自有页内容由用户写、内核读，仍是 best-effort，无需 barrier（与原设计一致）。
- **magic/version**：内核建页时写入；用户不应改 magic。跨任务读时可选择校验 magic（已在内核控制下，可省）。
- **PAGE_SIZE 非 4K 架构**：`SLOTS_PER_PAGE = PAGE_SIZE/64` 自动适配（16K 页 → 256 槽/页），无硬编码 64。

## 5. 已完成的前置工作（本次会话，勿重做）
- tag 分类重构：`compute_dense`+`branch_dense` → `exec_dense`（含 CTRL 位）；删 `io_dense`；
  `compute_prep` → `load_trend`（RISING/FALLING）；atomic_dense/unshared/dep_role 加 enum。
- magic：`0x5348494E`("SHIN") → `0x48494E54`("HINT")。
- **单一真源**：`include/uapi/linux/sched_hint.h` 为唯一 ABI 定义，经 `make headers_install` 共享；
  内核内部头/用户态/Pass 全部 `#include <linux/sched_hint.h>`；guard 用 `_LINUX_SCHED_HINT_KERNEL_H` 避免冲突。
- 均已 commit+push（kernel + sched_tags 两个 repo）。
- 遗留（与本重构独立）：scx_labeled 三个 bug（main.bpf.c:879 解引用顺序、:1973 未声明 cpu、
  :1911 tctx 未初始化）；schedulers repo 的 vmlinux.h 重生成；reader_atomic.c 的 `popcount>=4` 错误不变量（ASLR flaky）。
