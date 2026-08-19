/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * lima_compat.h - 3.10 compatibility shims for the v5.2 lima backport.
 *
 * Self-contained on purpose: mkseries.sh prepends this header to the
 * verbatim-kept upstream files, so it must be includable first and must
 * not rely on anything else from the driver. Semantics notes for every
 * shim are in docs/gap-analysis.md section 2.
 */
#ifndef __LIMA_COMPAT_H__
#define __LIMA_COMPAT_H__

#include <linux/version.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/gfp.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>

#include <drm/drmP.h>

#if !defined(DRM_UNLOCKED)
#error "lima 3.10 backport requires DRM_UNLOCKED in drmP.h"
#endif

/* ---- small helpers from newer kernels -------------------------------- */

/* 3.10 has ACCESS_ONCE, not READ_ONCE/WRITE_ONCE (3.18+) */
#ifndef READ_ONCE
#define READ_ONCE(x) ACCESS_ONCE(x)
#endif
#ifndef WRITE_ONCE
#define WRITE_ONCE(x, v) (ACCESS_ONCE(x) = (v))
#endif

#ifndef u64_to_user_ptr
#define u64_to_user_ptr(x) ((void __user *)(unsigned long)(x))
#endif

/* v4.12+: kvcalloc/kvfree. lima's allocations are small; kmalloc is fine. */
static inline void *kvcalloc(size_t n, size_t size, gfp_t flags)
{
	return kcalloc(n, size, flags);
}
#define kvfree(p) kfree(p)

/* v4.16+ hardening API; plain kmem_cache_create on 3.10. */
#define kmem_cache_create_usercopy(name, size, align, flags, uoff, usize, ctor) \
	kmem_cache_create(name, size, align, flags, ctor)

/*
 * 3.10 has no <linux/iopoll.h> (mkseries strips those includes from the
 * kept files); provide the one helper lima uses for reset-wait loops.
 */
#ifndef readl_poll_timeout
#define readl_poll_timeout(addr, val, cond, sleep_us, timeout_ms) ({\
	unsigned long __timeout = jiffies + msecs_to_jiffies(timeout_ms);\
	int __ret = 0;						\
	for (;;) {						\
		(val) = readl(addr);				\
		if (cond)					\
			break;					\
		if (time_after(jiffies, __timeout)) {		\
			(val) = readl(addr);			\
			__ret = (cond) ? 0 : -ETIMEDOUT;		\
			break;					\
		}						\
		if (sleep_us)					\
			udelay(sleep_us);			\
	}							\
	__ret;							\
})
#endif

/* dma_alloc_wc/dma_free_wc (v4.9+); dma_alloc_attrs exists since 3.4. */
#define dma_alloc_wc(dev, size, handle, gfp)				\
	dma_alloc_attrs(dev, size, handle, gfp, DMA_ATTR_WRITE_COMBINE)
#define dma_free_wc(dev, size, cpu, handle)				\
	dma_free_attrs(dev, size, cpu, handle, DMA_ATTR_WRITE_COMBINE)

/*
 * v5.2 allocates BO pages with GFP_DMA32. SC7730SE has at most 1GB RAM
 * and no >4G addressing, so there is nothing to constrain; 3.10 arm has
 * no ZONE_DMA32 either. Plain zeroed GFP_KERNEL is the right thing here.
 */
#define LIMA_GFP_DMA32	(GFP_KERNEL | __GFP_ZERO)

/* ---- GEM object API renames to the 3.10 names -------------------------- */

#define drm_gem_object_get(obj)		drm_gem_object_reference(obj)
#define drm_gem_object_put(obj)		drm_gem_object_unreference(obj)

/* 3.10: drm_gem_object_lookup(dev, filp, handle); v5.2 dropped dev. */
#define lima_gem_object_lookup(filp, handle)				\
	drm_gem_object_lookup((filp)->minor->dev, filp, handle)

/* 3.10 keeps nsecs_to_jiffies() in kernel/time/jiffies.c but does NOT
 * export it to modules, which breaks lima.ko at modpost. Re-implement it
 * with div_u64() (exported), rounding up like nsecs_to_jiffies64(). */
static inline unsigned long lima_nsecs_to_jiffies(u64 n)
{
	return (unsigned long)div_u64(n + NSEC_PER_SEC / HZ - 1,
				      NSEC_PER_SEC / HZ);
}

/* replaces v5.2's drm_timeout_abs_to_jiffies() (drm_utils.h, 4.20+) */
static inline long lima_timeout_abs_to_jiffies(u64 abs_ns)
{
	u64 now = ktime_to_ns(ktime_get());

	if ((s64)(abs_ns - now) <= 0)
		return 0;
	return (long)lima_nsecs_to_jiffies(abs_ns - now) + 1;
}

/* ---- mini dma_fence / reservation object ------------------------------- */

#include "lima_compat_fence.h"
#include "lima_compat_resv.h"

#endif /* __LIMA_COMPAT_H__ */
