// SPDX-License-Identifier: GPL-2.0-only
/* Turner calibrates ULPOSC using two VLP clocks. The main kernel owns
 * the other PLLs, including the GPU PLL, so only map the VLP meter.
 */
#include "clk-fmeter-mt6991.c"

static unsigned int turner_measure(unsigned int id, enum FMETER_TYPE type)
{
	if (type != VLPCK || (id != 49 && id != 60))
		return 0;
	return mt6991_fm_vlpck_freq(id);
}

static const struct fmeter_ops turner_ops = {
	.get_fmeter_freq = turner_measure,
};

static int __init turner_meter_init(void)
{
	struct device_node *node = of_find_compatible_node(NULL, NULL,
						"mediatek,mt6991-vlp_cksys");

	if (!node)
		return -ENODEV;
	fm_base[FM_VLP_CKSYS] = of_iomap(node, 0);
	of_node_put(node);
	if (!fm_base[FM_VLP_CKSYS])
		return -ENOMEM;
	mt6991_fmeter_set_ops(&turner_ops);
	return 0;
}

/* Keep the provider resident while the core owns its callback. */
module_init(turner_meter_init);
MODULE_LICENSE("GPL");
