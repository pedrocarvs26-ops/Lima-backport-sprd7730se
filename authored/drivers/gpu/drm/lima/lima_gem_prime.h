/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* Copyright 2018-2019 Qiang Yu <yuq825@gmail.com> */
/* 3.10 backport: gem_prime_import_sg_table has the 3.10 signature
 * (no dma_buf_attachment parameter; size passed explicitly). */
#ifndef __LIMA_GEM_PRIME_H__
#define __LIMA_GEM_PRIME_H__

#include <linux/types.h>

struct drm_device;
struct drm_gem_object;
struct sg_table;

struct drm_gem_object *lima_gem_prime_import_sg_table(
	struct drm_device *dev, size_t size, struct sg_table *sgt);
struct sg_table *lima_gem_prime_get_sg_table(struct drm_gem_object *obj);

#endif
