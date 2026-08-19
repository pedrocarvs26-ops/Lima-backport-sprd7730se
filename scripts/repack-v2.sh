#!/bin/bash
# v2 images: ViperOS-matched packaging = SPRD dt section, tags 0x100,
# board "sc8830", no SEANDROIDENFORCE, ViperOS ramdisk.
W=/home/rdp/work; B=/mnt/c/Users/RDP/lima-sc7730se-backport
MB=$W/mkbootimg-src/mkbootimg.py
RAMDISK=$B/inbox/viperos-ramdisk.gz
DT_VERBATIM=$B/inbox/viperos-dt-sprd.img
DT_STOCK=$W/out/dt-sprd-stock.img
DT_LIMA=$W/out/dt-sprd-lima.img
mk() { python3 $MB --kernel "$1" --ramdisk "$RAMDISK" --dt "$2" --cmdline "buildvariant=userdebug" --board sc8830 --base 0x00000000 --kernel_offset 0x00008000 --ramdisk_offset 0x01000000 --second_offset 0x00f00000 --tags_offset 0x00000100 --pagesize 2048 -o "$3" && echo "OK $3"; }
python3 $B/scripts/sprd_dtb_pack.py $W/kernel-stock/arch/arm/boot/dts $DT_STOCK
python3 $B/scripts/sprd_dtb_pack.py $W/kernel/arch/arm/boot/dts $DT_LIMA
[ -f "$DT_VERBATIM" ] && DT_SANITY=$DT_VERBATIM || DT_SANITY=$DT_STOCK
mk $W/kernel-stock/arch/arm/boot/zImage $DT_SANITY $W/out/boot-sanity2-gtel3g.img
mk $W/kernel/arch/arm/boot/zImage $DT_LIMA $W/out/boot-lima-m2-gtel3g.img
mk $B/out/zImage $DT_LIMA $W/out/boot-lima2-gtel3g.img
cp $W/out/boot-sanity2-gtel3g.img $W/out/boot-lima-m2-gtel3g.img $W/out/boot-lima2-gtel3g.img $B/out/
