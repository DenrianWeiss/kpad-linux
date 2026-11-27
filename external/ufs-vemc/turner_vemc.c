// SPDX-License-Identifier: GPL-2.0-only
/* Observe the boot-enabled MT6363 rail referenced by the firmware UFS node. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/mt6363-regulator.h>
#include <linux/spmi.h>

/* Mainline constants are relative to the regulator window. The stock
 * mt6363-regulator module uses absolute SPMI addresses (e.g. 0x1c23).
 */
#define MT6363_REGULATOR_BASE 0x30

static struct spmi_device *pmic;
static struct regulator_dev *rail;

static int read_byte(unsigned int address)
{
	u8 value;
	int ret = spmi_ext_register_readl(pmic, MT6363_REGULATOR_BASE + address, &value, 1);

	return ret ? ret : value;
}

static int vemc_enabled(struct regulator_dev *unused)
{
	int value = read_byte(MT6363_RG_LDO_VEMC_ADDR);

	return value < 0 ? value : !!(value & BIT(MT6363_RG_LDO_VEMC_EN_BIT));
}

static int vemc_voltage(struct regulator_dev *unused)
{
	static const int millivolts[] = {
		1200, 1300, 1500, 1700, 1800, 2000, 2500, 2600,
		2700, 2800, 2900, 3000, 3100, 3300, 3400, 3500
	};
	int trap, sel, cal;

	trap = read_byte(MT6363_TOP_TRAP);
	if (trap < 0)
		return trap;
	if (trap > 1)
		return -EINVAL;
	sel = read_byte(MT6363_RG_VEMC_VOSEL_ADDR);
	if (sel < 0)
		return sel;
	cal = read_byte(MT6363_RG_VEMC_VOCAL_ADDR);
	if (cal < 0)
		return cal;
	cal &= MT6363_RG_VEMC_VOCAL_MASK;
	if (cal > 10)
		return -ERANGE;
	sel = (sel >> (trap * 4)) & MT6363_RG_VEMC_VOSEL_MASK;
	return millivolts[sel] * 1000 + cal * 10000;
}

static const struct regulator_ops ops = {
	.is_enabled = vemc_enabled,
	.get_voltage = vemc_voltage,
};

static const struct regulator_desc desc = {
	.name = "mt6363_vemc",
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &ops,
};

static int __init turner_vemc_init(void)
{
	struct regulator_init_data init = { .constraints.always_on = true };
	struct regulator_config config = { .init_data = &init };
	struct device_node *ufs, *node, *regs, *parent;
	int ret, voltage;

	ufs = of_find_compatible_node(NULL, NULL, "mediatek,mt8183-ufshci");
	node = of_parse_phandle(ufs, "vcc-supply", 0);
	of_node_put(ufs);
	if (!node)
		return -ENODEV;
	regs = of_get_parent(node);
	parent = of_get_parent(regs);
	of_node_put(regs);
	if (of_device_is_compatible(parent, "mediatek,mt6363") &&
	    of_node_name_eq(node, "vemc") &&
	    of_property_read_bool(node, "regulator-always-on"))
		pmic = spmi_find_device_by_of_node(parent);
	of_node_put(parent);
	ret = -ENODEV;
	if (!pmic)
		goto put_node;
	ret = vemc_enabled(NULL);
	if (ret <= 0) {
		ret = ret ? ret : -ENODEV;
		goto put_pmic;
	}
	voltage = vemc_voltage(NULL);
	if (voltage < 0) {
		ret = voltage;
		goto put_pmic;
	}
	config.dev = &pmic->dev;
	config.of_node = node;
	rail = regulator_register(&pmic->dev, &desc, &config);
	ret = PTR_ERR_OR_ZERO(rail);
	if (!ret)
		dev_info(&pmic->dev, "firmware VEMC enabled at %d uV\n", voltage);
put_pmic:
	if (ret)
		put_device(&pmic->dev);
put_node:
	of_node_put(node);
	return ret;
}
module_init(turner_vemc_init);

static void __exit turner_vemc_exit(void)
{
	regulator_unregister(rail);
	put_device(&pmic->dev);
}
module_exit(turner_vemc_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Turner firmware-managed UFS VCC readback");
