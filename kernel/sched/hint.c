#include <linux/sched/hint.h>
#include <linux/sizes.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmap_lock.h>
#include <linux/types.h>

int set_sched_hint_prctl(unsigned long arg2, unsigned long arg3)
{
	const long offset = (long)arg2;
	// vaddr of the __sched_hint_data on main thread
	const unsigned long main_vaddr = arg3;
	struct page *page;
	struct sched_hint *hint;
	/* We should not allow wild guess of sched_hint_offset */
	if (current->mm->has_sched_hint)
		return -EPERM;
	/* we assume 64 bytes alignment for sched hint */
	if (offset & 63)
		return -EINVAL;
	if (offset > (long)SZ_16M || offset < -(long)SZ_16M)
		return -EINVAL;
	if (main_vaddr & 63)
		return -EINVAL;
	if (!access_ok((void __user *)main_vaddr, 64))
		return -EFAULT;
	/* should pin only 1 page. */
	if (pin_user_pages_fast(main_vaddr, 1, FOLL_WRITE | FOLL_LONGTERM,
				&page) != 1) {
		return -EFAULT;
	}

	unsigned long page_offset = main_vaddr & ~PAGE_MASK;
	hint = (struct sched_hint *)(page_address(page) + page_offset);
	if (hint && hint->atomic_magic == SCHED_HINT_MAGIC) {
		current->sched_hint_kaddr = hint;
		current->sched_hint_page = page;
	} else {
		return -EINVAL;
	}

	/* offset is reserved for other threads to use */
	current->mm->sched_hint_offset = offset;
	current->mm->has_sched_hint = true;
	return 0;
}
