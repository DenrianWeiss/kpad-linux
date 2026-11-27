// SPDX-License-Identifier: GPL-2.0-only
/* Turner MT6681 capture, restricted to the native 48 kHz stereo format. */
#include <linux/clk.h>
#include <linux/hrtimer.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "capture-regs.h"

extern int mt6991_afe_engine_get(struct device *dev);
extern int mt6991_afe_engine_put(struct device *dev);
extern int mt6681_set_mtkaif_protocol(struct snd_soc_component *, int);
static struct platform_device *pdev, *afe;
static struct regmap *map;
static struct clk_bulk_data gates[] = { { .id = "aud_ul0_adc_clk" }, { .id = "aud_ul1_adc_clk" } };
static struct pinctrl *pins;
static struct clk *codec_xo;
static struct snd_pcm_substream *active;
static struct hrtimer tick;
static bool enabled;
static ktime_t interval;
static struct {
	unsigned int reg, mask, value, saved;
} route[] = {
	{ AUDIO_ENGEN_CON0, BIT(AUDIO_F3P25M_EN_ON_SFT), BIT(AUDIO_F3P25M_EN_ON_SFT) },
	{ AFE_CONN018_0, BIT(16), BIT(16) },
	{ AFE_CONN019_0, BIT(17), BIT(17) },
	{ AFE_AUD_PAD_TOP_CFG0, 0xff, 0xb9 },
	{ AFE_MTKAIF0_CFG0, BIT(31) | BIT(16), BIT(31) | BIT(16) },
	{ AFE_MTKAIF0_RX_CFG0, BIT(0), 0 },
	{ AFE_ADDA_MTKAIFV4_TX_CFG0, BIT(0) | BIT(11), BIT(0) | BIT(11) },
	{ AFE_ADDA_MTKAIFV4_RX_CFG0, GENMASK(7,4)|BIT(16)|BIT(17)|3, (5 << 4)|BIT(16)|BIT(17)|3 },
	{ AFE_ADDA_UL0_SRC_CON0, GENMASK(19,17)|1, (3 << 17)|1 },
	{ AFE_VUL0_CON0, GENMASK(12,8)|0x1f, 10 << 8 },
};
static unsigned int applied;
static void restore(void)
{
	while (applied) {
		unsigned int i = --applied;
		regmap_update_bits(map, route[i].reg, route[i].mask, route[i].saved);
	}
}
static int sel(const char *name)
{
	struct pinctrl_state *s = pinctrl_lookup_state(pins, name);
	return IS_ERR(s) ? PTR_ERR(s) : pinctrl_select_state(pins, s);
}
static enum hrtimer_restart elapsed(struct hrtimer *t)
{
	if (!READ_ONCE(enabled))
		return HRTIMER_NORESTART;
	snd_pcm_period_elapsed(active);
	hrtimer_forward_now(t, interval);
	return HRTIMER_RESTART;
}
static const struct snd_pcm_hardware hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE, .rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000, .rate_max = 48000, .channels_min = 2, .channels_max = 2,
	.buffer_bytes_max = 65536, .period_bytes_min = 1024, .period_bytes_max = 16384,
	.periods_min = 2, .periods_max = 16,
};
static int open_pcm(struct snd_soc_component *c, struct snd_pcm_substream *s)
{
	int ret;
	snd_soc_set_runtime_hwparams(s, &hw);
	ret = pm_runtime_resume_and_get(&afe->dev);
	if (ret < 0)
		return ret;
	ret = clk_prepare_enable(codec_xo);
	if (ret) goto pm;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(gates), gates);
	if (ret)
		goto xo_off;
	ret = mt6991_afe_engine_get(&afe->dev);
	if (ret)
		goto gates_off;
	ret = sel("aud-clk-mosi-on");
	if (!ret)
		ret = sel("aud-dat-miso0-on");
	if (!ret)
		ret = sel("aud-dat-miso1-on");
	if (!ret)
		return 0;
	sel("aud-dat-miso1-off");
	sel("aud-dat-miso0-off");
	sel("aud-clk-mosi-off");
	mt6991_afe_engine_put(&afe->dev);
gates_off:
	clk_bulk_disable_unprepare(ARRAY_SIZE(gates), gates);
xo_off:
	clk_disable_unprepare(codec_xo);
pm:
	pm_runtime_put(&afe->dev);
	return ret;
}
static int close_pcm(struct snd_soc_component *c, struct snd_pcm_substream *s)
{
	hrtimer_cancel(&tick);
	restore();
	sel("aud-dat-miso1-off");
	sel("aud-dat-miso0-off");
	sel("aud-clk-mosi-off");
	mt6991_afe_engine_put(&afe->dev);
	clk_bulk_disable_unprepare(ARRAY_SIZE(gates), gates);
	clk_disable_unprepare(codec_xo);
	pm_runtime_put(&afe->dev);
	return 0;
}
static int prepare(struct snd_soc_component *c, struct snd_pcm_substream *s)
{
	dma_addr_t base = s->runtime->dma_addr, end = base + snd_pcm_lib_buffer_bytes(s) - 1;
	const u32 data[] = {upper_32_bits(base), lower_32_bits(base), upper_32_bits(end), lower_32_bits(end)};
	const u32 regs[] = {AFE_VUL0_BASE_MSB, AFE_VUL0_BASE, AFE_VUL0_END_MSB, AFE_VUL0_END};
	unsigned int v;
	int ret, i;

	restore();
	ret = regmap_read(map, AFE_VUL0_CON0, &v);
	if (ret || (v & BIT(28)))
		return ret ?: -EBUSY;
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = regmap_write(map, regs[i], data[i]);
		if (!ret)
			ret = regmap_read(map, regs[i], &v);
		if (ret || v != data[i])
			return ret ?: -EIO;
	}
	for (i = 0; i < ARRAY_SIZE(route); i++) {
		ret = regmap_read(map, route[i].reg, &route[i].saved);
		if (!ret)
			ret = regmap_update_bits(map, route[i].reg, route[i].mask, route[i].value);
		if (ret) {
			restore();
			return ret;
		}
		applied++;
	}
	active = s;
	interval = ns_to_ktime(div_u64((u64)s->runtime->period_size*NSEC_PER_SEC,48000));
	return 0;
}
static int trigger(struct snd_soc_component *c, struct snd_pcm_substream *s, int cmd)
{
	int ret;
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		ret = regmap_set_bits(map, AFE_VUL0_CON0, BIT(28));
		if (ret) return ret;
		WRITE_ONCE(enabled, true);
		hrtimer_start(&tick, interval, HRTIMER_MODE_REL);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		WRITE_ONCE(enabled, false);
		return regmap_clear_bits(map, AFE_VUL0_CON0, BIT(28));
	default:
		return -EINVAL;
	}
}
static int sync_stop(struct snd_soc_component *c, struct snd_pcm_substream *s)
{
	hrtimer_cancel(&tick);
	return 0;
}
static snd_pcm_uframes_t pointer(struct snd_soc_component *c, struct snd_pcm_substream *s)
{
	u32 pos;
	if (regmap_read(map, AFE_VUL0_CUR, &pos))
		return SNDRV_PCM_POS_XRUN;
	pos -= lower_32_bits(s->runtime->dma_addr);
	return pos < snd_pcm_lib_buffer_bytes(s) ? bytes_to_frames(s->runtime, pos) : 0;
}
static int alloc_pcm(struct snd_soc_component *c, struct snd_soc_pcm_runtime *rtd)
{
	return snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV,
					      &afe->dev, 65536, 65536);
}
static const struct snd_soc_component_driver component = {
	.name = "turner-mic-capture", .open = open_pcm, .close = close_pcm,
	.prepare = prepare, .trigger = trigger, .sync_stop = sync_stop,
	.pointer = pointer, .pcm_new = alloc_pcm,
};
static struct snd_soc_dai_driver dai = {
	.name = "Turner ADDA UL", .capture = {
		.stream_name = "Microphone", .channels_min = 2, .channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000, .formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
};
static int init_link(struct snd_soc_pcm_runtime *rtd)
{
	return mt6681_set_mtkaif_protocol(snd_soc_rtd_to_codec(rtd,0)->component, 2);
}
SND_SOC_DAILINK_DEFS(mic,
	DAILINK_COMP_ARRAY(COMP_CPU("Turner ADDA UL")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt6681-sound", "mt6681-snd-codec-aif1")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("turner-mic-capture")));
static struct snd_soc_dai_link link = {
	.name = "Built-in Microphone", .stream_name = "Microphone", .capture_only = 1,
	.init = init_link, SND_SOC_DAILINK_REG(mic),
};
static struct snd_soc_card card = {
	.name = "Turner Microphone", .owner = THIS_MODULE, .dai_link = &link, .num_links = 1,
};
static int register_capture(struct platform_device *dev)
{
	int ret;
	card.dev = &dev->dev;
	codec_xo = devm_clk_get(card.dev, "codec-xo");
	if (IS_ERR(codec_xo)) return PTR_ERR(codec_xo);
	ret = clk_prepare_enable(codec_xo);
	if (ret) return ret;
	ret = devm_snd_soc_register_component(card.dev, &component, &dai, 1);
	if (!ret)
		ret = devm_snd_soc_register_card(card.dev, &card);
	clk_disable_unprepare(codec_xo);
	return ret;
}
static struct platform_driver capture_driver = {
	.probe = register_capture,
	.driver.name = "turner-mic-capture",
};
static int __init add_card(void)
{
	struct device_node *node __free(device_node) = of_find_compatible_node(NULL,NULL,"mediatek,mt6991-sound");
	int ret;

	afe = of_find_device_by_node(node);
	if (!afe) return -ENODEV;
	map = dev_get_regmap(&afe->dev, NULL);
	if (!map) { ret = -ENODEV; goto put; }
	ret = clk_bulk_get(&afe->dev, ARRAY_SIZE(gates), gates);
	if (ret) goto put;
	pins = pinctrl_get(&afe->dev);
	if (IS_ERR(pins)) { ret = PTR_ERR(pins); goto clocks; }
	hrtimer_setup(&tick, elapsed, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ret = platform_driver_register(&capture_driver);
	if (ret) goto release_pins;
	pdev = platform_device_register_simple("turner-mic-capture", -1, NULL, 0);
	if (!IS_ERR(pdev) && card.instantiated) return 0;
	ret = IS_ERR(pdev) ? PTR_ERR(pdev) : -ENODEV;
	if (!IS_ERR(pdev))
		platform_device_unregister(pdev);
	platform_driver_unregister(&capture_driver);
release_pins:
	pinctrl_put(pins);
clocks:
	clk_bulk_put(ARRAY_SIZE(gates),gates);
put:
	put_device(&afe->dev);
	return ret;
}
module_init(add_card);
static void __exit remove_card(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&capture_driver);
	pinctrl_put(pins);
	clk_bulk_put(ARRAY_SIZE(gates),gates);
	put_device(&afe->dev);
}
module_exit(remove_card);
MODULE_LICENSE("GPL");
