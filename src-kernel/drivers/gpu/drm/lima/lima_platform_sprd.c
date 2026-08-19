// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * lima_platform_sprd.c - Spreadtrum SC7730SE (scx30g2) platform glue for
 * the 3.10 lima backport: GPU power domain on/off.
 *
 * Real sequence, ported from the vendor mali400 driver's platform file
 * (gtel3g/kernel_samsung_gtel3g @ lineage-16.0,
 * drivers/gpu/mali400/r4p1/platform/sc8830/mali_platform.c):
 *
 *   on:  program power-on delays in REG_PMU_APB_PD_GPU_TOP_CFG
 *        (PWR_ON_DLY/SEQ_DLY/ISO_ON_DLY), clear
 *        BIT_PD_GPU_TOP_FORCE_SHUTDOWN, udelay(100), then poll
 *        REG_PMU_APB_PWR_STATUS0_DBG until BITS_PD_GPU_TOP_STATE()==0.
 *   off: set BIT_PD_GPU_TOP_FORCE_SHUTDOWN.
 *
 * The register macros and the sci_glb_* helpers come from the vendor
 * tree's mach headers, included exactly the way the vendor mali
 * platform file includes them for 32-bit builds (<mach/hardware.h>,
 * <mach/sci.h>, <mach/sci_glb_regs.h>; the PD_* register macros reach
 * us through the mach-sc chip_x30g register headers pulled in by
 * sci_glb_regs.h).
 *
 * Ordering vs clocks (lima_device.c calls this BEFORE lima_clk_init):
 * the clk_gpu select/div register (0x60100004) sits inside the GPU's
 * own power domain, so the domain must be on before the clk framework
 * touches it; the clk_gpu_axi gate lives in AON space and is safe
 * anytime. Matches the vendor driver's effective order (sources/PLLs ->
 * domain on -> AXI gate -> core select/div), with the clk framework
 * enabling PLL parents before the core clock.
 *
 * lima does not runtime-PM the GPU (neither did upstream v5.2): power_on
 * runs at probe, power_off at remove.
 */

#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/jiffies.h>

#include <mach/hardware.h>
#include <mach/sci.h>
#include <mach/sci_glb_regs.h>

#include "lima_compat.h"
#include "lima_device.h"
#include "lima_platform.h"

#define SPRD_GPU_PD_TIMEOUT_JIFFIES	msecs_to_jiffies(2000)

/*
 * Triple-read the domain status until stable (the PMU status register
 * is sampled asynchronously); mirrors the vendor sprd_gpu_domain_state().
 */
static u32 sprd_gpu_domain_state(void)
{
	u32 s1, s2, s3;
	unsigned long timeout = jiffies + SPRD_GPU_PD_TIMEOUT_JIFFIES;

	do {
		cpu_relax();
		s1 = sci_glb_read(REG_PMU_APB_PWR_STATUS0_DBG,
				  BITS_PD_GPU_TOP_STATE(-1));
		s2 = sci_glb_read(REG_PMU_APB_PWR_STATUS0_DBG,
				  BITS_PD_GPU_TOP_STATE(-1));
		s3 = sci_glb_read(REG_PMU_APB_PWR_STATUS0_DBG,
				  BITS_PD_GPU_TOP_STATE(-1));
		if (time_after(jiffies, timeout))
			pr_emerg("lima: gpu domain status stuck, state %08x eb0 %08x\n",
				 sci_glb_read(REG_PMU_APB_PWR_STATUS0_DBG, -1),
				 sci_glb_read(REG_AON_APB_APB_EB0, -1));
	} while (s1 != s2 || s2 != s3);

	return s1;
}

/* mirrors the vendor sprd_gpu_domain_wait_for_ready() */
static int sprd_gpu_domain_wait_for_ready(void)
{
	int timeout_count = 2000;

	while (sprd_gpu_domain_state() != BITS_PD_GPU_TOP_STATE(0)) {
		if (!timeout_count--) {
			pr_emerg("lima: gpu domain not ready, state %08x eb0 %08x\n",
				 sci_glb_read(REG_PMU_APB_PWR_STATUS0_DBG, -1),
				 sci_glb_read(REG_AON_APB_APB_EB0, -1));
			return -ETIMEDOUT;
		}
		udelay(50);
	}
	return 0;
}

static int sprd_gpu_power_on_builtin(struct lima_device *ldev)
{
	struct device *dev = ldev->dev;
	int ret;

	/* power-on delays (vendor values) */
	sci_glb_write(REG_PMU_APB_PD_GPU_TOP_CFG,
		      BITS_PD_GPU_TOP_PWR_ON_DLY(1), 0xff0000);
	sci_glb_write(REG_PMU_APB_PD_GPU_TOP_CFG,
		      BITS_PD_GPU_TOP_PWR_ON_SEQ_DLY(1), 0xff00);
	sci_glb_write(REG_PMU_APB_PD_GPU_TOP_CFG,
		      BITS_PD_GPU_TOP_ISO_ON_DLY(1), 0xff);

	/* de-assert the power switch */
	sci_glb_clr(REG_PMU_APB_PD_GPU_TOP_CFG, BIT_PD_GPU_TOP_FORCE_SHUTDOWN);
	udelay(100);

	ret = sprd_gpu_domain_wait_for_ready();
	if (ret) {
		dev_err(dev, "GPU power domain did not come up\n");
		return ret;
	}

	dev_info(dev, "sprd gpu: power domain on (PD_GPU_TOP ok)\n");
	return 0;
}

static void sprd_gpu_power_off_builtin(struct lima_device *ldev)
{
	/* vendor shutdown path: assert the power switch. lima keeps the
	 * domain on while bound; this runs on driver remove only. */
	sci_glb_set(REG_PMU_APB_PD_GPU_TOP_CFG, BIT_PD_GPU_TOP_FORCE_SHUTDOWN);
}

int lima_platform_power_on(struct lima_device *ldev)
{
	struct lima_platform_data *pdata = ldev->pdata;

	if (pdata && pdata->power_on)
		return pdata->power_on(ldev->dev);

	return sprd_gpu_power_on_builtin(ldev);
}

void lima_platform_power_off(struct lima_device *ldev)
{
	struct lima_platform_data *pdata = ldev->pdata;

	if (pdata && pdata->power_off) {
		pdata->power_off(ldev->dev);
		return;
	}

	sprd_gpu_power_off_builtin(ldev);
}
