#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/clk-provider.h>
#include "clk-mux.h"
#include "blob.h"
static int overlay_id;
static DEFINE_SPINLOCK(mux_lock);
static const char * const parents[] = {"clk26m","ck_osc_d20","mainpll_d6","mainpll_d4","mainpll_d3","ck_apll1_ck"};
static const struct mtk_mux muxes[] = {
 MUX_GATE_FENC_CLR_SET_UPD(0, "turner_vlp_scp_sel", parents, 0x10, 0x14, 0x18, 0, 3, 7, 4, 0, 0x39c, 31),
};
static int probe(struct platform_device *pdev)
{
 struct device *dev = &pdev->dev;
 struct clk_hw_onecell_data *data;
 struct device_node *vlp;
 int ret;
 data = devm_kzalloc(dev, struct_size(data, hws, 1), GFP_KERNEL);
 if (!data) return -ENOMEM;
 data->num = 1;
 vlp = of_parse_phandle(dev->of_node, "mediatek,vlp", 0);
 if (!vlp) return -ENODEV;
 ret = mtk_clk_register_muxes(dev, muxes, 1, vlp, &mux_lock, data);
 of_node_put(vlp);
 if (ret) return ret;
 ret = of_clk_add_hw_provider(dev->of_node, of_clk_hw_onecell_get, data);
 if (ret) mtk_clk_unregister_muxes(muxes, 1, data);
 platform_set_drvdata(pdev, data);
 return ret;
}
static void remove_mux(struct platform_device *pdev)
{
 of_clk_del_provider(pdev->dev.of_node);
 mtk_clk_unregister_muxes(muxes, 1, platform_get_drvdata(pdev));
}
static const struct of_device_id ids[] = {{.compatible="xiaomi,turner-scp-clock"},{}};
static struct platform_driver driver = {.probe=probe, .remove=remove_mux, .driver={.name="turner-scp-clock",.of_match_table=ids}};
static int __init init_mux(void)
{
 int ret = platform_driver_register(&driver);
 if (ret) return ret;
 ret=of_overlay_fdt_apply(blob, blob_len, &overlay_id, NULL);
 if (ret) platform_driver_unregister(&driver);
 return ret;
}
static void __exit exit_mux(void)
{
 of_overlay_remove(&overlay_id);
 platform_driver_unregister(&driver);
}
module_init(init_mux);
module_exit(exit_mux);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Turner SCP selector clock test");
