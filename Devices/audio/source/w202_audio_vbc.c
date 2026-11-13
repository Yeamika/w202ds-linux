// SPDX-License-Identifier: GPL-2.0-only
/*
 * W202DS AP01 paths through AUDCP VBC:
 * playback  AP01 PCM -> VBC -> master IIS0 -> AW88166,
 * capture   UMP9620 MIC1 -> AGCP digital codec -> VBC ADC0 (IIS1) -> AP01 record FIFO.
 */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "w202_audio.h"

#define W202_PLY_FIFO_CONFIG	0x20
#define W202_RCD_FIFO_CONFIG	0x24
#define W202_SRC_MODE		0x28
#define W202_FIFO_ENABLE	0x2c
#define W202_FIFO_CLEAR		0x30
#define W202_DMA_ENABLE		0x50
#define W202_FIFO_FORMAT	GENMASK(10, 9)
#define W202_PLY_WATERMARK	GENMASK(7, 0)	/* empty level */
#define W202_RCD_WATERMARK	GENMASK(23, 16)	/* full level */
#define W202_RCD_SRC_MODE	GENMASK(7, 4)
#define W202_RCD_SRC_ENABLE	(BIT(2) | BIT(3))
#define W202_FIFO_L16		3		/* VBC_DAT_L16 */

/*
 * AGCP AHB 0x3c, arch_audio_iis_to_audio_top_enable_v2 0x11f288: ag_iis1 source,
 * bits 3:2, 0 pad_top, 1 aud_4ad_iis0_ad0 (ODM codec_c route).
 */
#define W202_AGCP_IIS_EXT_SEL	0x3c
#define W202_AG_IIS1_SEL	GENMASK(3, 2)
#define W202_AG_IIS1_CODEC_AD0	1

/* REA vbc-v4 ap_vbc_fifo_enable/aud_dma_chn_en/fifo_clear, AP01 FIFO ids 0 and 1. */
struct w202_vbc_fifo {
	u32 enable;
	u32 dma;
	u32 clear;
};

static const struct w202_vbc_fifo w202_vbc_fifos[] = {
	[SNDRV_PCM_STREAM_PLAYBACK] = { BIT(8) | BIT(9), BIT(0) | BIT(1), BIT(2) },
	[SNDRV_PCM_STREAM_CAPTURE] = { BIT(12) | BIT(13), BIT(2) | BIT(3), BIT(3) },
};

struct w202_vbc {
	struct device *dev;
	struct w202_audio_dsp *dsp;
	void __iomem *regs;
	struct regmap *agcp_ahb;
	spinlock_t lock;
	struct w202_pcm_dma_params dma[2];
	struct clk_bulk_data clocks[3];
	const struct firmware *profile;
	u32 profile_mode;
	bool opened[2];
};

/*
 * REA: fill_dsp_startup_data, factory defaults, ODM only_smart_spk_p and
 * only_codec_c. The factory keeps one VBC state for both directions, so both
 * carry the main-mic ADC routing: ADC0/1/2 from IIS1/IIS2/IIS1, RX0-2 24-bit.
 */
static void w202_vbc_startup_params(struct w202_dsp_startup *p, u32 stream)
{
	static const u32 tx_port[] = { 0, 0, 2 };
	static const u32 rx_port[] = { 1, 2, 1, 3 };
	static const u32 port_do[] = { 0, 1, 2, 2, 0 };
	int i;

	memset(p, 0, sizeof(*p));
	p->stream.stream = cpu_to_le32(stream);
	for (i = 0; i < ARRAY_SIZE(p->adc_source); i++)
		p->adc_source[i].id = cpu_to_le32(i);
	for (i = 0; i < ARRAY_SIZE(p->dac_out); i++)
		p->dac_out[i].id = cpu_to_le32(i);
	for (i = 0; i < ARRAY_SIZE(p->mux_tx); i++) {
		p->mux_tx[i].id = cpu_to_le32(i);
		p->mux_tx[i].value = cpu_to_le32(tx_port[i]);
		p->tx_width[i].id = cpu_to_le32(i);
		p->tx_width[i].value = cpu_to_le32(i < 2); /* 24-bit IIS0/1 */
		p->tx_lr[i].id = cpu_to_le32(i);
		p->tx_lr[i].value = cpu_to_le32(i == 0); /* RIGHT_HIGH on TX0 */
	}
	for (i = 0; i < ARRAY_SIZE(p->mux_rx); i++) {
		p->mux_rx[i].id = cpu_to_le32(i);
		p->mux_rx[i].value = cpu_to_le32(rx_port[i]);
		p->rx_width[i].id = cpu_to_le32(i);
		p->rx_width[i].value = cpu_to_le32(i < 3);
		p->rx_lr[i].id = cpu_to_le32(i);
	}
	for (i = 0; i < ARRAY_SIZE(p->iis_do); i++) {
		p->iis_do[i].id = cpu_to_le32(i);
		p->iis_do[i].value = cpu_to_le32(port_do[i]);
	}
	p->master[0] = cpu_to_le32(1); /* reload */
	p->master[1] = cpu_to_le32(1); /* enable */
	for (i = 0; i < ARRAY_SIZE(p->master_select); i++) {
		p->master_select[i].id = cpu_to_le32(i);
		p->master_select[i].value = cpu_to_le32(i == 0); /* internal */
	}
	for (i = 0; i < ARRAY_SIZE(p->mute_dg); i++)
		p->mute_dg[i][1] = cpu_to_le32(i);
	p->smooth_dg[2] = cpu_to_le32(0x1000);
	p->smooth_dg[3] = cpu_to_le32(0x1000);
	p->smooth_dg[5] = cpu_to_le32(8);
	for (i = 0; i < ARRAY_SIZE(p->mixer_dg); i++) {
		p->mixer_dg[i][1] = cpu_to_le32(i);
		p->mixer_dg[i][2] = cpu_to_le32(i);
		p->mixer_dg[i][3] = cpu_to_le32(0x1000);
		p->mixer_dg[i][4] = cpu_to_le32(0x1000);
		p->mixer_dg[i][5] = cpu_to_le32(i);
		p->mixer_dg[i][6] = cpu_to_le32(0x1000);
		p->mixer_dg[i][7] = cpu_to_le32(0x1000);
	}
	p->mixer_dg_step = cpu_to_le32(8);
	for (i = 0; i < ARRAY_SIZE(p->mixer); i++)
		p->mixer[i][1] = cpu_to_le32(i);
	p->ivsense[1] = cpu_to_le32(1);
	p->voice_record_type = cpu_to_le16(1);
}

static void w202_vbc_update(struct w202_vbc *vbc, u32 reg, u32 mask, u32 value)
{
	writel((readl(vbc->regs + reg) & ~mask) | value, vbc->regs + reg);
}

/*
 * Factory scene_normal_startup: playback carries the HAL route controls and
 * speaker profile before cmd 4; capture sends only dsp_startup(scene 0, stream 1).
 * The HAL applies the codec_c route (ag_iis1 from the codec ADC0 IIS) before
 * opening a capture stream; without it the record FIFO never requests DMA.
 */
static int w202_vbc_startup(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct w202_vbc *vbc = snd_soc_dai_get_drvdata(dai);
	int stream = substream->stream;
	struct w202_dsp_startup params;
	/* KCTL 0x26: master width. KCTL 3: {startup_reload, id, left, right}. */
	__le32 width = cpu_to_le32(1);
	__le32 dg[] = { 0, 0, cpu_to_le32(0x19), cpu_to_le32(0x19) };
	int ret;

	if (vbc->opened[stream])
		return -EBUSY;
	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = w202_audio_dsp_control(vbc->dsp, 0x26, &width, sizeof(width));
		if (ret)
			return ret;
		ret = w202_audio_dsp_profile(vbc->dsp, vbc->profile_mode,
				vbc->profile->data + 16, W202_DSP_PROFILE_BYTES);
		if (ret)
			return ret;
		ret = w202_audio_dsp_control(vbc->dsp, 3, dg, sizeof(dg));
		if (ret)
			return ret;
	} else {
		ret = regmap_update_bits(vbc->agcp_ahb, W202_AGCP_IIS_EXT_SEL, W202_AG_IIS1_SEL,
				FIELD_PREP(W202_AG_IIS1_SEL, W202_AG_IIS1_CODEC_AD0));
		if (ret)
			return ret;
	}
	w202_vbc_startup_params(&params, stream);
	ret = w202_audio_dsp_command(vbc->dsp, W202_DSP_CMD_STARTUP, stream,
				     &params, sizeof(params));
	if (ret)
		goto route_off;
	vbc->opened[stream] = true;
	snd_soc_dai_set_dma_data(dai, substream, &vbc->dma[stream]);
	return 0;
route_off:
	if (stream == SNDRV_PCM_STREAM_CAPTURE)
		regmap_update_bits(vbc->agcp_ahb, W202_AGCP_IIS_EXT_SEL, W202_AG_IIS1_SEL, 0);
	return ret;
}

/* Factory ap_hw_params: AP01 FIFO watermark 80, VBC_DAT_L16, record SRC mode 0 (48 kHz). */
static int w202_vbc_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct w202_vbc *vbc = snd_soc_dai_get_drvdata(dai);
	unsigned long flags;

	if (params_rate(params) != 48000 || params_channels(params) != 2 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S16_LE)
		return -EINVAL;
	if (!w202_audio_dsp_ready(vbc->dsp))
		return -EIO;
	spin_lock_irqsave(&vbc->lock, flags);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		w202_vbc_update(vbc, W202_PLY_FIFO_CONFIG, W202_FIFO_FORMAT | W202_PLY_WATERMARK,
			FIELD_PREP(W202_FIFO_FORMAT, W202_FIFO_L16) |
			FIELD_PREP(W202_PLY_WATERMARK, W202_PCM_FRAGMENT));
	} else {
		w202_vbc_update(vbc, W202_RCD_FIFO_CONFIG, W202_FIFO_FORMAT | W202_RCD_WATERMARK,
			FIELD_PREP(W202_FIFO_FORMAT, W202_FIFO_L16) |
			FIELD_PREP(W202_RCD_WATERMARK, W202_PCM_FRAGMENT));
		/* vbc_phy_audply_set_src_mode(1, 0) */
		w202_vbc_update(vbc, W202_SRC_MODE, W202_RCD_SRC_MODE, 0);
		w202_vbc_update(vbc, W202_FIFO_ENABLE, W202_RCD_SRC_ENABLE, W202_RCD_SRC_ENABLE);
	}
	spin_unlock_irqrestore(&vbc->lock, flags);
	/* Normal AP01 configures AP FIFOs; factory scene_normal_hw_params sends no cmd 6. */
	return 0;
}

/* Factory ap_trigger then DSP trigger; stop only gates the AP FIFO and DMA request. */
static int w202_vbc_trigger(struct snd_pcm_substream *substream, int cmd,
			    struct snd_soc_dai *dai)
{
	struct w202_vbc *vbc = snd_soc_dai_get_drvdata(dai);
	const struct w202_vbc_fifo *fifo = &w202_vbc_fifos[substream->stream];
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&vbc->lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (!w202_audio_dsp_ready(vbc->dsp)) {
			ret = -EIO;
			break;
		}
		w202_vbc_update(vbc, W202_FIFO_CLEAR, fifo->clear, fifo->clear);
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			/* ap_vbc_fifo_pre_fill */
			writel(0, vbc->regs);
			writel(0, vbc->regs + 4);
		}
		w202_vbc_update(vbc, W202_FIFO_ENABLE, fifo->enable, fifo->enable);
		w202_vbc_update(vbc, W202_DMA_ENABLE, fifo->dma, fifo->dma);
		ret = w202_audio_dsp_trigger(vbc->dsp, substream->stream);
		if (!ret)
			break;
		fallthrough;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		w202_vbc_update(vbc, W202_DMA_ENABLE, fifo->dma, 0);
		w202_vbc_update(vbc, W202_FIFO_ENABLE, fifo->enable, 0);
		break;
	default:
		ret = -EINVAL;
	}
	spin_unlock_irqrestore(&vbc->lock, flags);
	return ret;
}

/* Factory dsp_shutdown(scene 0, stream): cmd 5 with scene/stream and zero ids. */
static void w202_vbc_shutdown(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct w202_vbc *vbc = snd_soc_dai_get_drvdata(dai);
	int stream = substream->stream;
	struct w202_dsp_startup params = {
		.stream.stream = cpu_to_le32(stream),
	};
	int ret;

	if (!vbc->opened[stream])
		return;
	ret = w202_audio_dsp_command(vbc->dsp, W202_DSP_CMD_SHUTDOWN, stream,
				     &params, sizeof(params));
	if (ret)
		dev_warn(vbc->dev, "DSP stream %d shutdown failed: %d\n", stream, ret);
	vbc->opened[stream] = false;
	/* codec_c route off: ag_iis1 back to pad_top. */
	if (stream == SNDRV_PCM_STREAM_CAPTURE)
		regmap_update_bits(vbc->agcp_ahb, W202_AGCP_IIS_EXT_SEL, W202_AG_IIS1_SEL, 0);
}

static const struct snd_soc_dai_ops w202_vbc_dai_ops = {
	.startup = w202_vbc_startup,
	.shutdown = w202_vbc_shutdown,
	.hw_params = w202_vbc_hw_params,
	.trigger = w202_vbc_trigger,
};

/* Separate DAIs keep the speaker link playback-only and the mic link capture-only. */
static struct snd_soc_dai_driver w202_vbc_dais[] = {
	{
		.name = "w202-vbc-ap01",
		.playback = {
			.stream_name = "AP01 Playback",
			.channels_min = 2, .channels_max = 2,
			.rates = SNDRV_PCM_RATE_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &w202_vbc_dai_ops,
	}, {
		.name = "w202-vbc-ap01-capture",
		.capture = {
			.stream_name = "AP01 Capture",
			.channels_min = 2, .channels_max = 2,
			.rates = SNDRV_PCM_RATE_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &w202_vbc_dai_ops,
	},
};

static const struct snd_soc_component_driver w202_vbc_component = { .name = "w202-vbc" };

static void w202_vbc_clocks_off(void *data)
{
	struct w202_vbc *vbc = data;

	clk_bulk_disable_unprepare(ARRAY_SIZE(vbc->clocks), vbc->clocks);
}

static void w202_vbc_release_profile(void *data)
{
	release_firmware(data);
}

static int w202_vbc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct w202_vbc *vbc;
	struct resource *res;
	struct pinctrl *pins;
	const __le32 *header;
	int ret;

	vbc = devm_kzalloc(dev, sizeof(*vbc), GFP_KERNEL);
	if (!vbc)
		return -ENOMEM;
	vbc->dev = dev;
	spin_lock_init(&vbc->lock);
	vbc->dsp = w202_audio_dsp_get(dev);
	if (IS_ERR(vbc->dsp))
		return dev_err_probe(dev, PTR_ERR(vbc->dsp), "AUDCP dependency\n");
	ret = firmware_request_nowarn(&vbc->profile, "w202ds/vbc-speaker.bin", dev);
	if (ret)
		return dev_err_probe(dev, ret, "factory speaker profile\n");
	ret = devm_add_action_or_reset(dev, w202_vbc_release_profile, (void *)vbc->profile);
	if (ret)
		return ret;
	if (vbc->profile->size != 16 + W202_DSP_PROFILE_BYTES ||
	    memcmp(vbc->profile->data, "W202VBC\0", 8))
		return dev_err_probe(dev, -EINVAL, "invalid speaker profile header\n");
	header = (const __le32 *)vbc->profile->data;
	if (le32_to_cpu(header[3]) != W202_DSP_PROFILE_BYTES)
		return -EINVAL;
	vbc->profile_mode = le32_to_cpu(header[2]);
	vbc->clocks[0].id = "vbc";
	vbc->clocks[1].id = "vbc-24m";
	vbc->clocks[2].id = "vbc-ap";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(vbc->clocks), vbc->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "VBC clocks\n");
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(vbc->clocks), vbc->clocks);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, w202_vbc_clocks_off, vbc);
	if (ret)
		return ret;
	pins = devm_pinctrl_get_select(dev, "vbc_iism0_3");
	if (IS_ERR(pins))
		return dev_err_probe(dev, PTR_ERR(pins), "speaker SYS_IIS3 mux\n");
	vbc->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(vbc->regs))
		return PTR_ERR(vbc->regs);
	vbc->agcp_ahb = syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,syscon-agcp-ahb");
	if (IS_ERR(vbc->agcp_ahb))
		return dev_err_probe(dev, PTR_ERR(vbc->agcp_ahb), "AGCP AHB registers\n");
	/* AUDPLY_FIFO_WR_0/1 at 0x0/0x4, AUDRCD_FIFO_RD_0/1 at 0x8/0xc. */
	vbc->dma[SNDRV_PCM_STREAM_PLAYBACK].fifo[0] = res->start;
	vbc->dma[SNDRV_PCM_STREAM_PLAYBACK].fifo[1] = res->start + 0x4;
	vbc->dma[SNDRV_PCM_STREAM_CAPTURE].fifo[0] = res->start + 0x8;
	vbc->dma[SNDRV_PCM_STREAM_CAPTURE].fifo[1] = res->start + 0xc;
	platform_set_drvdata(pdev, vbc);
	return devm_snd_soc_register_component(dev, &w202_vbc_component, w202_vbc_dais,
					       ARRAY_SIZE(w202_vbc_dais));
}

static const struct of_device_id w202_vbc_match[] = {
	{ .compatible = "zte,w202ds-vbc" }, { }
};
MODULE_DEVICE_TABLE(of, w202_vbc_match);

static struct platform_driver w202_vbc_driver = {
	.probe = w202_vbc_probe,
	.driver = { .name = "w202-audio-vbc", .of_match_table = w202_vbc_match },
};
module_platform_driver(w202_vbc_driver);
MODULE_FIRMWARE("w202ds/vbc-speaker.bin");
MODULE_DESCRIPTION("W202DS AUDCP VBC AP01 speaker and microphone DAIs");
MODULE_LICENSE("GPL");
