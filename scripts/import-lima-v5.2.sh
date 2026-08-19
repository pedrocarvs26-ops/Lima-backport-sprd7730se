#!/bin/sh
# Fetch pristine upstream sources for the lima 3.10 backport.
#  - drivers/gpu/drm/lima/ @ v5.2 (torvalds/linux) - the driver being backported
#  - include/uapi/drm/lima_drm.h @ v5.2           - UAPI (unchanged through v6.x)
#  - drivers/gpu/drm/{Kconfig,Makefile} @ v3.10.103 (gregkh/linux) - patch 0004 context base
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REF="$HERE/reference/v5.2"
mkdir -p "$REF/lima" "$REF/uapi/drm" "$REF/v3.10.103-drm"
curl -sfL "https://api.github.com/repos/torvalds/linux/contents/drivers/gpu/drm/lima?ref=v5.2" \
  | grep -o '"name": *"[^"]*"' | cut -d'"' -f4 > /tmp/limafiles.txt
echo "lima files at v5.2: $(wc -l < /tmp/limafiles.txt)"
cat /tmp/limafiles.txt
while read -r f; do
	curl -sfL --retry 3 "https://raw.githubusercontent.com/torvalds/linux/v5.2/drivers/gpu/drm/lima/$f" -o "$REF/lima/$f" || echo "FAIL $f"
done < /tmp/limafiles.txt
curl -sfL --retry 3 "https://raw.githubusercontent.com/torvalds/linux/v5.2/include/uapi/drm/lima_drm.h" -o "$REF/uapi/drm/lima_drm.h"
for f in Kconfig Makefile; do
	curl -sfL --retry 3 "https://raw.githubusercontent.com/gregkh/linux/v3.10.103/drivers/gpu/drm/$f" -o "$REF/v3.10.103-drm/$f" || echo "FAIL 3.10 $f"
done
echo IMPORT_OK
