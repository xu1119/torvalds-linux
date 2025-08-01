/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_UACCESS_H__
#define __LINUX_UACCESS_H__

#include <linux/fault-inject-usercopy.h>
#include <linux/instrumented.h>
#include <linux/minmax.h>
#include <linux/nospec.h>
#include <linux/sched.h>
#include <linux/ucopysize.h>

#include <asm/uaccess.h>

/*
 * Architectures that support memory tagging (assigning tags to memory regions,
 * embedding these tags into addresses that point to these memory regions, and
 * checking that the memory and the pointer tags match on memory accesses)
 * redefine this macro to strip tags from pointers.
 *
 * Passing down mm_struct allows to define untagging rules on per-process
 * basis.
 *
 * It's defined as noop for architectures that don't support memory tagging.
 */
#ifndef untagged_addr
#define untagged_addr(addr) (addr)
#endif

#ifndef untagged_addr_remote
#define untagged_addr_remote(mm, addr)	({		\
	mmap_assert_locked(mm);				\
	untagged_addr(addr);				\
})
#endif

#ifdef masked_user_access_begin
 #define can_do_masked_user_access() 1
#else
 #define can_do_masked_user_access() 0
 #define masked_user_access_begin(src) NULL
 #define mask_user_address(src) (src)
#endif

/*
 * Architectures should provide two primitives (raw_copy_{to,from}_user())
 * and get rid of their private instances of copy_{to,from}_user() and
 * __copy_{to,from}_user{,_inatomic}().
 *
 * raw_copy_{to,from}_user(to, from, size) should copy up to size bytes and
 * return the amount left to copy.  They should assume that access_ok() has
 * already been checked (and succeeded); they should *not* zero-pad anything.
 * No KASAN or object size checks either - those belong here.
 *
 * Both of these functions should attempt to copy size bytes starting at from
 * into the area starting at to.  They must not fetch or store anything
 * outside of those areas.  Return value must be between 0 (everything
 * copied successfully) and size (nothing copied).
 *
 * If raw_copy_{to,from}_user(to, from, size) returns N, size - N bytes starting
 * at to must become equal to the bytes fetched from the corresponding area
 * starting at from.  All data past to + size - N must be left unmodified.
 *
 * If copying succeeds, the return value must be 0.  If some data cannot be
 * fetched, it is permitted to copy less than had been fetched; the only
 * hard requirement is that not storing anything at all (i.e. returning size)
 * should happen only when nothing could be copied.  In other words, you don't
 * have to squeeze as much as possible - it is allowed, but not necessary.
 *
 * For raw_copy_from_user() to always points to kernel memory and no faults
 * on store should happen.  Interpretation of from is affected by set_fs().
 * For raw_copy_to_user() it's the other way round.
 *
 * Both can be inlined - it's up to architectures whether it wants to bother
 * with that.  They should not be used directly; they are used to implement
 * the 6 functions (copy_{to,from}_user(), __copy_{to,from}_user_inatomic())
 * that are used instead.  Out of those, __... ones are inlined.  Plain
 * copy_{to,from}_user() might or might not be inlined.  If you want them
 * inlined, have asm/uaccess.h define INLINE_COPY_{TO,FROM}_USER.
 *
 * NOTE: only copy_from_user() zero-pads the destination in case of short copy.
 * Neither __copy_from_user() nor __copy_from_user_inatomic() zero anything
 * at all; their callers absolutely must check the return value.
 *
 * Biarch ones should also provide raw_copy_in_user() - similar to the above,
 * but both source and destination are __user pointers (affected by set_fs()
 * as usual) and both source and destination can trigger faults.
 */

static __always_inline __must_check unsigned long
__copy_from_user_inatomic(void *to, const void __user *from, unsigned long n)
{
	unsigned long res;

	instrument_copy_from_user_before(to, from, n);
	check_object_size(to, n, false);
	res = raw_copy_from_user(to, from, n);
	instrument_copy_from_user_after(to, from, n, res);
	return res;
}

static __always_inline __must_check unsigned long
__copy_from_user(void *to, const void __user *from, unsigned long n)
{
	unsigned long res;

	might_fault();
	instrument_copy_from_user_before(to, from, n);
	if (should_fail_usercopy())
		return n;
	check_object_size(to, n, false);
	res = raw_copy_from_user(to, from, n);
	instrument_copy_from_user_after(to, from, n, res);
	return res;
}

/**
 * __copy_to_user_inatomic: - Copy a block of data into user space, with less checking.
 * @to:   Destination address, in user space.
 * @from: Source address, in kernel space.
 * @n:    Number of bytes to copy.
 *
 * Context: User context only.
 *
 * Copy data from kernel space to user space.  Caller must check
 * the specified block with access_ok() before calling this function.
 * The caller should also make sure he pins the user space address
 * so that we don't result in page fault and sleep.
 */
static __always_inline __must_check unsigned long
__copy_to_user_inatomic(void __user *to, const void *from, unsigned long n)
{
	if (should_fail_usercopy())
		return n;
	instrument_copy_to_user(to, from, n);
	check_object_size(from, n, true);
	return raw_copy_to_user(to, from, n);
}

static __always_inline __must_check unsigned long
__copy_to_user(void __user *to, const void *from, unsigned long n)
{
	might_fault();
	if (should_fail_usercopy())
		return n;
	instrument_copy_to_user(to, from, n);
	check_object_size(from, n, true);
	return raw_copy_to_user(to, from, n);
}

/*
 * Architectures that #define INLINE_COPY_TO_USER use this function
 * directly in the normal copy_to/from_user(), the other ones go
 * through an extern _copy_to/from_user(), which expands the same code
 * here.
 *
 * Rust code always uses the extern definition.
 */
static inline __must_check unsigned long
_inline_copy_from_user(void *to, const void __user *from, unsigned long n)
{
	unsigned long res = n;
	might_fault();
	if (should_fail_usercopy())
		goto fail;
	if (can_do_masked_user_access())
		from = mask_user_address(from);
	else {
		if (!access_ok(from, n))
			goto fail;
		/*
		 * Ensure that bad access_ok() speculation will not
		 * lead to nasty side effects *after* the copy is
		 * finished:
		 */
		barrier_nospec();
	}
	instrument_copy_from_user_before(to, from, n);
	res = raw_copy_from_user(to, from, n);
	instrument_copy_from_user_after(to, from, n, res);
	if (likely(!res))
		return 0;
fail:
	memset(to + (n - res), 0, res);
	return res;
}
extern __must_check unsigned long
_copy_from_user(void *, const void __user *, unsigned long);

static inline __must_check unsigned long
_inline_copy_to_user(void __user *to, const void *from, unsigned long n)
{
	might_fault();
	if (should_fail_usercopy())
		return n;
	if (access_ok(to, n)) {
		instrument_copy_to_user(to, from, n);
		n = raw_copy_to_user(to, from, n);
	}
	return n;
}
extern __must_check unsigned long
_copy_to_user(void __user *, const void *, unsigned long);

static __always_inline unsigned long __must_check
copy_from_user(void *to, const void __user *from, unsigned long n)
{
	if (!check_copy_size(to, n, false))
		return n;
#ifdef INLINE_COPY_FROM_USER
	return _inline_copy_from_user(to, from, n);
#else
	return _copy_from_user(to, from, n);
#endif
}

static __always_inline unsigned long __must_check
copy_to_user(void __user *to, const void *from, unsigned long n)
{
	if (!check_copy_size(from, n, true))
		return n;

#ifdef INLINE_COPY_TO_USER
	return _inline_copy_to_user(to, from, n);
#else
	return _copy_to_user(to, from, n);
#endif
}

#ifndef copy_mc_to_kernel
/*
 * Without arch opt-in this generic copy_mc_to_kernel() will not handle
 * #MC (or arch equivalent) during source read.
 */
static inline unsigned long __must_check
copy_mc_to_kernel(void *dst, const void *src, size_t cnt)
{
	memcpy(dst, src, cnt);
	return 0;
}
#endif

static __always_inline void pagefault_disabled_inc(void)
{
	current->pagefault_disabled++;
}

static __always_inline void pagefault_disabled_dec(void)
{
	current->pagefault_disabled--;
}

/*
 * These routines enable/disable the pagefault handler. If disabled, it will
 * not take any locks and go straight to the fixup table.
 *
 * User access methods will not sleep when called from a pagefault_disabled()
 * environment.
 */
static inline void pagefault_disable(void)
{
	pagefault_disabled_inc();
	/*
	 * make sure to have issued the store before a pagefault
	 * can hit.
	 */
	barrier();
}

static inline void pagefault_enable(void)
{
	/*
	 * make sure to issue those last loads/stores before enabling
	 * the pagefault handler again.
	 */
	barrier();
	pagefault_disabled_dec();
}

/*
 * Is the pagefault handler disabled? If so, user access methods will not sleep.
 */
static inline bool pagefault_disabled(void)
{
	return current->pagefault_disabled != 0;
}

/*
 * The pagefault handler is in general disabled by pagefault_disable() or
 * when in irq context (via in_atomic()).
 *
 * This function should only be used by the fault handlers. Other users should
 * stick to pagefault_disabled().
 * Please NEVER use preempt_disable() to disable the fault handler. With
 * !CONFIG_PREEMPT_COUNT, this is like a NOP. So the handler won't be disabled.
 * in_atomic() will report different values based on !CONFIG_PREEMPT_COUNT.
 */
#define faulthandler_disabled() (pagefault_disabled() || in_atomic())

DEFINE_LOCK_GUARD_0(pagefault, pagefault_disable(), pagefault_enable())

#ifndef CONFIG_ARCH_HAS_SUBPAGE_FAULTS

/**
 * probe_subpage_writeable: probe the user range for write faults at sub-page
 *			    granularity (e.g. arm64 MTE)
 * @uaddr: start of address range
 * @size: size of address range
 *
 * Returns 0 on success, the number of bytes not probed on fault.
 *
 * It is expected that the caller checked for the write permission of each
 * page in the range either by put_user() or GUP. The architecture port can
 * implement a more efficient get_user() probing if the same sub-page faults
 * are triggered by either a read or a write.
 */
static inline size_t probe_subpage_writeable(char __user *uaddr, size_t size)
{
	return 0;
}

#endif /* CONFIG_ARCH_HAS_SUBPAGE_FAULTS */

#ifndef ARCH_HAS_NOCACHE_UACCESS

static inline __must_check unsigned long
__copy_from_user_inatomic_nocache(void *to, const void __user *from,
				  unsigned long n)
{
	return __copy_from_user_inatomic(to, from, n);
}

#endif		/* ARCH_HAS_NOCACHE_UACCESS */

extern __must_check int check_zeroed_user(const void __user *from, size_t size);

/**
 * copy_struct_from_user: copy a struct from userspace
 * @dst:   Destination address, in kernel space. This buffer must be @ksize
 *         bytes long.
 * @ksize: Size of @dst struct.
 * @src:   Source address, in userspace.
 * @usize: (Alleged) size of @src struct.
 *
 * Copies a struct from userspace to kernel space, in a way that guarantees
 * backwards-compatibility for struct syscall arguments (as long as future
 * struct extensions are made such that all new fields are *appended* to the
 * old struct, and zeroed-out new fields have the same meaning as the old
 * struct).
 *
 * @ksize is just sizeof(*dst), and @usize should've been passed by userspace.
 * The recommended usage is something like the following:
 *
 *   SYSCALL_DEFINE2(foobar, const struct foo __user *, uarg, size_t, usize)
 *   {
 *      int err;
 *      struct foo karg = {};
 *
 *      if (usize > PAGE_SIZE)
 *        return -E2BIG;
 *      if (usize < FOO_SIZE_VER0)
 *        return -EINVAL;
 *
 *      err = copy_struct_from_user(&karg, sizeof(karg), uarg, usize);
 *      if (err)
 *        return err;
 *
 *      // ...
 *   }
 *
 * There are three cases to consider:
 *  * If @usize == @ksize, then it's copied verbatim.
 *  * If @usize < @ksize, then the userspace has passed an old struct to a
 *    newer kernel. The rest of the trailing bytes in @dst (@ksize - @usize)
 *    are to be zero-filled.
 *  * If @usize > @ksize, then the userspace has passed a new struct to an
 *    older kernel. The trailing bytes unknown to the kernel (@usize - @ksize)
 *    are checked to ensure they are zeroed, otherwise -E2BIG is returned.
 *
 * Returns (in all cases, some data may have been copied):
 *  * -E2BIG:  (@usize > @ksize) and there are non-zero trailing bytes in @src.
 *  * -EFAULT: access to userspace failed.
 */
static __always_inline __must_check int
copy_struct_from_user(void *dst, size_t ksize, const void __user *src,
		      size_t usize)
{
	size_t size = min(ksize, usize);
	size_t rest = max(ksize, usize) - size;

	/* Double check if ksize is larger than a known object size. */
	if (WARN_ON_ONCE(ksize > __builtin_object_size(dst, 1)))
		return -E2BIG;

	/* Deal with trailing bytes. */
	if (usize < ksize) {
		memset(dst + size, 0, rest);
	} else if (usize > ksize) {
		int ret = check_zeroed_user(src + size, rest);
		if (ret <= 0)
			return ret ?: -E2BIG;
	}
	/* Copy the interoperable parts of the struct. */
	if (copy_from_user(dst, src, size))
		return -EFAULT;
	return 0;
}

/**
 * copy_struct_to_user: copy a struct to userspace
 * @dst:   Destination address, in userspace. This buffer must be @ksize
 *         bytes long.
 * @usize: (Alleged) size of @dst struct.
 * @src:   Source address, in kernel space.
 * @ksize: Size of @src struct.
 * @ignored_trailing: Set to %true if there was a non-zero byte in @src that
 * userspace cannot see because they are using an smaller struct.
 *
 * Copies a struct from kernel space to userspace, in a way that guarantees
 * backwards-compatibility for struct syscall arguments (as long as future
 * struct extensions are made such that all new fields are *appended* to the
 * old struct, and zeroed-out new fields have the same meaning as the old
 * struct).
 *
 * Some syscalls may wish to make sure that userspace knows about everything in
 * the struct, and if there is a non-zero value that userspce doesn't know
 * about, they want to return an error (such as -EMSGSIZE) or have some other
 * fallback (such as adding a "you're missing some information" flag). If
 * @ignored_trailing is non-%NULL, it will be set to %true if there was a
 * non-zero byte that could not be copied to userspace (ie. was past @usize).
 *
 * While unconditionally returning an error in this case is the simplest
 * solution, for maximum backward compatibility you should try to only return
 * -EMSGSIZE if the user explicitly requested the data that couldn't be copied.
 * Note that structure sizes can change due to header changes and simple
 * recompilations without code changes(!), so if you care about
 * @ignored_trailing you probably want to make sure that any new field data is
 * associated with a flag. Otherwise you might assume that a program knows
 * about data it does not.
 *
 * @ksize is just sizeof(*src), and @usize should've been passed by userspace.
 * The recommended usage is something like the following:
 *
 *   SYSCALL_DEFINE2(foobar, struct foo __user *, uarg, size_t, usize)
 *   {
 *      int err;
 *      bool ignored_trailing;
 *      struct foo karg = {};
 *
 *      if (usize > PAGE_SIZE)
 *		return -E2BIG;
 *      if (usize < FOO_SIZE_VER0)
 *		return -EINVAL;
 *
 *      // ... modify karg somehow ...
 *
 *      err = copy_struct_to_user(uarg, usize, &karg, sizeof(karg),
 *				  &ignored_trailing);
 *      if (err)
 *		return err;
 *      if (ignored_trailing)
 *		return -EMSGSIZE:
 *
 *      // ...
 *   }
 *
 * There are three cases to consider:
 *  * If @usize == @ksize, then it's copied verbatim.
 *  * If @usize < @ksize, then the kernel is trying to pass userspace a newer
 *    struct than it supports. Thus we only copy the interoperable portions
 *    (@usize) and ignore the rest (but @ignored_trailing is set to %true if
 *    any of the trailing (@ksize - @usize) bytes are non-zero).
 *  * If @usize > @ksize, then the kernel is trying to pass userspace an older
 *    struct than userspace supports. In order to make sure the
 *    unknown-to-the-kernel fields don't contain garbage values, we zero the
 *    trailing (@usize - @ksize) bytes.
 *
 * Returns (in all cases, some data may have been copied):
 *  * -EFAULT: access to userspace failed.
 */
static __always_inline __must_check int
copy_struct_to_user(void __user *dst, size_t usize, const void *src,
		    size_t ksize, bool *ignored_trailing)
{
	size_t size = min(ksize, usize);
	size_t rest = max(ksize, usize) - size;

	/* Double check if ksize is larger than a known object size. */
	if (WARN_ON_ONCE(ksize > __builtin_object_size(src, 1)))
		return -E2BIG;

	/* Deal with trailing bytes. */
	if (usize > ksize) {
		if (clear_user(dst + size, rest))
			return -EFAULT;
	}
	if (ignored_trailing)
		*ignored_trailing = ksize < usize &&
			memchr_inv(src + size, 0, rest) != NULL;
	/* Copy the interoperable parts of the struct. */
	if (copy_to_user(dst, src, size))
		return -EFAULT;
	return 0;
}

bool copy_from_kernel_nofault_allowed(const void *unsafe_src, size_t size);

long copy_from_kernel_nofault(void *dst, const void *src, size_t size);
long notrace copy_to_kernel_nofault(void *dst, const void *src, size_t size);

long copy_from_user_nofault(void *dst, const void __user *src, size_t size);
long notrace copy_to_user_nofault(void __user *dst, const void *src,
		size_t size);

long strncpy_from_kernel_nofault(char *dst, const void *unsafe_addr,
		long count);

long strncpy_from_user_nofault(char *dst, const void __user *unsafe_addr,
		long count);
long strnlen_user_nofault(const void __user *unsafe_addr, long count);

#ifndef __get_kernel_nofault
#define __get_kernel_nofault(dst, src, type, label)	\
do {							\
	type __user *p = (type __force __user *)(src);	\
	type data;					\
	if (__get_user(data, p))			\
		goto label;				\
	*(type *)dst = data;				\
} while (0)

#define __put_kernel_nofault(dst, src, type, label)	\
do {							\
	type __user *p = (type __force __user *)(dst);	\
	type data = *(type *)src;			\
	if (__put_user(data, p))			\
		goto label;				\
} while (0)
#endif

/**
 * get_kernel_nofault(): safely attempt to read from a location
 * @val: read into this variable
 * @ptr: address to read from
 *
 * Returns 0 on success, or -EFAULT.
 */
#define get_kernel_nofault(val, ptr) ({				\
	const typeof(val) *__gk_ptr = (ptr);			\
	copy_from_kernel_nofault(&(val), __gk_ptr, sizeof(val));\
})

#ifndef user_access_begin
#define user_access_begin(ptr,len) access_ok(ptr, len)
#define user_access_end() do { } while (0)
#define unsafe_op_wrap(op, err) do { if (unlikely(op)) goto err; } while (0)
#define unsafe_get_user(x,p,e) unsafe_op_wrap(__get_user(x,p),e)
#define unsafe_put_user(x,p,e) unsafe_op_wrap(__put_user(x,p),e)
#define unsafe_copy_to_user(d,s,l,e) unsafe_op_wrap(__copy_to_user(d,s,l),e)
#define unsafe_copy_from_user(d,s,l,e) unsafe_op_wrap(__copy_from_user(d,s,l),e)
static inline unsigned long user_access_save(void) { return 0UL; }
static inline void user_access_restore(unsigned long flags) { }
#endif
#ifndef user_write_access_begin
#define user_write_access_begin user_access_begin
#define user_write_access_end user_access_end
#endif
#ifndef user_read_access_begin
#define user_read_access_begin user_access_begin
#define user_read_access_end user_access_end
#endif

#ifdef CONFIG_HARDENED_USERCOPY
void __noreturn usercopy_abort(const char *name, const char *detail,
			       bool to_user, unsigned long offset,
			       unsigned long len);
#endif

#endif		/* __LINUX_UACCESS_H__ */
