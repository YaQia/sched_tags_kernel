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

#ifdef CONFIG_SCHED_HINT
int set_sched_hint_prctl(struct task_struct *t, unsigned long arg2,
			 unsigned long arg3);
#endif /* CONFIG_SCHED_HINT */

#endif /* _LINUX_SCHED_HINT_KERNEL_H */
