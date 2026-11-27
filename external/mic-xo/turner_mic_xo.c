// SPDX-License-Identifier: GPL-2.0-only
/* MT6685 BBCK5 supplies the MT6681 audio crystal clock on Turner. */
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spmi.h>

#define BBCK5_CONTROL 0x79e
#define BBCK5_MODE_ENABLE GENMASK(2, 0)
struct turner_xo {
	struct clk_hw hw;
	struct spmi_device *sdev;
	u8 saved;
};
static int set_bbck5(struct turner_xo *xo, u8 bits)
{
	u8 value;
	int ret = spmi_ext_register_readl(xo->sdev, BBCK5_CONTROL, &value, 1);

	if (ret)
		return ret;
	value = (value & ~BBCK5_MODE_ENABLE) | bits;
	return spmi_ext_register_writel(xo->sdev, BBCK5_CONTROL, &value, 1);
}
static int prepare(struct clk_hw *hw)
{
	struct turner_xo *xo = container_of(hw, struct turner_xo, hw);
	u8 value;
	int ret = spmi_ext_register_readl(xo->sdev, BBCK5_CONTROL, &value, 1);
	if (ret)
		return ret;
	xo->saved = value & BBCK5_MODE_ENABLE;
	ret = set_bbck5(xo, BIT(2));
	if (ret)
		return ret;
	usleep_range(400, 420);
	ret = spmi_ext_register_readl(xo->sdev, BBCK5_CONTROL, &value, 1);
	if (ret || (value & BBCK5_MODE_ENABLE) != BIT(2)) {
		set_bbck5(xo, xo->saved);
		return ret ?: -EIO;
	}
	return 0;
}
static void unprepare(struct clk_hw *hw)
{
	struct turner_xo *xo = container_of(hw, struct turner_xo, hw);
	if (set_bbck5(xo, xo->saved))
		dev_err(&xo->sdev->dev, "unable to restore BBCK5 mode\n");
}
static unsigned long rate(struct clk_hw *hw, unsigned long parent)
{
	return 26000000;
}
static const struct clk_ops ops = {
	.prepare = prepare, .unprepare = unprepare, .recalc_rate = rate,
};
static int probe(struct spmi_device *sdev)
{
	struct clk_init_data init = { .name = "mt6685-bbck5", .ops = &ops };
	struct turner_xo *xo;
	int ret;
	xo = devm_kzalloc(&sdev->dev, sizeof(*xo), GFP_KERNEL);
	if (!xo)
		return -ENOMEM;
	xo->sdev = sdev;
	xo->hw.init = &init;
	ret = devm_clk_hw_register(&sdev->dev, &xo->hw);
	if (ret)
		return ret;
	return devm_clk_hw_register_clkdev(&sdev->dev, &xo->hw, "codec-xo", "turner-mic-capture");
}
static const struct of_device_id ids[] = {
	{ .compatible = "mediatek,mt6685" }, {}
};
MODULE_DEVICE_TABLE(of, ids);
static struct spmi_driver xo_driver = {
	.driver = { .name = "turner-mic-xo", .of_match_table = ids },
	.probe = probe,
};
module_spmi_driver(xo_driver);
MODULE_LICENSE("GPL");
