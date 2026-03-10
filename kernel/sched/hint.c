#include <linux/sched/hint.h>
#include <linux/sizes.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmap_lock.h>
#include <linux/types.h>

int set_sched_hint_prctl(unsigned long arg2)
{
	const long offset = (long)arg2;
	if (current->mm->has_sched_hint) {
		/* We should not allow wild guess of sched_hint_offset */
		return -EPERM;
	}
	if (offset & 63) {
		/* we assume 64 bytes alignment for sched hint */
		return -EINVAL;
	}
	current->mm->sched_hint_offset = offset;
	current->mm->has_sched_hint = true;
	return 0;
}
