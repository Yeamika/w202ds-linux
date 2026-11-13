// SPDX-License-Identifier: GPL-2.0-only
/* Qogirn6pro AUDCP boot and AP/DSP IPC, checked against the factory modules. */
#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox_controller.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>

#include "w202_audio.h"

#define W202_IPC_PARAM_SIZE	0x400
#define W202_IPC_RING_SIZE	64
#define W202_IPC_RING_BYTES	(W202_IPC_RING_SIZE * sizeof(struct w202_dsp_message))
#define W202_IPC_SIZE		(W202_IPC_PARAM_SIZE + 2 * W202_IPC_RING_BYTES + 16)
#define W202_DSP_BOOT_MAGIC	0xbeee
#define W202_DSP_ASSERT		0x25
#define W202_PROFILE_OFFSET	0x1210
#define W202_PROFILE_SIZE	0x1400
#define W202_IPC_MAP_SIZE	(W202_PROFILE_OFFSET + W202_PROFILE_SIZE)

enum w202_dsp_control {
	SYS_SHUTDOWN, CORE_SHUTDOWN, DEEP_SLEEP, CORE_RESET, SYS_RESET,
	RESET_SELECT, BOOT_PROTECT, BOOT_VECTOR, BOOT_ADDRESS,
	SYS_STATUS, CORE_STATUS, SLEEP_STATUS, AP_ACCESS, CONTROL_COUNT,
};

static const char * const w202_dsp_control_names[] = {
	[SYS_SHUTDOWN] = "sysshutdown", [CORE_SHUTDOWN] = "coreshutdown",
	[DEEP_SLEEP] = "deepsleep", [CORE_RESET] = "corereset",
	[SYS_RESET] = "sysreset", [RESET_SELECT] = "reset_sel",
	[BOOT_PROTECT] = "bootprotect", [BOOT_VECTOR] = "bootvector",
	[BOOT_ADDRESS] = "bootaddress_sel", [SYS_STATUS] = "sysstatus",
	[CORE_STATUS] = "corestatus", [SLEEP_STATUS] = "sleepstatus",
	[AP_ACCESS] = "ap-access",
};

struct w202_dsp_reg {
	struct regmap *map;
	u32 offset;
	u32 mask;
};

struct w202_audio_dsp {
	struct device *dev;
	struct generic_pm_domain domain;
	struct w202_dsp_reg control[CONTROL_COUNT];
	void __iomem *firmware;
	phys_addr_t firmware_phys;
	size_t firmware_size;
	void __iomem *ipc;
	phys_addr_t ipc_phys;
	size_t ipc_size;
	void __iomem *aon_iram;
	size_t aon_iram_size;
	struct mbox_client client;
	struct mbox_chan *channel;
	spinlock_t lock;
	struct mutex command_lock;
	struct completion boot_done;
	struct completion reply;
	u16 pending_command;
	bool waiting;
	bool ready;
	bool failed;
	int reply_error;
	u64 doorbell;
	u32 saved_access;
};

static void __iomem *w202_ring(struct w202_audio_dsp *dsp, bool rx)
{
	return dsp->ipc + W202_IPC_PARAM_SIZE + (rx ? W202_IPC_RING_BYTES : 0);
}

static void __iomem *w202_ring_ptr(struct w202_audio_dsp *dsp, bool rx, bool wr)
{
	return dsp->ipc + W202_IPC_PARAM_SIZE + 2 * W202_IPC_RING_BYTES +
		(rx ? 8 : 0) + (wr ? 4 : 0);
}

static void w202_dsp_fail_locked(struct w202_audio_dsp *dsp, int error)
{
	dsp->failed = true;
	dsp->ready = false;
	dsp->reply_error = error;
	complete_all(&dsp->boot_done);
	if (dsp->waiting)
		complete(&dsp->reply);
}

static void w202_dsp_receive(struct mbox_client *client, void *unused)
{
	struct w202_audio_dsp *dsp = container_of(client, struct w202_audio_dsp, client);
	unsigned long flags;
	u32 rd, wr, count;

	spin_lock_irqsave(&dsp->lock, flags);
	rd = readl(w202_ring_ptr(dsp, true, false));
	wr = readl(w202_ring_ptr(dsp, true, true));
	count = wr - rd;
	if (count > W202_IPC_RING_SIZE) {
		w202_dsp_fail_locked(dsp, -EIO);
		goto out;
	}
	while (count--) {
		struct w202_dsp_message msg;
		u16 command, channel;

		memcpy_fromio(&msg, w202_ring(dsp, true) +
			      (rd % W202_IPC_RING_SIZE) * sizeof(msg), sizeof(msg));
		command = le16_to_cpu(msg.command);
		channel = le16_to_cpu(msg.channel);
		if (command == W202_DSP_BOOT_MAGIC && channel == W202_DSP_BOOT_MAGIC &&
		    le32_to_cpu(msg.parameter[0]) == W202_DSP_BOOT_MAGIC && !dsp->failed) {
			dsp->ready = true;
			complete_all(&dsp->boot_done);
		} else if (channel == 2 && command == W202_DSP_ASSERT) {
			w202_dsp_fail_locked(dsp, -EIO);
			dev_err_ratelimited(dsp->dev, "AUDCP assertion: %08x %08x %08x %08x\n",
				le32_to_cpu(msg.parameter[0]), le32_to_cpu(msg.parameter[1]),
				le32_to_cpu(msg.parameter[2]), le32_to_cpu(msg.parameter[3]));
		} else if (dsp->ready && dsp->waiting && !channel &&
			   command == dsp->pending_command) {
			dsp->reply_error = 0;
			dsp->waiting = false;
			complete(&dsp->reply);
		}
		rd++;
	}
	writel(rd, w202_ring_ptr(dsp, true, false));
out:
	spin_unlock_irqrestore(&dsp->lock, flags);
}

/*
 * Caller serializes sends. The matching unisoc-mailbox send_data copies the
 * u64 to hardware or its own software FIFO before returning. Like its SIPC
 * clients, retire that synchronous handoff with mbox_chan_txdone: the
 * controller advertises TXDONE_BY_IRQ, not TXDONE_BY_ACK. This is not a DSP
 * command acknowledgement; those arrive independently through the RX ring.
 */
static int w202_dsp_doorbell(struct w202_audio_dsp *dsp, u64 value)
{
	int ret;

	dsp->doorbell = value;
	ret = mbox_send_message(dsp->channel, &dsp->doorbell);
	if (ret < 0)
		return ret;
	mbox_chan_txdone(dsp->channel, 0);
	return 0;
}

static int w202_dsp_send_locked(struct w202_audio_dsp *dsp,
			       const struct w202_dsp_message *msg)
{
	u32 rd, wr;
	int ret;

	if (!dsp->ready || dsp->failed)
		return -EIO;
	rd = readl(w202_ring_ptr(dsp, false, false));
	wr = readl(w202_ring_ptr(dsp, false, true));
	if (wr - rd >= W202_IPC_RING_SIZE)
		return -EBUSY;
	memcpy_toio(w202_ring(dsp, false) +
		    (wr % W202_IPC_RING_SIZE) * sizeof(*msg), msg, sizeof(*msg));
	writel(wr + 1, w202_ring_ptr(dsp, false, true));
	ret = w202_dsp_doorbell(dsp, (u64)le16_to_cpu(msg->command) << 48 |
			       (u64)le16_to_cpu(msg->channel) << 32 |
			       le32_to_cpu(msg->parameter[0]));
	/* A published message cannot be retracted after a failed doorbell. */
	if (ret)
		w202_dsp_fail_locked(dsp, ret);
	return ret;
}

/* Caller serializes the command and profile shared-memory buffers. */
static int w202_dsp_command_locked(struct w202_audio_dsp *dsp, u16 command,
				  u32 id, u32 stream, const void *data, size_t size)
{
	struct w202_dsp_message msg = {
		.command = cpu_to_le16(command),
		.parameter = { cpu_to_le32(id), cpu_to_le32(stream) },
	};
	unsigned long flags;
	int ret;

	if (!data || size > W202_IPC_PARAM_SIZE)
		return -EINVAL;
	spin_lock_irqsave(&dsp->lock, flags);
	if (!dsp->ready || dsp->failed) {
		ret = -EIO;
		goto unlock;
	}
	/* The factory payload address is 32-bit. */
	msg.parameter[2] = cpu_to_le32(dsp->ipc_phys);
	memset_io(dsp->ipc, 0, W202_IPC_PARAM_SIZE);
	memcpy_toio(dsp->ipc, data, size);
	reinit_completion(&dsp->reply);
	dsp->pending_command = command;
	dsp->waiting = true;
	dsp->reply_error = -ETIMEDOUT;
	ret = w202_dsp_send_locked(dsp, &msg);
	spin_unlock_irqrestore(&dsp->lock, flags);
	if (!ret) {
		if (!wait_for_completion_timeout(&dsp->reply, msecs_to_jiffies(3000))) {
			/* No transaction ID in this protocol: never reuse after timeout. */
			spin_lock_irqsave(&dsp->lock, flags);
			w202_dsp_fail_locked(dsp, -ETIMEDOUT);
			spin_unlock_irqrestore(&dsp->lock, flags);
		}
		ret = READ_ONCE(dsp->reply_error);
	}
	spin_lock_irqsave(&dsp->lock, flags);
	dsp->waiting = false;
unlock:
	spin_unlock_irqrestore(&dsp->lock, flags);
	return ret;
}

int w202_audio_dsp_command(struct w202_audio_dsp *dsp, u16 command, u32 stream,
			   const void *data, size_t size)
{
	int ret;

	mutex_lock(&dsp->command_lock);
	ret = w202_dsp_command_locked(dsp, command, 0, stream, data, size);
	mutex_unlock(&dsp->command_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(w202_audio_dsp_command);

int w202_audio_dsp_control(struct w202_audio_dsp *dsp, u32 id,
			    const void *data, size_t size)
{
	int ret;

	mutex_lock(&dsp->command_lock);
	ret = w202_dsp_command_locked(dsp, W202_DSP_CMD_CONTROL, id, U32_MAX, data, size);
	mutex_unlock(&dsp->command_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(w202_audio_dsp_control);

int w202_audio_dsp_profile(struct w202_audio_dsp *dsp, u32 mode,
			    const void *data, size_t size)
{
	/* aud_send_block_param: id, profile type 1 (DSP VBC), address, length. */
	__le32 param[] = { cpu_to_le32(mode), cpu_to_le32(1),
		cpu_to_le32(dsp->ipc_phys + W202_PROFILE_OFFSET), cpu_to_le32(size) };
	int ret;

	if (!data || size != W202_DSP_PROFILE_BYTES)
		return -EINVAL;
	mutex_lock(&dsp->command_lock);
	if (!w202_audio_dsp_ready(dsp)) {
		ret = -EIO;
		goto out;
	}
	memcpy_toio(dsp->ipc + W202_PROFILE_OFFSET, data, size);
	ret = w202_dsp_command_locked(dsp, W202_DSP_CMD_PROFILE, mode, U32_MAX,
				      param, sizeof(param));
out:
	mutex_unlock(&dsp->command_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(w202_audio_dsp_profile);

/* Factory scene_normal_trigger: aud_send_cmd_no_wait(0, 7, 0, stream, 1, 0). */
int w202_audio_dsp_trigger(struct w202_audio_dsp *dsp, u32 stream)
{
	struct w202_dsp_message msg = {
		.command = cpu_to_le16(W202_DSP_CMD_TRIGGER),
		.parameter = { 0, cpu_to_le32(stream), cpu_to_le32(1) },
	};
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&dsp->lock, flags);
	ret = w202_dsp_send_locked(dsp, &msg);
	spin_unlock_irqrestore(&dsp->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(w202_audio_dsp_trigger);

bool w202_audio_dsp_ready(struct w202_audio_dsp *dsp)
{
	return READ_ONCE(dsp->ready) && !READ_ONCE(dsp->failed);
}
EXPORT_SYMBOL_GPL(w202_audio_dsp_ready);

static void w202_dsp_put(void *data)
{
	put_device(data);
}

struct w202_audio_dsp *w202_audio_dsp_get(struct device *consumer)
{
	struct device_node *node;
	struct platform_device *pdev;
	struct w202_audio_dsp *dsp;
	int ret;

	node = of_parse_phandle(consumer->of_node, "zte,audio-dsp", 0);
	if (!node)
		return ERR_PTR(-EINVAL);
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);
	dsp = platform_get_drvdata(pdev);
	if (!dsp || !w202_audio_dsp_ready(dsp)) {
		put_device(&pdev->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}
	if (!device_link_add(consumer, &pdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER)) {
		put_device(&pdev->dev);
		return ERR_PTR(-ENOMEM);
	}
	ret = devm_add_action_or_reset(consumer, w202_dsp_put, &pdev->dev);
	return ret ? ERR_PTR(ret) : dsp;
}
EXPORT_SYMBOL_GPL(w202_audio_dsp_get);

static int w202_dsp_update(struct w202_audio_dsp *dsp, enum w202_dsp_control id, u32 value)
{
	struct w202_dsp_reg *reg = &dsp->control[id];

	return regmap_update_bits(reg->map, reg->offset, reg->mask,
				value << __ffs(reg->mask));
}

/* PMU power states read 0 once the AGCP system/DSP domain is powered. */
static int w202_dsp_poll(struct w202_audio_dsp *dsp, enum w202_dsp_control id)
{
	struct w202_dsp_reg *reg = &dsp->control[id];
	u32 value;

	return regmap_read_poll_timeout(reg->map, reg->offset, value,
					!(value & reg->mask), 100, 1000000);
}

static int w202_dsp_attach(struct generic_pm_domain *domain, struct device *dev)
{
	return try_module_get(THIS_MODULE) ? 0 : -ENODEV;
}

static void w202_dsp_detach(struct generic_pm_domain *domain, struct device *dev)
{
	module_put(THIS_MODULE);
}

static int w202_dsp_map_reserved(struct device *dev, unsigned int index,
				 u32 offset, size_t length, void __iomem **virt,
				 phys_addr_t *phys, size_t *size)
{
	struct device_node *node = of_parse_phandle(dev->of_node, "memory-region", index);
	struct reserved_mem *mem;

	if (!node)
		return -EINVAL;
	mem = of_reserved_mem_lookup(node);
	of_node_put(node);
	if (!mem || !mem->size || offset >= mem->size)
		return -EINVAL;
	if (!length)
		length = mem->size - offset;
	if (length > mem->size - offset || mem->base + offset + length > (1ULL << 32))
		return -EINVAL;
	*virt = devm_ioremap(dev, mem->base + offset, length);
	if (!*virt)
		return -ENOMEM;
	*phys = mem->base + offset;
	*size = length;
	return 0;
}

static void w202_dsp_stop(struct w202_audio_dsp *dsp)
{
	unsigned long flags;

	spin_lock_irqsave(&dsp->lock, flags);
	w202_dsp_fail_locked(dsp, -ESHUTDOWN);
	spin_unlock_irqrestore(&dsp->lock, flags);
	w202_dsp_update(dsp, CORE_RESET, 1);
	w202_dsp_update(dsp, SYS_RESET, 1);
	w202_dsp_update(dsp, SYS_SHUTDOWN, 1);
	w202_dsp_update(dsp, CORE_SHUTDOWN, 1);
	regmap_update_bits(dsp->control[AP_ACCESS].map,
			   dsp->control[AP_ACCESS].offset,
			   dsp->control[AP_ACCESS].mask, dsp->saved_access);
}

/* Record power, IPC and image state before a failed boot is reset. */
static void w202_dsp_report_boot_timeout(struct w202_audio_dsp *dsp)
{
	struct w202_dsp_message msg;
	u32 status[3];
	u8 image[16];
	int i;

	for (i = 0; i < ARRAY_SIZE(status); i++) {
		struct w202_dsp_reg *reg = &dsp->control[SYS_STATUS + i];

		if (regmap_read(reg->map, reg->offset, &status[i]))
			status[i] = U32_MAX;
		else
			status[i] &= reg->mask;
	}
	memcpy_fromio(image, dsp->firmware, sizeof(image));
	memcpy_fromio(&msg, w202_ring(dsp, true), sizeof(msg));
	dev_err(dsp->dev,
		"AUDCP not ready: rx %u/%u tx %u/%u first %04x/%04x/%08x sys %#x core %#x sleep %#x image %16ph\n",
		readl(w202_ring_ptr(dsp, true, false)), readl(w202_ring_ptr(dsp, true, true)),
		readl(w202_ring_ptr(dsp, false, false)), readl(w202_ring_ptr(dsp, false, true)),
		le16_to_cpu(msg.command), le16_to_cpu(msg.channel), le32_to_cpu(msg.parameter[0]),
		status[0], status[1], status[2], image);
}

static int w202_dsp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct w202_audio_dsp *dsp;
	const struct firmware *fw;
	struct resource *res;
	u32 value, ipc_offset;
	int i, ret;

	static_assert(sizeof(struct w202_dsp_message) == 20);
	static_assert(sizeof(struct w202_dsp_startup) == W202_DSP_STARTUP_BYTES);
	dsp = devm_kzalloc(dev, sizeof(*dsp), GFP_KERNEL);
	if (!dsp)
		return -ENOMEM;
	dsp->dev = dev;
	spin_lock_init(&dsp->lock);
	mutex_init(&dsp->command_lock);
	init_completion(&dsp->boot_done);
	init_completion(&dsp->reply);
	for (i = 0; i < CONTROL_COUNT; i++) {
		u32 args[2];
		struct w202_dsp_reg *reg = &dsp->control[i];

		reg->map = syscon_regmap_lookup_by_phandle_args(dev->of_node,
					w202_dsp_control_names[i], 2, args);
		if (IS_ERR(reg->map))
			return dev_err_probe(dev, PTR_ERR(reg->map), "control %s\n",
					     w202_dsp_control_names[i]);
		reg->offset = args[0];
		reg->mask = args[1];
		if (!reg->mask)
			return -EINVAL;
	}
	ret = w202_dsp_map_reserved(dev, 0, 0, 0, &dsp->firmware, &dsp->firmware_phys,
				    &dsp->firmware_size);
	if (ret)
		return dev_err_probe(dev, ret, "AUDCP firmware reservation\n");
	ret = of_property_read_u32(dev->of_node, "zte,ipc-offset", &ipc_offset);
	if (ret)
		return ret;
	ret = w202_dsp_map_reserved(dev, 1, ipc_offset, W202_IPC_MAP_SIZE,
				    &dsp->ipc, &dsp->ipc_phys, &dsp->ipc_size);
	if (ret)
		return dev_err_probe(dev, ret, "AUDCP IPC reservation\n");
	if (dsp->ipc_size < W202_IPC_SIZE || upper_32_bits(dsp->ipc_phys) ||
	    dsp->firmware_phys + dsp->firmware_size > (1ULL << 32))
		return -EINVAL;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	dsp->aon_iram_size = resource_size(res);
	dsp->aon_iram = devm_ioremap_resource(dev, res);
	if (IS_ERR(dsp->aon_iram))
		return PTR_ERR(dsp->aon_iram);

	ret = request_firmware(&fw, "w202ds/agdsp.bin", dev);
	if (ret)
		return dev_err_probe(dev, ret, "AUDCP firmware\n");
	if (fw->size <= 0x80 || fw->size > dsp->firmware_size ||
	    memcmp(fw->data, "SharkL5_AUDCP_", 13)) {
		release_firmware(fw);
		return dev_err_probe(dev, -EINVAL, "unexpected AUDCP image\n");
	}
	memcpy_toio(dsp->firmware, fw->data, fw->size);
	memset_io(dsp->firmware + fw->size, 0, dsp->firmware_size - fw->size);
	release_firmware(fw);

	/* Register RX before releasing reset so the boot-ready message is kept. */
	memset_io(dsp->ipc, 0, W202_IPC_SIZE);
	dsp->client.dev = dev;
	dsp->client.rx_callback = w202_dsp_receive;
	dsp->channel = mbox_request_channel(&dsp->client, 0);
	if (IS_ERR(dsp->channel))
		return dev_err_probe(dev, PTR_ERR(dsp->channel), "AUDCP mailbox\n");
	ret = regmap_read(dsp->control[AP_ACCESS].map, dsp->control[AP_ACCESS].offset, &value);
	if (ret)
		goto free_mailbox;
	dsp->saved_access = value & dsp->control[AP_ACCESS].mask;

	/* Reset/boot order from sprd_audcp_boot.ko:start_store, DDR mode 0. */
	memset_io(dsp->aon_iram, 0, dsp->aon_iram_size);
#define W202_BOOT_WRITE(id, val) do { \
	ret = w202_dsp_update(dsp, id, val); \
	if (ret) goto stop; \
} while (0)
	W202_BOOT_WRITE(RESET_SELECT, 0);
	W202_BOOT_WRITE(CORE_RESET, 1);
	W202_BOOT_WRITE(SYS_RESET, 1);
	W202_BOOT_WRITE(SYS_SHUTDOWN, 0);
	W202_BOOT_WRITE(CORE_SHUTDOWN, 0);
	W202_BOOT_WRITE(DEEP_SLEEP, 0);
	W202_BOOT_WRITE(BOOT_PROTECT, 0x9620);
	W202_BOOT_WRITE(BOOT_VECTOR, (dsp->firmware_phys + 0x80) >> 1);
	W202_BOOT_WRITE(BOOT_ADDRESS, 1);
	W202_BOOT_WRITE(AP_ACCESS, 1);
	W202_BOOT_WRITE(CORE_RESET, 0);
	W202_BOOT_WRITE(SYS_RESET, 0);
#undef W202_BOOT_WRITE
	/*
	 * AGCP stays powered down until the AUDCP sees a wakeup. Like
	 * agdsp_access_enable, ring doorbell 100 and wait for the PMU to report
	 * the AGCP system and DSP domains on before expecting the boot message.
	 */
	ret = w202_dsp_doorbell(dsp, 100);
	if (ret)
		goto stop;
	ret = w202_dsp_poll(dsp, SYS_STATUS);
	if (!ret)
		ret = w202_dsp_poll(dsp, CORE_STATUS);
	if (ret) {
		w202_dsp_report_boot_timeout(dsp);
		goto stop;
	}
	if (!wait_for_completion_timeout(&dsp->boot_done, msecs_to_jiffies(3000)) ||
	    !w202_audio_dsp_ready(dsp)) {
		w202_dsp_report_boot_timeout(dsp);
		ret = -ETIMEDOUT;
		goto stop;
	}

	/* Keep AP access enabled: clock/DMA consumers cannot take vendor votes. */
	dsp->domain.name = dev_name(dev);
	dsp->domain.flags = GENPD_FLAG_ALWAYS_ON;
	dsp->domain.attach_dev = w202_dsp_attach;
	dsp->domain.detach_dev = w202_dsp_detach;
	ret = pm_genpd_init(&dsp->domain, NULL, false);
	if (ret)
		goto stop;
	ret = of_genpd_add_provider_simple(dev->of_node, &dsp->domain);
	if (ret) {
		pm_genpd_remove(&dsp->domain);
		goto stop;
	}
	platform_set_drvdata(pdev, dsp);
	dev_info(dev, "AUDCP ready, firmware at %pa, IPC at %pa\n",
		 &dsp->firmware_phys, &dsp->ipc_phys);
	return 0;
stop:
	w202_dsp_stop(dsp);
free_mailbox:
	mbox_free_channel(dsp->channel);
	return dev_err_probe(dev, ret, "AUDCP boot failed\n");
}

static void w202_dsp_remove(struct platform_device *pdev)
{
	struct w202_audio_dsp *dsp = platform_get_drvdata(pdev);

	of_genpd_del_provider(pdev->dev.of_node);
	pm_genpd_remove(&dsp->domain);
	w202_dsp_stop(dsp);
	mbox_free_channel(dsp->channel);
}

static const struct of_device_id w202_dsp_match[] = {
	{ .compatible = "zte,w202ds-audio-dsp" },
	{ }
};
MODULE_DEVICE_TABLE(of, w202_dsp_match);

static struct platform_driver w202_dsp_driver = {
	.probe = w202_dsp_probe,
	.remove = w202_dsp_remove,
	.driver = {
		.name = "w202-audio-dsp",
		.of_match_table = w202_dsp_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(w202_dsp_driver);
MODULE_FIRMWARE("w202ds/agdsp.bin");
MODULE_DESCRIPTION("W202DS AUDCP boot and bounded VBC IPC");
MODULE_LICENSE("GPL");
