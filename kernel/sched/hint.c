#include <linux/sched/hint.h>
#include <linux/sizes.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmap_lock.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/stddef.h>

/*
 * The 64-byte layout is an ABI contract shared with the userspace header
 * and the LLVM pass. Pin it down at compile time.
 */
static_assert(sizeof(struct sched_hint) == 64);
static_assert(offsetof(struct sched_hint, exec_dense) == 8);
static_assert(offsetof(struct sched_hint, memory_dense) == 9);
static_assert(offsetof(struct sched_hint, atomic_dense) == 10);
static_assert(offsetof(struct sched_hint, unshared) == 11);
static_assert(offsetof(struct sched_hint, load_trend) == 12);
static_assert(offsetof(struct sched_hint, atomic_magic) == 16);
static_assert(offsetof(struct sched_hint, dep_magic) == 24);
static_assert(offsetof(struct sched_hint, unshared_magic) == 32);
static_assert(offsetof(struct sched_hint, dep_role) == 40);

/*
 * Force BTF generation for scheduling hint enums.
 *
 * The compiler only emits DWARF (and hence BTF) entries for types that are
 * actually *referenced as types* in a translation unit.  Using an enum
 * constant (e.g.  SCHED_HINT_MAGIC) in an expression is not enough -- the
 * compiler folds it to an integer literal and never records the enum type.
 *
 * Declaring an unused pointer variable of each enum type is the lightest
 * way to make the compiler emit the type information.  pahole then converts
 * DWARF -> BTF, and `bpftool btf dump` makes them visible in vmlinux.h.
 */
enum sched_hint_metadata *__btf_sched_hint_metadata __attribute__((unused));
enum sched_hint_exec_dense *__btf_sched_hint_exec_dense
	__attribute__((unused));
enum sched_hint_memory_dense *__btf_sched_hint_memory_dense
	__attribute__((unused));
enum sched_hint_load_trend *__btf_sched_hint_load_trend
	__attribute__((unused));
enum sched_hint_atomic_dense *__btf_sched_hint_atomic_dense
	__attribute__((unused));
enum sched_hint_unshared *__btf_sched_hint_unshared
	__attribute__((unused));
enum sched_hint_dep_role *__btf_sched_hint_dep_role
	__attribute__((unused));

int set_sched_hint_prctl(struct task_struct *t, unsigned long arg2,
			 unsigned long arg3)
{
	const long offset = (long)arg2;
	// vaddr of the __sched_hint_data on main thread
	const unsigned long main_vaddr = arg3;
	struct page *page;
	struct sched_hint *hint;
	int ret;
	/* We should not allow wild guess of sched_hint_offset */
	if (t->mm->has_sched_hint) {
		ret = -EPERM;
		goto err;
	}
	/* we assume 64 bytes alignment for sched hint */
	if (offset > (long)SZ_16M || offset < -(long)SZ_16M) {
		ret = -EINVAL;
		goto err;
	}
	if (main_vaddr & 63) {
		ret = -EINVAL;
		goto err;
	}
	if (!access_ok((void __user *)main_vaddr, 64)) {
		ret = -EFAULT;
		goto err;
	}
	/* should pin only 1 page. */
	if (pin_user_pages_fast(main_vaddr, 1, FOLL_WRITE | FOLL_LONGTERM,
				&page) != 1) {
		ret = -EFAULT;
		goto err;
	}

	unsigned long page_offset = main_vaddr & ~PAGE_MASK;
	hint = (struct sched_hint *)(page_address(page) + page_offset);
	if (!hint || hint->magic != SCHED_HINT_MAGIC) {
		ret = -EINVAL;
		pr_info("sched_hint_kaddr set failed: hint magic not matched, hint->magic is %x, should be %x\n",
			hint ? hint->magic : 0, SCHED_HINT_MAGIC);
		goto err_unpin;
	}
	if (hint->version != SCHED_HINT_VERSION) {
		ret = -EINVAL;
		pr_info("sched_hint_kaddr set failed: version mismatch (hint has %u, kernel expects %u) — userspace sched_hint.h and kernel out of sync\n",
			hint->version, SCHED_HINT_VERSION);
		goto err_unpin;
	}

	t->sched_hint_kaddr = hint;
	t->sched_hint_page = page;

	/* offset is reserved for other threads to use */
	t->mm->sched_hint_offset = offset;
	t->mm->has_sched_hint = true;
	return 0;

err_unpin:
	unpin_user_page(page);
err:
	printk(KERN_WARNING "set_sched_hint_prctl failed, ret = %d\n", ret);
	return ret;
}
