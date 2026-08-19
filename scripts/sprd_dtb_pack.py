#!/usr/bin/env python3
# Pack board dtbs into the SPRD-format dt image this device's bootloader
# expects (magic "SPRD", v1, 12 fixed 0xd800 slots; reverse-engineered from
# the working ViperOS boot partition and validated byte-identical).
import struct, sys, os, re

dts_dir, out_path = sys.argv[1], sys.argv[2]
CHIP, FLAGS, SLOT, HDR = 0x227e, 0x20000, 0xd800, 0x800
dtbs = {}
for f in os.listdir(dts_dir):
    m = re.match(r'sprd-scx35_gtel3g_rev(\d+)\.dtb$', f)
    if m:
        dtbs[int(m.group(1))] = open(os.path.join(dts_dir, f), 'rb').read()
assert len(dtbs) == 12, 'expected 12 gtel3g dtbs, got %d' % len(dtbs)
recs, blobs = b'', b''
for rev in sorted(dtbs):
    blob = dtbs[rev]
    assert len(blob) <= SLOT, 'rev%d dtb too big for slot' % rev
    recs += struct.pack('<5I', CHIP, rev, FLAGS, HDR + rev*SLOT, SLOT)
    blobs += blob + b'\0' * (SLOT - len(blob))
img = b'SPRD' + struct.pack('<II', 1, 12) + recs
img += b'\0' * (HDR - len(img)) + blobs
open(out_path, 'wb').write(img)
print('wrote %s: %d bytes, 12 slots' % (out_path, len(img)))
