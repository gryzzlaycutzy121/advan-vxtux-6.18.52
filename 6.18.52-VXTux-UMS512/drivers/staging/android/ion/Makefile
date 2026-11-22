# SPDX-License-Identifier: GPL-2.0
# Port VXTux 6.18: ION Memory Manager di-backport dari tree OEM 5.4
# (drivers/staging/android/ion). ION dihapus dari mainline 6.x; ABI /dev/ion
# dipertahankan untuk kompatibilitas userspace Android (gralloc/Trusty ion).
obj-$(CONFIG_ION)	+= ion.o ion_buffer.o ion_dma_buf.o ion_heap.o
CFLAGS_ion_buffer.o 	= -I$(src)
obj-y			+= heaps/
