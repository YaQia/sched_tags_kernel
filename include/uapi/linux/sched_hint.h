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
 * One instance lives per THREAD (TLS). Userspace registers it once via
 * prctl(PR_SET_SCHED_HINT_OFFSET, ...); the kernel then reads it lazily at
 * scheduling hooks. Every write is a plain store — non-zero payload = active.
 *
 * ABI is NOT yet stable (prototype phase): the layout may change, but when it
 * does it changes HERE, in one place. Bump SCHED_HINT_VERSION on any change;
 * the prctl path rejects a mismatched version at registration time.
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
 * exec_dense sub-type (bitmask, 4 bits).
 * Which execution resources this region presses on.
 */
enum sched_hint_exec_dense {
	SCHED_EXEC_NONE = 0,
	SCHED_EXEC_INT = (1U << 0),   /* integer ALU */
	SCHED_EXEC_FLOAT = (1U << 1), /* floating point */
	SCHED_EXEC_SIMD = (1U << 2),  /* vector / SIMD */
	SCHED_EXEC_CTRL = (1U << 3),  /* branch / control flow */
};

/*
 * memory_dense sub-type.
 * Memory access pattern of this region.
 */
enum sched_hint_memory_dense {
	SCHED_MEMORY_NONE = 0,
	SCHED_MEMORY_STREAM = 1, /* sequential / streaming */
	SCHED_MEMORY_RANDOM = 2, /* random access */
};

/*
 * load_trend sub-type.
 * Announced direction of the upcoming load change.
 */
enum sched_hint_load_trend {
	SCHED_LOAD_NONE = 0,
	SCHED_LOAD_RISING = 1,  /* about to enter a heavy phase */
	SCHED_LOAD_FALLING = 2, /* about to wind down / go idle */
};

/*
 * atomic_dense state.
 * Currently a boolean; named as an enum so the wire vocabulary is symbolic
 * and future modes can be appended without touching the layout.
 */
enum sched_hint_atomic_dense {
	SCHED_ATOMIC_NONE = 0,
	SCHED_ATOMIC_DENSE = 1, /* lock-free / spin region (see atomic_magic) */
};

/*
 * unshared state.
 * Boolean today; enum reserves room for future modes (append only).
 */
enum sched_hint_unshared {
	SCHED_UNSHARED_NONE = 0,
	SCHED_UNSHARED_HELD = 1, /* holding an exclusive resource (see unshared_magic) */
};

/*
 * dep_role — producer/consumer for IPC dependency grouping.
 */
enum sched_hint_dep_role {
	SCHED_DEP_PRODUCER = 0,
	SCHED_DEP_CONSUMER = 1,
};

/*
 * Bloom-filter parameters for atomic_magic / unshared_magic.
 * Each 64-byte-aligned pointer sets SCHED_HINT_BLOOM_K bit positions in a
 * 64-bit word:
 *   h = (ptr & ~63) * SCHED_HINT_BLOOM_PRIME
 *   bloom |= (1 << (h & 63)) | (1 << ((h>>16) & 63))
 *         |  (1 << ((h>>32) & 63)) | (1 << ((h>>48) & 63))
 * The scheduler detects overlap via popcount(a & b) >= SCHED_HINT_BLOOM_K.
 * These are part of the ABI: the writer (userspace / LLVM pass) and any reader
 * that recomputes hashes must agree.
 */
#define SCHED_HINT_BLOOM_PRIME 0x9E3779B97F4A7C15ULL /* fibonacci hashing */
#define SCHED_HINT_BLOOM_K     4

/*
 * struct sched_hint (64 bytes total)
 *
 *   [0..3]    magic          SCHED_HINT_MAGIC
 *   [4..7]    version        SCHED_HINT_VERSION (checked at prctl)
 *
 *   Tag payloads (non-zero = active):
 *   [8]       exec_dense     SCHED_EXEC_xxx bitmask
 *   [9]       memory_dense   SCHED_MEMORY_xxx
 *   [10]      atomic_dense   SCHED_ATOMIC_xxx, pairs with atomic_magic
 *   [11]      unshared       SCHED_UNSHARED_xxx, pairs with unshared_magic
 *   [12]      load_trend     SCHED_LOAD_xxx
 *   [13..15]  reserved
 *
 *   Extended payloads (all serve the sync tags):
 *   [16..23]  atomic_magic   bloom filter for lock-free co-scheduling
 *   [24..31]  dep_magic      IPC producer/consumer grouping
 *   [32..39]  unshared_magic bloom filter for exclusive-lock ownership
 *   [40]      dep_role       SCHED_DEP_PRODUCER / SCHED_DEP_CONSUMER
 *   [41..63]  reserved
 */
struct sched_hint {
	/* header (8 bytes) */
	__u32 magic;
	__u32 version;

	/* tag payloads (8 bytes) */
	__u8 exec_dense;   /* SCHED_EXEC_xxx */
	__u8 memory_dense; /* SCHED_MEMORY_xxx */
	__u8 atomic_dense; /* SCHED_ATOMIC_xxx */
	__u8 unshared;     /* SCHED_UNSHARED_xxx */
	__u8 load_trend;   /* SCHED_LOAD_xxx */
	__u8 reserved[3];

	/* extended payloads (24 bytes) */
	__u64 atomic_magic;
	__u64 dep_magic;
	__u64 unshared_magic;
	__u8 dep_role; /* SCHED_DEP_PRODUCER / SCHED_DEP_CONSUMER */

	/* reserved for future tags (23 bytes) */
	__u8 reserved2[23];
} __attribute__((aligned(64)));

#endif /* _UAPI_LINUX_SCHED_HINT_H */
