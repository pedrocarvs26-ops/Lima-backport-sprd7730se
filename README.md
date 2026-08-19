# lima 3.10 backport for SC7730SE (scx30g2, Mali-400 MP2)

Backport of the upstream **v5.2** `drivers/gpu/drm/lima/` DRM driver to
the Spreadtrum vendor kernel **3.10.103**, verified against

    gtel3g/kernel_samsung_gtel3g @ lineage-16.0      (Linux 3.10.103)
    https://github.com/gtel3g/kernel_samsung_gtel3g
    defconfig: arch/arm/configs/gtel3g_defconfig
    board dts: arch/arm/boot/dts/sprd-scx35_gtel3g_revNN.dts
               (-> sprd-scx35.dtsi + sprd-scx35_sc7730.dtsi)

(branch los-14.1 = 3.10.17 works the same; the lineage-14.1 manifest pins
it. Render-only driver; display stays with the vendor fbdev (sprdfb).)

Docs: `docs/gap-analysis.md` (plan + 5.x->3.10 API delta + risks),
`docs/vendor-tree-gtel3g.md` (mined hardware facts with provenance).

## Layout

    patches/       the git format-patch series (apply these)
    src-kernel/    assembled final tree (what the patches add)
    authored/      hand-written 3.10 files (edit THESE, then regenerate)
    reference/     pristine upstream sources (v5.2 lima, 3.10.103 drm)
    scripts/       import-lima-v5.2.sh (fetch), mkseries.sh (assemble)
    integration/   defconfig.fragment, copy of the dtsi
    docs/

## Quickstart (vendor tree)

1. Apply the series:

       cd kernel_samsung_gtel3g            # checkout lineage-16.0
       git am /path/to/patches/000*.patch  # or: patch -p1 < ...

   Patch 0004 (drm Kbuild hook) is a context diff vs vanilla 3.10.103 -
   apply by hand if the vendor file differs (two lines, see its message).

2. Replace the GPU node: in `arch/arm/boot/dts/sprd-scx35_sc7730.dtsi`
   replace the `gpu:gpu { compatible = "sprd,mali-utgard"; ... }` node
   with the lima node from `arch/arm/boot/dts/scx30g2-lima-gpu.dtsi`
   (values already match the vendor node: reg 0x60000000, shared irq
   SPI 39 x6, clocks clk_gpu_axi+clk_gpu). For pmOS, `#include` the dtsi.

3. Power glue: `drivers/gpu/drm/lima/lima_platform_sprd.c` already has
   the real SC7730SE sequence (PD_GPU_TOP force-shutdown clear + status
   poll) using the vendor tree's own sci_glb helpers - nothing to fill.

4. Config (see `integration/defconfig.fragment`): in gtel3g_defconfig

       # CONFIG_MALI400 is not set
       CONFIG_DRM=y
       CONFIG_DRM_LIMA=m
       (# CONFIG_MODVERSIONS is not set  - recommended while iterating)

   Keep CONFIG_ION/CONFIG_ION_SPRD/CONFIG_SPRD_IOMMU as they are (ION is
   still used by Android; the SPRD IOMMU is not on the GPU path).

5. Build with a 3.10-era toolchain (AOSP arm-linux-androideabi-4.9 from
   the nougat-mr2 prebuilts is what these trees were built with; pmOS
   carries modern-gcc patches for 3.10):

       make ARCH=arm CROSS_COMPILE=arm-linux-androideabi- gtel3g_defconfig
       make ARCH=arm CROSS_COMPILE=arm-linux-androideabi- -j$(nproc) \
            zImage dtbs modules

6. Flash kernel+dtb. The vendor mali driver was built-in; with
   CONFIG_MALI400 unset it is simply gone - nothing to blacklist.

## Load order and first test

    modprobe lima
    dmesg | grep -i lima          # "power domain on", clk rates,
                                  # "Initialized lima 1.0.0 20190217 ..."
    ls /dev/dri/                  # cardN (no udev: mknod /dev/dri/card0 c 226 0)
    grep -i mali /proc/interrupts # the shared SPI 39 handlers (x6)

    # smoke test (host: make -C tools/lima KDIR=... CROSS_COMPILE=...)
    ./lima_smoke /dev/dri/card0   # VERSION/GET_PARAM/CTX/GEM/mmap/WAIT

GET_PARAM must report gpu_id=MALI400, num_pp=2, nonzero gp/pp versions;
garbage there = power domain off or wrong register base.

Then Mesa (>= 19.1; header drift check:
`diff include/uapi/drm/lima_drm.h <mesa>/include/drm-uapi/lima_drm.h`):

    EGL_PLATFORM=surfaceless eglinfo -B     # pmOS: offscreen GL check
    # piglit / dEQP-GLES2 (surfaceless)     # no KMS display on this tree

Android (LineageOS for this device): Mesa 19.x + GBM + gbm_gralloc +
vendor hwc (private-handle compat) - see docs/gap-analysis.md section 5,
incl. the phase-2 sync_file note for SurfaceFlinger acquire fences.

## Bring-up knobs / debugging

* `modprobe lima sched_timeout_ms=5000` - reset wedged jobs instead of
  hanging (default 0 = never; set it for bring-up).
* `echo 0xff > /sys/module/drm/parameters/debug` - DRM core debug.
* `/proc/interrupts` - the shared mali IRQ count must increase on jobs.
* dmesg: MMU page faults print address + pipe (from lima_mmu.c).
* `strace -e ioctl ./lima_smoke ...` - UAPI-level tracing.

## Known limitations of the backport (vs upstream v5.2)

* No DRM render node (3.10 predates them): primary /dev/dri/cardN only;
  ioctls run DRM_UNLOCKED without DRM_AUTH (same trust model as other
  3.10 GPU drivers).
* Explicit sync rejected: submits with in_sync[]/out_sync or
  LIMA_SUBMIT_FLAG_EXPLICIT_FENCE return -EINVAL (no syncobj/sync_file
  on 3.10). Implicit sync (BO reservation fences) works - which is what
  Mesa 19.x uses.
* dma-buf mmap of an exported BO returns -EINVAL (3.10 prime core); CPU
  mapping goes through the DRM fd + GEM_INFO offset (what Mesa uses).
* FIFO scheduler per pipe, single priority, no preemption; GPU stays
  powered while the driver is bound (no runtime-PM, as upstream v5.2).
* Shared GPU IRQ (SPI 39) works as-is: lima requests each IP irq with
  IRQF_SHARED and handlers early-out when their block is idle.

## Regenerating the series

    scripts/import-lima-v5.2.sh   # fetch pristine v5.2 + 3.10.103 sources
    # edit files under authored/
    scripts/mkseries.sh           # reassemble src-kernel/ + export patches/

mkseries.sh fails its own verification sweep if any upstream-only
include/API leaks into the assembled tree.
