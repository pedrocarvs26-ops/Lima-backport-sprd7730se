# lima -> Linux 3.10.103 (SC7730SE) backport: gap analysis

**UPDATE:** vendor tree identified and mined: `gtel3g/kernel_samsung_gtel3g`
@ `lineage-16.0` (Linux 3.10.103). All hardware values are verified against
it - see `docs/vendor-tree-gtel3g.md`; the DT fragment, platform glue and
defconfig fragment in this repo carry the real values.

## 1. Baseline and source selection

* Driver source: `drivers/gpu/drm/lima/` **at Linux v5.2** (the merge
  release, commit a1d2a6339961). v5.2 is the sweet spot for a 3.10 target:
  it already has the final UAPI (include/uapi/drm/lima_drm.h, unchanged
  through v6.x) but does NOT yet use pm_runtime, iommu_domain,
  drm_gem_cma_helper, or tracepoints (all of which would add 3.10
  breakage for zero benefit on this SoC).
* Userspace: Mesa **>= 19.1** (lima gallium merged April 2019). UAPI
  drift check: `diff include/uapi/drm/lima_drm.h
  <mesa>/include/drm-uapi/lima_drm.h` - expected empty.
* The vendor kernel ships ARM's closed utgard DDK (`drivers/gpu/mali400`,
  r4p1, built-in via CONFIG_MALI400=y). It drives the same hardware, so
  it must be Kconfig'd out (`# CONFIG_MALI400 is not set`); lima's
  Kconfig carries `depends on !MALI400` to make the conflict explicit.
  lima does not reuse any of its code at runtime - only its platform
  file informed the power sequence (docs/vendor-tree-gtel3g.md).

## 2. Module-by-module API delta, v5.2 -> 3.10.103

### 2.1 DRM core
* 3.10 `struct drm_driver`: has `.load/.unload` (platform via
  `drm_platform_init`/`drm_platform_exit`), `.open/.postclose`,
  `gem_free_object`/`gem_open_object`/`gem_close_object`, `gem_vm_ops`,
  `gem_prime_import_sg_table(dev, size_t, sgt)` (no attach param),
  `gem_prime_get_sg_table`, `const struct file_operations *fops`,
  `struct drm_ioctl_desc *ioctls`. No `gem_prime_mmap`, no
  `gem_prime_res_obj`, no `dumb_*` needed.
* No render nodes (3.10 is legacy-primary-only): ioctls run
  `DRM_UNLOCKED` and without `DRM_AUTH`; `/dev/dri/cardN` only. Same
  trust model as other 3.10 GPU drivers; noted in README limitations.
* GEM refs: `drm_gem_object_reference/unreference` (3.10) <-
  `drm_gem_object_get/put` (5.x) - macro rename in lima_compat.h.
* `drm_gem_object_lookup(dev, filp, handle)` - 3.10 keeps the dev arg
  (lima_compat.h: `lima_gem_object_lookup`).
* mmap offsets: 3.10 GEM uses the `obj->map_list.hash.key <<
  PAGE_SHIFT` fake-offset hash (no drm_vma_offset_manager). The offset
  is created once at handle create (3.10's drm_gem_create_mmap_offset
  is not safely idempotent). `drm_gem_mmap` itself sets
  driver->gem_vm_ops + VM_IO|VM_PFNMAP|VM_DONTEXPAND|VM_DONTDUMP +
  writecombine pgprot - exactly what lima needs (WC userspace mapping =
  no explicit cache flush before GPU reads, same assumption as
  upstream).
* No `drm_gem_get_pages/put_pages`: BO pages via `read_mapping_page` on
  the GEM shmem filp + per-page `dma_map_page(DMA_BIDIRECTIONAL)`;
  eager allocation at create (lima_object.c).
* 3.10 fault handler signature `(vma, vmf)` with `vmf->virtual_address`;
  pages eager, so fault = bounds check + `vm_insert_page`.
* Headers: 3.10 has no `drm/drm_device.h`, `drm/drm_file.h`,
  `drm/drm_gem.h`, `drm/drm_prime.h` - everything is `drm/drmP.h`.

### 2.2 Scheduler (drm_sched -> FIFO workqueue)
* 3.10 has no `drm/gpu_scheduler.h`. lima_sched.c/h are rewritten:
  strict in-order FIFO per pipe on an `alloc_ordered_workqueue`, same
  external interface (task_init/fini, context_init/fini,
  context_queue_task -> fence, pipe_init/fini, pipe_task_done,
  pipe_mmu_error, slab_init/fini). run/done/timeout work items are
  serialized by the ordered wq; completion arrives from the gp/pp irq
  handlers via task_done -> done_work (process context; GEM puts are
  not irq-safe). Hang recovery: `sched_timeout_ms` module param
  (default 0 = off; set 5000 for bring-up) -> delayed work forces an
  error + the pipe's task_error reset. context_fini drains with a 5s
  cap + forced reset so file close can't hang.
* Single priority, no preemption - fine for Mesa's lima usage.

### 2.3 Fences / reservation (lima_compat_fence.h, lima_compat_resv.h)
* 3.10 predates dma-fence/reservation_object (3.17+). Mini dma_fence:
  signalable completion object, status 0/1/<0, waitqueue, kmem_cache
  release so `dma_fence_put` is IRQ-safe, wait propagates the error.
* Mini reservation object embedded in `struct lima_bo` (mutex +
  exclusive fence + growable shared array; snapshot/wait helpers).
  Submit collects dep fences from all BOs; the task fence becomes each
  BO's exclusive fence at submit - implicit sync, which is what Mesa
  19.x uses.
* Explicit sync is absent on 3.10 (no syncobj/sync_file): submits with
  `in_sync[]`/`out_sync`/`LIMA_SUBMIT_FLAG_EXPLICIT_FENCE` get
  `-EINVAL`. Phase-2 option for Android SF fences: sync_file bridge.

### 2.4 MMU / IOMMU
* lima drives the Mali's internal MMU (gpmmu/ppmmu blocks); no
  iommu_domain needed. Verified the vendor mali driver does not use the
  SPRD IOMMU for the GPU either (no iommu refs in its platform code) -
  CONFIG_SPRD_IOMMU serves camera/video and stays untouched.

### 2.5 Clocks / regulator / power
* 3.10 has clk/regulator frameworks; lima uses devm_clk_get
  "bus"/"core" mapped to the DT clock nodes `clk_gpu_axi`/`clk_gpu`
  (registered by the vendor sprd clock driver from scx30g2-clocks.dtsi).
  Tolerant-optional: without DT clocks, warn and continue.
* `devm_regulator_get_optional` is 3.13+; emulated with
  `devm_regulator_get` + treating -ENODEV/-EPROBE_DEFER/-ENOENT as
  "absent" (the GPU rail is handled by the power-domain sequence).
* No reset framework use (3.10 <linux/reset.h> absent; the SoC glue
  owns it).
* ORDERING (verified against the vendor driver): the GPU core-clock
  control register (0x60100004) lives INSIDE the GPU power domain, so
  lima_device_init runs lima_platform_power_on (PD_GPU_TOP
  force-shutdown clear + status poll, real sequence ported from
  drivers/gpu/mali400/r4p1/platform/sc8830/mali_platform.c) BEFORE
  clk_prepare_enable; fini disables clocks before asserting shutdown.
  The AXI gate (clk_gpu_axi) is in AON space, safe anytime.
* No runtime-PM (upstream v5.2 had none either); GPU stays powered
  while bound.

### 2.6 Small shims (lima_compat.h)
u64_to_user_ptr, kvcalloc/kvfree, kmem_cache_create_usercopy (4.16+,
used by the kept gp/pp.c task slabs), readl_poll_timeout (no
<linux/iopoll.h> in 3.10; includes stripped from kept files at series
assembly), dma_alloc_wc/dma_free_wc via dma_alloc_attrs,
lima_timeout_abs_to_jiffies (replaces drm_timeout_abs_to_jiffies),
LIMA_GFP_DMA32 (= GFP_KERNEL|__GFP_ZERO; no ZONE_DMA32 on 3.10 arm, no
>4G constraint on this SoC), xarray -> idr (lima_ctx.c), 3.10
`gem_prime_import_sg_table` signature (lima_gem_prime.c).

### 2.7 dma-buf / PRIME on 3.10
* DRIVER_PRIME + gem_prime_get_sg_table/import_sg_table work (dma-buf
  share with ION possible). 3.10 dma-buf has no ->mmap for exported
  BOs; CPU mapping via the DRM fd + GEM_INFO offset (Mesa's path).

## 3. Integration
* DT fragment `arch/arm/boot/dts/scx30g2-lima-gpu.dtsi` with verified
  values (reg 0x60000000/0x10000; shared GIC SPI 39 listed per lima irq
  name - lima requests IRQF_SHARED and early-outs, so the shared line
  needs no driver change; clocks bus=clk_gpu_axi core=clk_gpu). On the
  vendor tree: REPLACE the `sprd,mali-utgard` node in
  sprd-scx35_sc7730.dtsi. pmOS: #include the fragment.
* platform_data fallback documented in
  Documentation/arm/sprd/lima-sc7730se.txt for non-DT kernels.
* Kbuild hook = patch 0004 (two lines; vanilla 3.10.103 context).
* Vendor mali disable = config only (MALI400 off), Kconfig guard
  enforces mutual exclusion.

## 4. Userspace
* pmOS: mainline Mesa (>= 19.1) lima gallium + GBM; surfaceless first
  (no KMS display on this vendor tree): `EGL_PLATFORM=surfaceless
  eglinfo -B`, then piglit/dEQP-GLES2.
* Android (LineageOS 14.1/16.0 for gtel3g): Mesa 19.x built for
  Android, GBM + gbm_gralloc replacing vendor gralloc.sc7730se.so,
  vendor hwcomposer via private-handle compat; pre-Treble so HAL
  replacement is a straight vendor.img swap. SurfaceFlinger acquire
  fences are the known gap (see 2.3 phase-2 note).

## 5. Bring-up / test sequence
1. probe: dmesg power-domain note, clk rates, "Initialized lima 1.0.0";
   /dev/dri/card0 appears; /proc/interrupts shows 6 handlers on SPI 39.
2. `lima_smoke /dev/dri/card0` (tools/lima): VERSION, GET_PARAM
   (gpu_id=MALI400, num_pp=2, nonzero versions), CTX_CREATE, GEM_CREATE
   1MB, GEM_INFO (va + mmap offset), mmap + CPU write, GEM_WAIT,
   cleanup. Garbage versions => power domain off / wrong reg base.
3. Mesa surfaceless EGL (pmOS) -> piglit/dEQP-GLES2.
4. Android HAL bridge only after 3 is green.
Debugging: `sched_timeout_ms=5000`, drm debug param, /proc/interrupts
counts, MMU fault prints, strace on ioctls.

## 6. Risk register / known limitations
* No render node / no DRM_AUTH enforcement (3.10 core) - acceptable for
  this device class; documented.
* Explicit sync rejected (-EINVAL) - the real functional gap for
  Android SF; phase-2 sync_file bridge possible.
* FIFO scheduler: no priorities/preemption; one wedged app can stall
  its pipe until the timeout resets it (set sched_timeout_ms).
* Shared IRQ: handled by lima's IRQF_SHARED + early-out handlers (this
  is upstream behavior since v5.2, just verified against the vendor
  wiring).
* dma-buf mmap of exported BOs: -EINVAL (3.10 prime core) - Mesa
  unaffected.
* Patch 0004 context is vanilla 3.10.103; vendor drift -> apply 2 lines
  by hand.
* Not build-tested against every possible config; the reference build
  config is gtel3g_defconfig + the fragment.
