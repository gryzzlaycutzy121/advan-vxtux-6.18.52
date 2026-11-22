# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_ION_SYSTEM_HEAP) += ion_sys_heap.o
ion_sys_heap-y := ion_system_heap.o ion_page_pool.o

# ion_ipc_trusty tidak di-port ke sini: port sudah punya padanannya di
# drivers/dma-buf/sprd/ion_ipc_trusty.c (RE-port dari blob, tanpa ION core).
obj-$(CONFIG_ION_CMA_HEAP) += ion_cma_heap.o
