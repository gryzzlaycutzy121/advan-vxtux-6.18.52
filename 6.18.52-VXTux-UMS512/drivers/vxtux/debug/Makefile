# SPDX-License-Identifier: GPL-2.0
# drivers/vxtux/debug/Makefile
obj-$(CONFIG_VXTUX_FLIGHTREC)	+= vxtux_flightrec.o
# Perekam tahan-reset: 8 slot di cache + fsync + verifikasi baca-dari-disk +
# record RAM yang dipanen boot berikutnya (menggantikan vxtux_flightrec).
obj-$(CONFIG_VXTUX_LOGREC)	+= vxtux_logrec.o
# Trace hook resource API + regmap (deblob compare 5.4<->6.18): ring RAM
# tahan reset di 0xAF800000 (cmdline vxtuxtrace=), kprobe resource API +
# regmap.  Lihat vxtux_hook.c untuk koreksi desain (regmap bukan mmiotrace).
obj-$(CONFIG_VXTUX_HOOK)	+= vxtux_hook.o
obj-$(CONFIG_VXTUX_BEACON)	+= vxtux_beacon.o
obj-$(CONFIG_VXTUX_LKFB)		+= vxtux_lkfb.o
obj-$(CONFIG_VXTUX_INITTRACE)	+= vxtux_inittrace.o
# BLDB (Berry Linux Debug Bridge): ring 16 MiB + /dev/bldb0.
obj-$(CONFIG_VXTUX_BLDB)	+= bldb_core.o
