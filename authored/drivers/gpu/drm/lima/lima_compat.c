// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * lima_compat.c - mini dma_fence + reservation object implementation.
 * See lima_compat_fence.h / lima_compat_resv.h for the design notes.
 */

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/ktime.h>

#include "lima_compat.h"

/* ---------------- mini dma_fence ---------------- */

static struct kmem_cache *lima_fence_slab;

int lima_fence_slab_init(void)
{
	lima_fence_slab = kmem_cache_create("lima_fence",
					    sizeof(struct dma_fence), 0,
					    SLAB_HWCACHE_ALIGN, NULL);
	return lima_fence_slab ? 0 : -ENOMEM;
}

void lima_fence_slab_fini(void)
{
	kmem_cache_destroy(lima_fence_slab);
}

struct dma_fence *lima_fence_create(const char *name)
{
	struct dma_fence *f;

	f = kmem_cache_zalloc(lima_fence_slab, GFP_KERNEL);
	if (!f)
		return NULL;
	kref_init(&f->refcount);
	init_waitqueue_head(&f->wait);
	f->status = 0;
	f->name = name;
	return f;
}

static void lima_fence_release(struct kref *kref)
{
	struct dma_fence *f = container_of(kref, struct dma_fence, refcount);

	/* kmem_cache_free never sleeps: safe from irq context */
	kmem_cache_free(lima_fence_slab, f);
}

void dma_fence_put(struct dma_fence *f)
{
	if (f)
		kref_put(&f->refcount, lima_fence_release);
}

/* single-signaler invariant (only the pipe completion path signals);
 * first signal wins, later ones (e.g. timeout vs irq race) lose. */
static int lima_fence_do_signal(struct dma_fence *f, int v)
{
	if (cmpxchg(&f->status, 0, v) != 0)
		return -EBUSY;
	f->timestamp_ns = ktime_to_ns(ktime_get());
	wake_up_all(&f->wait);
	return 0;
}

int dma_fence_signal(struct dma_fence *f)
{
	return lima_fence_do_signal(f, 1);
}

int dma_fence_signal_error(struct dma_fence *f, int error)
{
	if (error >= 0)
		error = -EIO;
	return lima_fence_do_signal(f, error);
}

long dma_fence_wait_timeout(struct dma_fence *f, bool intr, long timeout)
{
	long ret;

	if (dma_fence_is_signaled(f))
		return f->status < 0 ? f->status : timeout;

	if (intr)
		ret = wait_event_interruptible_timeout(f->wait,
						       dma_fence_is_signaled(f),
						       timeout);
	else
		ret = wait_event_timeout(f->wait, dma_fence_is_signaled(f),
					 timeout);

	if (ret < 0)
		return ret;		/* -ERESTARTSYS */
	if (ret == 0 && !dma_fence_is_signaled(f))
		return 0;			/* genuine timeout */
	if (f->status < 0)
		return f->status;
	return ret ? ret : 1;
}

/* ---------------- mini reservation object ---------------- */

void lima_resv_init(struct lima_resv *resv)
{
	mutex_init(&resv->lock);
	resv->fence_excl = NULL;
	resv->shared = NULL;
	resv->num_shared = 0;
	resv->max_shared = 0;
}

void lima_resv_fini(struct lima_resv *resv)
{
	u32 i;

	dma_fence_put(resv->fence_excl);
	for (i = 0; i < resv->num_shared; i++)
		dma_fence_put(resv->shared[i]);
	kfree(resv->shared);
}

int lima_resv_set_excl(struct lima_resv *resv, struct dma_fence *fence)
{
	mutex_lock(&resv->lock);
	dma_fence_put(resv->fence_excl);
	resv->fence_excl = fence;	/* takes caller's reference */
	mutex_unlock(&resv->lock);
	return 0;
}

int lima_resv_add_shared(struct lima_resv *resv, struct dma_fence *fence)
{
	int ret = 0;

	mutex_lock(&resv->lock);
	if (resv->num_shared == resv->max_shared) {
		u32 nmax = resv->max_shared ? resv->max_shared * 2 : 4;
		struct dma_fence **n;

		n = krealloc(resv->shared, nmax * sizeof(*n), GFP_KERNEL);
		if (!n) {
			ret = -ENOMEM;
			goto out;
		}
		resv->shared = n;
		resv->max_shared = nmax;
	}
	resv->shared[resv->num_shared++] = dma_fence_get(fence);
out:
	mutex_unlock(&resv->lock);
	return ret;
}

int lima_resv_get_fences(struct lima_resv *resv, struct dma_fence ***fences)
{
	u32 i, n = 0;
	struct dma_fence **arr;

	mutex_lock(&resv->lock);
	if (resv->fence_excl)
		n++;
	n += resv->num_shared;
	if (!n) {
		mutex_unlock(&resv->lock);
		*fences = NULL;
		return 0;
	}
	arr = kcalloc(n, sizeof(*arr), GFP_KERNEL);
	if (!arr) {
		mutex_unlock(&resv->lock);
		return -ENOMEM;
	}
	n = 0;
	if (resv->fence_excl)
		arr[n++] = dma_fence_get(resv->fence_excl);
	for (i = 0; i < resv->num_shared; i++)
		arr[n++] = dma_fence_get(resv->shared[i]);
	mutex_unlock(&resv->lock);

	*fences = arr;
	return n;
}

long lima_resv_wait_timeout(struct lima_resv *resv, bool intr, long timeout)
{
	struct dma_fence **fences;
	int i, n;
	long ret = timeout;

	n = lima_resv_get_fences(resv, &fences);
	if (n < 0)
		return n;
	for (i = 0; i < n; i++) {
		long r = dma_fence_wait_timeout(fences[i], intr, timeout);

		dma_fence_put(fences[i]);
		if (r < 0) {		/* error or interrupted */
			ret = r;
			break;
		}
		if (r == 0)
			ret = 0;	/* timed out; keep waiting the rest out */
	}
	while (++i < n)
		dma_fence_put(fences[i]);
	kfree(fences);
	return ret;
}
