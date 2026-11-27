// SPDX-License-Identifier: GPL-2.0-only
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mfd/core.h>
#include <linux/mfd/mt6681.h>
#include <linux/mfd/mt6681-private.h>
#include <linux/pinctrl/consumer.h>

bool mt6681_probe_done;
EXPORT_SYMBOL_GPL(mt6681_probe_done);
static const struct regmap_config cfg = {
	.reg_bits = 16, .val_bits = 8, .reg_format_endian = REGMAP_ENDIAN_BIG,
	.use_single_read = true, .use_single_write = true,
};
static const struct mfd_cell sound = {
	.name = "mt6681-sound", .of_compatible = "mediatek,mt6681-sound",
};
/* MT6681 startup and idle power sequence from the Turner vendor MFD. */
static const struct reg_sequence power_on_sequence[] = {
	{ MT6681_TOP_DIG_WPK, 0x81 },
	{ MT6681_TOP_DIG_WPK_H, 0x66 },
	{ MT6681_TOP_TMA_KEY, 0x7e },
	{ MT6681_TOP_TMA_KEY_H, 0x99 },
	{ MT6681_PSC_WPK_L, 0x29 },
	{ MT6681_PSC_WPK_H, 0x47 },
	{ MT6681_HK_TOP_WKEY_L, 0x81 },
	{ MT6681_HK_TOP_WKEY_H, 0x66 },
	{ MT6681_TOP_CON, 0x7 },
	{ MT6681_TEST_CON0, 0x1f },
	{ MT6681_SMT_CON0, 0x3 },
	{ MT6681_GPIO_PULLEN0, 0xf9 },
	{ MT6681_GPIO_PULLEN1, 0x1f },
	{ MT6681_TOP_CKPDN_CON0, 0x5b },
	{ MT6681_HK_TOP_CLK_CON0, 0x15 },
	{ MT6681_DA_INTF_STTING3, 0x0c },
	{ MT6681_MTC_CTL0, 0x13 },
	{ MT6681_PLT_CON0, 0x0 },
	{ MT6681_PLT_CON1, 0x0 },
	{ MT6681_HK_TOP_CLK_CON0, 0x15 },
	{ MT6681_AUXADC_CON0, 0x0 },
	{ MT6681_AUXADC_TRIM_SEL2, 0x40 },
	{ MT6681_TOP_TOP_CKHWEN_CON0, 0x0f },
	{ MT6681_LDO_TOP_CLK_DCM_CON0, 0x01 },
	{ MT6681_LDO_TOP_VR_CLK_CON0, 0x00 },
	{ MT6681_LDO_VAUD18_CON2, 0x1c },
	{ MT6681_PLT_CON0, 0x3a },
	{ MT6681_PLT_CON1, 0x0c },
	{ MT6681_LDO_VAUD18_MULTI_SW_0, 0x1 },
	{ MT6681_MTC_CTL0, 0x10 },
	{ MT6681_MTC_CTL0, 0x11 },
	{ MT6681_MTC_CTL0, 0x13 },
	{ MT6681_DA_INTF_STTING3, 0x08 },
	{ MT6681_LDO_VAUD18_CON2, 0x1C },
	{ MT6681_LDO_VAUD18_OP_EN0, 0x01 },
	{ MT6681_LDO_VAUD18_OP_CFG0, 0x01 },
	{ MT6681_DA_INTF_STTING1, 0x64 },
	{ MT6681_DA_INTF_STTING1, 0x66 },
	{ MT6681_DA_INTF_STTING1, 0x76 },
};
static int probe(struct i2c_client *client)
{
	struct mt6681_pmic_info *p;
	unsigned int id;
	int ret;

	p = devm_kzalloc(&client->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->i2c = client;
	p->dev = &client->dev;
	p->regmap = devm_regmap_init_i2c(client, &cfg);
	if (IS_ERR(p->regmap))
		return PTR_ERR(p->regmap);
	ret = regmap_read(p->regmap, 0xb, &id);
	if (ret || id != 0x81)
		return ret ?: -ENODEV;
	ret = regmap_multi_reg_write(p->regmap, power_on_sequence, ARRAY_SIZE(power_on_sequence));
	if (ret)
		return ret;
	i2c_set_clientdata(client, p);
	return devm_mfd_add_devices(&client->dev, PLATFORM_DEVID_NONE,
				    &sound, 1, NULL, 0, NULL);
}
static const struct of_device_id ids[] = {
	{ .compatible = "mediatek,mt6681_pmic" }, {}
};
MODULE_DEVICE_TABLE(of, ids);
static struct i2c_driver codec_parent = {
	.driver = { .name = "turner-mic-pmic", .of_match_table = ids },
	.probe = probe,
};
module_i2c_driver(codec_parent);
MODULE_LICENSE("GPL");
