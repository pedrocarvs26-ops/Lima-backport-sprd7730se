# Verified vendor-tree facts (gtel3g tree)

Source tree: `gtel3g/kernel_samsung_gtel3g`
https://github.com/gtel3g/kernel_samsung_gtel3g

* branch `lineage-16.0` = **Linux 3.10.103**  <- target tree
* branch `los-14.1`     = Linux 3.10.17 (the lineage-14.1 local manifest
  pins this one; the port is identical for both, same 3.10.y DRM core)
* device defconfig: `arch/arm/configs/gtel3g_defconfig`
* board dts: `arch/arm/boot/dts/sprd-scx35_gtel3g_revNN.dts`
  (rev00..rev11), including `sprd-scx35.dtsi` + `sprd-scx35_sc7730.dtsi`
* machine config: CONFIG_ARCH_SC=y, CONFIG_ARCH_SCX35=y,
  CONFIG_ARCH_SCX30G2=y, CONFIG_MACH_SCX35_DT=y (DT-based; mach dir
  `arch/arm/mach-sc/`)

## Vendor GPU node (sprd-scx35_sc7730.dtsi, lines ~40-70)

    gpu:gpu {
        compatible = "sprd,mali-utgard";
        mali_pp_core_number = <4>;      /* MP4 value; SC7730SE is MP2.
                                           lima ignores this property */
        reg = <0x60001000 0x200>  mali_l2
              <0x60000000 0x100>  mali_gp
              <0x60003000 0x100>  mali_gp_mmu
              <0x60008000 0x1100> mali_pp0
              <0x60004000 0x100>  mali_pp0_mmu
              <0x6000A000 0x1100> mali_pp1
              <0x60005000 0x100>  mali_pp1_mmu
              <0x60002000 0x100>  mali_pmu
        interrupts = <0 39 0x0> x6      /* ONE shared line: GIC SPI 39 */
        clock-names = "clk_gpu_axi","clk_gpu",... (parents 153m6..460m8)
        clocks = <&clk_gpu_axi>,<&clk_gpu>,...
    };

* Register base **0x60000000**; per-block offsets match lima's
  lima_ip_desc[] exactly (gp +0x0000, l2 +0x1000, pmu +0x2000, gpmmu
  +0x3000, ppmmu0/1 +0x4000/+0x5000, pp0/pp1 +0x8000/+0xa000) => one
  contiguous `reg = <0x60000000 0x10000>` window for lima.
* **Shared IRQ**: all blocks share GIC SPI 39 (vendor builds with
  CONFIG_MALI_SHARED_INTERRUPTS=y). lima v5.2 already requests every irq
  with IRQF_SHARED and gp/pp/mmu handlers early-out with IRQ_NONE when
  their block is idle, so the shared line works unmodified; the lima DT
  node lists the same interrupt once per name.
* Clocks (scx30g2-clocks.dtsi): `clk_gpu_axi` (gate, parent clk_aon_apb,
  AON-space registers 0x402e0000/0x402b0021 - safe anytime) and `clk_gpu`
  (composite mux+div at 0x60100004: 153m6/208m/256m/300m/312m/384m/460m8
  - NOTE: this register is inside the GPU power domain; touch only after
  the domain is on). lima maps bus->clk_gpu_axi, core->clk_gpu. The
  vendor mali runs the core at 312 MHz.

## Vendor mali400 driver (drivers/gpu/mali400, r4p1)

* Kconfig symbol `MALI400` (tristate; choice MALI_VER_R4P1 inside).
  Stock gtel3g_defconfig: `CONFIG_MALI400=y` (built-in), no UMP,
  MALI_DMA_BUF_MAP_ON_ATTACH=y, MALI_SHARED_INTERRUPTS=y,
  `# CONFIG_DRM is not set`, ION/ION_SPRD=y, SPRD_IOMMU=y, CMA off.
* Platform code: `drivers/gpu/mali400/r4p1/platform/sc8830/mali_platform.c`
  (+ base.h). **No iommu references**: the GPU is a direct bus master;
  SPRD_IOMMU serves camera/video blocks. lima needs no IOMMU handling.
* Power sequence (ported into lima_platform_sprd.c):
    - on:  program power-on delays in REG_PMU_APB_PD_GPU_TOP_CFG
      (BITS_PD_GPU_TOP_PWR_ON_DLY/SEQ_DLY/ISO_ON_DLY), then clear
      BIT_PD_GPU_TOP_FORCE_SHUTDOWN, udelay(100), then poll
      REG_PMU_APB_PWR_STATUS0_DBG until BITS_PD_GPU_TOP_STATE()==0
      (triple-read for stability, ~2000x50us timeout).
    - off: set BIT_PD_GPU_TOP_FORCE_SHUTDOWN.
    - clock: REG_GPU_APB_APB_CLK_CTRL BITS_CLK_GPU_SEL/DIV (vendor uses
      DT clocks via of_clk_get; lima uses the clk API instead).
  Macros/helpers come from the mach headers (`<mach/hardware.h>`,
  `<mach/sci.h>`, `<mach/sci_glb_regs.h>` -> chip_x30g register headers),
  used via `sci_glb_{read,write,set,clr}`.

## Vendor DRM core

`include/drm/drmP.h` is vanilla 3.10 (verified: DRM_UNLOCKED 0x10,
DRM_AUTH 0x1, DRIVER_GEM 0x1000/DRIVER_PRIME 0x4000, gem hooks
gem_open/close_object + gem_vm_ops + gem_prime_import_sg_table
(dev,size,sgt), drm_platform_init/exit, no separate drm_gem.h/drm_prime.h
headers - everything in drmP.h; mmap offsets via obj->map_list.hash.key
<< PAGE_SHIFT). The lima compat design applies unchanged; patch 0004's
context is vanilla 3.10.103 - hand-apply if the vendor file drifted.

## Toolchain

LineageOS built these kernels with the AOSP prebuilt
arm-linux-androideabi-4.9 (repo:
platform/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9, branch
nougat-mr2-release - default master branch is empty, use the nougat
branch). Any arm-linux-gnueabihf gcc 4.8/4.9 works; newer gcc needs the
usual 3.10 fixups (pmOS carries them).
