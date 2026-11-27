// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/delay.h>

static int __init inspect(void)
{
	struct device_node *bus __free(device_node) = of_find_node_by_path("/soc/i2c@1cc40000");
	struct device_node *clock __free(device_node) = of_parse_phandle(bus, "clocks", 0);
	void __iomem *vlp, *cg, *chan;
	struct resource res;
	u32 base, request[3], status[3], off, old, cur, gated;
	int ret;

	if (!bus || !clock || !of_device_is_compatible(clock, "mediatek,mt6991-scp_i3c") ||
	    of_property_read_u32(bus,"vlpcfg-base",&base) ||
	    of_property_read_u32_array(bus,"vlp-scp-sleep",request,3) ||
	    of_property_read_u32_array(bus,"vlp-scp-sleep-stat",status,3) ||
	    of_property_read_u32(bus,"ch-offset-i2c",&off) ||
	    of_address_to_resource(bus,0,&res) || off!=0x20000 ||
	    off+0x1000>resource_size(&res) || request[0]>0xffc || status[0]>0xffc)
		return -ENODEV;
	vlp=ioremap(base,0x1000);
	cg=of_iomap(clock,0);
	chan=ioremap(res.start+off,0x1000);
	if (!vlp || !cg || !chan) { ret=-ENOMEM; goto unmap; }
	old=readl(vlp+request[0]);
	cur=readl(vlp+status[0]);
	pr_info("turner-mic original wake=%08x status=%08x\n",old,cur);
	if (old & (request[1]|request[2])) {ret=-EBUSY;goto unmap;}
	writel(old|request[1],vlp+request[0]);
	ret=readl_poll_timeout(vlp+status[0],cur,!(cur&status[1]),20,800);
	writel(readl(vlp+request[0])|request[2],vlp+request[0]);
	if (ret) ret=readl_poll_timeout(vlp+status[0],cur,!(cur&status[2]),20,800);
	writel(readl(vlp+request[0])&~request[1],vlp+request[0]);
	pr_info("turner-mic wake status=%08x ret=%d\n",cur,ret);
	if(ret) goto release;
	gated=readl(cg+0xe10)&BIT(1);
	if(gated)writel(BIT(1),cg+0xe14);
	ret=readl_poll_timeout(cg+0xe10,cur,!(cur&BIT(1)),5,1000);
	if(!ret)pr_info("turner-mic timing=%08x low=%08x start=%08x ctrl=%08x irq=%08x\n",
		readl(chan+0x20),readl(chan+0x2c),readl(chan+0x24),readl(chan+0x10),readl(chan+0x0c));
	if (!ret && !readl(chan+0x24) && readl(chan+0x20)) {
		unsigned int saved_ctrl=readl(chan+0x10), saved_irq=readl(chan+8);
		writel(0,chan+8);
		writel((saved_ctrl & ~0x326) | 0x12,chan+0x10);
		writel(0xd6,chan+0x94);
		writel(0x3f,chan+0xc);
		writel(4,chan+0x38);
		writel(1,chan+0x38);
		writel(2,chan+0x14);
		writel(1,chan+0x44);
		writel(2,chan+0x18);
		writel(0,chan); writel(0xb,chan);
		if ((readl(chan+0xf4)&31)==2) {
			writel(0x4001,chan+0x24);
			ret=readl_poll_timeout(chan+0xc,cur,cur&0x2f,5,50000);
			pr_info("turner-mic chip-id irq=%08x fifo=%08x ret=%d\n",cur,readl(chan+0xf4),ret);
			if(!ret && (cur&0x2f)==1 && (readl(chan+0xf4)&31)==1)
				pr_info("turner-mic SWCID_H=%02x\n",readl(chan)&255);
			else ret=ret ?: -EIO;
			writel(cur,chan+0xc);
		} else ret=-EIO;
		writel(saved_ctrl,chan+0x10);
		writel(saved_irq,chan+8);
	}
	if(gated)writel(BIT(1),cg+0xe18);
release:
	writel((readl(vlp+request[0])&~(request[1]|request[2]))|old,vlp+request[0]);
unmap:
	if(chan)iounmap(chan);
	if(cg)iounmap(cg);
	if(vlp)iounmap(vlp);
	return ret;
}
module_init(inspect);
static void __exit done(void) {}
module_exit(done);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Inspect Turner codec bus timing under the SCP wake handshake");
