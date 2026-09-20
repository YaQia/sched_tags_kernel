/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_HINT_KERNEL_H
#define _LINUX_SCHED_HINT_KERNEL_H

/*
 * The sched_hint ABI (struct + enums + magic/version + bloom params) lives in
 * the single source of truth <uapi/linux/sched_hint.h>, shared with userspace
 * and the LLVM pass via `make headers_install`. This internal header only adds
 * the in-kernel prctl prototype.
 */
#include <uapi/linux/sched_hint.h>

struct task_struct;
struct mm_struct;
struct page;

#ifdef CONFIG_SCHED_HINT

#include <asm/page.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/mutex.h>
#include <linux/threads.h>
#include <linux/types.h>

/*
 * Slot geometry: one slot per struct sched_hint, so one page holds
 * PAGE_SIZE / sizeof(struct sched_hint) slots (64 on 4K pages, 256 on 16K
 * pages -- no hardcoded 64 anywhere). hint.c pins sizeof == 64 statically.
 */
#define SCHED_HINT_SLOT_SIZE	sizeof(struct sched_hint)
#define SLOTS_PER_PAGE		(PAGE_SIZE / SCHED_HINT_SLOT_SIZE)

/*
 * The hint area grows one segment at a time, on demand. The first segment
 * is a single page; each new segment doubles the previous page count until
 * SCHED_HINT_SEG_MAX_PAGES, then stays flat.
 *
 * Doubling keeps the segment count at O(log threads); the per-segment cap
 * bounds the size of any single get_unmapped_area() reservation, so growth
 * never needs a huge contiguous virtual range that might not exist.
 *
 * Because segments never move or shrink once created, addresses already
 * handed out to userspace stay valid: growth only appends a new segment at
 * a fresh virtual address, never disturbs an existing one.
 */
#define SCHED_HINT_SEG_FIRST_PAGES	1UL
#define SCHED_HINT_SEG_MAX_PAGES	512UL

/*
 * Soft upper bound on total slots across all segments, used only as a
 * sanity assert against an allocator logic bug. Each registered thread
 * uses at most one slot, and the number of threads that can exist at once
 * is capped by the pid allocator at PID_MAX_LIMIT (4M on 64-bit, 32768 on
 * 32-bit). That is the true ceiling on concurrently-used slots; actual
 * capacity is still bounded by memory, not by this number.
 *
 * (FUTEX_TID_MASK, ~1 billion, is the *width* of the TID value field in a
 * futex word, not a count of live threads, so it is the wrong bound here.)
 */
#define SCHED_HINT_MAX_SLOTS	((unsigned long)PID_MAX_LIMIT)

/*
 * One grow-on-demand segment: a reserved user VMA of @nr_pages pages plus
 * the kernel-owned pages backing it. Segments are chained oldest-first in
 * sched_hint_area::segments and never freed until mm teardown.
 *
 * @spec is embedded so the fault handler can container_of() from the VMA's
 * vm_private_data back to the owning segment without any lookup or lock.
 *
 * Slots are identified per-segment: a thread stores its segment pointer and
 * an in-segment slot index, so neither registration nor exit has to map a
 * global index back to a segment.
 */
struct sched_hint_segment {
	struct vm_special_mapping	spec;
	unsigned long			uaddr_base;	/* base user address of this segment's VMA */
	unsigned long			nr_pages;
	unsigned long			nr_slots_used;
	struct page		      **pages;		/* [nr_pages], NULL = unbacked */
	unsigned long		       *slot_bitmap;	/* [nr_pages * SLOTS_PER_PAGE bits], dense reuse */
	struct list_head		node;
};

/* Slot capacity is derived from the page count, never stored separately. */
static inline unsigned long sched_hint_seg_slots(const struct sched_hint_segment *seg)
{
	return seg->nr_pages * SLOTS_PER_PAGE;
}

/*
 * Per-mm scheduling-hint area: the list of segments shared by all threads
 * of this process.
 *
 * Locking: @lock (mutex; registration and thread-exit are sleepable)
 * protects @segments and each segment's bitmap/pages/counters. The fault
 * path does NOT take @lock: it reaches its segment via container_of() and
 * reads seg->pages[] with READ_ONCE (page pointers are published under
 * @lock and never cleared before mm teardown).
 *
 * A task's sched_hint_kaddr is computed once at registration and cached in
 * the task_struct; it is never re-derived from these structures, so the
 * scheduler reads it lock-free and segment growth never disturbs a live
 * task.
 */
struct sched_hint_area {
	struct list_head	segments;	/* struct sched_hint_segment, oldest first */
	unsigned long		nr_segments;
	unsigned long		total_slots;	/* sum of [nr_pages * SLOTS_PER_PAGE] over all segments */
	struct mutex		lock;
};

/* prctl(PR_SET_SCHED_HINT, uptr): register the calling thread, back-fill uptr. */
int set_sched_hint_prctl(struct task_struct *t, unsigned long uptr);

/* Thread exit: return this task's slot to the bitmap (mm still held). */
void sched_hint_exit_task(struct task_struct *t);

/* mm teardown: free all segments, pages and the area itself (once, in __mmput). */
void sched_hint_free_area(struct mm_struct *mm);

#endif /* CONFIG_SCHED_HINT */

#endif /* _LINUX_SCHED_HINT_KERNEL_H */
