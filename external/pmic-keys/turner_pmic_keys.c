// SPDX-License-Identifier: GPL-2.0-only
/* Read the MT6363 debounced key levels without changing PMIC IRQ/reset setup. */
#include <linux/input.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spmi.h>

#define MT6363_TOPSTATUS 0x1e

static struct spmi_device *pmic;
static struct input_dev *input;
static const struct {
	const char *name;
	u8 mask;
} lines[] = {
	{ "power", BIT(1) },
	{ "home", BIT(3) },
};
static unsigned int codes[ARRAY_SIZE(lines)];

static void poll_keys(struct input_dev *dev)
{
	u8 status;
	unsigned int i;

	if (spmi_ext_register_readl(pmic, MT6363_TOPSTATUS, &status, 1))
		return;
	for (i = 0; i < ARRAY_SIZE(lines); i++)
		input_report_key(dev, codes[i], !(status & lines[i].mask));
	input_sync(dev);
}

static int __init turner_keys_init(void)
{
	struct device_node *keys, *parent, *child;
	unsigned int i;
	int ret;
	u8 status;

	keys = of_find_compatible_node(NULL, NULL, "mediatek,mt6363-keys");
	if (!keys)
		return -ENODEV;
	parent = of_get_parent(keys);
	pmic = spmi_find_device_by_of_node(parent);
	of_node_put(parent);
	if (!pmic) {
		of_node_put(keys);
		return -ENODEV;
	}
	for (i = 0; i < ARRAY_SIZE(lines); i++) {
		child = of_get_child_by_name(keys, lines[i].name);
		ret = of_property_read_u32(child, "linux,keycodes", &codes[i]);
		of_node_put(child);
		if (ret || !codes[i] || codes[i] > KEY_MAX)
			break;
	}
	of_node_put(keys);
	if (i != ARRAY_SIZE(lines)) {
		ret = -EINVAL;
		goto put;
	}
	ret = spmi_ext_register_readl(pmic, MT6363_TOPSTATUS, &status, 1);
	if (ret)
		goto put;
	input = input_allocate_device();
	if (!input) {
		ret = -ENOMEM;
		goto put;
	}
	input->name = "Turner MT6363 buttons";
	input->phys = "mt6363/input0";
	input->dev.parent = &pmic->dev;
	input->id.bustype = BUS_HOST;
	for (i = 0; i < ARRAY_SIZE(lines); i++)
		input_set_capability(input, EV_KEY, codes[i]);
	ret = input_setup_polling(input, poll_keys);
	if (!ret) {
		input_set_poll_interval(input, 30);
		ret = input_register_device(input);
	}
	if (!ret)
		return 0;
	input_free_device(input);
put:
	put_device(&pmic->dev);
	return ret;
}

static void __exit turner_keys_exit(void)
{
	input_unregister_device(input);
	put_device(&pmic->dev);
}
module_init(turner_keys_init);
module_exit(turner_keys_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Turner MT6363 power and volume buttons");
