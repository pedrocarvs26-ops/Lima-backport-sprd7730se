# SPDX-License-Identifier: GPL-2.0 OR MIT
# Copyright 2017-2019 Qiang Yu <yuq825@gmail.com>
# 3.10 backport: + lima_compat.o (fence/resv/shims) and
# lima_platform_sprd.o (SC7730SE power domain glue).

lima-y := \
	lima_drv.o \
	lima_device.o \
	lima_pmu.o \
	lima_l2_cache.o \
	lima_mmu.o \
	lima_gp.o \
	lima_pp.o \
	lima_gem.o \
	lima_vm.o \
	lima_sched.o \
	lima_ctx.o \
	lima_gem_prime.o \
	lima_dlbu.o \
	lima_bcast.o \
	lima_object.o \
	lima_compat.o \
	lima_platform_sprd.o

obj-$(CONFIG_DRM_LIMA) += lima.o
