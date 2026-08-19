// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright 2018-2019 Qiang Yu <yuq825@gmail.com> */
/* 3.10 backport of lima_ctx.c: xarray -> idr (complete in 3.10). */

#include <linux/slab.h>

#include "lima_ctx.h"
#include "lima_sched.h"

static void lima_ctx_release(struct kref *kref)
{
	struct lima_ctx *ctx = container_of(kref, struct lima_ctx, refcnt);
	int i;

	for (i = 0; i < lima_pipe_num; i++)
		lima_sched_context_fini(ctx->dev->pipe + i, ctx->context + i);
	kfree(ctx);
}

void lima_ctx_put(struct lima_ctx *ctx)
{
	if (ctx)
		kref_put(&ctx->refcnt, lima_ctx_release);
}

int lima_ctx_create(struct lima_device *dev, struct lima_ctx_mgr *mgr,
		    u32 *id)
{
	struct lima_ctx *ctx;
	int i, err;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	kref_init(&ctx->refcnt);
	ctx->dev = dev;
	ctx->freed = false;
	atomic_set(&ctx->guilty, 0);
	for (i = 0; i < lima_pipe_num; i++) {
		err = lima_sched_context_init(dev->pipe + i,
					      ctx->context + i, &ctx->guilty);
		if (err)
			goto err_free;
	}

	if (!idr_pre_get(&mgr->handles, GFP_KERNEL)) {
		err = -ENOMEM;
		goto err_free;
	}
	mutex_lock(&mgr->lock);
	err = idr_alloc(&mgr->handles, ctx, 1, 0, GFP_KERNEL);
	mutex_unlock(&mgr->lock);
	if (err < 0)
		goto err_free;

	*id = err;	/* table holds the initial ref */
	return 0;

err_free:
	kfree(ctx);
	return err;
}

struct lima_ctx *lima_ctx_get(struct lima_ctx_mgr *mgr, u32 id)
{
	struct lima_ctx *ctx;

	mutex_lock(&mgr->lock);
	ctx = idr_find(&mgr->handles, id);
	if (ctx && !ctx->freed)
		kref_get(&ctx->refcnt);
	else
		ctx = NULL;
	mutex_unlock(&mgr->lock);
	return ctx;
}

int lima_ctx_free(struct lima_ctx_mgr *mgr, u32 id)
{
	struct lima_ctx *ctx;

	mutex_lock(&mgr->lock);
	ctx = idr_find(&mgr->handles, id);
	if (ctx) {
		ctx->freed = true;
		idr_remove(&mgr->handles, id);
	}
	mutex_unlock(&mgr->lock);
	if (!ctx)
		return -EINVAL;

	lima_ctx_put(ctx);	/* drop the table's ref; queued tasks
				 * keep their own until completion */
	return 0;
}

void lima_ctx_mgr_init(struct lima_ctx_mgr *mgr)
{
	mutex_init(&mgr->lock);
	idr_init(&mgr->handles);
}

void lima_ctx_mgr_fini(struct lima_ctx_mgr *mgr)
{
	struct lima_ctx *ctx;
	int id;

	/* free every remaining context (file closed with live contexts);
	 * lima_ctx_put -> release -> context_fini drains in-flight tasks */
	for (;;) {
		mutex_lock(&mgr->lock);
		ctx = NULL;
		idr_for_each_entry(&mgr->handles, ctx, id)
			break;
		if (ctx) {
			ctx->freed = true;
			idr_remove(&mgr->handles, id);
		}
		mutex_unlock(&mgr->lock);
		if (!ctx)
			break;
		lima_ctx_put(ctx);
	}
	idr_destroy(&mgr->handles);
}
