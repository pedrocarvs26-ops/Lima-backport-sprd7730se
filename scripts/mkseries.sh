#!/bin/sh
# mkseries.sh - assemble src-kernel/ and export the format-patch series.
#
# Steps: copy upstream v5.2 lima verbatim -> overlay authored/ -> strip
# <linux/iopoll.h> from + force-include lima_compat.h into the kept
# upstream .c files -> verification sweep (no upstream-only includes/APIs
# may survive) -> build a 6-commit series in /tmp (git cannot run on
# DrvFS) -> export to patches/.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REF="$HERE/reference/v5.2"
AUTH="$HERE/authored"
SRC="$HERE/src-kernel"
REPO=/tmp/lima-series-repo
OUT="$REPO/patches"

rm -rf "$SRC"
mkdir -p "$SRC/drivers/gpu/drm/lima" "$SRC/include/uapi/drm"
cp -r "$REF/lima/." "$SRC/drivers/gpu/drm/lima/"
cp "$REF/uapi/drm/lima_drm.h" "$SRC/include/uapi/drm/"
cp -rf "$AUTH/." "$SRC/"
L="$SRC/drivers/gpu/drm/lima"

KEPT="lima_gp.c lima_pp.c lima_mmu.c lima_pmu.c lima_l2_cache.c lima_vm.c lima_dlbu.c lima_bcast.c"
for f in $KEPT; do
	[ -f "$L/$f" ] || { echo "missing kept file $f"; exit 1; }
	sed -i '/^#include <linux.iopoll.h>$/d' "$L/$f"
	sed -i '0,/^#include/s//#include "lima_compat.h"\n\n&/' "$L/$f"
done

# kept-file fixups (one-line 3.10 adaptations):
# - 3.10 drm_mm_insert_node() takes (mm, node, size, alignment, color);
#   v5.2's call site is 3-arg -> route through drm_mm_insert_node_generic
#   (5-arg, verified in the vendor tree's include/drm/drm_mm.h)
sed -i 's/drm_mm_insert_node(&vm->mm, &bo_va->node, bo->gem.size)/drm_mm_insert_node_generic(\&vm->mm, \&bo_va->node, bo->gem.size, 0, 0)/' "$L/lima_vm.c"
grep -q 'drm_mm_insert_node_generic(&vm->mm' "$L/lima_vm.c" || { echo "lima_vm.c drm_mm fixup failed"; exit 1; }

bad=0
for f in "$L"/*.c "$L"/*.h; do
	b="$(basename "$f")"
	case "$b" in lima_compat*|lima_sched.c|lima_sched.h) continue;; esac
	grep -Hn '^#include <\(drm/gpu_scheduler\|drm/drm_syncobj\|linux/dma-fence\|linux/reservation\|linux/sync_file\|linux/xarray\|linux/iopoll\)' "$f" && bad=1
	grep -Hn 'drm_sched_\|drm_syncobj\|reservation_object\|vmf_insert_mixed\|drm_gem_get_pages\|drm_gem_put_pages\|sync_file\|xa_alloc\|xarray' "$f" | grep -v '^[^:]*:[0-9]*:\s*[\*\/]' && bad=1
done
[ "$bad" -eq 0 ] || { echo "SWEEP FAILED"; exit 1; }
echo "sweep ok"

rm -rf "$REPO"; mkdir -p "$REPO"; cd "$REPO"
git init -q
git config user.email "backport@lima.local"
git config user.name "lima 3.10 backport"
git config commit.gpgsign false
mkdir -p drivers/gpu/drm/lima include/uapi/drm arch/arm/boot/dts Documentation/arm/sprd tools/lima

# base (unexported) commit: vanilla 3.10.103 drm kbuild context for 0004
cp "$REF/v3.10.103-drm/Kconfig" drivers/gpu/drm/Kconfig
cp "$REF/v3.10.103-drm/Makefile" drivers/gpu/drm/Makefile
git add -A
git commit -q -m "base: vanilla v3.10.103 drivers/gpu/drm kbuild context"
BASE=$(git rev-parse HEAD)

cp "$SRC/include/uapi/drm/lima_drm.h" include/uapi/drm/
git add include/uapi/drm/lima_drm.h
git commit -q -m "drm/lima: add UAPI header verbatim from Linux v5.2

include/uapi/drm/lima_drm.h as merged upstream in v5.2. The lima UAPI is
unchanged through v6.x, so this header matches any Mesa >= 19.1 (verify
with: diff against <mesa>/include/drm-uapi/lima_drm.h)."

for f in lima_compat.h lima_compat_fence.h lima_compat_resv.h lima_compat.c; do
	cp "$L/$f" drivers/gpu/drm/lima/
done
git add drivers/gpu/drm/lima
git commit -q -m "drm/lima: 3.10 compatibility layer

lima_compat.h: shims for post-3.10 API used by the v5.2 driver:
u64_to_user_ptr, kvcalloc/kvfree, kmem_cache_create_usercopy,
readl_poll_timeout (iopoll.h is absent in 3.10), dma_alloc_wc,
drm_gem_object get/put renames, 3.10 drm_gem_object_lookup signature,
lima_timeout_abs_to_jiffies, LIMA_GFP_DMA32.

lima_compat_fence.h/.c: mini dma_fence (3.10 predates the fence/
reservation framework): signalable completion object, waitqueue based,
IRQ-safe slab release, wait returns fence errors.

lima_compat_resv.h: mini reservation object embedded in struct lima_bo
(mutex + exclusive fence + growable shared array) for implicit sync."

cp -r "$L/." drivers/gpu/drm/lima/
git add drivers/gpu/drm/lima
git commit -q -m "drm/lima: backport v5.2 lima driver to Linux 3.10 (SC7730SE)

Kept verbatim from v5.2 (hardware engines): lima_gp.c, lima_pp.c,
lima_mmu.c, lima_pmu.c, lima_l2_cache.c, lima_vm.c, lima_dlbu.c,
lima_bcast.c + headers (lima_compat.h is force-included into them;
iopoll.h includes stripped).

Rewritten for 3.10:
- lima_sched.c/h: drm_sched does not exist in 3.10 -> strict FIFO per
  pipe on an ordered workqueue, same external interface. Hang timeout
  via sched_timeout_ms module param (0 = off).
- lima_drv.c/h: 3.10 drm_driver layout (.load/.unload via
  drm_platform_init, gem_open/close_object, gem_vm_ops, 3.10 prime
  hooks); ioctls DRM_UNLOCKED, no DRM_AUTH/render nodes (3.10 has none).
- lima_device.c/h: IP descriptor table (offsets match the vendor
  sprd-scx35_sc7730.dtsi gpu node); tolerant-optional DT clocks
  (bus=clk_gpu_axi, core=clk_gpu); regulator optional; SoC power domain
  through lima_platform_power_on/off BEFORE clock init (the GPU core
  clock register lives inside the GPU power domain on scx30g2).
- lima_platform_sprd.c: SC7730SE power-domain glue - real sequence
  ported from the vendor mali400 platform code (PD_GPU_TOP force
  shutdown + status poll), using the vendor tree's own sci_glb helpers.
- lima_gem.c / lima_object.c/h: 3.10 GEM (map_list hash mmap offsets,
  eager pages via read_mapping_page + dma_map_page, 3.10 fault
  signature, no ww lock_bos); implicit sync via mini reservation
  object; explicit sync (in_sync/out_sync/EXPLICIT_FENCE) rejected with
  -EINVAL - no syncobj/sync_file on 3.10.
- lima_ctx.c/h: idr instead of xarray.
- lima_gem_prime.c/h: 3.10 gem_prime_import_sg_table signature.
- Kconfig/Makefile: no DRM_SCHED; depends on !MALI400 (conflicts with
  the vendor closed driver)."

echo 'obj-$(CONFIG_DRM_LIMA) += lima/' >> drivers/gpu/drm/Makefile
echo 'source "drivers/gpu/drm/lima/Kconfig"' >> drivers/gpu/drm/Kconfig
git commit -q -am "drm: hook lima into drivers/gpu/drm Kconfig/Makefile

Two lines against the vanilla 3.10.103 files. If your vendor tree
diverged there, apply by hand (add the obj-\$(CONFIG_DRM_LIMA) line to
drivers/gpu/drm/Makefile and source drivers/gpu/drm/lima/Kconfig from
drivers/gpu/drm/Kconfig)."

cp "$SRC/arch/arm/boot/dts/scx30g2-lima-gpu.dtsi" arch/arm/boot/dts/
cp "$SRC/Documentation/arm/sprd/lima-sc7730se.txt" Documentation/arm/sprd/
git add arch/arm/boot/dts Documentation/arm/sprd
git commit -q -m "arm: scx30g2 lima GPU DT fragment + board-file notes

arch/arm/boot/dts/scx30g2-lima-gpu.dtsi carries the verified
vendor-tree values (gtel3g/kernel_samsung_gtel3g, lineage-16.0):
reg 0x60000000/0x10000, the shared GIC SPI 39 interrupt listed once per
lima irq name, and the clk_gpu_axi/clk_gpu clocks as bus/core. Replace
the vendor \"sprd,mali-utgard\" node in sprd-scx35_sc7730.dtsi with it.
Documentation/arm/sprd/lima-sc7730se.txt has the platform_device
fallback for non-DT kernels."

cp -r "$SRC/tools/lima/." tools/lima/
git add tools/lima
git commit -q -m "tools: lima_smoke - minimal lima UAPI ioctl harness

VERSION + GET_PARAM x4 + CTX_CREATE + GEM_CREATE + GEM_INFO + mmap/CPU
write + GEM_WAIT + cleanup, no Mesa needed. First thing to run on the
device (see README bring-up order)."

mkdir -p "$OUT"
git format-patch -o "$OUT" "$BASE"..HEAD
rm -rf "$HERE/patches"; mkdir -p "$HERE/patches"
cp -r "$OUT/." "$HERE/patches/"
echo "== series =="
ls -la "$HERE/patches"
wc -l "$HERE"/patches/*.patch | tail -1
