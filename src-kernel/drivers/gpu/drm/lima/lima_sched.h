/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* Copyright 2017-2019 Qiang Yu <yuq825@gmail.com> */
/*
 * 3.10 backport: replacement for the upstream (drm_sched-based)
 * lima_sched.h. Same external interface; internally a strict FIFO on a
 * per-pipe ordered workqueue. See docs/gap-analysis.md section 2.3.
 *
 * Differences vs upstream:
 *  - no drm_sched_job/entity (no drm/sched in 3.10)
 *  - task deps are a plain array of mini-fences (no xarray)
 *  - pipe fence is the mini dma_fence from lima_compat_fence.h
 *  - one priority level only, no preemption
 */
#ifndef __LIMA_SCHED_H__
#define __LIMA_SCHED_H__

#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/slab.h>

#include "lima_compat.h"

struct lima_vm;
struct lima_ctx;
struct lima_ip;
struct lima_bo;
struct lima_device;

struct lima_sched_task {
	struct list_head list;		/* node in pipe queue */

	struct lima_sched_pipe *pipe;
	struct lima_sched_context *context;
	struct lima_ctx *ctx;		/* ref held while queued/running */

	struct lima_vm *vm;
	void *frame;

	/* fences this task waits on before running (owned refs) */
	struct dma_fence **deps;
	int num_deps;

	struct lima_bo **bos;
	int num_bos;

	/* pipe fence: signaled on completion (1 ok / <0 error) */
	struct dma_fence *fence;
};

struct lima_sched_context {
	struct lima_sched_pipe *pipe;
	atomic_t *guilty;
	atomic_t in_flight;
};

#define LIMA_SCHED_PIPE_MAX_MMU       8
#define LIMA_SCHED_PIPE_MAX_L2_CACHE  2
#define LIMA_SCHED_PIPE_MAX_PROCESSOR 8

struct lima_sched_pipe {
	const char *name;

	/* hardware assignment, filled by lima_device.c before pipe_init */
	struct lima_ip *mmu[LIMA_SCHED_PIPE_MAX_MMU];
	int num_mmu;

	struct lima_ip *l2_cache[LIMA_SCHED_PIPE_MAX_L2_CACHE];
	int num_l2_cache;

	struct lima_ip *processor[LIMA_SCHED_PIPE_MAX_PROCESSOR];
	int num_processor;

	struct lima_ip *bcast_processor;
	struct lima_ip *bcast_mmu;

	u32 done;
	bool error;
	atomic_t task;

	struct lima_sched_task *current_task;
	struct lima_vm *current_vm;

	int frame_size;
	struct kmem_cache *task_slab;

	int (*task_validate)(struct lima_sched_pipe *pipe, struct lima_sched_task *task);
	void (*task_run)(struct lima_sched_pipe *pipe, struct lima_sched_task *task);
	void (*task_fini)(struct lima_sched_pipe *pipe);
	void (*task_error)(struct lima_sched_pipe *pipe);
	void (*task_mmu_error)(struct lima_sched_pipe *pipe);

	/* 3.10 FIFO engine: an ordered workqueue serializes run/done/error
	 * handling per pipe, so no extra locking is needed for
	 * current_task/current_vm (queue list itself is ioctl-producer vs
	 * wq-consumer and takes queue_lock). */
	struct workqueue_struct *wq;
	struct work_struct run_work;
	struct work_struct done_work;
	struct delayed_work timeout_work;
	struct list_head queue;
	struct mutex queue_lock;
};

int lima_sched_task_init(struct lima_sched_task *task,
			 struct lima_sched_context *context,
			 struct lima_bo **bos, int num_bos,
			 struct lima_vm *vm);
void lima_sched_task_fini(struct lima_sched_task *task);
/* takes ownership of the caller's fence reference */
int lima_sched_task_add_dep(struct lima_sched_task *task,
			    struct dma_fence *fence);

int lima_sched_context_init(struct lima_sched_pipe *pipe,
			    struct lima_sched_context *context,
			    atomic_t *guilty);
void lima_sched_context_fini(struct lima_sched_pipe *pipe,
			     struct lima_sched_context *context);
struct dma_fence *lima_sched_context_queue_task(struct lima_sched_context *context,
						struct lima_sched_task *task);

int lima_sched_pipe_init(struct lima_sched_pipe *pipe, const char *name);
void lima_sched_pipe_fini(struct lima_sched_pipe *pipe);
void lima_sched_pipe_task_done(struct lima_sched_pipe *pipe);

static inline void lima_sched_pipe_mmu_error(struct lima_sched_pipe *pipe)
{
	pipe->error = true;
	pipe->task_mmu_error(pipe);
}

int lima_sched_slab_init(void);
void lima_sched_slab_fini(void);

#endif
