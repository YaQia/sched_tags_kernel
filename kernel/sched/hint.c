// SPDX-License-Identifier: GPL-2.0
#include <linux/sched/hint.h>
#include <linux/bitops.h>
#include <linux/compat.h>
#include <linux/container_of.h>
#include <linux/err.h>
#include <linux/find.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmap_lock.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

/*
 * The 64-byte layout is an ABI contract shared with the userspace header
 * and the LLVM pass. Pin it down at compile time.
 */
static_assert(sizeof(struct sched_hint) == 64);
static_assert(offsetof(struct sched_hint, exec_dense) == 8);
static_assert(offsetof(struct sched_hint, memory_dense) == 9);
static_assert(offsetof(struct sched_hint, atomic_dense) == 10);
static_assert(offsetof(struct sched_hint, unshared) == 11);
static_assert(offsetof(struct sched_hint, load_trend) == 12);
static_assert(offsetof(struct sched_hint, atomic_magic) == 16);
static_assert(offsetof(struct sched_hint, dep_magic) == 24);
static_assert(offsetof(struct sched_hint, unshared_magic) == 32);
static_assert(offsetof(struct sched_hint, dep_role) == 40);

/*
 * A segment's VMA spans nr_pages << PAGE_SHIFT bytes, capped at
 * SCHED_HINT_SEG_MAX_PAGES; assert that length can never overflow.
 */
static_assert(SCHED_HINT_SEG_MAX_PAGES <= (~0UL >> PAGE_SHIFT));

/*
 * Force BTF generation for scheduling hint enums.
 *
 * The compiler only emits DWARF (and hence BTF) entries for types that are
 * actually *referenced as types* in a translation unit.  Using an enum
 * constant (e.g.  SCHED_HINT_MAGIC) in an expression is not enough -- the
 * compiler folds it to an integer literal and never records the enum type.
 *
 * Declaring an unused pointer variable of each enum type is the lightest
 * way to make the compiler emit the type information.  pahole then converts
 * DWARF -> BTF, and `bpftool btf dump` makes them visible in vmlinux.h.
 */
enum sched_hint_metadata *__btf_sched_hint_metadata __maybe_unused;
enum sched_hint_exec_dense *__btf_sched_hint_exec_dense __maybe_unused;
enum sched_hint_memory_dense *__btf_sched_hint_memory_dense __maybe_unused;
enum sched_hint_load_trend *__btf_sched_hint_load_trend __maybe_unused;
enum sched_hint_atomic_dense *__btf_sched_hint_atomic_dense __maybe_unused;
enum sched_hint_unshared *__btf_sched_hint_unshared __maybe_unused;
enum sched_hint_dep_role *__btf_sched_hint_dep_role __maybe_unused;

/*
 * Special mapping
 *
 * The fault handler reaches its segment via container_of() on the embedded
 * spec, so it needs no lookup and does not take area->lock. It still runs
 * under the core mm fault serialization (mmap_read_lock or the per-VMA read
 * lock), not lock-free. It reads seg->pages[] with READ_ONCE because a fault
 * on an already-installed VMA can run concurrently with a registration
 * publishing a page (WRITE_ONCE): the per-VMA read lock does not exclude the
 * registrant's mmap_write_lock. seg->pages base and seg->nr_pages are fixed
 * at segment creation and never change, so they are read plainly. Pages are
 * never cleared before mm teardown, so a fetched page cannot be freed under
 * the fault.
 *
 * spec.close is deliberately left NULL: pages and segments are released
 * exactly once at mm teardown (sched_hint_free_area), never on VMA removal.
 */

static vm_fault_t sched_hint_vma_fault(const struct vm_special_mapping *sm,
				       struct vm_area_struct *vma,
				       struct vm_fault *vmf)
{
	struct sched_hint_segment *seg =
		container_of(sm, struct sched_hint_segment, spec);
	struct page *page;

	if (vmf->pgoff >= seg->nr_pages)
		return VM_FAULT_SIGBUS;

	page = READ_ONCE(seg->pages[vmf->pgoff]);
	if (!page)
		return VM_FAULT_SIGBUS;

	get_page(page);
	vmf->page = page;
	return 0;
}

/* Addresses were handed out for the old location; refuse to move. */
static int sched_hint_vma_mremap(const struct vm_special_mapping *sm,
				 struct vm_area_struct *new_vma)
{
	return -EINVAL;
}

/*
 * Segment allocator
 *
 * Segments are chained oldest-first. Slots are handed out densely from the
 * oldest segment with a free slot, so a slot freed on thread exit is reused
 * before a new segment is created. All of this runs under area->lock;
 * installing a segment's VMA additionally needs mmap_write_lock (held by
 * the sole caller, prctl registration).
 */

static unsigned long sched_hint_next_seg_pages(struct sched_hint_area *area)
{
	struct sched_hint_segment *last;

	if (list_empty(&area->segments))
		return SCHED_HINT_SEG_FIRST_PAGES;

	last = list_last_entry(&area->segments, struct sched_hint_segment,
			       node);
	return min(last->nr_pages * 2, SCHED_HINT_SEG_MAX_PAGES);
}

static struct sched_hint_segment *
sched_hint_seg_create(struct mm_struct *mm, struct sched_hint_area *area)
{
	unsigned long nr_pages = sched_hint_next_seg_pages(area);
	unsigned long nr_slots = nr_pages * SLOTS_PER_PAGE;
	unsigned long len = nr_pages << PAGE_SHIFT;
	struct sched_hint_segment *seg;
	struct vm_area_struct *vma;
	unsigned long addr;
	int ret;

	if (WARN_ON_ONCE(area->total_slots > SCHED_HINT_MAX_SLOTS - nr_slots)) {
		ret = -ENOSPC;
		goto err_ret;
	}

	seg = kzalloc_obj(*seg, GFP_KERNEL);
	if (!seg) {
		ret = -ENOMEM;
		goto err_ret;
	}

	seg->pages = kvzalloc_objs(*seg->pages, nr_pages, GFP_KERNEL);
	seg->slot_bitmap = kvzalloc_objs(*seg->slot_bitmap,
					 BITS_TO_LONGS(nr_slots), GFP_KERNEL);
	if (!seg->pages || !seg->slot_bitmap) {
		ret = -ENOMEM;
		goto err_free;
	}

	seg->spec.name = "[sched_hint]";
	seg->spec.fault = sched_hint_vma_fault;
	seg->spec.mremap = sched_hint_vma_mremap;

	addr = get_unmapped_area(NULL, 0, len, 0, 0);
	if (IS_ERR_VALUE(addr)) {
		ret = addr;
		goto err_free;
	}

	vma = _install_special_mapping(mm, addr, len,
				       VM_READ | VM_WRITE | VM_MAYREAD |
				       VM_MAYWRITE | VM_SEALED | VM_DONTCOPY,
				       &seg->spec);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto err_free;
	}

	seg->uaddr_base = addr;
	seg->nr_pages = nr_pages;
	list_add_tail(&seg->node, &area->segments);
	area->nr_segments++;
	area->total_slots += nr_slots;
	return seg;

err_free:
	kvfree(seg->slot_bitmap);
	kvfree(seg->pages);
	kfree(seg);
err_ret:
	return ERR_PTR(ret);
}

/*
 * Allocate a slot, returning its segment in @seg_out and the kernel-side
 * slot address in @hintp. Backs the slot's page on first use. Called with
 * area->lock held (and mmap_write_lock, in case a new segment is created).
 * Returns the in-segment slot index or -errno.
 */
static int sched_hint_alloc_slot(struct mm_struct *mm,
				 struct sched_hint_area *area,
				 struct sched_hint_segment **seg_out,
				 struct sched_hint **hintp)
{
	struct sched_hint_segment *seg = NULL, *iter;
	unsigned long slot = 0, page_idx;
	struct page *page;
	struct sched_hint *hint;

	list_for_each_entry(iter, &area->segments, node) {
		unsigned long nr_slots = sched_hint_seg_slots(iter);

		if (iter->nr_slots_used == nr_slots)
			continue;
		slot = find_first_zero_bit(iter->slot_bitmap, nr_slots);
		if (slot < nr_slots) {
			seg = iter;
			break;
		}
	}

	if (!seg) {
		seg = sched_hint_seg_create(mm, area);
		if (IS_ERR(seg))
			return PTR_ERR(seg);
		slot = 0;
	}

	page_idx = slot / SLOTS_PER_PAGE;
	page = seg->pages[page_idx];
	if (!page) {
		/*
		 * __GFP_ZERO clears the whole page, not just this slot: the
		 * fault handler maps the entire page to userspace, so the
		 * other SLOTS_PER_PAGE-1 slots (not yet handed out) must not
		 * expose stale kernel data. Per-slot zeroing cannot cover
		 * them -- they are readable before their owners register.
		 */
		page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!page)
			return -ENOMEM;
		/*
		 * Publish the zeroed page for the fault handler, which reads
		 * this slot with READ_ONCE and does not take area->lock: a
		 * fault on an already-installed VMA can run concurrently with
		 * this store (its per-VMA read lock does not exclude our
		 * mmap_write_lock). The single-copy-atomic store hands the
		 * reader either NULL or a fully valid, already-zeroed page.
		 */
		WRITE_ONCE(seg->pages[page_idx], page);
	}

	hint = (struct sched_hint *)((char *)page_address(page) +
			(slot % SLOTS_PER_PAGE) * SCHED_HINT_SLOT_SIZE);
	/*
	 * Pages outlive threads, so a reused slot may still carry the
	 * previous owner's tags: re-zero the payload and stamp the header.
	 */
	memset(hint, 0, sizeof(*hint));
	hint->magic = SCHED_HINT_MAGIC;
	hint->version = SCHED_HINT_VERSION;

	set_bit(slot, seg->slot_bitmap);
	seg->nr_slots_used++;

	*seg_out = seg;
	*hintp = hint;
	return slot;
}

/*
 * Return a slot to its segment's bitmap. The backing page stays allocated
 * for the lifetime of the mm. Called with area->lock held.
 */
static void sched_hint_put_slot(struct sched_hint_segment *seg,
				unsigned long slot)
{
	clear_bit(slot, seg->slot_bitmap);
	seg->nr_slots_used--;
}

/* Create the (empty) area. Called with mm->mmap_lock held for writing. */
static struct sched_hint_area *sched_hint_area_create(void)
{
	struct sched_hint_area *area;

	area = kzalloc_obj(*area, GFP_KERNEL);
	if (!area)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&area->segments);
	mutex_init(&area->lock);
	return area;
}

int set_sched_hint_prctl(struct task_struct *t, unsigned long uptr)
{
	unsigned long __user *up = (unsigned long __user *)uptr;
	struct mm_struct *mm = t->mm;
	struct sched_hint_area *area;
	struct sched_hint_segment *seg;
	struct sched_hint *hint = NULL;
	unsigned long uaddr;
	int slot, ret;

	if (!mm)
		return -EINVAL;
	/*
	 * The uptr backfill writes a native-width pointer; a compat task
	 * would have its neighbouring word clobbered. Native tasks only.
	 */
	if (in_compat_syscall())
		return -EOPNOTSUPP;
	if (!access_ok(up, sizeof(*up)))
		return -EFAULT;

	/* Idempotent: re-registration backfills the same address. */
	if (t->sched_hint_slot >= 0) {
		seg = t->sched_hint_seg;
		uaddr = seg->uaddr_base +
			(unsigned long)t->sched_hint_slot *
			SCHED_HINT_SLOT_SIZE;
		return put_user(uaddr, up);
	}

	ret = mmap_write_lock_killable(mm);
	if (ret)
		return ret;
	area = mm->sched_hint_area;
	if (!area) {
		area = sched_hint_area_create();
		if (IS_ERR(area)) {
			mmap_write_unlock(mm);
			return PTR_ERR(area);
		}
		mm->sched_hint_area = area;
	}

	mutex_lock(&area->lock);
	slot = sched_hint_alloc_slot(mm, area, &seg, &hint);
	mutex_unlock(&area->lock);
	mmap_write_unlock(mm);
	if (slot < 0)
		return slot;

	uaddr = seg->uaddr_base + (unsigned long)slot * SCHED_HINT_SLOT_SIZE;

	/*
	 * put_user() outside the locks: it may fault, and the fault may be
	 * served by one of these VMAs (user pointed into the reservation),
	 * and a faulting store cannot be taken under mmap_write_lock.
	 */
	ret = put_user(uaddr, up);
	if (ret) {
		mutex_lock(&area->lock);
		sched_hint_put_slot(seg, slot);
		mutex_unlock(&area->lock);
		return ret;
	}

	t->sched_hint_seg = seg;
	t->sched_hint_slot = slot;
	WRITE_ONCE(t->sched_hint_kaddr, hint);
	return 0;
}

void sched_hint_exit_task(struct task_struct *t)
{
	struct sched_hint_area *area;
	struct sched_hint_segment *seg = t->sched_hint_seg;
	int slot = t->sched_hint_slot;

	if (slot < 0)
		return;

	/*
	 * Runs in the task's own exit/exec path, before its mm reference is
	 * dropped, so mm->sched_hint_area is still valid.
	 */
	area = t->mm->sched_hint_area;
	t->sched_hint_seg = NULL;
	t->sched_hint_slot = -1;
	WRITE_ONCE(t->sched_hint_kaddr, NULL);

	if (WARN_ON_ONCE(!area || !seg))
		return;

	mutex_lock(&area->lock);
	sched_hint_put_slot(seg, slot);
	mutex_unlock(&area->lock);
}

void sched_hint_free_area(struct mm_struct *mm)
{
	struct sched_hint_area *area = mm->sched_hint_area;
	struct sched_hint_segment *seg, *tmp;
	unsigned long i;

	if (!area)
		return;
	mm->sched_hint_area = NULL;

	list_for_each_entry_safe(seg, tmp, &area->segments, node) {
		for (i = 0; i < seg->nr_pages; i++)
			if (seg->pages[i])
				put_page(seg->pages[i]);
		list_del(&seg->node);
		kvfree(seg->slot_bitmap);
		kvfree(seg->pages);
		kfree(seg);
	}
	mutex_destroy(&area->lock);
	kfree(area);
}
