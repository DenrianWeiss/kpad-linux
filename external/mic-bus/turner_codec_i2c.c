// SPDX-License-Identifier: GPL-2.0-only
/* FIFO transfers on Turner's firmware-configured SCP I2C1 codec channel. */
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#define DATA 0
#define IRQ_EN 8
#define IRQ_STAT 0xc
#define CTRL 0x10
#define LENGTH 0x14
#define TRANSACTIONS 0x18
#define TIMING 0x20
#define START 0x24
#define FIFO_CLEAR 0x38
#define AUX_LENGTH 0x44
#define ADDRESS 0x94
#define FIFO_LEVEL 0xf4
#define IRQ_FINAL 0x2f
#define FIFO_DEPTH 16

static void __iomem *chan, *gate, *vlp;
static struct platform_device *controller;
static struct resource window;
static u32 request[3], status[3];

static int transfer(struct i2c_adapter *a, struct i2c_msg *msgs, int n)
{
	struct i2c_msg *wr = &msgs[0], *rd = n == 2 ? &msgs[1] : NULL;
	u32 val, ctrl, ien, gated;
	int i, ret;

	if (n < 1 || n > 2 || wr->addr != 0x6b ||
	    (wr->flags & ~I2C_M_DMA_SAFE) || !wr->len || wr->len > FIFO_DEPTH ||
	    (rd && (rd->addr != wr->addr || (rd->flags & ~I2C_M_DMA_SAFE) != I2C_M_RD ||
		    !rd->len || rd->len > FIFO_DEPTH)))
		return -EOPNOTSUPP;

	val = readl(vlp + request[0]);
	if (val & (request[1] | request[2]))
		return -EBUSY;
	writel(val | request[1], vlp + request[0]);
	ret = readl_poll_timeout(vlp + status[0], val, !(val & status[1]), 20, 800);
	writel(readl(vlp + request[0]) | request[2], vlp + request[0]);
	if (ret)
		ret = readl_poll_timeout(vlp + status[0], val, !(val & status[2]), 20, 800);
	writel(readl(vlp + request[0]) & ~request[1], vlp + request[0]);
	if (ret)
		goto sleep;
	gated = readl(gate + 0xe10) & BIT(1);
	if (gated)
		writel(BIT(1), gate + 0xe14);
	ret = readl_poll_timeout(gate + 0xe10, val, !(val & BIT(1)), 5, 1000);
	if (ret)
		goto gate_off;
	if ((readl(chan + START) & BIT(0)) || !readl(chan + TIMING)) {
		ret = -EBUSY;
		goto gate_off;
	}

	ctrl = readl(chan + CTRL);
	ien = readl(chan + IRQ_EN);
	writel(0, chan + IRQ_EN);
	writel((ctrl & ~0x316) | (rd ? 0x12 : 0), chan + CTRL);
	writel(wr->addr << 1, chan + ADDRESS);
	writel(0x3f, chan + IRQ_STAT);
	writel(4, chan + FIFO_CLEAR);
	writel(1, chan + FIFO_CLEAR);
	writel(wr->len, chan + LENGTH);
	writel(rd ? rd->len : 0, chan + AUX_LENGTH);
	writel(n, chan + TRANSACTIONS);
	for (i = 0; i < wr->len; i++)
		writel(wr->buf[i], chan + DATA);
	if ((readl(chan + FIFO_LEVEL) & 31) != wr->len) {
		ret = -EIO;
		goto restore;
	}
	writel(0x4001, chan + START);
	ret = readl_poll_timeout(chan + IRQ_STAT, val, val & IRQ_FINAL, 5, 50000);
	if (!ret && (val & IRQ_FINAL) != 1)
		ret = (val & 6) ? -ENXIO : -EIO;
	if (!ret && rd) {
		if ((readl(chan + FIFO_LEVEL) & 31) != rd->len)
			ret = -EIO;
		else
			for (i = 0; i < rd->len; i++)
				rd->buf[i] = readl(chan + DATA);
	}
	writel(val & 0x3f, chan + IRQ_STAT);
restore:
	writel(ctrl, chan + CTRL);
	writel(ien, chan + IRQ_EN);
gate_off:
	if (gated)
		writel(BIT(1), gate + 0xe18);
sleep:
	writel(readl(vlp + request[0]) & ~request[2], vlp + request[0]);
	return ret ?: n;
}

static u32 functionality(struct i2c_adapter *a)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm algo = {
	.xfer = transfer,
	.functionality = functionality,
};
static struct i2c_adapter adapter = {
	.owner = THIS_MODULE,
	.algo = &algo,
	.name = "Turner SCP codec FIFO",
};

static void cleanup(void)
{
	if (chan)
		iounmap(chan);
	if (vlp)
		iounmap(vlp);
	if (gate)
		iounmap(gate);
	release_mem_region(window.start, resource_size(&window));
	put_device(&controller->dev);
}

static int __init add_adapter(void)
{
	struct device_node *node __free(device_node) = of_find_node_by_path("/soc/i2c@1cc40000");
	struct device_node *clock __free(device_node) = NULL;
	u32 base, off;
	int ret;

	if (!node)
		return -ENODEV;
	clock = of_parse_phandle(node, "clocks", 0);
	if (!of_device_is_compatible(clock, "mediatek,mt6991-scp_i3c") ||
	    of_address_to_resource(node, 0, &window) ||
	    of_property_read_u32(node, "ch-offset-i2c", &off) || off != 0x20000 ||
	    resource_size(&window) < off + SZ_4K ||
	    of_property_read_u32(node, "vlpcfg-base", &base) ||
	    of_property_read_u32_array(node, "vlp-scp-sleep", request, 3) ||
	    of_property_read_u32_array(node, "vlp-scp-sleep-stat", status, 3) ||
	    request[0] > 0xffc || status[0] > 0xffc)
		return -ENODEV;

	controller = of_find_device_by_node(node);
	if (!controller)
		return -ENODEV;
	window.start += off;
	window.end = window.start + SZ_4K - 1;
	if (controller->dev.driver || !request_mem_region(window.start, SZ_4K, adapter.name)) {
		put_device(&controller->dev);
		return -EBUSY;
	}
	chan = ioremap(window.start, SZ_4K);
	vlp = ioremap(base, SZ_4K);
	gate = of_iomap(clock, 0);
	ret = -ENOMEM;
	if (chan && vlp && gate) {
		adapter.dev.parent = &controller->dev;
		adapter.dev.of_node = node;
		ret = i2c_add_adapter(&adapter);
	}
	if (ret)
		cleanup();
	return ret;
}
module_init(add_adapter);
static void __exit remove_adapter(void)
{
	i2c_del_adapter(&adapter);
	cleanup();
}
module_exit(remove_adapter);
MODULE_LICENSE("GPL");
