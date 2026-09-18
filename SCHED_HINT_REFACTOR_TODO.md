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
- **多线程共享页，按 per-mm slot 位图密排**。`SCHED_HINT_SLOT_SIZE = sizeof(struct sched_hint)`（=64）→
  每页 `PAGE_SIZE / SCHED_HINT_SLOT_SIZE` 槽（4K 页 64 槽，16K 页 256 槽）。
  - 依据：省内存 + 同进程线程 hint 挤在同几页 → 调度器批量读多个任务 hint 时缓存友好。
  - 注意：**没有「进程内稠密线程序号」这种东西**（tid 全局稀疏、会回收），必须用位图分配器；
    位图还比「tid 递进」更省（退出的槽立即复用，无空洞）。
- **预留一个大 VMA，物理页全惰性**。上限锚定内核既有常量而非经验值：
  64 位 `FUTEX_TID_MASK`（0x3fffffff ≈ 2^30 槽，64GiB VA）；32 位 `PID_MAX_LIMIT * 32`（2^20 槽，64MiB VA）。
  - 依据：`FUTEX_TID_MASK` 是 futex 字 TID 域宽度，**高于内核可能发出的任何 TID**，
    故高于任何单进程线程总数，即「一个进程把系统上限的线程全建了」也覆盖；
    64GiB VA 在 64 位（128TiB 用户空间）里免费。
  - 32 位用户空间仅 ~3GiB，装不下 64GiB，故钳到 `PID_MAX_LIMIT * 32`（= 64MiB，仍 32 倍于 pid 上限）。
  - **per-mm 元数据（位图 + pages[]）绝不按 MAX_SLOTS 一次性分配**（2^30 槽 → 128MiB/进程，
    几千个活跃进程就是几十上百 GiB，不可接受）：初始仅一页槽位的量（几十字节），
    随注册线程数**翻倍增长**（`kvrealloc`，`area->lock` 下）。典型进程（百级线程）元数据 ~1KiB 量级。
  - **预留必须足够大的真正理由**：避免运行期扩 VMA 导致**已发给用户的地址搬迁失效**。
    不是为了省页表。
  - **不要用 `threads-max` 当预留依据**：它是可写 sysctl，运行期会变。锚定 `FUTEX_TID_MASK`/
    `PID_MAX_LIMIT` 这种编译期常量。
- **内核只读 hint，用户全权写**。移除内核回写用户页的能力。
  - 影响：删除 `scx_bpf_clear_sched_hint` kfunc。与「hint 尽量 level-triggered、当前值即真相」方向一致。
- **必须支持读「非 current 任务」的 hint**（wakeup/select_cpu 看目标任务 p）。
  - 这条需求正是**必须要内核侧常驻映射指针**（`p->sched_hint_kaddr`）的根本原因；
    per-read `copy_from_user_nofault` 只能读 current，服务不了跨任务读，故排除 per-read copy 方案。
  - 内核侧指针现在指向**内核自有页**（`page_address` 永久有效），跨任务读天然成立。
- **子线程不自动分配**：每线程各自调一次 `prctl` 拿自己的地址（因为要 `put_user` 回填，只能在该线程
  自身上下文做）。取代旧的「clone 时自动 pin」。
- **VMA 保护走 `VM_SEALED`（mseal），不手写 patch munmap 入口**。本内核为 7.0-rc2，mseal 已进主线：
  `mm/vma.c:1403/1423`（整段/部分 munmap）、`mm/mremap.c:1666`、`mm/mprotect.c:706`、`mm/madvise.c:1302`
  都已有 `vma_is_sealed()` 检查返回 `-EPERM`，upstream 长期维护，**打个 `VM_SEALED` flag 即免费复用**。
  - 依据：既满足「禁 munmap，防一个线程 munmap 掉全进程调度能力」，又不用长期维护侵入式 mm patch。
  - 局限：`mm/vma.h:667` 明确 `VM_SEALED` 在**非 64 位恒为 `VM_NONE`、`vma_is_sealed()` 恒 false**，
    即 seal 在 32 位是 no-op。32 位接受降级（见下「释放点解耦」，靠 refcount 兜底不崩，只是自废武功）。
  - VMA flags：`VM_SEALED | VM_DONTEXPAND | VM_DONTCOPY`
    （`SEALED` 防 munmap/mremap/mprotect、`DONTEXPAND` 防扩张、`DONTCOPY` 防 fork 复制 VMA）。
- **释放点与 seal 正交，统一放 mm 销毁钩子（`__mmput`），`.close` 一律空转**。这是本次讨论纠正的关键点：
  - **不要把释放塞进 `.close`**。`.close` 是逐-VMA 回调，任何 VMA 消失都触发（munmap/mremap/exec/exit）。
    32 位未 seal 时用户 munmap 会中途触发 `.close`，若在此 `put_page` → 页释放但存活线程 `sched_hint_kaddr`
    悬挂 → **UAF**，且与「refcount 兜底」直接矛盾（兜底成立的前提就是 `.close` 不 put_page）。
  - **正解**：`page->_refcount` 有两份——`area->pages[i]` 一份（内核持有）、用户 PTE 一份（`vm_insert_page` 加）。
    munmap 只掉 PTE 那份，`area->pages[i]` 那份仍在 → 页不释放 → kaddr 永久有效。真正 `put_page` 只在
    `__mmput` 里做一次。于是：64 位 seal 挡住 munmap；32 位 munmap → `.close` 空转 → 页 refcount 停在 1 →
    活着（自废武功但不崩）。**一套代码同时覆盖 32/64 位，无矛盾。**
  - `VM_SEALED` 退化为纯粹的「防用户自废武功」增益，**不影响释放正确性**。

## 2. 目标架构

### 用户侧 ABI（极简）
```c
struct sched_hint *h;
prctl(PR_SET_SCHED_HINT, &h, 0, 0, 0);   /* 内核回填本线程专属、已含页内偏移的地址 */
h->exec_dense = SCHED_EXEC_INT;          /* 直接写自己那 64 字节 */
```
- `arg2` = `struct sched_hint **`（用户提供，内核 `put_user` 回填）。
- 每线程各调一次（含主线程）；**重复调用幂等**——回填同一地址，Pass 的「确保已注册」插桩可放心重复触发。
- 仅支持原生宽度任务：compat（64 位内核跑 32 位程序）返回 `-EOPNOTSUPP`
  （回填 native 宽度指针会踩坏相邻字；真 32 位内核 `unsigned long` 即指针宽，无此问题）。
- PR 号：`PR_SET_SCHED_HINT_OFFSET`(83) 语义已变，**改名 `PR_SET_SCHED_HINT`**，值沿用 83（实验期无历史包袱）。

### 数据结构
```c
/* mm_struct 里，替换现有 sched_hint_offset / has_sched_hint */
struct sched_hint_area {
    unsigned long   uaddr_base;     /* 预留 VMA 的用户态基址 */
    struct page   **pages;          /* 按需扩容（翻倍），pages[i]==NULL 表示未分配 */
    unsigned long  *slot_bitmap;    /* 按需扩容（翻倍）；占用位图，密排复用 */
    unsigned long   capacity_slots; /* 元数据当前容量（slot 数），< SCHED_HINT_MAX_SLOTS */
    unsigned long   nr_slots_used;
    struct mutex    lock;
};
struct sched_hint_area *sched_hint_area;  /* mm 里；NULL = 本进程未启用 */

/* task_struct 里，替换现有 sched_hint_kaddr / sched_hint_page */
struct sched_hint *sched_hint_kaddr;  /* = page_address(pages[slot/SLOTS_PER_PAGE])
                                         + (slot%SLOTS_PER_PAGE)*SCHED_HINT_SLOT_SIZE */
int                sched_hint_slot;   /* -1 = 无 */
```
常量：`SCHED_HINT_SLOT_SIZE = sizeof(struct sched_hint)`；`SLOTS_PER_PAGE = PAGE_SIZE/SCHED_HINT_SLOT_SIZE`；
`SCHED_HINT_MAX_SLOTS`：64 位 `FUTEX_TID_MASK` / 32 位 `PID_MAX_LIMIT * 32`（见第 1 节）。

### 关键流程
1. **首个注册线程**：分配 `sched_hint_area`，`get_unmapped_area` + `_install_special_mapping` 建
   `SCHED_HINT_MAX_SLOTS*SCHED_HINT_SLOT_SIZE` 的特殊 VMA（`vm_special_mapping`：`.fault` 惰性填页、
   `.close` **空转**；`special_mapping_vmops` 自带 `.may_split=-EINVAL` + `VM_DONTEXPAND`），
   flags 打 `VM_SEALED | VM_DONTCOPY | VM_READ | VM_WRITE | VM_MAYREAD | VM_MAYWRITE`，存 `uaddr_base`。
2. **每次 prctl**：位图取空闲 slot `i`（容量内扫描，不够则翻倍扩容元数据 `kvrealloc`）→
   若对应页空则 `alloc_page(__GFP_ZERO)` → **slot 交付时** memset 清 payload + 写 magic/version
   （页比线程活得久，复用槽位必须清除前任标签；不采用「建页时写」，交付时初始化才能覆盖
   页已被用户先 touch 过的场景）→ 算内核侧 `task->sched_hint_kaddr` 与用户侧
   `uaddr = uaddr_base + i*SCHED_HINT_SLOT_SIZE` → `put_user(uaddr, arg2)`（**area->lock 外**，
   见第 4 节自死锁风险）→ 记 `task->sched_hint_slot = i`；`put_user` 失败回滚 slot。
3. **`.fault`**：只映射已注册 slot 的页（`pages[pgoff]` 非空）→ `get_page` + `vmf->page = page`
   （special mapping 约定，核心 mm 负责插 PTE，用户 PTE 那份 `_refcount` 由核心加上）；
   未注册区域 **SIGBUS**（vDSO 先例）。这同时把元数据增长绑定在实际注册量上——
   否则用户单次访问预留区末尾就会强迫位图/pages[] 扩容到 ~256MiB（DoS）。
4. **线程退出（`exit_mm` 之前、`task->mm` 仍持引用处）**：持 `area->lock` 归还 slot 到位图，
   清 `task->sched_hint_kaddr/slot`；**不释放页**。安全性：还 slot 时该线程尚未 mmput，area 必然还在；
   且该 task 即将 dead，调度器不会再读它的 kaddr → **还 slot 无需与调度器读同步**（只有位图位操作要锁）。
5. **进程退出**：在 `__mmput` 里 `exit_mmap(mm)` **之后**调 `sched_hint_free_area(mm)`——
   以 `mm->sched_hint_area` 为准，`put_page` 所有 `pages[]` + free 位图 + free area + 置 NULL，**只跑一次**。
   放 `exit_mmap` 之后：此时用户 PTE 那份 refcount 已被 `exit_mmap` 掉，`put_page` 归零、页干净回 buddy。
   **`.close` 不参与释放**（见第 1 节「释放点解耦」）。
6. **跨任务读**：`p->sched_hint_kaddr`（判 NULL 后）直接读，`p != current` 成立。读侧不加锁。

## 3. 分阶段任务清单（TODO）

> 环境注意：本开发环境**不能 boot 内核**，只能逐文件编译检查（`.config` 在，可 `make kernel/sched/hint.o` 等）。
> 用户（YaQia）会在自己笔记本上 boot 实测。建议先把内核骨架编译稳定，再动用户态 Pass。

### 阶段 A：内核数据结构与分配器 ✅
- [x] 常量放内核内部头（未动 uapi，struct sched_hint 本身不变）：
      `include/linux/sched/hint.h` 定义 `SCHED_HINT_SLOT_SIZE/SLOTS_PER_PAGE/SCHED_HINT_MAX_SLOTS`
      （64 位锚 `FUTEX_TID_MASK`，32 位钳 `PID_MAX_LIMIT*32`）、`struct sched_hint_area`、
      三个函数原型（`set_sched_hint_prctl`/`sched_hint_exit_task`/`sched_hint_free_area`）。
- [x] `include/linux/mm_types.h`：`sched_hint_offset`/`has_sched_hint` → `struct sched_hint_area *`。
- [x] `include/linux/sched.h`：`sched_hint_page` → `int sched_hint_slot`（-1=无），保留 `sched_hint_kaddr`。
- [x] `kernel/sched/hint.c`：area 创建、位图翻倍扩容（`kvrealloc` + **显式 memset 尾部**，本树 krealloc/
      vrealloc 对新增尾部清零语义不一致）、slot alloc/free、内核页惰性 alloc、kaddr 计算、
      magic/version **slot 交付时**写。VMA 长度溢出有 `static_assert` 编译期保证。

### 阶段 B：prctl 接口（hint.c 部分完成；uapi/sys.c 未动）
- [ ] `include/uapi/linux/prctl.h`：`PR_SET_SCHED_HINT_OFFSET` → `PR_SET_SCHED_HINT`（值 83）。
- [ ] `kernel/sys.c`：dispatch 改名，调用新签名（当前 sys.c 还是旧三参数调用，**树暂不可整编**）。
- [x] `kernel/sched/hint.c` `set_sched_hint_prctl(task, uptr)`：`mmap_write_lock_killable` 下
      check-and-create area+VMA（首次注册竞争串行化）；分配 slot；`put_user` 在 **area->lock 外**
      （自死锁规避）；EFAULT 回滚 slot；重复调用幂等（回填同一地址）；compat 拒绝 `-EOPNOTSUPP`。

### 阶段 C：VMA 与惰性映射 ✅
- [x] 采用 `vm_special_mapping` + `_install_special_mapping`（评估点已解决：接受任意 vm_flags，
      可叠 `VM_SEALED`；x86 vDSO 同款调用模式）。`special_mapping_vmops` 自带
      `.may_split=-EINVAL` + `VM_DONTEXPAND` + 防 VMA merge。`.fault` 用 `get_page`+`vmf->page`
      （不用 `vm_insert_page`）。`sm->close` 留 NULL（special_mapping_close 判空跳过 = 空转），
      释放唯一在 `__mmput`。`sm->mremap` 返回 `-EINVAL`。
- [x] flags：`VM_SEALED | VM_DONTCOPY | VM_READ | VM_WRITE | VM_MAYREAD | VM_MAYWRITE`
      （`VM_DONTEXPAND` 由 `__install_special_mapping` 自动补）。
      64 位 mseal 拦截免费生效；32 位 `VM_SEALED==VM_NONE` no-op，靠 refcount 兜底。
- [x] `.fault` 只映射已注册 slot 的页、其余 SIGBUS（决策与理由见第 2 节流程 3）。

### 阶段 D：fork / exit 清理（DF/UAF 向量逐条处理）
> 根因：`dup_task_struct`/`dup_mm` 是结构体**浅拷贝**，会把 hint 指针/slot 原样复制给子 task/子 mm。
> 新模型无 pin refcount 兜底，每处必须显式清。三大杀手：DF1（共享 area 指针）、DF2（继承 slot）、UAF2（exec 换 mm）。
- [ ] **DF1**：`mm_init()`（约 `kernel/fork.c:1079`）无条件 `mm->sched_hint_area = NULL;`。
      fresh mm 和 dup mm 都过这里；否则子 mm 浅拷贝父 area 指针 → teardown 双重释放 + slot 位图打架。
- [ ] **DF2（最关键）**：`dup_task_struct()`（约 `kernel/fork.c:952`，`seccomp.filter = NULL` 那一带）显式
      `tsk->sched_hint_kaddr = NULL; tsk->sched_hint_slot = -1;`。否则子线程继承父 slot → 退出时释放父的 slot。
- [ ] **UAF2**：`fs/exec.c` `begin_new_exec()`（:1091）在 `exec_mmap()`（:1148）**之前**清
      `current->sched_hint_kaddr = NULL; current->sched_hint_slot = -1;`。exec 换 mm 后旧 area 被 `__mmput` 释放，
      存活的 exec 线程 task 不变、kaddr 会悬挂。放 exec_mmap 之前是不给调度器留读悬挂指针的窗口。
- [ ] **删除旧 pin 代码**：`kernel/fork.c:558-562`（free_task 的 unpin 段）、
      `kernel/fork.c:2244-2274`（copy_thread 后的 clone-pin 段）。
- [ ] **free_task 不还 slot**：`free_task` 时 `task->mm` 已 NULL、area 可能已被 `__mmput` 释放，
      在此还 slot 会写已释放的 bitmap → UAF。`free_task` 对 hint 只做空操作（可 `WARN_ON_ONCE(kaddr)` 断言）。
- [ ] **还 slot 放对位置**：`kernel/exit.c` `do_exit`/`exit_mm` 中、`exit_mm()` 之前（`task->mm` 仍持引用处），
      新增 `sched_hint_exit_task(current)`：持 `area->lock` 清位图 bit + 清本 task kaddr/slot。
- [ ] **释放 area（唯一真源）**：`kernel/fork.c:1174` `__mmput` 里、`exit_mmap(mm)` **之后**调
      `sched_hint_free_area(mm)`：以 `mm->sched_hint_area` 为准，`put_page` 所有 pages + free bitmap + free area + 置 NULL。
      只跑一次。**与 `.close` 职责划分**：`.close` 空转，释放只在这里，从根上杜绝 double-free 和 32 位 munmap-UAF。
- [ ] **prctl 错误路径**：若 area 已建但后续步骤失败，就地释放 area 并把 `mm->sched_hint_area` 置回 NULL。

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
- [ ] **注册后预触（pre-touch）**：确保已注册的 helper 在 prctl 成功后**读一次 `h->magic` 并校验**——
      把该 slot 所在页唯一一次 page fault 挪出实际用标签的临界区，兼作 slot header 校验。
      注意别用「写 0」预触（常量 store 有被 DCE 的风险，读 + 使用值不会）。fault 按页摊销：
      密排下 64 slot 共享一页，N 线程共 ~N/64 次；页不进 LRU/不被换出/无 THP → 进程生命周期内
      **每页恰好一次**。备选：内核侧 prctl 末尾 `handle_mm_fault()` 主动装 PTE（「prctl 返回即映射就绪」），
      暂不做，boot 实测有需要再上。

### 阶段 G：文档与收尾
- [ ] `sched_tags/README.zh.md`、`AGENTS.md`：更新使用方式（每线程 prctl 注册模型）。
- [ ] 更新本 TODO 完成状态。

## 4. 待评估 / 风险点（实现时注意）
- **`install_special_mapping` vs 自定义 `.mmap`（已定论）**：采用 `_install_special_mapping`，
  接受任意 vm_flags（可叠 `VM_SEALED`，x86 vDSO 同款），自带 `.may_split=-EINVAL`/`VM_DONTEXPAND`/防 merge。
- **`.close` 与 mm 销毁的清理职责划分（已定论）**：`.close` **一律空转、绝不 put_page**；
  area/pages/bitmap 释放**只**在 `__mmput`（`exit_mmap` 之后）`sched_hint_free_area` 一处做一次。
  理由：`.close` 是逐-VMA 回调，32 位未 seal 时 munmap 会中途触发它，若在此 put_page → 存活线程 kaddr 悬挂 UAF，
  且与 refcount 兜底矛盾。把释放解耦到「mm 消失」这一唯一时刻，32/64 位一套代码、无 double-free、无 UAF。
- **refcount 兜底原理**：页有两份 `_refcount`——`area->pages[i]`（内核持有）+ 用户 PTE 一份
  （`.fault` 经 `get_page`+`vmf->page` 由核心 mm 加）。
  用户 munmap 只掉 PTE 那份，`area->pages[i]` 那份仍在 → 页不进回收 → `page_address()` 算的 kaddr 永久有效。
  这是新模型对 pin 方案的根本升级：内核拥有**自己的页**，用户页表只是其视图；撤视图不动数据源。
- **`VM_SEALED` 与释放正交**：seal 只决定「用户能否中途拆 VMA」（64 位不能/32 位能），
  释放点始终是 `__mmput`，与 seal 无关。别把两者耦合（本次讨论纠正的关键教训）。
- **put_user 必须在 area->lock 外（已实现）**：put_user 可能缺页，缺页可能恰好由 sched_hint VMA 的
  `.fault` 服务（用户把回填目标指进了预留区），`.fault` 要拿 `area->lock` → 锁内 put_user 会自死锁。
- **首次注册竞争（已实现）**：两个线程同时首次 prctl → `mmap_write_lock_killable` 下
  check-and-create，天然串行化；slot 分配只需 `area->lock`。
- **kvrealloc 尾部清零（已实现）**：本树 `kvrealloc` 已是三参数新 API（`p, new_size, flags`），
  krealloc/vrealloc 两条路径对「新增尾部」的清零语义不一致 → 扩容后**显式 memset 尾部**，
  不依赖 `__GFP_ZERO`。
- **VMA 长度溢出（已实现）**：`static_assert(SCHED_HINT_MAX_SLOTS <= ~0UL / SCHED_HINT_SLOT_SIZE)`
  编译期钉死：64 位 0x3fffffff*64 ≈ 64GiB，32 位 2^20*64 = 64MiB，均不溢出 `unsigned long`。
- **并发**：`sched_hint_area->lock`（mutex）保护 slot 位图、pages[] 及其**按需翻倍扩容**（`kvrealloc` 会搬迁数组，
  但 `task->sched_hint_kaddr` 在注册时算好存进 task_struct、**从不回读数组**，故扩容不影响任何活线程，读侧也不依赖数组地址）；
  prctl 上下文可睡眠，但调度器读 `sched_hint_kaddr` 在原子上下文——**读侧不加锁**，依据是
  「kaddr 在线程生命周期内不被 area 维护修改、页在该线程存活期间不释放」。
- **还 slot 无需与调度器读同步（已澄清）**：还 slot 只清位图一个 bit；被还的 task 即将 dead，
  调度器不会再通过 `p->sched_hint_kaddr` 读它 → 无争用。slot 被新线程复用时是**另一个 task_struct**，
  指针绑 task、老 task 死了没人读，不会「读到上一租户残留」。锁只护位图位操作，读路径完全不碰锁。
- **32 位降级（已接受）**：`VM_SEALED == VM_NONE`、`vma_is_sealed()` 恒 false，seal 全 no-op。
  32 位用户可 munmap 自己的映射（自废武功），但靠 refcount 兜底**不崩**（页 refcount 停在 1，kaddr 仍有效），
  后续该线程写 hint 会段错误——属预期行为。sched_ext 场景几乎只在 64 位，不为 32 位投入侵入式 patch。
- **跨任务读的内存序**：`p->sched_hint_kaddr` 设置（prctl）与调度器读之间，靠常规发布语义；
  内核自有页内容由用户写、内核读，仍是 best-effort，无需 barrier（与原设计一致）。
  **访问标注规则**：`sched_hint_kaddr` 有跨 CPU 无锁读者（调度器读非 current 任务）→ 其**所有写点**
  （prctl 发布、exit/exec 清除）统一 `WRITE_ONCE`；`sched_hint_slot` 无任何跨任务读者（仅本任务
  prctl/exit/exec 及 fork 写未运行的 child）→ 全部普通访问。同一位置禁止混用标注/裸访问（KCSAN）。
  若将来 BPF 需要读 `p->sched_hint_slot`（当前设计不需要），须将 slot 全访问点迁移到 READ_ONCE/WRITE_ONCE。
- **magic/version**：**slot 交付时**写入（页比线程活得久，复用槽位必须清前任标签，交付时初始化
  才能覆盖页被用户先 touch 过的场景）；用户不应改 magic。跨任务读时可选择校验 magic（已在内核控制下，可省）。
- **PAGE_SIZE 非 4K 架构**：`SLOTS_PER_PAGE = PAGE_SIZE / SCHED_HINT_SLOT_SIZE` 自动适配
  （16K 页 → 256 槽/页），无硬编码 64。

## 5. 已完成的前置工作（本次会话，勿重做）
- tag 分类重构：`compute_dense`+`branch_dense` → `exec_dense`（含 CTRL 位）；删 `io_dense`；
  `compute_prep` → `load_trend`（RISING/FALLING）；atomic_dense/unshared/dep_role 加 enum。
- magic：`0x5348494E`("SHIN") → `0x48494E54`("HINT")。
- **单一真源**：`include/uapi/linux/sched_hint.h` 为唯一 ABI 定义，经 `make headers_install` 共享；
  内核内部头/用户态/Pass 全部 `#include <linux/sched_hint.h>`；guard 用 `_LINUX_SCHED_HINT_KERNEL_H` 避免冲突。
- 均已 commit+push（kernel + sched_tags 两个 repo）。
- 遗留（与本重构独立）：scx_labeled 三个 bug（main.bpf.c:879 解引用顺序、:1973 未声明 cpu、
  :1911 tctx 未初始化）；schedulers repo 的 vmlinux.h 重生成；reader_atomic.c 的 `popcount>=4` 错误不变量（ASLR flaky）。
