/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_HINT_H
#define _LINUX_SCHED_HINT_H

#include <uapi/linux/sched/hint.h>

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
extern int set_sched_hint_prctl(unsigned long arg2, unsigned long arg3);
#endif /* CONFIG_SCHED_HINT */

#endif /* _LINUX_SCHED_HINT_H */
