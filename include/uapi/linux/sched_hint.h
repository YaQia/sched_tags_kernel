/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_SCHED_HINT_H
#define _UAPI_LINUX_SCHED_HINT_H

/*
 * struct sched_hint - per-thread scheduling hint descriptor.
 *
 * This is the single source of truth for the sched_hint ABI. It is shared,
 * via `make headers_install`, with:
 *   - the kernel      (include/linux/sched/hint.h wraps this + prctl proto)
 *   - userspace       (#include <linux/sched_hint.h>)
 *   - BPF schedulers  (through BTF / vmlinux.h; do NOT include this header)
 *
 * One instance lives per THREAD, in a slot of a kernel-owned page mapped
 * into the process. Each thread calls prctl(PR_SET_SCHED_HINT, &ptr, 0, 0, 0)
 * once; the kernel registers it, back-fills ptr with its slot address, and
 * reads it lazily at scheduling hooks. Every write is a plain store -- non-zero
 * state byte = active.
 *
 * Hints are sampled at scheduling hooks. A hint is only observed if it is
 * active at a hook. Short userspace-only regions may never be observed.
 * Hints are advisory and must not be used for correctness.
 *
 * ABI is NOT yet stable (prototype phase): the layout may change, but when it
 * does it changes HERE, in one place. Bump SCHED_HINT_VERSION on any change;
 * the kernel stamps magic/version into each slot when handing it out.
 *
 * Tags fall into two classes:
 *   - hardware resource : compute_dense, memory_dense, atomic_dense
 *   - software contract : unshared, dep_role
 *
 * Some tags carry an opaque 64-bit magic key. Its encoding and comparison
 * rule are tag-specific; see the tag definitions below. Do not assume a
 * common meaning across tags.
 *
 * Nesting is handled by userspace save/restore tokens; the scheduler only
 * sees the innermost current object.
 */

#include <linux/types.h>

/*
 * We use enums here so the constants are exported into BTF (vmlinux.h) for
 * BPF schedulers, and shared symbolically with userspace.
 */
enum sched_hint_metadata {
	SCHED_HINT_MAGIC = 0x48494E54U, /* "HINT" in ASCII */
	SCHED_HINT_VERSION = 1,
};

/*
 * ===== hardware resource class =====
 */

/*
 * Which kind of CPU compute this region leans on.
 * The mask granularity exists for SMT pairing: two tasks whose compute masks do
 * not overlap make good sibling threads (e.g. SIMD-heavy paired with
 * branch-heavy), since they press on different execution units.
 */
enum sched_hint_compute_dense {
	SCHED_COMPUTE_NONE = 0,
	SCHED_COMPUTE_INT = (1U << 0),   /* integer ALU */
	SCHED_COMPUTE_FLOAT = (1U << 1), /* floating point */
	SCHED_COMPUTE_SIMD = (1U << 2),  /* vector / SIMD */
	SCHED_COMPUTE_CTRL = (1U << 3),  /* branch / control flow */
};

/*
 * How this memory-dense region is shared. Non-zero state means the thread is
 * currently in a memory-dense phase.
 *
 * READ_MOSTLY : writes are negligible, so copies may be spread across LLCs
 *               to let readers proceed in parallel.
 * WRITE_HEAVY : writes are frequent enough that cache coherence is the
 *               bottleneck. Keep such tasks within one LLC, preferring
 *               same-core / SMT siblings before falling back to separate
 *               cores in the same LLC.
 */
enum sched_hint_memory_dense {
	SCHED_MEMORY_NONE = 0,
	SCHED_MEMORY_READ_MOSTLY = 1,  /* effectively read-only shared data */
	SCHED_MEMORY_WRITE_HEAVY = 2,  /* meaningful write traffic */
};

/*
 * atomic_dense is active when this thread operates on an atomic-variable set.
 * atomic_magic is the exact 64-bit identity of that set: two tasks are
 * co-scheduled only when their sets are exactly the same.
 *
 * Exact-set matching keeps both the decision and the lookup predictable. A
 * partial overlap (A: {x}, B: {x,y}) has no guaranteed win: moving B away from
 * a core that is already good for y can cost more than sharing x gains, and
 * without knowing the rest of the workload the right placement is unknowable.
 * The scheduler therefore acts only on exact set equality and leaves partial
 * overlap to the normal scheduler; exact keys also keep the per-set lookup
 * O(1) instead of scanning every task that carries an atomic tag.
 */
enum sched_hint_atomic_dense {
	SCHED_ATOMIC_NONE = 0,
	SCHED_ATOMIC_DENSE = 1, /* lock-free / spin region (see atomic_magic) */
};

/*
 * ===== software contract class =====
 */

/*
 * unshared is active while this thread waits on or holds one exclusive
 * resource. unshared_magic is the exact key of that one lock.
 */
enum sched_hint_unshared {
	SCHED_UNSHARED_NONE = 0,
	SCHED_UNSHARED_HELD = 1, /* holding an exclusive resource (see unshared_magic) */
};

/*
 * dep_role -- this thread's role in a producer/consumer dependency, paired
 * with dep_magic (the exact identity of the current producer/consumer handoff
 * object: pipe, queue, socket, channel, ring buffer, ...).
 *
 * It is a bitmask so a single bidirectional handoff object can be both ends at
 * once. Different handoff objects use nesting/sequence, not multiple bits: the
 * scheduler only sees the innermost current object.
 */
enum sched_hint_dep_role {
	SCHED_DEP_NONE = 0,
	SCHED_DEP_PRODUCER = (1U << 0),
	SCHED_DEP_CONSUMER = (1U << 1),
	SCHED_DEP_BOTH = SCHED_DEP_PRODUCER | SCHED_DEP_CONSUMER,
};

/*
 * struct sched_hint (64 bytes total)
 *
 *   [0..3]    magic          SCHED_HINT_MAGIC
 *   [4..7]    version        SCHED_HINT_VERSION (stamped at slot delivery)
 *
 *   State bytes, grouped by class (non-zero = active):
 *     hardware resource
 *   [8]       compute_dense  SCHED_COMPUTE_xxx bitmask
 *   [9]       memory_dense   SCHED_MEMORY_xxx
 *   [10]      atomic_dense   SCHED_ATOMIC_xxx, pairs with atomic_magic
 *     software contract
 *   [11]      unshared       SCHED_UNSHARED_xxx, pairs with unshared_magic
 *   [12]      dep_role       SCHED_DEP_xxx bitmask, pairs with dep_magic
 *   [13..15]  reserved       (alignment padding; free for future state bytes)
 *
 *   Magic words, mirroring the state-byte order (every magic sits below its
 *   state byte). atomic_dense/unshared/dep use exact identity keys:
 *   [16..23]  atomic_magic   exact hash of the current atomic-variable set
 *   [24..31]  unshared_magic exact key of the current lock
 *   [32..39]  dep_magic      exact key of the current handoff object
 *
 *   Memory locality keys, valid while memory_dense is active. local_memory_magic
 *   is compared by similarity; global_memory_magic by equality:
 *   [40..47]  local_memory_magic   per-mm hot-page sketch
 *   [48..55]  global_memory_magic  cross-process shared-object identity
 *
 *   Exact-key derivation is a userspace contract:
 *     pointer identity : key = hash64(ptr ^ per-process salt)
 *     static identity  : key = hash64(id)
 *   Pointer keys are only meaningful inside one process; use static ids for
 *   cross-process objects. The scheduler only sees the hashed key, never a
 *   raw virtual address.
 *
 *   [56..63]  reserved       all further growth carves from here
 */
struct sched_hint {
	/* header (8 bytes) */
	__u32 magic;
	__u32 version;

	/* state bytes (8 bytes), grouped by class */
	__u8 compute_dense; /* SCHED_COMPUTE_xxx */
	__u8 memory_dense; /* SCHED_MEMORY_xxx */
	__u8 atomic_dense; /* SCHED_ATOMIC_xxx */
	__u8 unshared;     /* SCHED_UNSHARED_xxx */
	__u8 dep_role;     /* SCHED_DEP_xxx */
	__u8 reserved[3];

	/* magic words (24 bytes), mirroring state-byte order */
	__u64 atomic_magic;
	__u64 unshared_magic;
	__u64 dep_magic;

	/*
	 * Memory locality keys; valid while memory_dense is active.
	 * local_memory_magic: per-mm 4x16-bit MinHash sketch of hot pages.
	 * global_memory_magic: cross-process shared-object identity.
	 */
	__u64 local_memory_magic;
	__u64 global_memory_magic;

	/* reserved for future tags (8 bytes) */
	__u8 reserved2[8];
} __attribute__((aligned(64)));

#endif /* _UAPI_LINUX_SCHED_HINT_H */
