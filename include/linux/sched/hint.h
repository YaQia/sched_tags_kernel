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
#include <linux/mutex.h>
#include <linux/threads.h>
#include <linux/types.h>
#include <uapi/linux/futex.h>

/*
 * Slot geometry: one slot per struct sched_hint, so one page holds
 * PAGE_SIZE / sizeof(struct sched_hint) slots (64 on 4K pages, 256 on 16K
 * pages -- no hardcoded 64 anywhere). hint.c pins sizeof == 64 statically.
 */
#define SCHED_HINT_SLOT_SIZE	sizeof(struct sched_hint)
#define SLOTS_PER_PAGE		(PAGE_SIZE / SCHED_HINT_SLOT_SIZE)

/*
 * Size of the slot space; the reserved VMA is SCHED_HINT_MAX_SLOTS *
 * SCHED_HINT_SLOT_SIZE bytes of *virtual* space. Physical pages and the
 * per-mm metadata stay lazy/grow-on-demand, so this only costs address
 * space: one vm_area_struct, no page tables until touched.
 *
 * The reservation must never need to grow (and thus move) at runtime, or
 * addresses already handed out to userspace would be invalidated. So size
 * it above any possible thread count:
 *
 * 64-bit: FUTEX_TID_MASK (0x3fffffff) bounds the TID field of a futex
 * word and is above any TID the kernel can hand out, hence above any
 * per-process thread total; ~64GiB out of the 128TiB user address space.
 *
 * 32-bit: user address space is only ~3GiB and cannot host 64GiB; clamp
 * to 32x the pid ceiling (64MiB).
 */
#ifdef CONFIG_64BIT
#define SCHED_HINT_MAX_SLOTS	((unsigned long)FUTEX_TID_MASK)
#else
#define SCHED_HINT_MAX_SLOTS	((unsigned long)PID_MAX_LIMIT * 32)
#endif

/*
 * Per-mm scheduling-hint area. Owns the kernel pages backing the reserved
 * user VMA; hands out SCHED_HINT_SLOT_SIZE-byte slots to threads via a
 * bitmap allocator.
 *
 * Locking: @lock (mutex; registration / thread-exit / .fault are all
 * sleepable) protects @slot_bitmap, @pages and their growth. A task's
 * sched_hint_kaddr is computed once at registration and stored in the
 * task_struct; it is never re-derived from these arrays, so they may be
 * reallocated (grown) under @lock without disturbing any live task, and
 * the scheduler never takes @lock when dereferencing kaddr.
 */
struct sched_hint_area {
	unsigned long	uaddr_base;	/* base user address of the reserved VMA */
	/* grow-on-demand under @lock; capacity measured in slots */
	struct page   **pages;		/* [capacity_slots / SLOTS_PER_PAGE], NULL = unbacked */
	unsigned long  *slot_bitmap;	/* [capacity_slots bits], dense reuse */
	unsigned long	capacity_slots;
	unsigned long	nr_slots_used;
	struct mutex	lock;
};

/* prctl(PR_SET_SCHED_HINT, uptr): register the calling thread, back-fill uptr. */
int set_sched_hint_prctl(struct task_struct *t, unsigned long uptr);

/* Thread exit: return this task's slot to the bitmap (mm still held). */
void sched_hint_exit_task(struct task_struct *t);

/* mm teardown: free all pages, bitmap and the area itself (once, in __mmput). */
void sched_hint_free_area(struct mm_struct *mm);

#endif /* CONFIG_SCHED_HINT */

#endif /* _LINUX_SCHED_HINT_KERNEL_H */
