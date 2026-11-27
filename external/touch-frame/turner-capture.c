// SPDX-License-Identifier: GPL-2.0-only
/* Capture the NT36536E event window, optionally loading CSOT firmware at probe. */
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/wait.h>
#include <linux/timekeeping.h>
#include <linux/unaligned.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>

#include "nt36536-fw.h"

static bool load_firmware;
module_param(load_firmware, bool, 0444);
MODULE_PARM_DESC(load_firmware, "Load the Turner CSOT SRAM firmware before capture");

#define PIPELINE_BYTES		2
#define CONTROLLER_BYTES	256
#define MAX_PAYLOAD		7934

/* The stock module's fallback is used until the running firmware reports its size. */
static unsigned int payload_bytes = MAX_PAYLOAD;
module_param(payload_bytes, uint, 0444);
MODULE_PARM_DESC(payload_bytes, "Firmware event payload length, excluding controller and pipeline bytes");

static unsigned int burst_frames = 16;
module_param(burst_frames, uint, 0444);
MODULE_PARM_DESC(burst_frames, "Number of IRQ-triggered transfers per burst (1..256)");

struct frame_device {
	struct spi_device *spi;
	struct mutex lock;
	struct dentry *frame, *burst, *stream_file;
	struct frame_stream *active;
	struct kref ref;
	bool removing;
};

struct capture {
	size_t size;
	u8 bytes[];
};

struct transfer_state {
	struct frame_device *dev;
	struct capture *capture;
	u8 *tx, *rx;
	size_t len;
	struct completion done;
	unsigned int frames;
	int error;
	u64 timestamp;
};

static int read_event(struct transfer_state *state)
{
	static bool logged;

	struct spi_transfer xfer = {
		.len = state->len,
		.bits_per_word = 8,
		.tx_buf = state->tx,
		.rx_buf = state->rx,
	};
	struct spi_message msg;
	int ret;

	spi_message_init_with_transfers(&msg, &xfer, 1);
	ret = spi_sync(state->dev->spi, &msg);
	if (!ret && msg.actual_length != state->len)
		ret = -EIO;
	if (!ret && nt36536_read_shift) {
		u8 shift = nt36536_read_shift;
		size_t i;

		for (i = state->len - 1; i > 0; i--)
			state->rx[i] = (state->rx[i] >> shift) |
				       (state->rx[i - 1] << (8 - shift));
		state->rx[0] >>= shift;
	}
	if (!ret && !logged) {
		u8 *head = state->rx + PIPELINE_BYTES + CONTROLLER_BYTES;

		logged = true;
		dev_notice(&state->dev->spi->dev,
			   "frame len=%zu head=%*ph count=%#x csum=%#x/%#x icount=%#x\n",
			   state->len, 20, head, get_unaligned_le32(head + 8),
			   get_unaligned_le16(head + 4), get_unaligned_le16(head + 12),
			   get_unaligned_le32(head + 16));
	}
	return ret;
}

static irqreturn_t burst_irq(int irq, void *data)
{
	struct transfer_state *state = data;

	state->timestamp = ktime_get_ns();
	return IRQ_WAKE_THREAD;
}

static irqreturn_t burst_thread(int irq, void *data)
{
	struct transfer_state *state = data;
	u8 *record;

	if (state->frames == burst_frames || state->error)
		return IRQ_HANDLED;
	state->error = read_event(state);
	if (!state->error) {
		record = state->capture->bytes + 16 + state->frames * (8 + state->len);
		put_unaligned_le64(state->timestamp, record);
		memcpy(record + 8, state->rx, state->len);
		state->frames++;
	}
	if (state->error || state->frames == burst_frames)
		complete(&state->done);
	return IRQ_HANDLED;
}

static int capture_burst(struct transfer_state *state)
{
	int irq = state->dev->spi->irq;
	u8 *header = state->capture->bytes;
	long waited;
	int ret;

	memcpy(header, "NVTBUR01", 8);
	put_unaligned_le32(state->len, header + 8);
	put_unaligned_le32(burst_frames, header + 12);
	init_completion(&state->done);
	ret = request_threaded_irq(irq, burst_irq, burst_thread, IRQF_ONESHOT,
				   "turner-touch-burst", state);
	if (ret)
		return ret;
	waited = wait_for_completion_interruptible_timeout(&state->done, 10 * HZ);
	free_irq(irq, state);
	if (waited < 0)
		return waited;
	return waited ? state->error : -ETIMEDOUT;
}

static int open_capture(struct inode *inode, struct file *file, bool burst)
{
	struct frame_device *dev = inode->i_private;
	struct transfer_state state = {
		.dev = dev,
		.len = PIPELINE_BYTES + CONTROLLER_BYTES + payload_bytes,
	};
	size_t size = burst ? 16 + burst_frames * (8 + state.len) : state.len;
	int ret = -ENOMEM;

	if (!mutex_trylock(&dev->lock))
		return -EBUSY;
	if (dev->active || dev->removing) {
		ret = dev->removing ? -ENODEV : -EBUSY;
		goto out;
	}
	state.capture = kvmalloc(sizeof(*state.capture) + size, GFP_KERNEL);
	state.tx = kzalloc(state.len, GFP_KERNEL);
	state.rx = kmalloc(state.len, GFP_KERNEL);
	if (!state.capture || !state.tx || !state.rx)
		goto out;
	state.capture->size = size;
	if (burst) {
		ret = capture_burst(&state);
	} else {
		ret = read_event(&state);
		if (!ret)
			memcpy(state.capture->bytes, state.rx, state.len);
	}
	if (!ret) {
		file->private_data = state.capture;
		state.capture = NULL;
	}
out:
	kvfree(state.capture);
	kfree(state.tx);
	kfree(state.rx);
	mutex_unlock(&dev->lock);
	return ret;
}

static int frame_open(struct inode *inode, struct file *file)
{
	return open_capture(inode, file, false);
}

static int burst_open(struct inode *inode, struct file *file)
{
	return open_capture(inode, file, true);
}

static ssize_t frame_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	struct capture *capture = file->private_data;

	return simple_read_from_buffer(buf, count, pos, capture->bytes, capture->size);
}

static int frame_release(struct inode *inode, struct file *file)
{
	kvfree(file->private_data);
	return 0;
}

static const struct file_operations frame_fops = {
	.owner = THIS_MODULE,
	.open = frame_open,
	.read = frame_read,
	.release = frame_release,
};

static const struct file_operations burst_fops = {
	.owner = THIS_MODULE,
	.open = burst_open,
	.read = frame_read,
	.release = frame_release,
};

/* Each queued record is a standalone one-frame NVTBUR01 capture. */
struct frame_stream {
	struct transfer_state xfer;
	struct kfifo fifo;
	wait_queue_head_t wait;
	struct mutex reader;
	int error;
};

static void drop_device(struct kref *ref)
{
	kfree(container_of(ref, struct frame_device, ref));
}

static irqreturn_t stream_irq(int irq, void *data)
{
	struct frame_stream *stream = data;

	stream->xfer.timestamp = ktime_get_ns();
	return IRQ_WAKE_THREAD;
}

static irqreturn_t stream_thread(int irq, void *data)
{
	struct frame_stream *stream = data;
	struct transfer_state *xfer = &stream->xfer;
	u8 header[24] = "NVTBUR01";
	int ret;

	if (READ_ONCE(stream->error))
		return IRQ_HANDLED;
	if (kfifo_avail(&stream->fifo) < sizeof(header) + xfer->len) {
		ret = -EOVERFLOW;
	} else {
		ret = read_event(xfer);
		if (!ret) {
			put_unaligned_le32(xfer->len, header + 8);
			put_unaligned_le32(1, header + 12);
			put_unaligned_le64(xfer->timestamp, header + 16);
			kfifo_in(&stream->fifo, header, sizeof(header));
			kfifo_in(&stream->fifo, xfer->rx, xfer->len);
		}
	}
	if (ret)
		WRITE_ONCE(stream->error, ret);
	wake_up_interruptible(&stream->wait);
	return IRQ_HANDLED;
}

static void free_stream(struct frame_stream *stream)
{
	kfree(stream->xfer.rx);
	kfree(stream->xfer.tx);
	kfifo_free(&stream->fifo);
	kfree(stream);
}

static int capture_stream_open(struct inode *inode, struct file *file)
{
	struct frame_device *dev = inode->i_private;
	struct frame_stream *stream;
	size_t len = PIPELINE_BYTES + CONTROLLER_BYTES + payload_bytes;
	int ret;

	if (!mutex_trylock(&dev->lock))
		return -EBUSY;
	if (dev->removing || dev->active) {
		ret = dev->removing ? -ENODEV : -EBUSY;
		goto unlock;
	}
	stream = kzalloc(sizeof(*stream), GFP_KERNEL);
	if (!stream) {
		ret = -ENOMEM;
		goto unlock;
	}
	stream->xfer.dev = dev;
	stream->xfer.len = len;
	init_waitqueue_head(&stream->wait);
	mutex_init(&stream->reader);
	ret = kfifo_alloc(&stream->fifo, 8 * (24 + len), GFP_KERNEL);
	if (ret)
		goto free;
	stream->xfer.tx = kzalloc(len, GFP_KERNEL);
	stream->xfer.rx = kmalloc(len, GFP_KERNEL);
	if (!stream->xfer.tx || !stream->xfer.rx) {
		ret = -ENOMEM;
		goto free;
	}
	ret = request_threaded_irq(dev->spi->irq, stream_irq, stream_thread,
				   IRQF_ONESHOT, "turner-touch-stream", stream);
	if (ret)
		goto free;
	kref_get(&dev->ref);
	dev->active = stream;
	file->private_data = stream;
	ret = stream_open(inode, file);
	goto unlock;
free:
	free_stream(stream);
unlock:
	mutex_unlock(&dev->lock);
	return ret;
}

static ssize_t stream_read(struct file *file, char __user *buf, size_t size, loff_t *pos)
{
	struct frame_stream *stream = file->private_data;
	unsigned int copied;
	int ret;

	if (!size)
		return 0;
	ret = mutex_lock_interruptible(&stream->reader);
	if (ret)
		return ret;
	if (kfifo_is_empty(&stream->fifo) && !READ_ONCE(stream->error) &&
	    (file->f_flags & O_NONBLOCK)) {
		ret = -EAGAIN;
		goto out;
	}
	ret = wait_event_interruptible(stream->wait,
			!kfifo_is_empty(&stream->fifo) || READ_ONCE(stream->error));
	if (ret)
		goto out;
	if (kfifo_is_empty(&stream->fifo)) {
		ret = READ_ONCE(stream->error);
		goto out;
	}
	ret = kfifo_to_user(&stream->fifo, buf, size, &copied);
	if (copied)
		ret = copied;
out:
	mutex_unlock(&stream->reader);
	return ret;
}

static void stop_stream(struct frame_device *dev)
{
	struct frame_stream *stream = dev->active;

	if (stream) {
		free_irq(dev->spi->irq, stream);
		WRITE_ONCE(stream->error, -ENODEV);
		wake_up_interruptible(&stream->wait);
		dev->active = NULL;
	}
}

static int stream_release(struct inode *inode, struct file *file)
{
	struct frame_stream *stream = file->private_data;
	struct frame_device *dev = stream->xfer.dev;

	mutex_lock(&dev->lock);
	if (dev->active == stream)
		stop_stream(dev);
	mutex_unlock(&dev->lock);
	free_stream(stream);
	kref_put(&dev->ref, drop_device);
	return 0;
}

static const struct file_operations stream_fops = {
	.owner = THIS_MODULE,
	.open = capture_stream_open,
	.read = stream_read,
	.release = stream_release,
};

static int turner_frame_probe(struct spi_device *spi)
{
	struct frame_device *dev;

	if (!payload_bytes || payload_bytes > MAX_PAYLOAD || !burst_frames || burst_frames > 256)
		return -EINVAL;
	if (load_firmware) {
		int ret = nt36536_load_turner_firmware(spi);

		if (ret)
			return dev_err_probe(&spi->dev, ret, "CSOT SRAM download failed\n");
	}
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	kref_init(&dev->ref);
	dev->spi = spi;
	mutex_init(&dev->lock);
	dev->frame = debugfs_create_file("turner-touch-frame", 0400, NULL, dev, &frame_fops);
	if (IS_ERR(dev->frame)) {
		int err = PTR_ERR(dev->frame);

		kref_put(&dev->ref, drop_device);
		return err;
	}
	if (spi->irq > 0) {
		dev->burst = debugfs_create_file("turner-touch-burst", 0400, NULL, dev, &burst_fops);
		if (IS_ERR(dev->burst)) {
			int err = PTR_ERR(dev->burst);

			debugfs_remove(dev->frame);
			kref_put(&dev->ref, drop_device);
			return err;
		}
		dev->stream_file = debugfs_create_file("turner-touch-stream", 0400, NULL, dev, &stream_fops);
		if (IS_ERR(dev->stream_file)) {
			int err = PTR_ERR(dev->stream_file);

			debugfs_remove(dev->burst);
			debugfs_remove(dev->frame);
			kref_put(&dev->ref, drop_device);
			return err;
		}
	}
	spi_set_drvdata(spi, dev);
	return 0;
}

static void turner_frame_remove(struct spi_device *spi)
{
	struct frame_device *dev = spi_get_drvdata(spi);

	mutex_lock(&dev->lock);
	dev->removing = true;
	stop_stream(dev);
	mutex_unlock(&dev->lock);
	debugfs_remove(dev->stream_file);
	debugfs_remove(dev->burst);
	debugfs_remove(dev->frame);
	kref_put(&dev->ref, drop_device);
}

static const struct of_device_id turner_frame_match[] = {
	{ .compatible = "novatek,nt36536e" },
	{}
};
MODULE_DEVICE_TABLE(of, turner_frame_match);

static const struct spi_device_id turner_frame_ids[] = {
	{ "nt36536e" },
	{}
};
MODULE_DEVICE_TABLE(spi, turner_frame_ids);

static struct spi_driver turner_frame_driver = {
	.driver = { .name = "turner-touch-frame", .of_match_table = turner_frame_match },
	.id_table = turner_frame_ids,
	.probe = turner_frame_probe,
	.remove = turner_frame_remove,
};
module_spi_driver(turner_frame_driver);

MODULE_DESCRIPTION("Turner NT36536E event capture and optional SRAM firmware loader");
MODULE_LICENSE("GPL");
