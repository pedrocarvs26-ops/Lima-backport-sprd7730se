/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Mini dma_fence for the 3.10 lima backport.
 *
 * 3.10 predates the dma-fence/reservation framework (reservation_object
 * landed in 3.17). lima only needs a signalable per-task completion
 * object plus a wait primitive, so this is a deliberately small subset
 * wearing upstream-shaped names:
 *
 *  - status: 0 = unsignaled, 1 = signaled ok, <0 = signaled with error
 *    (mmu fault / timeout) - the error reaches waiters, unlike upstream
 *    dma_fence_wait which hides it; lima submits treat <0 as job failure.
 *  - no callbacks, no RCU: waiters sleep on a waitqueue, release is a
 *    kmem_cache_free, so dma_fence_put() is IRQ-safe (called from the
 *    gp/pp irq handlers).
 *
 * dma_fence_wait_timeout() returns >0 remaining jiffies, 0 on timeout,
 * <0 on fence error (or -ERESTARTSYS if intr and interrupted).
 */
#ifndef __LIMA_COMPAT_FENCE_H__
#define __LIMA_COMPAT_FENCE_H__

#include <linux/kref.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/compiler.h>

struct dma_fence {
	struct kref refcount;
	wait_queue_head_t wait;
	int status;		/* 0 unsignaled, 1 ok, <0 error */
	const char *name;	/* string literal, never freed */
	u64 timestamp_ns;
};

struct dma_fence *lima_fence_create(const char *name);

static inline struct dma_fence *dma_fence_get(struct dma_fence *f)
{
	kref_get(&f->refcount);
	return f;
}

void dma_fence_put(struct dma_fence *f);		/* IRQ-safe */

int dma_fence_signal(struct dma_fence *f);		/* -> status = 1 */
int dma_fence_signal_error(struct dma_fence *f, int error);

static inline bool dma_fence_is_signaled(struct dma_fence *f)
{
	return READ_ONCE(f->status) != 0;
}

long dma_fence_wait_timeout(struct dma_fence *f, bool intr, long timeout);

static inline long dma_fence_wait(struct dma_fence *f, bool intr)
{
	return dma_fence_wait_timeout(f, intr, MAX_SCHEDULE_TIMEOUT);
}

int lima_fence_slab_init(void);
void lima_fence_slab_fini(void);

#endif /* __LIMA_COMPAT_FENCE_H__ */
