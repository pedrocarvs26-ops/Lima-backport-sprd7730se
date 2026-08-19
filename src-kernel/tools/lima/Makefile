# Build lima_smoke for the target. Needs the kernel UAPI headers on the
# include path (the backport installs include/uapi/drm/lima_drm.h into
# the vendor tree; DRM headers come from the tree's include/uapi/drm).
KDIR ?= ../../..
CROSS_COMPILE ?=

CC = $(CROSS_COMPILE)gcc
CFLAGS += -Wall -O2 -I$(KDIR)/include/uapi -I$(KDIR)/arch/arm/include/uapi \
	-I$(KDIR)/include -I$(KDIR)/arch/arm/include

lima_smoke: lima_smoke.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f lima_smoke
