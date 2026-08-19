# out/ - build artifacts

## v2 images (current; ViperOS-matched packaging: SPRD dt table, tags 0x100,
## board sc8830, no SEANDROIDENFORCE trailer, ViperOS ramdisk)
- boot-sanity2-gtel3g.img  - stock 3.10.103 kernel (vermagic 3.10.103-fuckingkernel),
  flash FIRST to prove the base+packaging boot
- boot-lima-m2-gtel3g.img  - lima built as a module (safe: cannot hang boot);
  insmod out/lima.ko via adb to probe
- boot-lima2-gtel3g.img    - lima built-in (bounded PD poll + probe breadcrumbs)

- lima.ko                  - lima module for the m2 image (nsecs_to_jiffies shim, bounded poll)
- lima_smoke               - static ARM UAPI test binary
- sprd-scx35_gtel3g_rev11.dtb - single lima-enabled board dtb (rev11)

## v1-obsolete/ - first-round images with QCDT dt packaging + /e/OS ramdisk;
## known NOT to boot on this device. Kept for reference only.
