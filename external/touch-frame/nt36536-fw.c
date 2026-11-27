// SPDX-License-Identifier: GPL-2.0-only
/* SRAM download for the Turner CSOT production digitizer. */
#include <crypto/sha2.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>
#include "nt36536-fw.h"

#define FW_NAME "novatek/novatek_nt36536_turner_fw_csot.bin"
#define CHUNK 15360
#define RESET 0x1fb43e
#define TRIM_ID 0x1fb104
#define TOPOLOGY 0x1fb12c
#define BOOT_READY 0x1fb50d
#define AUTO_COPY 0x1fc825

u8 nt36536_read_shift = 2;

struct nvt_bus {
	struct spi_device *spi;
	u8 *tx;
	u8 *rx;
	size_t chunk;
	u8 shift;
};

struct nvt_segment {
	u32 source, addr, last, crc;
};

static int transfer(struct nvt_bus *bus, size_t len, bool read)
{
	struct spi_transfer xfer = {
		.tx_buf = bus->tx, .rx_buf = read ? bus->rx : NULL,
		.len = len, .bits_per_word = 8,
	};
	struct spi_message msg;
	int ret;

	spi_message_init_with_transfers(&msg, &xfer, 1);
	ret = spi_sync(bus->spi, &msg);
	return ret ?: (msg.actual_length == len ? 0 : -EIO);
}

/* Read the controller state with its clock held, for protocol comparison. */
static void nvt_log_spi_regs(struct nvt_bus *bus)
{
	struct device *ctlr = &bus->spi->controller->dev;
	void __iomem *regs = ioremap(0x161d0000, 0x100);
	int ret = pm_runtime_get_sync(ctlr);

	if (ret < 0 || !regs) {
		dev_notice(&bus->spi->dev, "SPI peek unavailable ret=%d\n", ret);
		if (regs)
			iounmap(regs);
		if (ret >= 0)
			pm_runtime_put_sync(ctlr);
		return;
	}
	dev_notice(&bus->spi->dev, "SPI cfg0=%#x cfg1=%#x cmd=%#x pad=%#x cfg2=%#x cfg3=%#x\n",
		   readl(regs + 0x00), readl(regs + 0x04), readl(regs + 0x18),
		   readl(regs + 0x24), readl(regs + 0x28), readl(regs + 0x40));
	iounmap(regs);
	pm_runtime_put_sync(ctlr);
}

static int page(struct nvt_bus *bus, u32 addr)
{
	bus->tx[0] = 0xff;
	bus->tx[1] = addr >> 15;
	bus->tx[2] = addr >> 7;
	return transfer(bus, 3, false);
}

static int write_bytes(struct nvt_bus *bus, u32 addr, const u8 *data, size_t len)
{
	int ret = page(bus, addr);

	if (ret)
		return ret;
	bus->tx[0] = 0x80 | (addr & 0x7f);
	memcpy(bus->tx + 1, data, len);
	return transfer(bus, len + 1, false);
}

static int write_value(struct nvt_bus *bus, u32 addr, u32 value, size_t width)
{
	__le32 bytes = cpu_to_le32(value);

	return write_bytes(bus, addr, (const u8 *)&bytes, width);
}

/* One command byte, two idle bytes and the payload. The receiver samples the
 * reply late by a fixed time, so the byte stream is realigned afterwards.
 */
static int read_stream(struct nvt_bus *bus, u32 addr, u8 *raw, size_t len)
{
	int ret = page(bus, addr);

	if (ret)
		return ret;
	memset(bus->tx, 0, len + 2);
	bus->tx[0] = addr & 0x7f;
	ret = transfer(bus, len + 2, true);
	if (!ret)
		memcpy(raw, bus->rx + 2, len);
	return ret;
}

static void shift_stream(const u8 *raw, u8 *data, size_t len, unsigned int shift)
{
	size_t i;

	for (i = 0; i < len; i++) {
		u8 carry = shift && i ? (u8)(raw[i - 1] << (8 - shift)) : 0;

		data[i] = (raw[i] >> shift) | carry;
	}
}

/* Read @len bytes at @addr. The receiver samples the reply two bits late, so
 * the window starts one byte before the target; realigning the whole window
 * keeps the carry that carries the target byte's top bits.
 */
static int read_bytes(struct nvt_bus *bus, u32 addr, u8 *data, size_t len)
{
	u8 raw[34], aligned[33];
	int ret;

	if (!addr || len + 2 > sizeof(raw))
		return -EINVAL;
	ret = read_stream(bus, addr - 1, raw, len + 2);
	if (!ret) {
		shift_stream(raw, aligned, len + 1, bus->shift);
		memcpy(data, aligned + 1, len);
	}
	return ret;
}

static int bootloader_reset(struct nvt_bus *bus)
{
	int ret = write_value(bus, RESET, 0x69, 1);

	if (!ret)
		usleep_range(5000, 5500);
	return ret;
}

static int read_trim_id(struct nvt_bus *bus, u32 *event)
{
	static bool logged;
	u8 raw[18], aligned[18], id[8] = {}, topology;
	unsigned int shift;
	int ret;

	ret = bootloader_reset(bus);
	if (!ret)
		ret = write_bytes(bus, TRIM_ID, id, 6);
	if (!ret)
		ret = read_stream(bus, TRIM_ID - 1, raw, sizeof(raw));
	if (ret)
		return ret;
	for (shift = 0; shift < 8; shift++) {
		shift_stream(raw, aligned, sizeof(aligned), shift);
		memcpy(id, aligned + 1, sizeof(id));
		if ((id[0] == 0x14 || id[0] == 0x15) &&
		    id[3] == 0x23 && id[4] == 0x65 && id[5] == 0x03)
			break;
	}
	if (shift == 8) {
		dev_notice(&bus->spi->dev, "unexpected trim ID %*ph\n", (int)sizeof(raw), raw);
		if (!logged) {
			logged = true;
			nvt_log_spi_regs(bus);
		}
		return -ENODEV;
	}
	if (shift != bus->shift)
		dev_notice(&bus->spi->dev, "trim ID shift=%u %*ph\n", shift,
			   (int)sizeof(id), id);
	bus->shift = shift;
	nt36536_read_shift = shift;
	ret = read_bytes(bus, TOPOLOGY, &topology, 1);
	if (!ret)
		*event = topology & 1 ? 0x130900 : 0x133300;
	return ret;
}

static int identify(struct nvt_bus *bus, u32 *event)
{
	int ret, attempt;

	/* The digitizer shares the panel reset pin. Use the stock SPI
	 * engineering reset once, before entering the trim-ID probe loop.
	 */
	ret = write_value(bus, 0x7fff80, 0x5a, 1);
	if (ret)
		return ret;
	usleep_range(11000, 12000);
	for (attempt = 0; attempt < 5; attempt++) {
		ret = read_trim_id(bus, event);
		if (ret != -ENODEV)
			return ret;
		msleep(10);
	}
	return ret;
}

static int wait_value(struct nvt_bus *bus, u32 addr, u8 low, u8 high,
		      unsigned int attempts, unsigned int delay_us)
{
	unsigned int i;
	u8 value = 0;
	int ret;

	for (i = 0; i < attempts; i++) {
		ret = read_bytes(bus, addr, &value, 1);
		if (ret)
			return ret;
		if (value >= low && value <= high)
			return 0;
		usleep_range(delay_us, delay_us + 100);
	}
	dev_notice(&bus->spi->dev, "wait %#x timed out, last=%#x\n", addr, value);
	return -ETIMEDOUT;
}

static int download(struct nvt_bus *bus, const struct firmware *fw,
		    const struct nvt_segment *segments, u32 event)
{
	static const u8 zeros[6], crc_command[] = { 0xae, 0 };
	const char *stage = "reset";
	u8 version[2];
	unsigned int i;
	int ret = bootloader_reset(bus);

	if (!ret) {
		stage = "banks";
		for (i = 0; !ret && i < 2; i++) {
			u32 bank = 0x1fcc30 + i * 16;

			ret = write_value(bus, bank, segments[i].addr, 3);
			if (!ret)
				ret = write_value(bus, bank + 4, segments[i].last, 3);
			if (!ret)
				ret = write_value(bus, bank + 8, segments[i].crc, 4);
		}
	}
	if (!ret && (ret = write_value(bus, AUTO_COPY, 0x56, 1)))
		stage = "auto-copy";
	for (i = 0; !ret && i < 4; i++) {
		const struct nvt_segment *s = &segments[i];
		size_t offset = 0, size = s->last + 1;

		stage = "payload";
		while (!ret && offset < size) {
			size_t len = min(bus->chunk, size - offset);

			ret = write_bytes(bus, s->addr + offset, fw->data + s->source + offset, len);
			offset += len;
		}
	}
	if (!ret && (ret = wait_value(bus, AUTO_COPY, 0, 0, 200, 1000)))
		stage = "wait-copy";
	if (!ret && (ret = write_bytes(bus, event + 0x60, zeros, sizeof(zeros))))
		stage = "event-clear";
	if (!ret && (ret = write_bytes(bus, event + 0x50, crc_command, sizeof(crc_command))))
		stage = "event-crc";
	if (!ret && (ret = write_value(bus, BOOT_READY, 1, 1)))
		stage = "boot-ready";
	if (ret) {
		dev_notice(&bus->spi->dev, "download stopped at %s: %d\n", stage, ret);
		return ret;
	}
	usleep_range(5000, 5500);
	ret = wait_value(bus, event + 0x60, 0xa0, 0xaf, 11, 10000);
	if (ret) {
		dev_notice(&bus->spi->dev, "firmware start poll failed at event %#x: %d\n",
			   event, ret);
		return ret;
	}
	ret = read_bytes(bus, event + 0x78, version, sizeof(version));
	if (!ret && (version[0] != fw->data[fw->size - 4096] ||
		     version[0] + version[1] != 255)) {
		dev_notice(&bus->spi->dev, "firmware version %*ph mismatch\n",
			   (int)sizeof(version), version);
		return -EBADMSG;
	}
	return ret;
}

int nt36536_load_turner_firmware(struct spi_device *spi)
{
	/* The partition layout below is verified for this production image. */
	static const u8 digest[] = {
		0xfa, 0x5a, 0xa6, 0xf3, 0x0c, 0x9f, 0xac, 0xfb, 0xa4, 0x29, 0x8e, 0x6e, 0x6b, 0x80, 0x33, 0xf6,
		0x88, 0x5e, 0xef, 0x8a, 0x58, 0xeb, 0x2f, 0x75, 0x77, 0x66, 0x07, 0xd8, 0x39, 0x24, 0x16, 0x5f,
	};
	struct nvt_segment segments[4];
	struct nvt_bus bus = { .spi = spi };
	u8 actual[SHA256_DIGEST_SIZE];
	const struct firmware *fw;
	u32 event;
	int ret, i;
	size_t max = min(spi_max_transfer_size(spi), spi_max_message_size(spi));

	if (max <= 128)
		return -EMSGSIZE;
	ret = request_firmware(&fw, FW_NAME, &spi->dev);
	if (ret)
		return ret;
	if (fw->size != 253952) {
		ret = -EBADMSG;
		goto out;
	}
	sha256(fw->data, fw->size, actual);
	if (memcmp(digest, actual, sizeof(digest))) {
		ret = -EBADMSG;
		goto out;
	}
	for (i = 0; i < 4; i++) {
		const u8 *entry = fw->data + (i < 2 ? 0x220 + 16 * i : 0xf0 + 256 * (i - 2));

		segments[i].source = get_unaligned_le32(entry + (i < 2 ? 0 : 8));
		segments[i].addr = get_unaligned_le32(entry + (i < 2 ? 4 : 0));
		segments[i].last = get_unaligned_le32(entry + (i < 2 ? 8 : 4));
		segments[i].crc = get_unaligned_le32(entry + 12);
		dev_notice(&spi->dev, "segment %d addr=%#x last=%#x crc=%#x source=%#x\n",
			   i, segments[i].addr, segments[i].last, segments[i].crc,
			   segments[i].source);
	}
	/* Keep subsequent commands at the same offset within a 128-byte page. */
	bus.chunk = round_down(min_t(size_t, CHUNK, max - 1), 128);
	bus.tx = kmalloc(bus.chunk + 1, GFP_KERNEL);
	bus.rx = kmalloc(32, GFP_KERNEL);
	if (!bus.tx || !bus.rx) {
		ret = -ENOMEM;
		goto out;
	}
	ret = identify(&bus, &event);
	if (!ret) {
		for (i = 0; i < 3; i++) {
			ret = download(&bus, fw, segments, event);
			if (!ret)
				break;
		}
	}
	/* The stock resume path leaves the firmware in REK (0xa0) and then
	 * brings the pipeline up once the event page reports that state.
	 */
	if (!ret)
		ret = wait_value(&bus, event + 0x60, 0xa0, 0xaf, 51, 10000);
	if (!ret)
		dev_info(&spi->dev, "CSOT firmware ready, event page %#x\n", event);
out:
	kfree(bus.rx);
	kfree(bus.tx);
	release_firmware(fw);
	return ret;
}
MODULE_FIRMWARE(FW_NAME);
