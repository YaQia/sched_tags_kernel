/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_HINT_H
#define _LINUX_SCHED_HINT_H

#include <linux/types.h>

/*
 * We use enum here to export them in vmlinux easily.
 */
enum sched_hint_metadata {
	SCHED_HINT_MAGIC = 0x5348494EU, /* "SHIN" in ASCII */
	SCHED_HINT_VERSION = 1,
};

/*
 * Compute-dense sub-type (bitmask, 3 bits).
 * Encodes the dominant compute type when compute_dense is set.
 */
enum sched_hint_compute_dense {
	SCHED_COMPUTE_NONE = 0,
	SCHED_COMPUTE_INT = (1U << 0), /* integer ALU dominant */
	SCHED_COMPUTE_FLOAT = (1U << 1), /* floating-point dominant */
	SCHED_COMPUTE_SIMD = (1U << 2), /* vector / SIMD dominant */
};

/*
 * Memory-dense sub-type (2 bits).
 * Encodes the memory access pattern when memory_dense is set.
 */
enum sched_hint_memory_dense {
	SCHED_MEMORY_NONE = 0,
	SCHED_MEMORY_STREAM = 1, /* sequential / streaming */
	SCHED_MEMORY_RANDOM = 2, /* random access */
};

/*
 * struct sched_hint - per-thread scheduling hint descriptor
 *
 * Layout (64 bytes total, naturally aligned):
 *
 *   [0..3]    magic          SCHED_HINT_MAGIC for validation
 *   [4..7]    version        struct layout version
 *
 *   Tag payloads (fixed offsets for kernel fast-path access).
 *   The scheduler checks each payload directly: non-zero means active.
 *
 *   [8]       compute_dense  SCHED_COMPUTE_xxx sub-type bitmask
 *   [9]       branch_dense   0/1
 *   [10]      memory_dense   SCHED_MEMORY_xxx sub-type
 *   [11]      atomic_dense   0/1
 *   [12]      io_dense       0/1
 *   [13]      unshared       0/1
 *   [14]      compute_prep   0/1
 *   [15]      reserved_pad   alignment padding
 *
 *   Extended payloads:
 *
 *   [16..23]  atomic_magic   magic number for atomic co-scheduling
 *   [24..31]  dep_magic      — dependency magic number for IPC grouping
 *
 *   [32..39]  unshared_magic — 64-bit bloom filter for unshared co-scheduling
 *             Encodes which critical section variables (mutex, semaphore,
 *             etc.) this thread's unshared region is protecting.
 *
 *             Unlike atomic_magic (for lock-free CAS), unshared_magic tracks
 *             exclusive resource ownership (locks, critical sections).
 *
 *             Uses same bloom filter hash as atomic_magic:
 *             Hash:  h = (aligned_ptr) * 0x9E3779B97F4A7C15  (fibonacci)
 *             Bits:  bloom |= (1 << (h & 63))
 *                         | (1 << ((h>>16) & 63))
 *                         | (1 << ((h>>32) & 63))
 *                         | (1 << ((h>>48) & 63))    (k=4 bits per ptr)
 *
 *             Scheduler usage:
 *               When a task with unshared=1 is about to be preempted:
 *               overlap = popcount(magic_a & magic_b) >= 4
 *               → true:  another RUNNING task holds same lock → extend slice
 *               → false: no contention detected → normal preemption
 *             This prevents priority inversion by extending time slice when
 *             another thread is waiting for the same critical section.
 *
 *   [40]      dep_role       — 0 = producer, 1 = consumer
 *   [41..63]  reserved[23]   — padding for future tag payloads
 */
struct sched_hint {
	/* header (8 bytes) */
	__u32 magic;
	__u32 version;

	/* tag payloads: byte-sized for simplicity (8 bytes) */
	__u8 compute_dense; /* SCHED_COMPUTE_xxx */
	__u8 branch_dense; /* 0 or 1 */
	__u8 memory_dense; /* SCHED_MEMORY_xxx */
	__u8 atomic_dense; /* 0 or 1 */
	__u8 io_dense; /* 0 or 1 */
	__u8 unshared; /* 0 or 1 */
	__u8 compute_prep; /* 0 or 1 */
	__u8 reserved_pad;

	/* extended payloads (24 bytes) */
	__u64 atomic_magic;
	__u64 dep_magic;
	__u64 unshared_magic;
	__u8 dep_role; /* 0 = producer, 1 = consumer */

	/* reserved for future tag types (23 bytes) */
	__u8 reserved2[23];
} __attribute__((aligned(64)));

/*
 * Access helpers
 *
 * The hint variable is Thread-Local Storage (TLS). Every extern declaration
 * must include the TLS specifier so the linker resolves it correctly
 * (ELF symbol type STT_TLS).
 *
 * In userspace:
 *   extern SCHED_HINT_TLS struct sched_hint __sched_hint_data;
 *   struct sched_hint *hint = &__sched_hint_data;
 *
 * In kernel space (after your ELF loader modification):
 *   struct sched_hint *hint = current->sched_hint;
 *   if (hint && hint->magic == SCHED_HINT_MAGIC) { ... }
 *
 * Fast-path check example:
 *   if (hint->tags_active & SCHED_TAG_COMPUTE_DENSE) {
 *       switch (hint->compute_dense) {
 *       case SCHED_COMPUTE_INT:   ... break;
 *       case SCHED_COMPUTE_FLOAT: ... break;
 *       case SCHED_COMPUTE_SIMD:  ... break;
 *       }
 *   }
 */
#ifdef CONFIG_SCHED_HINT
int set_sched_hint_prctl(struct task_struct *t, unsigned long arg2,
			 unsigned long arg3);
#endif /* CONFIG_SCHED_HINT */

#endif /* _LINUX_SCHED_HINT_H */
