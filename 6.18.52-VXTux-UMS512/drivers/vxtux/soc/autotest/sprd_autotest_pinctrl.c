// SPDX-License-Identifier: GPL-2.0
/*
 * sprd_autotest_pinctrl - handler autotest untuk pinctrl states.
 *
 * Port of the vendor 5.4 module `pinctrl.ko` ("sprd autotest pinctrl
 * driver", depends=core).  Bukti: docs/evidence-20261023/pinctrl.objdump.txt
 * (objdump -dr) + symtab runtime (58 entri, fungsi pinctrl_pre_test /
 * pinctrl_test + init/cleanup).
 *
 * Handler di .data blob: +0x00 "pinctrl", +0x08 cmd = 0xc0044100
 * (_IOWR('A', 0, 4)), +0x20 pinctrl_pre_test (dipanggil core sebagai
 * handler->init saat register), +0x28 pinctrl_test (handler->test).
 *
 * pre_test : cari node DT compatible "sprd,autotest", ambil phandle
 *            "sprd,pinctrl", lalu platform device-nya.
 * test     : argumen user = pointer ke u32 gpio index. Cari state
 *            "gpio_%d" lalu pilih lewat pinctrl_select_state().
 */
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#include "sprd_autotest_core.h"

#define SPRD_AUTOTEST_PINCTRL_CMD	0xc0044100

/* .bss blob: +0x00 pinctrl handle, +0x08 platform device. */
static struct pinctrl *pinctrl_handle;
static struct device *pinctrl_dev;

static int pinctrl_pre_test(struct sprd_autotest_handler *h, void *data)
{
	struct device_node *np, *phandle;
	struct platform_device *pdev;

	np = of_find_compatible_node(NULL, NULL, "sprd,autotest");
	if (!np) {
		pr_err("failed to find autotest node.\n");
		return -ENODEV;
	}

	phandle = of_parse_phandle(np, "sprd,pinctrl", 0);
	if (!phandle) {
		pr_err("failed to find pinctrl node.\n");
		return -ENODEV;
	}

	pdev = of_find_device_by_node(phandle);
	pinctrl_dev = pdev ? &pdev->dev : NULL;
	if (!pdev) {
		pr_err("failed to get pinctrl platform device.\n");
		return -ENODEV;
	}

	return 0;
}

static int pinctrl_test(struct sprd_autotest_handler *h, unsigned long arg)
{
	struct pinctrl_state *state;
	char name[9];
	int gpio, ret;

	if (get_user(gpio, (u32 __user *)arg))
		return -EFAULT;

	pr_info("gpio = %d\n", gpio);

	if (!pinctrl_handle) {
		pinctrl_handle = devm_pinctrl_get(pinctrl_dev);
		if (IS_ERR(pinctrl_handle)) {
			ret = PTR_ERR(pinctrl_handle);
			pr_err("get pinctrl handle failed.\n");
			return ret;
		}
	}

	snprintf(name, sizeof(name), "gpio_%d", gpio);
	state = pinctrl_lookup_state(pinctrl_handle, name);
	if (IS_ERR(state)) {
		pr_err("there are no autotest gpios state.\n");
		return PTR_ERR(state);
	}

	return pinctrl_select_state(pinctrl_handle, state);
}

static struct sprd_autotest_handler pinctrl_handler = {
	.name = "pinctrl",
	.cmd = SPRD_AUTOTEST_PINCTRL_CMD,
	.init = pinctrl_pre_test,
	.test = pinctrl_test,
};

static int __init sprd_autotest_pinctrl_init(void)
{
	return sprd_autotest_register_handler(&pinctrl_handler);
}

static void __exit sprd_autotest_pinctrl_exit(void)
{
	sprd_autotest_unregister_handler(&pinctrl_handler);
}

module_init(sprd_autotest_pinctrl_init);
module_exit(sprd_autotest_pinctrl_exit);

MODULE_DESCRIPTION("sprd autotest pinctrl driver - port vendor 5.4 pinctrl.ko");
MODULE_LICENSE("GPL v2");
