// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright 2018-2019 Qiang Yu <yuq825@gmail.com> */
/* 3.10 backport of lima_gem_prime.c (import_sg_table 3.10 signature).
 * Note: 3.10's dma-buf core has no ->mmap for exported BOs; userspace
 * CPU mapping goes through the DRM fd + GEM_INFO offset (what Mesa's
 * lima winsys uses anyway). */

#include <drm/drmP.h>
#include <linux/scatterlist.h>

#include "lima_compat.h"
#include "lima_device.h"
#include "lima_object.h"
#include "lima_gem_prime.h"

struct drm_gem_object *lima_gem_prime_import_sg_table(
	struct drm_device *dev, size_t size, struct sg_table *sgt)
{
	struct lima_device *ldev = to_lima_dev(dev);
	struct lima_bo *bo;

	bo = lima_bo_create(ldev, size, 0, sgt, NULL);
	if (IS_ERR(bo))
		return ERR_CAST(bo);
	return &bo->gem;
}

struct sg_table *lima_gem_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct lima_bo *bo = to_lima_bo(obj);
	int i, npages = obj->size >> PAGE_SHIFT;
	struct sg_table *sgt;
	struct scatterlist *sg;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	if (sg_alloc_table(sgt, npages, GFP_KERNEL)) {
		kfree(sgt);
		return ERR_PTR(-ENOMEM);
	}

	/* pages are allocated eagerly at bo create in this backport */
	for_each_sg(sgt->sgl, sg, npages, i)
		sg_set_page(sg, bo->pages[i], PAGE_SIZE, 0);

	return sgt;
}
