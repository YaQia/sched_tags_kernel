/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_LINUX_SCHED_HINT_H
#define _UAPI_LINUX_SCHED_HINT_H

#include <linux/types.h>

/* Constants */
#define SCHED_HINT_MAGIC 0x5348494EU /* "SHIN" in ASCII                     */
#define SCHED_HINT_VERSION 1
#ifndef PR_SET_SCHED_HINT_OFFSET
#define PR_SET_SCHED_HINT_OFFSET 83
#endif

/* Section name — valid C identifier so linker generates __start_ / __stop_   */
#define SCHED_HINT_SECTION "__sched_hint"

/* TLS storage-class specifier — portable across C11/GCC/Clang/MSVC    */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define SCHED_HINT_TLS _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#define SCHED_HINT_TLS __thread
#elif defined(_MSC_VER)
#define SCHED_HINT_TLS __declspec(thread)
#else
#define SCHED_HINT_TLS /* unsupported — fallback to plain global */
#endif

/*
 * Tag presence bitmap (tags_present / tags_active)
 *
 * Each bit indicates whether a particular scheduling tag is present
 * (statically determined at compile time) or active (may be toggled at
 * runtime for dynamic tags).
 *
 * Using uint64_t gives us 64 tag slots. Bits 0..7 are allocated below;
 * bits 8..63 are reserved for future tag types.
 */

#define SCHED_TAG_COMPUTE_DENSE (1ULL << 0)
#define SCHED_TAG_BRANCH_DENSE (1ULL << 1)
#define SCHED_TAG_MEMORY_DENSE (1ULL << 2)
#define SCHED_TAG_ATOMIC_DENSE (1ULL << 3)
#define SCHED_TAG_IO_DENSE (1ULL << 4)
#define SCHED_TAG_UNSHARED (1ULL << 5)
#define SCHED_TAG_COMPUTE_PREP (1ULL << 6)
#define SCHED_TAG_DEPENDENCY (1ULL << 7)
/* bits 8..63: reserved for future tags                                       */

/*
 * compute-dense sub-type (bitmask, 3 bits)
 * Encodes the dominant compute type when SCHED_TAG_COMPUTE_DENSE is set.
 */

#define SCHED_COMPUTE_NONE 0 /* no compute type present */
#define SCHED_COMPUTE_INT (1U << 0) /* integer ALU dominant */
#define SCHED_COMPUTE_FLOAT (1U << 1) /* floating-point dominant */
#define SCHED_COMPUTE_SIMD (1U << 2) /* vector / SIMD dominant */

/*
 * memory-dense sub-type (2 bits)
 * Encodes the memory access pattern when SCHED_TAG_MEMORY_DENSE is set.
 */

#define SCHED_MEMORY_NONE 0 /* no dominant memory pattern                 */
#define SCHED_MEMORY_STREAM 1 /* sequential / streaming access              */
#define SCHED_MEMORY_RANDOM 2 /* random access                              */

/*
 * struct sched_hint — per-thread scheduling hint (TLS instance)
 *
 * Layout (64 bytes total, naturally aligned):
 *
 *   [0..3]    magic          — SCHED_HINT_MAGIC for validation
 *   [4..7]    version        — struct layout version
 *   [8..15]   tags_present   — bitmap: which tags are statically set
 *   [16..23]  tags_active    — bitmap: runtime-toggleable copy (mutable)
 *
 *   --- Tag payloads (fixed offsets for kernel fast-path access) ---
 *
 *   [24]      compute_dense  — SCHED_COMPUTE_xxx sub-type
 *   [25]      branch_dense   — 0/1
 *   [26]      memory_dense   — SCHED_MEMORY_xxx sub-type
 *   [27]      atomic_dense   — 0/1 (presence flag; magic_num below)
 *   [28]      io_dense       — 0/1
 *   [29]      unshared       — 0/1
 *   [30]      compute_prep   — 0/1
 *   [31]      reserved_pad   — alignment padding
 *
 *   [32..39]  atomic_magic   — magic number for atomic co-scheduling
 *   [40..47]  dep_magic      — dependency magic number for IPC grouping
 *   [48]      dep_role       — 0 = producer, 1 = consumer
 *   [49..55]  reserved1[7]   — future use
 *
 *   [56..63]  reserved2[8]   — generous padding for future tag payloads
 *
 */

struct sched_hint {
	/* ---- header (24 bytes) ---- */
	uint32_t magic; /* SCHED_HINT_MAGIC                           */
	uint32_t version; /* SCHED_HINT_VERSION                         */
	uint64_t tags_present; /* bitmap: tags determined at compile time    */
	uint64_t tags_active; /* bitmap: runtime copy (scheduler reads this)*/

	/* ---- tag payloads: byte-sized for simplicity (8 bytes) ---- */
	uint8_t compute_dense; /* SCHED_COMPUTE_xxx                          */
	uint8_t branch_dense; /* 0 or 1                                     */
	uint8_t memory_dense; /* SCHED_MEMORY_xxx                           */
	uint8_t atomic_dense; /* 0 or 1                                     */
	uint8_t io_dense; /* 0 or 1                                     */
	uint8_t unshared; /* 0 or 1                                     */
	uint8_t compute_prep; /* 0 or 1                                     */
	uint8_t reserved_pad; /* padding to 8-byte boundary                 */

	/* ---- extended payloads (24 bytes) ---- */
	uint64_t atomic_magic; /* shared magic for atomic co-scheduling      */
	uint64_t dep_magic; /* dependency magic for IPC grouping          */
	uint8_t dep_role; /* 0 = producer, 1 = consumer                 */
	uint8_t reserved1[7]; /* future use                                 */

	/* ---- reserved for future tag types (8 bytes) ---- */
	uint8_t reserved2[8];
};

/* Compile-time size assertion: struct must be exactly 64 bytes              */
_Static_assert(sizeof(struct sched_hint) == 64,
	       "sched_hint must be exactly 64 bytes");

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
 *	 uint8_t m = hint->compute_dense;
 *
 *	 if (m & SCHED_COMPUTE_INT)   { ... }
 *	 if (m & SCHED_COMPUTE_FLOAT) { ... }
 *	 if (m & SCHED_COMPUTE_SIMD)  { ... }
 *   }
 */

#endif /* _UAPI_LINUX_SCHED_HINT_H */
