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
 *   - the LLVM pass   (same, on the compiler's header search path)
 *   - BPF schedulers  (through BTF / vmlinux.h; do NOT include this header)
 *
 * One instance lives per THREAD, in a slot of a kernel-owned page mapped
 * into the process. Each thread calls prctl(PR_SET_SCHED_HINT, &ptr, 0, 0, 0)
 * once; the kernel registers it, back-fills ptr with its slot address, and
 * reads it lazily at
 * scheduling hooks. Every write is a plain store — non-zero payload = active.
 *
 * ABI is NOT yet stable (prototype phase): the layout may change, but when it
 * does it changes HERE, in one place. Bump SCHED_HINT_VERSION on any change;
 * the kernel stamps magic/version into each slot when handing it out.
 *
 * Tags fall into three classes, and the layout groups them that way:
 *   - hardware resource : funit_dense, memory_dense, atomic_dense
 *   - software contract : unshared, dep_role
 *   - forecast          : load_ahead
 */

#include <linux/types.h>

/*
 * We use enums here so the constants are exported into BTF (vmlinux.h) for
 * BPF schedulers, and shared symbolically with userspace and the LLVM pass.
 */
enum sched_hint_metadata {
	SCHED_HINT_MAGIC = 0x48494E54U, /* "HINT" in ASCII */
	SCHED_HINT_VERSION = 1,
};

/*
 * ===== hardware resource class =====
 */

/*
 * Which CPU functional units this region leans on. "funit" (functional unit)
 * is the per-core execution resource.
 * The mask granularity exists for SMT pairing: two tasks whose funit masks do
 * not overlap make good sibling threads (e.g. SIMD-heavy paired with
 * branch-heavy), since they press on different units.
 */
enum sched_hint_funit_dense {
	SCHED_FUNIT_NONE = 0,
	SCHED_FUNIT_INT = (1U << 0),   /* integer ALU */
	SCHED_FUNIT_FLOAT = (1U << 1), /* floating point */
	SCHED_FUNIT_SIMD = (1U << 2),  /* vector / SIMD */
	SCHED_FUNIT_CTRL = (1U << 3),  /* branch / control flow */
};

/*
 * Memory access pattern of this region. If several threads have the same 
 * memory access pattern, try to isolate them from each other (with cache 
 * isolation or NUMA node isolation).
 */
enum sched_hint_memory_dense {
	SCHED_MEMORY_NONE = 0,
	SCHED_MEMORY_STREAM = 1, /* sequential / streaming */
	SCHED_MEMORY_RANDOM = 2, /* random access */
};

/*
 * If several threads share the same atomic_magic, they are trying to write the
 * same atomic variable, which will invalidate each other's cache of the atomic 
 * variable. Try to colocate these threads within the same core, so they can not
 * clear each other's cache.
 */
enum sched_hint_atomic_dense {
	SCHED_ATOMIC_NONE = 0,
	SCHED_ATOMIC_DENSE = 1, /* lock-free / spin region (see atomic_magic) */
};

/*
 * ===== software contract class =====
 */

/*
 * Exclusive resources held by tasks. If threads share the same exclusive resource,
 * the scheduler should let the total latency of tasks as small as possible.
 */
enum sched_hint_unshared {
	SCHED_UNSHARED_NONE = 0,
	SCHED_UNSHARED_HELD = 1, /* holding an exclusive resource (see unshared_magic) */
};

/*
 * dep_role — this thread's role in a producer/consumer dependency, paired
 * with dep_magic (the shared dependency id).
 */
enum sched_hint_dep_role {
	SCHED_DEP_NONE = 0,
	SCHED_DEP_PRODUCER = 1,
	SCHED_DEP_CONSUMER = 2,
};

/*
 * ===== forecast class =====
 */

/*
 * The load phase the thread is about to enter. This is a forecast the
 * application *declares* (it knows what it is about to do), not a trend the
 * kernel *observes*, so acting on it does not wait for load detection.
 */
enum sched_hint_load_ahead {
	SCHED_LOAD_NONE = 0,
	SCHED_LOAD_RISING = 1,  /* about to enter a heavy phase */
	SCHED_LOAD_FALLING = 2, /* about to wind down / go idle */
};

/*
 * Bloom-filter parameters for all tags containing magic.
 * Each 64-byte-aligned pointer sets SCHED_HINT_BLOOM_K bit positions in a
 * 64-bit word:
 *   h = (ptr & ~63) * SCHED_HINT_BLOOM_PRIME
 *   bloom |= (1 << (h & 63)) | (1 << ((h>>16) & 63))
 *         |  (1 << ((h>>32) & 63)) | (1 << ((h>>48) & 63))
 * The scheduler can use the magic as a key to reference all tasks contain the
 * same key.
 */
#define SCHED_HINT_BLOOM_PRIME 0x9E3779B97F4A7C15ULL /* fibonacci hashing */
#define SCHED_HINT_BLOOM_K     4

/*
 * struct sched_hint (64 bytes total)
 *
 *   [0..3]    magic          SCHED_HINT_MAGIC
 *   [4..7]    version        SCHED_HINT_VERSION (stamped at slot delivery)
 *
 *   State bytes, grouped by class (non-zero = active):
 *     hardware resource
 *   [8]       funit_dense    SCHED_FUNIT_xxx bitmask
 *   [9]       memory_dense   SCHED_MEMORY_xxx
 *   [10]      atomic_dense   SCHED_ATOMIC_xxx, pairs with atomic_magic
 *     software contract
 *   [11]      unshared       SCHED_UNSHARED_xxx, pairs with unshared_magic
 *   [12]      dep_role       SCHED_DEP_xxx, pairs with dep_magic
 *     forecast
 *   [13]      load_ahead     SCHED_LOAD_xxx
 *   [14..15]  reserved       (alignment padding before the magic words; free
 *                             room for the next couple of state bytes)
 *
 *   Magic words, mirroring the state-byte order (every magic sits below its
 *   state byte): atomic -> unshared -> dep.
 *   [16..23]  atomic_magic   bloom filter for lock-free co-scheduling
 *   [24..31]  unshared_magic bloom filter for exclusive-lock ownership
 *   [32..39]  dep_magic      producer/consumer dependency id
 *
 *   [40..63]  reserved       all further growth (new magic words, and the
 *                             3rd+ new state byte) carves from the bottom
 */
struct sched_hint {
	/* header (8 bytes) */
	__u32 magic;
	__u32 version;

	/* state bytes (8 bytes), grouped by class */
	__u8 funit_dense;  /* SCHED_FUNIT_xxx */
	__u8 memory_dense; /* SCHED_MEMORY_xxx */
	__u8 atomic_dense; /* SCHED_ATOMIC_xxx */
	__u8 unshared;     /* SCHED_UNSHARED_xxx */
	__u8 dep_role;     /* SCHED_DEP_xxx */
	__u8 load_ahead;   /* SCHED_LOAD_xxx */
	__u8 reserved[2];

	/* magic words (24 bytes), mirroring state-byte order */
	__u64 atomic_magic;
	__u64 unshared_magic;
	__u64 dep_magic;

	/* reserved for future tags (24 bytes) */
	__u8 reserved2[24];
} __attribute__((aligned(64)));

#endif /* _UAPI_LINUX_SCHED_HINT_H */
