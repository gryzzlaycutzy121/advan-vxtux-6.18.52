// SPDX-License-Identifier: GPL-2.0
/*
 * VXTux test harness: stub Trusty virtio-IPC device.
 *
 * PURPOSE
 * -------
 * The LKVM/QEMU module-load test has no secure world, so no Trusty
 * virtio device ever appears. Without one, drivers/trusty/trusty-ipc.c
 * never gets a virtio device to bind to, `default_vdev` stays NULL, and
 * `tipc_create_channel()` fails with -ENOENT. That makes
 * drivers/dma-buf/sprd/ion_ipc_trusty.ko fail to load with
 * "ion create channel failed (errno=-2)".
 *
 * This module registers a virtio device with the Trusty IPC device id
 * (13) so the tipc virtio driver binds to it, `default_vdev` is set and
 * ion_ipc_trusty can be insmod'ed inside the emulated guest.
 *
 * HONEST SCOPE
 * ------------
 * This is NOT a Trusty/TEE implementation. There is no secure world,
 * no service registry and no real data path: the virtqueues are backed
 * by plain vrings that nobody ever drains. The stub only lets the
 * guest-side code path (channel create + connect request queueing) run
 * to completion so the module can be loaded and its probe exercised.
 *
 * The REAL Trusty ABI is verified separately on the real device by the
 * userspace client (unisoc-tee-ipc), which gets a live
 * "ion connect done!" reply from the secure world.
 *
 * To be paired with trusty-ipc.ko's `test_autoonline=1` module
 * parameter: the tipc driver normally waits for a
 * TIPC_CTRL_MSGTYPE_GO_ONLINE control message from the secure world
 * before the device becomes usable, and no such message can arrive
 * here.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>

/*
 * Device id 13 is the Trusty IPC id (drivers/trusty/trusty-virtio-ids.h).
 * NOTE: in 6.18 id 13 is also VIRTIO_ID_MEMORY_BALLOON. The load-test
 * defconfig (vxtux_618_qemu.defconfig) does not enable
 * CONFIG_VIRTIO_BALLOON, so exactly one driver (tipc) matches here.
 */
#define STUB_VIRTIO_ID_TRUSTY_IPC	13
#define STUB_VQ_NUM			16
#define STUB_VRING_ALIGN		0x1000

struct stub_trusty_dev {
	struct virtio_device vdev;
	struct virtqueue *vqs[2];
};

static struct stub_trusty_dev *stub_dev;

/* --- virtio_config_ops ------------------------------------------------- */

static void stub_get(struct virtio_device *vdev, unsigned int offset,
		     void *buf, unsigned int len)
{
	/*
	 * The tipc driver pre-fills the struct with its own defaults
	 * (msg_buf_max_size / alignment) before calling get(), so an
	 * all-zero config space keeps those defaults. dev_name stays
	 * empty -> no /dev/trusty-ipc-dev* node is created (there is no
	 * service behind it anyway).
	 */
	memset(buf, 0, len);
}

static void stub_set(struct virtio_device *vdev, unsigned int offset,
		     const void *buf, unsigned int len)
{
}

static u8 stub_get_status(struct virtio_device *vdev)
{
	return 0;
}

static void stub_set_status(struct virtio_device *vdev, u8 status)
{
}

static void stub_reset(struct virtio_device *vdev)
{
}

static u64 stub_get_features(struct virtio_device *vdev)
{
	return 0;
}

static int stub_finalize_features(struct virtio_device *vdev)
{
	return 0;
}

static const char *stub_bus_name(struct virtio_device *vdev)
{
	return "stub-trusty-ipc";
}

static bool stub_notify(struct virtqueue *vq)
{
	/* No device side ever consumes the ring. */
	return true;
}

static int stub_find_vqs(struct virtio_device *vdev, unsigned int nvqs,
			 struct virtqueue *vqs[],
			 struct virtqueue_info vqs_info[],
			 struct irq_affinity *desc)
{
	struct stub_trusty_dev *sd = container_of(vdev, struct stub_trusty_dev,
						  vdev);
	unsigned int i;

	for (i = 0; i < nvqs; i++) {
		struct virtqueue *vq;

		vq = vring_create_virtqueue(i, STUB_VQ_NUM,
					    STUB_VRING_ALIGN, vdev,
					    false, false, false,
					    stub_notify, vqs_info[i].callback,
					    vqs_info[i].name);
		if (!vq) {
			unsigned int j;

			for (j = 0; j < i; j++) {
				vring_del_virtqueue(vqs[j]);
				vqs[j] = NULL;
			}
			return -ENOMEM;
		}
		vqs[i] = vq;
		sd->vqs[i] = vq;
	}

	return 0;
}

static void stub_del_vqs(struct virtio_device *vdev)
{
	struct stub_trusty_dev *sd = container_of(vdev, struct stub_trusty_dev,
						  vdev);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sd->vqs); i++) {
		if (sd->vqs[i]) {
			vring_del_virtqueue(sd->vqs[i]);
			sd->vqs[i] = NULL;
		}
	}
}

static const struct virtio_config_ops stub_config_ops = {
	.get			= stub_get,
	.set			= stub_set,
	.get_status		= stub_get_status,
	.set_status		= stub_set_status,
	.reset			= stub_reset,
	.get_features		= stub_get_features,
	.finalize_features	= stub_finalize_features,
	.find_vqs		= stub_find_vqs,
	.del_vqs		= stub_del_vqs,
	.bus_name		= stub_bus_name,
};

/* --- module lifecycle --------------------------------------------------- */

static void stub_release(struct device *dev)
{
	struct stub_trusty_dev *sd = container_of(dev, struct stub_trusty_dev,
						  vdev.dev);

	kfree(sd);
}

static int __init stub_trusty_init(void)
{
	struct stub_trusty_dev *sd;
	int ret;

	sd = kzalloc(sizeof(*sd), GFP_KERNEL);
	if (!sd)
		return -ENOMEM;

	sd->vdev.id.device = STUB_VIRTIO_ID_TRUSTY_IPC;
	sd->vdev.id.vendor = VIRTIO_DEV_ANY_ID;
	sd->vdev.config = &stub_config_ops;
	sd->vdev.dev.release = stub_release;

	ret = register_virtio_device(&sd->vdev);
	if (ret) {
		kfree(sd);
		return ret;
	}

	stub_dev = sd;
	pr_info("stub-trusty: registered virtio device id=%d (no secure world)\n",
		STUB_VIRTIO_ID_TRUSTY_IPC);
	return 0;
}

static void __exit stub_trusty_exit(void)
{
	if (!stub_dev)
		return;

	unregister_virtio_device(&stub_dev->vdev);
	put_device(&stub_dev->vdev.dev);
	stub_dev = NULL;
}

module_init(stub_trusty_init);
module_exit(stub_trusty_exit);

MODULE_DESCRIPTION("VXTux test harness: stub Trusty virtio-IPC device");
MODULE_LICENSE("GPL");
