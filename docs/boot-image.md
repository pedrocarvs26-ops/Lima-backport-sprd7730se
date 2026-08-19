# boot-lima-gtel3g.img - Android 7 (Nougat) boot image with lima

Built on WSL from the vendor tree gtel3g/kernel_samsung_gtel3g
(lineage-16.0 = Linux 3.10.103) with the lima backport built in:

    CONFIG_DRM=y  CONFIG_DRM_LIMA=y  # CONFIG_MALI400 is not set

and the lima GPU node (arm,mali-400 @ 0x60000000, shared IRQ SPI 39,
clk_gpu_axi/clk_gpu) replacing the vendor "sprd,mali-utgard" node in
sprd-scx35_sc7730.dtsi.

## Contents

* kernel: zImage 3.10.103 (gtel3g_defconfig + DRM/LIMA), lima built-in
* dt: dt.img from dtbToolCM over all sprd-scx35 board dtbs (rev00..rev11
  gtel3g + gtelwifi etc.; the bootloader picks by sprd,sc-id tag)
* ramdisk: from the e-0.21-n-20250403-UNOFFICIAL-gtel3g Nougat build
  (stock LineageOS-14.1-family Android 7.1 userdebug ramdisk, unchanged)
* header: base 0x00000000, kernel 0x00008000, ramdisk 0x01000000,
  tags 0x01d88000, page 2048, cmdline "buildvariant=userdebug",
  SEANDROIDENFORCE trailer - same layout as the stock LOS/eOS image

Rebuild:

    python3 mkbootimg.py --kernel zImage --ramdisk boot.img-ramdisk.gz \
      --dt dt.img --cmdline "buildvariant=userdebug" --base 0x00000000 \
      --kernel_offset 0x00008000 --ramdisk_offset 0x01000000 \
      --second_offset 0x00f00000 --tags_offset 0x01d88000 \
      --pagesize 2048 -o boot-lima-gtel3g.img
    echo "SEANDROIDENFORCE" >> boot-lima-gtel3g.img

(mkbootimg.py/unpackbootimg.py: LineageOS cm-14.1 system/core/mkbootimg;
dtbToolCM: gtel3g android_device_samsung_scx35-common dtbtool/)

## Flash

* Odin/Heimdall: `tar cf boot-lima.tar boot-lima-gtel3g.img` and flash
  the tar in Download mode (BOOT slot), or
* TWRP: Install -> Images -> select the img -> flash to Boot.

Keep a copy of your previous boot image so you can go back.

## First-boot checklist (lima bring-up)

    adb shell dmesg | grep -i lima        # "power domain on", clk rates,
                                          # "Initialized lima 1.0.0"
    adb shell ls /dev/dri/                # card0
    adb push lima_smoke /data/local/tmp/ && \
      adb shell /data/local/tmp/lima_smoke
    # expect: gpu_id=MALI400 num_pp=2, nonzero gp/pp versions

If the GPU never comes up: check /proc/interrupts for the six handlers
on SPI 39 and the PD_GPU_TOP status prints from lima_platform_sprd.c.
For wedged-job resets during bring-up, append `lima.sched_timeout_ms=5000`
to the dts chosen/bootargs (or the image cmdline) and repack.

Note: the lima UAPI is header-stable since v5.2, so Mesa >= 19.1 (pmOS)
or a Mesa-for-Android + gbm_gralloc build is the userspace side - see
docs/gap-analysis.md sections 4-5. Explicit sync is -EINVAL in this
backport (3.10 has no syncobj/sync_file) - SurfaceFlinger acquire-fence
support is the known phase-2 item.
