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
- **多段（segment）VMA，按需增长，取代「单个大预留 VMA」**（本次讨论纠正的关键点）。
  - **为什么放弃单大 VMA**：`_install_special_mapping` → `__install_special_mapping` 无条件
    `vm_stat_account(len >> PAGE_SHIFT)`，即预留长度**全额计入 `mm->total_vm`**（`top` 的 VIRT）；
    且本 VMA flags 恰好命中 `is_data_mapping`（`(flags & (VM_WRITE|VM_SHARED|VM_STACK)) == VM_WRITE`）
    → **同时计入 `mm->data_vm`**。于是 `may_expand_vm` 会拿这 64GiB 去撞 `RLIMIT_AS` **和** `RLIMIT_DATA`
    （mm/mmap.c:1337-1355）。容器/systemd 常把 `RLIMIT_AS` 设到 1-2GiB → **一注册就 mmap 失败**。
    这不是审美问题（VIRT 难看），是真实功能 bug。旧 hint.h 那句「this only costs address space」是错的。
  - **多段方案**：首段 1 页，每追加一段页数**翻倍**（1→2→4→…），封顶 `SCHED_HINT_SEG_MAX_PAGES`(512 页/2MiB)。
    段数分两段函数（4K 页、64 槽/页）：**翻倍阶段** 1,2,…,512 页共 **10 段**，累计 1023 页 = 65472 槽，
    覆盖绝大多数进程（含数万线程的重型服务）→ 段数 ⌈log₂⌉ 级、**≤10 段**，多数进程仅 **1 段**
    （VIRT 仅 +4KiB，几乎不可见，RLIMIT 增量可忽略）。**封顶后线性阶段**：每段固定 512 页 = 32768 槽，
    段数随线程数**线性**增长（每 32768 线程 +1 段）。极端的 `PID_MAX_LIMIT`(4M 线程) ≈ (4194304−65472)/32768
    + 10 ≈ **136 段**——对真有 400 万线程的进程可忽略（光 task_struct 就 400 万个，maps 多 136 行不算什么）。
    注意：段数**不是**全程对数级，只在前 65472 槽是对数级，之后线性；封顶把「每段 VA 请求有上界」换成
    「病态规模下段数线性增长」，是有意的权衡。
  - **单段封顶的理由**：无限翻倍会让极端进程的最后一段要求 GiB 级**连续** VA，`get_unmapped_area` 可能失败；
    封顶后每次 `get_unmapped_area` 最多请求 2MiB，稳。
  - **「旧地址不失效」如何保住**：段一旦创建**永不移动、永不缩小、永不释放**（直到 `__mmput`）；
    增长 = 在新的 VA 装一个**新段**，绝不动已有段。已通过 prctl 回填给用户的 `uaddr`/缓存的 `kaddr`
    天然全部有效。（对比：单段原地 `mremap` 扩容会搬迁 → 已发地址失效；靠「恰好有相邻空闲 VA」不保证 →
    单段扩容方案被否。）
  - **每段元数据（`pages[]` + `slot_bitmap`）在建段时按段大小一次性定长分配**，不再需要
    `kvrealloc` 翻倍 + 尾部 memset。增长从「重分配数组」变成「追加段」，那块反复打磨的数组增长逻辑整个消失。
  - **slot 标识改为 (段指针, 段内 slot 号)**，不用全局 slot 号。task_struct 直接存**段指针**（段永不释放，
    指针恒稳）：释放路径 O(1)，无需「全局号→段」反查；分配时也不必从全局号反算段内 offset。
  - **soft 上限 `SCHED_HINT_MAX_SLOTS` = `PID_MAX_LIMIT`**（64 位 4M / 32 位 32768），降级为 sanity 断言
    （`WARN_ON_ONCE` 防分配器逻辑 bug），不再用于预留 VA。真实容量由内存和线程数自然封顶。
    **锚 `PID_MAX_LIMIT` 而非 `FUTEX_TID_MASK` 的理由**：每线程至多用一个 slot，"能同时存在多少线程"由 pid
    分配器钳在 `PID_MAX_LIMIT`——这才是并发 slot 数的真上限；`FUTEX_TID_MASK`(~10 亿)是 futex 字里 TID
    字段的**位宽**（一个 TID 值最大能取到多少），不是活线程**个数**，用它是概念错位（本轮纠正）。
    编译期改钉 `static_assert(SCHED_HINT_SEG_MAX_PAGES <= ~0UL >> PAGE_SHIFT)`（单段 VMA 长度不溢出）。
  - **额外收益：隔离性更好**。32 位未 seal 时，某线程 munmap 只毁掉它落在的那一段（影响该段上的线程），
    不再是单个巨型 VMA 一发入魂拆掉全进程调度能力。
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
/* 一个按需增长的段：一段预留 VMA + 背后的内核页。段建后永不移动/缩小/释放（直到 __mmput）。 */
struct sched_hint_segment {
    struct vm_special_mapping spec;  /* 内嵌：.fault 里 container_of 回到段，无需查表/加锁 */
    unsigned long   uaddr_base;      /* 本段 VMA 用户态基址 */
    unsigned long   nr_pages;        /* 本段页数（建段时定长）；slot 容量 = nr_pages*SLOTS_PER_PAGE，派生不另存 */
    unsigned long   nr_slots_used;
    struct page   **pages;           /* [nr_pages]，pages[i]==NULL 表示未 alloc；建段时定长 */
    unsigned long  *slot_bitmap;     /* [nr_pages*SLOTS_PER_PAGE bits]，密排复用；建段时定长 */
    struct list_head node;
};
/* slot 容量用 sched_hint_seg_slots(seg) = seg->nr_pages*SLOTS_PER_PAGE 就地派生 */

/* mm_struct 里 */
struct sched_hint_area {
    struct list_head segments;    /* struct sched_hint_segment，oldest first */
    unsigned long    nr_segments;
    unsigned long    total_slots; /* 各段 [nr_pages*SLOTS_PER_PAGE] 之和 */
    struct mutex     lock;
};
struct sched_hint_area *sched_hint_area;  /* mm 里；NULL = 本进程未启用 */

/* task_struct 里 */
struct sched_hint         *sched_hint_kaddr; /* = page_address(seg->pages[slot/SLOTS_PER_PAGE])
                                                + (slot%SLOTS_PER_PAGE)*SCHED_HINT_SLOT_SIZE */
struct sched_hint_segment *sched_hint_seg;   /* 本线程 slot 所属段；NULL = 无 */
int                        sched_hint_slot;  /* 段内 slot 号；-1 = 无 */
```
常量：`SCHED_HINT_SLOT_SIZE = sizeof(struct sched_hint)`；`SLOTS_PER_PAGE = PAGE_SIZE/SCHED_HINT_SLOT_SIZE`；
`SCHED_HINT_SEG_FIRST_PAGES=1`、`SCHED_HINT_SEG_MAX_PAGES=512`；`SCHED_HINT_MAX_SLOTS`（sanity 断言用，见第 1 节）。

### 关键流程
1. **首个注册线程**：分配 `sched_hint_area`（空段链表），暂不建任何 VMA。
2. **每次 prctl（`mmap_write_lock` + `area->lock` 下）**：从**最老段**往新扫，找第一个有空位的段取 slot；
   全满则**追加一段**（`sched_hint_next_seg_pages` 定页数：首段 1 页、否则上一段 ×2 封顶 512 页
   → `get_unmapped_area` + `_install_special_mapping` 装 VMA，flags
   `VM_SEALED | VM_DONTCOPY | VM_READ | VM_WRITE | VM_MAYREAD | VM_MAYWRITE`；段内嵌 `spec`：
   `.fault` 惰性填页 / `.mremap` 返 -EINVAL / `.close` 留 NULL）。取到 slot 后：若对应页空则
   `alloc_page(__GFP_ZERO)` 并 `WRITE_ONCE(seg->pages[i], page)`（对无锁 fault 发布）→ **slot 交付时**
   memset 清 payload + 写 magic/version（页比线程活得久，复用槽位必须清前任标签）→ 算 `kaddr` 与
   `uaddr = seg->uaddr_base + slot*SLOT_SIZE` → `put_user(uaddr, arg2)`（**两把锁都放掉之后**，见第 4 节）→
   记 `task->{sched_hint_seg, sched_hint_slot}` + `WRITE_ONCE(kaddr)`；`put_user` 失败回滚 slot。
   - 密排复用**跨段**：某段里线程退出留下的空洞会被后续注册优先填掉（先扫老段），页占用不会白涨。
3. **`.fault`（无锁）**：`container_of(sm, seg, spec)` 拿到段 → `pgoff < seg->nr_pages` 且
   `READ_ONCE(seg->pages[pgoff])` 非空 → `get_page` + `vmf->page = page`（核心 mm 插 PTE，用户 PTE 那份
   `_refcount` 由核心加）；否则 **SIGBUS**。不碰 `area->lock`、不依赖 `mm->sched_hint_area`。
4. **线程退出（`exit_mm` 之前、`task->mm` 仍持引用处）**：持 `area->lock`，用 `task->sched_hint_seg`
   直接归还 slot 到该段位图（O(1)，无全局号反查），清 `task->{kaddr,seg,slot}`；**不释放页/段**。
5. **进程退出**：`__mmput` 里 `exit_mmap(mm)` **之后**调 `sched_hint_free_area(mm)`——遍历段链表，逐段
   `put_page` 所有 `pages[]` + free 位图 + free 段结构，最后 free area + 置 NULL，**只跑一次**。
   段结构须活到此刻（VMA 持有其内嵌 `spec` 指针）。**`.close` 不参与释放**（见第 1 节「释放点解耦」）。
6. **跨任务读**：`p->sched_hint_kaddr`（判 NULL 后）直接读，`p != current` 成立。读侧不加锁。

## 3. 分阶段任务清单（TODO）

> 环境注意：本开发环境**不能 boot 内核**，只能逐文件编译检查（`.config` 在，可 `make kernel/sched/hint.o` 等）。
> 用户（YaQia）会在自己笔记本上 boot 实测。建议先把内核骨架编译稳定，再动用户态 Pass。

### 阶段 A：内核数据结构与分配器 ✅（多段模型）
- [x] 常量放内核内部头（未动 uapi，struct sched_hint 本身不变）：
      `include/linux/sched/hint.h` 定义 `SCHED_HINT_SLOT_SIZE/SLOTS_PER_PAGE`、
      `SCHED_HINT_SEG_FIRST_PAGES=1`/`SCHED_HINT_SEG_MAX_PAGES=512`、
      `SCHED_HINT_MAX_SLOTS`（sanity 断言用，= `PID_MAX_LIMIT`，不再区分架构/不再锚 `FUTEX_TID_MASK`）、
      `struct sched_hint_segment`（内嵌 `vm_special_mapping`）、`struct sched_hint_area`（段链表）、
      三个函数原型（`set_sched_hint_prctl`/`sched_hint_exit_task`/`sched_hint_free_area`）。
- [x] `include/linux/mm_types.h`：`sched_hint_offset`/`has_sched_hint` → `struct sched_hint_area *`（段链表）。
- [x] `include/linux/sched.h`：task 存 `sched_hint_kaddr` + `struct sched_hint_segment *sched_hint_seg`
      + `int sched_hint_slot`（段内号，-1=无）。
- [x] `kernel/sched/hint.c`：area 创建（空段链表）、段建（`get_unmapped_area`+`_install_special_mapping`，
      页数指数增长封顶）、每段 `pages[]`/`slot_bitmap` 建段时定长分配、跨段密排 slot alloc/free、
      内核页惰性 `alloc_page`（`WRITE_ONCE` 发布）、kaddr 计算、magic/version **slot 交付时**写。
      单段 VMA 长度溢出有 `static_assert(SCHED_HINT_SEG_MAX_PAGES <= ~0UL >> PAGE_SHIFT)` 保证。
      **无 `kvrealloc` 数组翻倍**（多段模型下每段定长，不再需要）。checkpatch 0/0、`W=1` 编译干净。

### 阶段 B：prctl 接口 ✅
- [x] `include/uapi/linux/prctl.h`：`PR_SET_SCHED_HINT_OFFSET` → `PR_SET_SCHED_HINT`（值 83）。
- [x] `kernel/sys.c`：dispatch 改名，调用新双参签名，`arg3/4/5` 非零返回 `-EINVAL`
      （整树编译仍待阶段 D 清理 fork.c 旧 pin 代码）。
- [x] `kernel/sched/hint.c` `set_sched_hint_prctl(task, uptr)`：`mmap_write_lock_killable` 下
      check-and-create area（首次注册竞争串行化），`area->lock` 下跨段分配 slot（满则追加段、建 VMA）；
      `put_user` 在 **两把锁都放掉之后**（自死锁规避）；EFAULT 回滚 slot；重复调用幂等（回填同一地址，
      经 `task->sched_hint_seg`）；compat 拒绝 `-EOPNOTSUPP`。

### 阶段 C：VMA 与惰性映射 ✅（每段一个 VMA）
- [x] 采用 `vm_special_mapping` + `_install_special_mapping`（评估点已解决：接受任意 vm_flags，
      可叠 `VM_SEALED`；x86 vDSO 同款调用模式）。`special_mapping_vmops` 自带
      `.may_split=-EINVAL` + `VM_DONTEXPAND` + 防 VMA merge。**每段一个 VMA**，`vm_private_data` 指向
      段内嵌的 `spec`；`.fault` 经 `container_of` 回到段、**无锁**读 `READ_ONCE(seg->pages[pgoff])`，
      `get_page`+`vmf->page`（不用 `vm_insert_page`）。`sm->close` 留 NULL（special_mapping_close
      判空跳过 = 空转），释放唯一在 `__mmput`。`sm->mremap` 返回 `-EINVAL`。
- [x] flags：`VM_SEALED | VM_DONTCOPY | VM_READ | VM_WRITE | VM_MAYREAD | VM_MAYWRITE`
      （`VM_DONTEXPAND` 由 `__install_special_mapping` 自动补）。
      64 位 mseal 拦截免费生效；32 位 `VM_SEALED==VM_NONE` no-op，靠 refcount 兜底。
- [x] `.fault` 只映射已注册 slot 的页、其余 SIGBUS（决策与理由见第 2 节流程 3）。

### 阶段 D：fork / exit 清理 ✅（DF/UAF 向量逐条处理）
> 根因：`dup_task_struct`/`dup_mm` 是结构体**浅拷贝**，会把 hint 指针/slot 原样复制给子 task/子 mm。
> 新模型无 pin refcount 兜底，每处必须显式清。三大杀手：DF1（共享 area 指针）、DF2（继承 slot）、UAF2（exec 换 mm）。
- [x] **DF1**：`mm_init()` 经 `mm_init_sched_hint(mm)` helper 清 `mm->sched_hint_area`（与
      `mm_init_owner`/`mm_init_uprobes_state` 同款模式）。fresh mm 和 dup mm 都过这里。
- [x] **DF2（最关键）**：`dup_task_struct()` 在"继承指针脱离"集群（`splice_pipe`/`wake_q` 一带）清三字段，
      kaddr 用 `WRITE_ONCE`（与其余写点标注统一）。否则子线程继承父 seg/slot → 退出时释放父的 slot。
- [x] **UAF2**：`fs/exec.c` `begin_new_exec()` 在 `exec_mmap()` 紧前清三字段（kaddr `WRITE_ONCE`），清而不还
      slot——旧位图随 area 在 `__mmput` 整体销毁。失败语义已核实：`bprm->point_of_no_return`（de_thread 之前）
      之后所有失败均致命（SIGKILL 已 pending，或 `bprm_execve` out: 的 `force_fatal_sig(SIGSEGV)`）；唯一幸存的
      失败（`bprm_creds_from_file`）发生在清点之前。
- [x] **删除旧 pin 代码**：free_task 的 unpin 段 + copy_thread 后的 clone-pin 段（fork.c，共 -45 行）。
- [x] **free_task 不还 slot**：对 hint 零操作（`WARN_ON_ONCE(kaddr)` 断言为可选项，未加）。
- [x] **还 slot 放对位置**：`kernel/exit.c` `do_exit` 在 `exit_mm()` 之前调 `sched_hint_exit_task(tsk)`：
      经 `task->sched_hint_seg` 持 `area->lock` 清段位图 bit + 清本 task kaddr/seg/slot。
- [x] **释放 area（唯一真源）**：`kernel/fork.c` `__mmput` 在 `exit_mmap(mm)` 之后调 `sched_hint_free_area(mm)`。
      段结构须活到此刻（VMA 持有其内嵌 `spec` 指针）；`.close` 空转，释放只在这里。
- [x] **prctl 错误路径**：**决定不做**。空 area 无害（重试语义与无 area 完全一致，`__mmput` 兜底回收）；
      有段的 area **必须**保留（VMA 持有 `&seg->spec`，回滚即 UAF）。唯一影响正确性的回滚（slot 位）已在
      put_user 失败路径实现；本条字面的"就地释放"在常见失败形态（EFAULT/配页失败）下是 UAF 陷阱。

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
  段/pages/bitmap/area 释放**只**在 `__mmput`（`exit_mmap` 之后）`sched_hint_free_area` 一处遍历段链表做一次。
  理由：`.close` 是逐-VMA 回调，32 位未 seal 时 munmap 会中途触发它，若在此 put_page → 存活线程 kaddr 悬挂 UAF，
  且与 refcount 兜底矛盾。把释放解耦到「mm 消失」这一唯一时刻，32/64 位一套代码、无 double-free、无 UAF。
- **refcount 兜底原理**：页有两份 `_refcount`——`seg->pages[i]`（内核持有）+ 用户 PTE 一份
  （`.fault` 经 `get_page`+`vmf->page` 由核心 mm 加）。
  用户 munmap 只掉 PTE 那份，`seg->pages[i]` 那份仍在 → 页不进回收 → `page_address()` 算的 kaddr 永久有效。
  这是新模型对 pin 方案的根本升级：内核拥有**自己的页**，用户页表只是其视图；撤视图不动数据源。
- **`VM_SEALED` 与释放正交**：seal 只决定「用户能否中途拆 VMA」（64 位不能/32 位能），
  释放点始终是 `__mmput`，与 seal 无关。别把两者耦合（本次讨论纠正的关键教训）。
- **put_user 必须在锁外（已实现）**：put_user 可能缺页，缺页可能恰好由某段 sched_hint VMA 的
  `.fault` 服务（用户把回填目标指进了预留区）；且 slot 分配路径持 `mmap_write_lock`，而缺页要拿
  mmap_lock → 锁内 put_user 会自死锁。故 `put_user` 放在 `area->lock` **和** `mmap_write_lock` 都释放之后。
- **首次注册 / 段追加竞争（已实现）**：首次 prctl 建 area 在 `mmap_write_lock_killable` 下 check-and-create
  串行化；段追加要装 VMA，必须持 `mmap_write_lock` → slot 分配整段在 `mmap_write_lock` + `area->lock`
  下进行（比旧单 VMA 模型多拿 mmap_write_lock，但段追加是罕见事件：每 1/2/4…512 页一次，可接受）。
- **段几何与 VMA 长度溢出（已实现）**：段页数 1→2→4…封顶 `SCHED_HINT_SEG_MAX_PAGES`(512)；
  `static_assert(SCHED_HINT_SEG_MAX_PAGES <= ~0UL >> PAGE_SHIFT)` 编译期钉死单段 VMA 长度不溢出。
  `SCHED_HINT_MAX_SLOTS` 仅在段追加时作 `WARN_ON_ONCE` sanity（防分配器逻辑 bug），不参与预留。
  **每段 `pages[]`/`slot_bitmap` 建段时按段大小定长分配**，无 `kvrealloc` 翻倍、无尾部 memset。
- **并发**：`sched_hint_area->lock`（mutex）保护段链表及每段的位图/pages[]/计数。段一旦建成其 `pages[]`
  数组指针**永不搬迁**（定长），`seg->pages[i]` 元素由 `WRITE_ONCE` 发布、`.fault` 侧 `READ_ONCE` 读，
  fault 全程**不碰 `area->lock`**（经 `container_of` 直达段）。`task->sched_hint_kaddr` 注册时算好存 task、
  **从不回读段数组**，故段增长/追加不影响任何活线程；prctl 上下文可睡眠，调度器读 `sched_hint_kaddr`
  在原子上下文——**读侧不加锁**，依据是「kaddr 在线程生命周期内不被 area 维护修改、页在该线程存活期间不释放」。
- **还 slot 无需与调度器读同步（已澄清）**：还 slot 只清位图一个 bit；被还的 task 即将 dead，
  调度器不会再通过 `p->sched_hint_kaddr` 读它 → 无争用。slot 被新线程复用时是**另一个 task_struct**，
  指针绑 task、老 task 死了没人读，不会「读到上一租户残留」。锁只护位图位操作，读路径完全不碰锁。
- **32 位降级（已接受）**：`VM_SEALED == VM_NONE`、`vma_is_sealed()` 恒 false，seal 全 no-op。
  32 位用户可 munmap 自己的映射（自废武功），但靠 refcount 兜底**不崩**（页 refcount 停在 1，kaddr 仍有效），
  后续该线程写 hint 会段错误——属预期行为。sched_ext 场景几乎只在 64 位，不为 32 位投入侵入式 patch。
- **跨任务读的内存序**：`p->sched_hint_kaddr` 设置（prctl）与调度器读之间，靠常规发布语义；
  内核自有页内容由用户写、内核读，仍是 best-effort，无需 barrier（与原设计一致）。
  **访问标注规则**：`sched_hint_kaddr` 有跨 CPU 无锁读者（调度器读非 current 任务）→ 其**所有写点**
  （prctl 发布、exit/exec 清除）统一 `WRITE_ONCE`；`sched_hint_seg`/`sched_hint_slot` 无任何跨任务读者
  （仅本任务 prctl/exit/exec 及 fork 写未运行的 child）→ 全部普通访问。同一位置禁止混用标注/裸访问（KCSAN）。
  另外 `seg->pages[i]` 有无锁 `.fault` 读者 → 发布用 `WRITE_ONCE`、fault 侧 `READ_ONCE`。
  若将来 BPF 需要读 `p->sched_hint_seg/slot`（当前设计不需要），须将其全访问点迁移到 READ_ONCE/WRITE_ONCE。
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
