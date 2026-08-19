// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright 2018-2019 Qiang Yu <yuq825@gmail.com> */
/*
 * 3.10 backport of lima_object.c.
 *
 *  - 3.10 has no drm_gem_get_pages/put_pages: BO pages come from the
 *    GEM shmem filp via read_mapping_page() and each is dma_map_page()d
 *    for the GPU (DMA_BIDIRECTIONAL - GP reads command streams, PP both
 *    reads and writes render targets).
 *  - Pages are allocated EAGERLY at create. Upstream v5.2 is lazy; on
 *    this device BOs are needed by the GPU at submit/mmap time anyway,
 *    and eager keeps the 3.10 fault/export paths simple.
 *  - GFP_DMA32 -> LIMA_GFP_DMA32 (GFP_KERNEL; see lima_compat.h).
 */

#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/vmalloc.h>

#include <drm/drmP.h>

#include "lima_compat.h"
#include "lima_device.h"
#include "lima_object.h"
#include "lima_gem.h"

static int lima_bo_get_pages(struct lima_bo *bo)
{
	struct drm_gem_object *obj = &bo->gem;
	struct address_space *mapping = obj->filp->f_mapping;
	int npages = obj->size >> PAGE_SHIFT;
	int i;

	bo->pages = kcalloc(npages, sizeof(*bo->pages), GFP_KERNEL);
	if (!bo->pages)
		return -ENOMEM;
	bo->pages_dma_addr = kcalloc(npages, sizeof(*bo->pages_dma_addr),
				     GFP_KERNEL);
	if (!bo->pages_dma_addr) {
		kfree(bo->pages);
		bo->pages = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < npages; i++) {
		bo->pages[i] = read_mapping_page(mapping, i, NULL);
		if (IS_ERR(bo->pages[i])) {
			bo->pages[i] = NULL;
			goto err;
		}
		bo->pages_dma_addr[i] = dma_map_page(obj->dev->dev,
						     bo->pages[i], 0, PAGE_SIZE,
						     DMA_BIDIRECTIONAL);
		if (dma_mapping_error(obj->dev->dev, bo->pages_dma_addr[i])) {
			bo->pages_dma_addr[i] = 0;
			i++;	/* include this page in the cleanup below */
			goto err;
		}
	}
	return 0;

err:
	while (--i >= 0) {
		if (bo->pages_dma_addr[i])
			dma_unmap_page(obj->dev->dev, bo->pages_dma_addr[i],
				       PAGE_SIZE, DMA_BIDIRECTIONAL);
		put_page(bo->pages[i]);
	}
	kfree(bo->pages_dma_addr);
	kfree(bo->pages);
	bo->pages = NULL;
	bo->pages_dma_addr = NULL;
	return -ENOMEM;
}

struct lima_bo *lima_bo_create(struct lima_device *dev, u32 size,
			       u32 flags, struct sg_table *sgt,
			       void *resv_unused)
{
	struct lima_bo *bo;
	struct drm_gem_object *obj;
	int err;

	size = PAGE_ALIGN(size);
	if (!size)
		return ERR_PTR(-EINVAL);

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	obj = &bo->gem;
	mutex_init(&bo->lock);
	INIT_LIST_HEAD(&bo->va);
	lima_resv_init(&bo->resv);

	if (sgt) {
		/* PRIME import: pages/dma addresses belong to the exporter */
		struct scatterlist *sg;
		int i, idx = 0, npages = size >> PAGE_SHIFT;

		bo->sgt = sgt;
		drm_gem_private_object_init(dev->ddev, obj, size);

		bo->pages = kcalloc(npages, sizeof(*bo->pages), GFP_KERNEL);
		bo->pages_dma_addr = kcalloc(npages, sizeof(dma_addr_t),
					     GFP_KERNEL);
		if (!bo->pages || !bo->pages_dma_addr)
			goto err_free;

		for_each_sg(sgt->sgl, sg, sgt->nents, i) {
			struct page *p = sg_page(sg);
			int j, n = sg->length >> PAGE_SHIFT;

			for (j = 0; j < n && idx < npages; j++, idx++) {
				bo->pages[idx] = nth_page(p, j);
				bo->pages_dma_addr[idx] =
					sg_dma_address(sg) + j * PAGE_SIZE;
			}
		}
	} else {
		err = drm_gem_object_init(dev->ddev, obj, size);
		if (err)
			goto err_free;

		err = lima_bo_get_pages(bo);
		if (err)
			goto err_release;
	}

	return bo;

err_release:
	drm_gem_object_release(obj);
err_free:
	lima_resv_fini(&bo->resv);
	kfree(bo->pages);
	kfree(bo->pages_dma_addr);
	kfree(bo);
	return ERR_PTR(err ? err : -ENOMEM);
}

void lima_bo_destroy(struct lima_bo *bo)
{
	struct drm_gem_object *obj = &bo->gem;

	lima_resv_fini(&bo->resv);

	if (bo->vaddr)
		lima_bo_vunmap(bo);

	if (bo->sgt) {
		/* imported: pages/dma addresses are the exporter's */
		kfree(bo->pages);
		kfree(bo->pages_dma_addr);
	} else if (bo->pages) {
		int i, npages = obj->size >> PAGE_SHIFT;

		for (i = 0; i < npages; i++) {
			dma_unmap_page(obj->dev->dev, bo->pages_dma_addr[i],
				       PAGE_SIZE, DMA_BIDIRECTIONAL);
			put_page(bo->pages[i]);
		}
		kfree(bo->pages);
		kfree(bo->pages_dma_addr);
	}

	drm_gem_object_release(obj);
	kfree(bo);
}

void *lima_bo_vmap(struct lima_bo *bo)
{
	if (!bo->vaddr && bo->pages) {
		int npages = bo->gem.size >> PAGE_SHIFT;

		bo->vaddr = vmap(bo->pages, npages, VM_MAP,
				 pgprot_writecombine(PAGE_KERNEL));
	}
	return bo->vaddr;
}

void lima_bo_vunmap(struct lima_bo *bo)
{
	if (bo->vaddr) {
		vunmap(bo->vaddr);
		bo->vaddr = NULL;
	}
}
