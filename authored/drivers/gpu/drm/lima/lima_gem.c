// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright 2017-2019 Qiang Yu <yuq825@gmail.com> */
/*
 * 3.10 backport of lima_gem.c. Deltas vs upstream v5.2:
 *
 *  - 3.10 GEM: mmap offsets come from obj->map_list.hash.key (page
 *    index); the offset is created once, eagerly, at handle create
 *    (3.10's drm_gem_create_mmap_offset is not safely idempotent).
 *  - fault(): 3.10 signature (vma, vmf) with vmf->virtual_address;
 *    pages are eager, so the handler is a straight vm_insert_page.
 *    The user mapping stays write-combine (set by the 3.10 drm core in
 *    drm_gem_mmap), so CPU writes bypass the caches and no explicit
 *    flush is needed before the GPU reads - same assumption upstream
 *    lima makes with lima_set_vma_flags().
 *  - sync: no drm_syncobj/sync_file on 3.10 - in_sync[]/out_sync and
 *    LIMA_SUBMIT_FLAG_EXPLICIT_FENCE are rejected with -EINVAL.
 *    Implicit sync goes through the per-BO mini reservation object
 *    (lima_compat_resv.h): deps are collected before run, and the
 *    task fence becomes the BO's exclusive fence at submit.
 *  - no ww-mutex lock_bos: pages are eager and immutable over the BO
 *    lifetime, lima_vm_bo_add/del take bo->lock themselves, and the
 *    reservation object has its own mutex, so upstream's ww locking
 *    has nothing left to protect here.
 */

#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/uaccess.h>

#include <drm/drmP.h>
#include <drm/lima_drm.h>

#include "lima_compat.h"
#include "lima_drv.h"
#include "lima_gem.h"
#include "lima_object.h"
#include "lima_device.h"
#include "lima_sched.h"
#include "lima_vm.h"

struct lima_bo *lima_gem_create_bo(struct drm_device *dev, u32 size,
				   u32 flags)
{
	struct lima_device *ldev = to_lima_dev(dev);

	return lima_bo_create(ldev, size, flags, NULL, NULL);
}

int lima_gem_create_handle(struct drm_device *dev, struct drm_file *file,
			   u32 size, u32 flags, u32 *handle)
{
	struct lima_bo *bo;
	int err;

	bo = lima_gem_create_bo(dev, size, flags);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	err = drm_gem_handle_create(file, &bo->gem, handle);
	if (err)
		goto out_put;

	/* 3.10: allocate the fake mmap offset exactly once per object */
	err = drm_gem_create_mmap_offset(&bo->gem);
	if (err) {
		drm_gem_handle_delete(file, *handle);
		goto out_put;
	}

out_put:
	drm_gem_object_put(&bo->gem);	/* the handle holds its own ref */
	return err;
}

void lima_gem_free_object(struct drm_gem_object *obj)
{
	struct lima_bo *bo = to_lima_bo(obj);

	/* vm mappings are torn down via gem close / file release first */
	WARN_ON(!list_empty(&bo->va));
	lima_bo_destroy(bo);
}

int lima_gem_object_open(struct drm_gem_object *obj, struct drm_file *file)
{
	struct lima_drm_priv *priv = to_lima_drm_priv(file);
	struct lima_bo *bo = to_lima_bo(obj);

	return lima_vm_bo_add(priv->vm, bo, true);
}

void lima_gem_object_close(struct drm_gem_object *obj, struct drm_file *file)
{
	struct lima_drm_priv *priv = to_lima_drm_priv(file);
	struct lima_bo *bo = to_lima_bo(obj);

	lima_vm_bo_del(priv->vm, bo);
}

int lima_gem_get_info(struct drm_file *file, u32 handle, u32 *va,
		      u64 *offset)
{
	struct lima_drm_priv *priv = to_lima_drm_priv(file);
	struct drm_gem_object *obj;
	struct lima_bo *bo;

	obj = lima_gem_object_lookup(file, handle);
	if (!obj)
		return -ENOENT;
	bo = to_lima_bo(obj);

	*va = lima_vm_get_va(priv->vm, bo);
	/* created at handle time (3.10: page index in map_list.hash.key) */
	*offset = (u64)obj->map_list.hash.key << PAGE_SHIFT;

	drm_gem_object_put(obj);
	return 0;
}

static int lima_gem_fault(struct vm_area_struct *vma,
			  struct vm_fault *vmf)
{
	struct drm_gem_object *obj = vma->vm_private_data;
	struct lima_bo *bo = to_lima_bo(obj);
	unsigned long address = (unsigned long)vmf->virtual_address;
	pgoff_t pgoff;
	int err;

	if (!bo->pages)
		return VM_FAULT_SIGBUS;

	pgoff = (address - vma->vm_start) >> PAGE_SHIFT;
	if (pgoff >= (obj->size >> PAGE_SHIFT))
		return VM_FAULT_SIGBUS;

	err = vm_insert_page(vma, address, bo->pages[pgoff]);
	if (err == -ENOMEM)
		return VM_FAULT_OOM;
	if (err)
		return VM_FAULT_SIGBUS;
	return VM_FAULT_NOPAGE;
}

const struct vm_operations_struct lima_gem_vm_ops = {
	.fault = lima_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

void lima_set_vma_flags(struct vm_area_struct *vma)
{
	/* 3.10's drm_gem_mmap already set these; kept for interface parity
	 * with the kept lima_gem.h. Write-combine is REQUIRED for
	 * coherency (CPU writes must reach memory uncached for the GPU). */
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
}

int lima_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int ret;

	ret = drm_gem_mmap(filp, vma);
	if (ret)
		return ret;
	lima_set_vma_flags(vma);
	return 0;
}

static int lima_gem_add_deps(struct lima_sched_task *task,
			     struct lima_bo **bos, u32 nr_bos)
{
	u32 i;

	for (i = 0; i < nr_bos; i++) {
		struct dma_fence **fences = NULL;
		int j, n, err;

		n = lima_resv_get_fences(&bos[i]->resv, &fences);
		if (n < 0)
			return n;
		for (j = 0; j < n; j++) {
			err = lima_sched_task_add_dep(task, fences[j]);
			if (err) {	/* takes the ref on success */
				dma_fence_put(fences[j]);
				for (j++; j < n; j++)
					dma_fence_put(fences[j]);
				kfree(fences);
				return err;
			}
		}
		kfree(fences);
	}
	return 0;
}

int lima_gem_submit(struct drm_file *file, struct lima_submit *submit)
{
	struct lima_device *ldev = submit->ctx->dev;
	struct lima_drm_priv *priv = to_lima_drm_priv(file);
	struct lima_vm *vm = priv->vm;
	struct dma_fence *fence;
	struct lima_bo **lbos;
	int err, i;

	/* no syncobj/sync_file on 3.10: explicit sync is unsupported */
	if ((submit->flags & LIMA_SUBMIT_FLAG_EXPLICIT_FENCE) ||
	    submit->in_sync[0] || submit->in_sync[1] || submit->out_sync)
		return -EINVAL;

	lbos = kcalloc(submit->nr_bos, sizeof(*lbos), GFP_KERNEL);
	if (!lbos)
		return -ENOMEM;

	for (i = 0; i < submit->nr_bos; i++) {
		struct drm_gem_object *obj;

		if (submit->bos[i].flags &
		    ~(LIMA_SUBMIT_BO_READ | LIMA_SUBMIT_BO_WRITE)) {
			err = -EINVAL;
			goto out_put;
		}
		obj = lima_gem_object_lookup(file, submit->bos[i].handle);
		if (!obj) {
			err = -ENOENT;
			goto out_put;
		}
		lbos[i] = to_lima_bo(obj);
	}

	/* make sure every BO is mapped in this file's VM (self-locked) */
	for (i = 0; i < submit->nr_bos; i++) {
		err = lima_vm_bo_add(vm, lbos[i], false);
		if (err)
			goto out_put;
	}

	err = lima_sched_task_init(submit->task,
				   &submit->ctx->context[submit->pipe],
				   lbos, submit->nr_bos, vm);
	if (err)
		goto out_put;	/* init consumed nothing on failure */

	/* implicit sync: depend on everything these BOs are fences of */
	err = lima_gem_add_deps(submit->task, lbos, submit->nr_bos);
	if (err)
		goto out_fini;

	/* transfer the ctx ref to the task before it can complete */
	submit->task->ctx = submit->ctx;

	fence = lima_sched_context_queue_task(
		&submit->ctx->context[submit->pipe], submit->task);

	/* task now belongs to the pipe; publish its fence as the new
	 * exclusive writer of every involved BO */
	for (i = 0; i < submit->nr_bos; i++)
		lima_resv_set_excl(&lbos[i]->resv, dma_fence_get(fence));

	dma_fence_put(fence);
	return 0;

out_fini:
	lima_sched_task_fini(submit->task);
	return err;	/* slab memory is freed by the ioctl caller */
out_put:
	while (--i >= 0)
		drm_gem_object_put(&lbos[i]->gem);
	kfree(lbos);
	return err;
}

int lima_gem_wait(struct drm_file *file, u32 handle, u32 op,
		  s64 timeout_ns)
{
	struct drm_gem_object *obj;
	struct lima_bo *bo;
	long ret;

	if (op & ~(LIMA_GEM_WAIT_READ | LIMA_GEM_WAIT_WRITE))
		return -EINVAL;

	obj = lima_gem_object_lookup(file, handle);
	if (!obj)
		return -ENOENT;
	bo = to_lima_bo(obj);

	/* uapi passes an absolute CLOCK_MONOTONIC timeout */
	ret = lima_resv_wait_timeout(&bo->resv, true,
				     lima_timeout_abs_to_jiffies(timeout_ns));

	drm_gem_object_put(obj);

	if (ret > 0)
		return 0;		/* signaled */
	if (ret == 0)
		return -ETIME;		/* timeout */
	return ret;			/* fence error / interrupted */
}
