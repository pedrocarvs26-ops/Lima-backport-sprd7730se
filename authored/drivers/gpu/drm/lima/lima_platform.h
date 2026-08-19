/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * lima_platform.h - board/SoC glue interface for the 3.10 backport.
 *
 * lima_device_init() calls lima_platform_power_on() before touching any
 * GPU register (and before the DT clocks, whose control registers on
 * scx30g2 partly live inside the GPU's own power domain), and
 * lima_platform_power_off() at fini. The default implementation for
 * SC7730SE lives in lima_platform_sprd.c (real sequence ported from the
 * vendor mali400 platform code); board platform_data can override it.
 */
#ifndef __LIMA_PLATFORM_H__
#define __LIMA_PLATFORM_H__

#include <linux/types.h>

struct device;
struct lima_device;

struct lima_platform_data {
	u32 gpu_id;		/* enum lima_gpu_id */
	int (*power_on)(struct device *dev);
	void (*power_off)(struct device *dev);
};

int lima_platform_power_on(struct lima_device *ldev);
void lima_platform_power_off(struct lima_device *ldev);

#endif
