// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright 2017-2019 Qiang Yu <yuq825@gmail.com> */
/*
 * 3.10 backport: FIFO replacement for drm_sched.
 *
 * Model: each pipe owns an ordered workqueue. Submits append to a list
 * and kick run_work; run_work waits the head task's dependency fences,
 * switches the VM on the pipe's MMUs when it changes, validates and
 * starts the task, then returns - completion arrives via the gp/pp irq
 * handlers calling lima_sched_pipe_task_done(), which defers to
 * done_work (process context; gem put is not irq-safe) to finish the
 * task, signal its fence, reset the hardware if pipe->error, and start
 * the next one. A delayed timeout work (sched_timeout_ms param, default
 * off) force-completes wedged tasks with an error.
 *
 * Semantics vs upstream: strict in-order execution per pipe (upstream
 * lima is also in-order per pipe), single priority, no preemption.
 */

#include <linux/module.h>
#include <linux/delay.h>

#include "lima_sched.h"
#include "lima_device.h"
#include "lima_vm.h"
#include "lima_mmu.h"
#include "lima_object.h"
#include "lima_ctx.h"

int lima_sched_timeout_ms = 0;
module_param_named(sched_timeout_ms, lima_sched_timeout_ms, int, 0644);
MODULE_PARM_DESC(sched_timeout_ms,
		 "task hang timeout in ms (0 = never; set for bring-up)");

int lima_sched_slab_init(void)
{
	return lima_fence_slab_init();
}

void lima_sched_slab_fini(void)
{
	lima_fence_slab_fini();
}

int lima_sched_task_init(struct lima_sched_task *task,
			 struct lima_sched_context *context,
			 struct lima_bo **bos, int num_bos,
			 struct lima_vm *vm)
{
	INIT_LIST_HEAD(&task->list);
	task->pipe = context->pipe;
	task->context = context;
	task->ctx = NULL;
	task->vm = lima_vm_get(vm);
	task->bos = bos;      /* takes over array + gem refs from caller */
	task->num_bos = num_bos;
	task->deps = NULL;
	task->num_deps = 0;
	task->fence = lima_fence_create(context->pipe->name);
	if (!task->fence) {
		lima_vm_put(vm);
		return -ENOMEM;
	}
	return 0;
}

void lima_sched_task_fini(struct lima_sched_task *task)
{
	int i;

	for (i = 0; i < task->num_deps; i++)
		dma_fence_put(task->deps[i]);
	kfree(task->deps);
	for (i = 0; i < task->num_bos; i++)
		drm_gem_object_put(&task->bos[i]->gem);
	kfree(task->bos);
	if (task->ctx)
		lima_ctx_put(task->ctx);
	lima_vm_put(task->vm);
	dma_fence_put(task->fence);
}

int lima_sched_task_add_dep(struct lima_sched_task *task,
			    struct dma_fence *fence)
{
	struct dma_fence **deps;

	deps = krealloc(task->deps, (task->num_deps + 1) * sizeof(*deps),
			GFP_KERNEL);
	if (!deps)
		return -ENOMEM;
	deps[task->num_deps++] = fence;   /* takes the caller's ref */
	task->deps = deps;
	return 0;
}

int lima_sched_context_init(struct lima_sched_pipe *pipe,
			    struct lima_sched_context *context,
			    atomic_t *guilty)
{
	context->pipe = pipe;
	context->guilty = guilty;
	atomic_set(&context->in_flight, 0);
	return 0;
}

void lima_sched_context_fini(struct lima_sched_pipe *pipe,
			     struct lima_sched_context *context)
{
	/* file close must not hang: drain this context's tasks with a 5s
	 * cap; on timeout force an error + pipe reset, then allow a short
	 * grace period for the error path to unwind. */
	unsigned long timeout = jiffies + msecs_to_jiffies(5000);

	while (atomic_read(&context->in_flight) > 0) {
		if (time_after(jiffies, timeout)) {
			pipe->error = true;
			queue_work(pipe->wq, &pipe->done_work);
			break;
		}
		msleep(10);
	}
	timeout = jiffies + msecs_to_jiffies(1000);
	while (atomic_read(&context->in_flight) > 0 &&
	       !time_after(jiffies, timeout))
		msleep(10);
	WARN_ON(atomic_read(&context->in_flight) > 0);
}

struct dma_fence *lima_sched_context_queue_task(struct lima_sched_context *context,
						struct lima_sched_task *task)
{
	struct lima_sched_pipe *pipe = context->pipe;
	struct dma_fence *fence = dma_fence_get(task->fence);

	atomic_inc(&context->in_flight);
	mutex_lock(&pipe->queue_lock);
	list_add_tail(&task->list, &pipe->queue);
	mutex_unlock(&pipe->queue_lock);
	queue_work(pipe->wq, &pipe->run_work);
	return fence;
}

static void lima_sched_finish_task(struct lima_sched_pipe *pipe,
				   struct lima_sched_task *task, bool err)
{
	if (err) {
		dma_fence_signal_error(task->fence, -EIO);
		if (task->context->guilty)
			atomic_inc(task->context->guilty);
	} else {
		dma_fence_signal(task->fence);
	}
	atomic_dec(&task->context->in_flight);
	lima_sched_task_fini(task);
	kmem_cache_free(pipe->task_slab, task);
}

/* start queued tasks; runs on the pipe's ordered wq */
static void lima_sched_run_work(struct work_struct *w)
{
	struct lima_sched_pipe *pipe =
		container_of(w, struct lima_sched_pipe, run_work);
	struct lima_sched_task *task;
	int i, err;

	for (;;) {
		mutex_lock(&pipe->queue_lock);
		if (list_empty(&pipe->queue) || pipe->current_task) {
			mutex_unlock(&pipe->queue_lock);
			return;
		}
		task = list_first_entry(&pipe->queue,
					struct lima_sched_task, list);
		list_del(&task->list);
		pipe->current_task = task;
		mutex_unlock(&pipe->queue_lock);

		/* dependency fences (writers/readers of our BOs) */
		err = 0;
		for (i = 0; i < task->num_deps; i++) {
			long r = dma_fence_wait(task->deps[i], false);
			if (r < 0) {
				err = r;
				break;
			}
		}
		if (!err && pipe->task_validate)
			err = pipe->task_validate(pipe, task);
		if (err) {
			dev_err(pipe->processor[0]->dev->dev,
				"lima: %s task error before run: %d\n",
				pipe->name, err);
			lima_sched_finish_task(pipe, task, true);
			pipe->current_task = NULL;
			continue;
		}

		/* switch address space on the pipe's MMUs if it changed */
		if (pipe->current_vm != task->vm) {
			for (i = 0; i < pipe->num_mmu; i++)
				lima_mmu_switch_vm(pipe->mmu[i], task->vm);
			pipe->current_vm = task->vm;
		}

		if (lima_sched_timeout_ms > 0)
			queue_delayed_work(pipe->wq, &pipe->timeout_work,
					   msecs_to_jiffies(lima_sched_timeout_ms));

		pipe->task_run(pipe, task);
		/* completion via irq -> lima_sched_pipe_task_done() */
		return;
	}
}

/* task completion, process context (called from the irq via task_done) */
static void lima_sched_done_work(struct work_struct *w)
{
	struct lima_sched_pipe *pipe =
		container_of(w, struct lima_sched_pipe, done_work);
	struct lima_sched_task *task = pipe->current_task;
	bool err;

	if (!task)
		return;

	err = pipe->error;
	lima_sched_finish_task(pipe, task, err);
	pipe->current_task = NULL;

	if (err) {
		/* hardware reset (gp/pp task_error) */
		pipe->task_error(pipe);
		pipe->error = false;
	}

	queue_work(pipe->wq, &pipe->run_work);
}

/* job hang timeout (sched_timeout_ms > 0) */
static void lima_sched_timeout_work(struct work_struct *w)
{
	struct lima_sched_pipe *pipe =
		container_of(to_delayed_work(w), struct lima_sched_pipe,
			     timeout_work);

	if (pipe->current_task) {
		dev_err(pipe->processor[0]->dev->dev,
			"lima: %s task timeout, resetting\n", pipe->name);
		pipe->error = true;
		queue_work(pipe->wq, &pipe->done_work);
	}
}

/* called from the gp/pp irq handlers (and mmu error path) */
void lima_sched_pipe_task_done(struct lima_sched_pipe *pipe)
{
	if (lima_sched_timeout_ms > 0)
		cancel_delayed_work(&pipe->timeout_work);
	queue_work(pipe->wq, &pipe->done_work);
}

int lima_sched_pipe_init(struct lima_sched_pipe *pipe, const char *name)
{
	pipe->name = name;
	pipe->num_mmu = 0;
	pipe->num_l2_cache = 0;
	pipe->num_processor = 0;
	pipe->bcast_processor = NULL;
	pipe->bcast_mmu = NULL;
	pipe->done = 0;
	pipe->error = false;
	atomic_set(&pipe->task, 0);
	pipe->current_task = NULL;
	pipe->current_vm = NULL;

	INIT_LIST_HEAD(&pipe->queue);
	mutex_init(&pipe->queue_lock);
	INIT_WORK(&pipe->run_work, lima_sched_run_work);
	INIT_WORK(&pipe->done_work, lima_sched_done_work);
	INIT_DELAYED_WORK(&pipe->timeout_work, lima_sched_timeout_work);

	pipe->wq = alloc_ordered_workqueue("lima-%s", 0, name);
	if (!pipe->wq)
		return -ENOMEM;
	return 0;
}

void lima_sched_pipe_fini(struct lima_sched_pipe *pipe)
{
	/* all contexts are drained before device teardown reaches this */
	destroy_workqueue(pipe->wq);
}
