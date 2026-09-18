// SPDX-License-Identifier: GPL-2.0
#include <linux/sched/hint.h>
#include <linux/bitops.h>
#include <linux/bitmap.h>
#include <linux/compat.h>
#include <linux/err.h>
#include <linux/find.h>
#include <linux/kernel.h>
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
 * The reserved VMA length is SCHED_HINT_MAX_SLOTS * SLOT_SIZE: ~64GiB on
 * 64-bit, 64MiB on 32-bit (where FUTEX_TID_MASK would not fit the ~3GiB
 * user address space, hence the smaller clamp). Assert the product can
 * never overflow an unsigned long.
 */
static_assert(SCHED_HINT_MAX_SLOTS <= ~0UL / SCHED_HINT_SLOT_SIZE);

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
 * Slot allocator
 *
 * The bitmap and the page-pointer array are grown by doubling under
 * area->lock; everything below capacity_slots within the current
 * allocations is either valid data or zero/NULL.
 */

static int sched_hint_area_grow(struct sched_hint_area *area)
{
	unsigned long new_capacity = min(area->capacity_slots * 2,
					 SCHED_HINT_MAX_SLOTS);
	size_t old_words = BITS_TO_LONGS(area->capacity_slots);
	size_t new_words = BITS_TO_LONGS(new_capacity);
	size_t old_entries = DIV_ROUND_UP(area->capacity_slots,
					  SLOTS_PER_PAGE);
	size_t new_entries = DIV_ROUND_UP(new_capacity, SLOTS_PER_PAGE);
	unsigned long *bitmap;
	struct page **pages;

	if (new_capacity == area->capacity_slots)
		return -ENOSPC;

	bitmap = kvrealloc(area->slot_bitmap,
			   new_words * sizeof(unsigned long), GFP_KERNEL);
	if (!bitmap)
		return -ENOMEM;
	/*
	 * Zero the tail explicitly: krealloc and vrealloc differ in how
	 * (and whether) they zero the newly available tail.
	 */
	memset(bitmap + old_words, 0,
	       (new_words - old_words) * sizeof(unsigned long));
	area->slot_bitmap = bitmap;

	pages = kvrealloc(area->pages, new_entries * sizeof(struct page *),
			  GFP_KERNEL);
	if (!pages)
		return -ENOMEM;
	memset(pages + old_entries, 0,
	       (new_entries - old_entries) * sizeof(struct page *));
	area->pages = pages;

	area->capacity_slots = new_capacity;
	return 0;
}

/*
 * Allocate a slot and return its kernel-side address in @hintp.
 * Backs the slot's page on first use. Called with area->lock held.
 * Returns the slot index or -errno.
 */
static int sched_hint_alloc_slot(struct sched_hint_area *area,
				 struct sched_hint **hintp)
{
	unsigned long slot, page_idx;
	struct page *page;
	struct sched_hint *hint;
	int ret;

	slot = find_first_zero_bit(area->slot_bitmap, area->capacity_slots);
	if (slot >= area->capacity_slots) {
		ret = sched_hint_area_grow(area);
		if (ret)
			return ret;
		slot = find_first_zero_bit(area->slot_bitmap,
					   area->capacity_slots);
	}

	page_idx = slot / SLOTS_PER_PAGE;
	page = area->pages[page_idx];
	if (!page) {
		page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!page)
			return -ENOMEM;
		area->pages[page_idx] = page;
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

	set_bit(slot, area->slot_bitmap);
	area->nr_slots_used++;

	*hintp = hint;
	return slot;
}

/*
 * Return a slot to the bitmap. The backing page stays allocated for the
 * lifetime of the mm. Called with area->lock held.
 */
static void sched_hint_put_slot(struct sched_hint_area *area,
				unsigned long slot)
{
	clear_bit(slot, area->slot_bitmap);
	area->nr_slots_used--;
}

/*
 * Special mapping
 *
 * sm->close is deliberately NULL: release of the pages and the area
 * happens exactly once at mm teardown (sched_hint_free_area), never on
 * VMA removal.
 */

static vm_fault_t sched_hint_vma_fault(const struct vm_special_mapping *sm,
				       struct vm_area_struct *vma,
				       struct vm_fault *vmf)
{
	struct sched_hint_area *area = vma->vm_mm->sched_hint_area;
	unsigned long nr_pages;
	struct page *page;
	vm_fault_t ret = VM_FAULT_SIGBUS;

	/*
	 * Only pages carrying registered slots are backed; touching any
	 * other part of the reservation is out of contract. This also
	 * bounds metadata growth to actual registrations.
	 */
	if (!area)
		return VM_FAULT_SIGBUS;

	mutex_lock(&area->lock);
	nr_pages = DIV_ROUND_UP(area->capacity_slots, SLOTS_PER_PAGE);
	if (vmf->pgoff < nr_pages) {
		page = area->pages[vmf->pgoff];
		if (page) {
			get_page(page);
			vmf->page = page;
			ret = 0;
		}
	}
	mutex_unlock(&area->lock);
	return ret;
}

/* Addresses were handed out for the old location; refuse to move. */
static int sched_hint_vma_mremap(const struct vm_special_mapping *sm,
				 struct vm_area_struct *new_vma)
{
	return -EINVAL;
}

static const struct vm_special_mapping sched_hint_mapping = {
	.name = "[sched_hint]",
	.fault = sched_hint_vma_fault,
	.mremap = sched_hint_vma_mremap,
};

/*
 * Create the area and reserve the user VMA. Called with mm->mmap_lock
 * held for writing (this serializes first registrations).
 */
static struct sched_hint_area *sched_hint_area_create(struct mm_struct *mm)
{
	unsigned long len = SCHED_HINT_MAX_SLOTS * SCHED_HINT_SLOT_SIZE;
	struct sched_hint_area *area;
	struct vm_area_struct *vma;
	unsigned long addr;
	int ret;

	area = kzalloc_obj(*area, GFP_KERNEL);
	if (!area)
		return ERR_PTR(-ENOMEM);

	mutex_init(&area->lock);
	area->capacity_slots = SLOTS_PER_PAGE;
	area->slot_bitmap =
		kvzalloc_objs(*area->slot_bitmap,
			      BITS_TO_LONGS(area->capacity_slots),
			      GFP_KERNEL);
	area->pages =
		kvzalloc_objs(*area->pages,
			      DIV_ROUND_UP(area->capacity_slots,
					   SLOTS_PER_PAGE),
			      GFP_KERNEL);
	if (!area->slot_bitmap || !area->pages) {
		ret = -ENOMEM;
		goto err_free;
	}

	addr = get_unmapped_area(NULL, 0, len, 0, 0);
	if (IS_ERR_VALUE(addr)) {
		ret = addr;
		goto err_free;
	}

	area->uaddr_base = addr;
	/*
	 * Publish before installing the VMA: a fault (under mmap_read_lock)
	 * must find the area whenever it can see the VMA.
	 */
	mm->sched_hint_area = area;

	vma = _install_special_mapping(mm, addr, len,
				       VM_READ | VM_WRITE | VM_MAYREAD |
				       VM_MAYWRITE | VM_SEALED | VM_DONTCOPY,
				       &sched_hint_mapping);
	if (IS_ERR(vma)) {
		mm->sched_hint_area = NULL;
		ret = PTR_ERR(vma);
		goto err_free;
	}

	return area;

err_free:
	kvfree(area->pages);
	kvfree(area->slot_bitmap);
	mutex_destroy(&area->lock);
	kfree(area);
	return ERR_PTR(ret);
}

int set_sched_hint_prctl(struct task_struct *t, unsigned long uptr)
{
	unsigned long __user *up = (unsigned long __user *)uptr;
	struct mm_struct *mm = t->mm;
	struct sched_hint_area *area;
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
		area = mm->sched_hint_area;
		uaddr = area->uaddr_base +
			(unsigned long)t->sched_hint_slot *
			SCHED_HINT_SLOT_SIZE;
		return put_user(uaddr, up);
	}

	ret = mmap_write_lock_killable(mm);
	if (ret)
		return ret;
	area = mm->sched_hint_area;
	if (!area) {
		area = sched_hint_area_create(mm);
		if (IS_ERR(area)) {
			mmap_write_unlock(mm);
			return PTR_ERR(area);
		}
	}
	mmap_write_unlock(mm);

	ret = mutex_lock_interruptible(&area->lock);
	if (ret)
		return ret;
	slot = sched_hint_alloc_slot(area, &hint);
	mutex_unlock(&area->lock);
	if (slot < 0)
		return slot;

	uaddr = area->uaddr_base + (unsigned long)slot * SCHED_HINT_SLOT_SIZE;

	/*
	 * put_user() outside area->lock: it may fault, and the fault may be
	 * served by this very VMA (user pointed into the reservation), whose
	 * fault handler takes area->lock.
	 */
	ret = put_user(uaddr, up);
	if (ret) {
		mutex_lock(&area->lock);
		sched_hint_put_slot(area, slot);
		mutex_unlock(&area->lock);
		return ret;
	}

	t->sched_hint_slot = slot;
	WRITE_ONCE(t->sched_hint_kaddr, hint);
	return 0;
}

void sched_hint_exit_task(struct task_struct *t)
{
	struct sched_hint_area *area;
	int slot = t->sched_hint_slot;

	if (slot < 0)
		return;

	/*
	 * Runs in the task's own exit/exec path, before its mm reference is
	 * dropped, so mm->sched_hint_area is still valid.
	 */
	area = t->mm->sched_hint_area;
	t->sched_hint_slot = -1;
	WRITE_ONCE(t->sched_hint_kaddr, NULL);

	if (WARN_ON_ONCE(!area))
		return;

	mutex_lock(&area->lock);
	sched_hint_put_slot(area, slot);
	mutex_unlock(&area->lock);
}

void sched_hint_free_area(struct mm_struct *mm)
{
	struct sched_hint_area *area = mm->sched_hint_area;
	unsigned long i, nr_pages;

	if (!area)
		return;
	mm->sched_hint_area = NULL;

	nr_pages = DIV_ROUND_UP(area->capacity_slots, SLOTS_PER_PAGE);
	for (i = 0; i < nr_pages; i++)
		if (area->pages[i])
			put_page(area->pages[i]);
	kvfree(area->pages);
	kvfree(area->slot_bitmap);
	mutex_destroy(&area->lock);
	kfree(area);
}
