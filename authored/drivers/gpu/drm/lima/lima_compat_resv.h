/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Mini reservation object for the 3.10 lima backport, embedded in
 * struct lima_bo (upstream shares one reservation_object between BOs;
 * a per-BO object is sufficient for lima's implicit-sync model: each
 * submitted writer replaces the exclusive fence, readers append to the
 * shared set - which in Mesa's lima usage stays empty most of the time).
 */
#ifndef __LIMA_COMPAT_RESV_H__
#define __LIMA_COMPAT_RESV_H__

#include <linux/mutex.h>

struct dma_fence;

struct lima_resv {
	struct mutex lock;
	struct dma_fence *fence_excl;	/* most recent writer */
	struct dma_fence **shared;	/* concurrent readers (rare) */
	u32 num_shared;
	u32 max_shared;
};

void lima_resv_init(struct lima_resv *resv);
void lima_resv_fini(struct lima_resv *resv);

/* takes over the caller's fence reference */
int lima_resv_set_excl(struct lima_resv *resv, struct dma_fence *fence);
/* takes its own reference */
int lima_resv_add_shared(struct lima_resv *resv, struct dma_fence *fence);

/* snapshot excl+shared with refs taken; caller kfree()s *fences.
 * returns fence count (>=0) or -ENOMEM. */
int lima_resv_get_fences(struct lima_resv *resv, struct dma_fence ***fences);

/* wait for every fence present when called; >0, 0 timeout, <0 error */
long lima_resv_wait_timeout(struct lima_resv *resv, bool intr, long timeout);

#endif /* __LIMA_COMPAT_RESV_H__ */
