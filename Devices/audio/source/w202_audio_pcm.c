// SPDX-License-Identifier: GPL-2.0-only
/*
 * W202DS AP01 PCM: independent left/right DMA FIFOs.
 * Uses the linked-list interface of the mainline SPRD DMA controller, as in
 * sound/soc/sprd/sprd-pcm-dma.c (Copyright 2019 Spreadtrum Communications Inc.).
 */
#include <linux/dma-mapping.h>
#include <linux/dma/sprd-dma.h>
#include <linux/dmaengine.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "w202_audio.h"

#define W202_DMA_NODE_BYTES	64
#define W202_DMA_PERIODS		(PAGE_SIZE / W202_DMA_NODE_BYTES)
#define W202_DMA_BUFFER_BYTES	(128 * 1024)
#define W202_PCM_PERIOD_STEP	(W202_PCM_FRAGMENT * 2 * sizeof(s16))

struct w202_pcm_channel {
	struct dma_chan *chan;
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	void *nodes;
	dma_addr_t nodes_dma;
};

struct w202_pcm {
	struct w202_pcm_channel channels[2];
	struct snd_pcm_substream *substream;
	struct w202_pcm_dma_params params;
	bool running;
};

static const struct snd_pcm_hardware w202_pcm_hardware = {
	/* Planar samples: the DMA controller advances one sample per request. */
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_NONINTERLEAVED,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.period_bytes_min = W202_PCM_PERIOD_STEP,
	.period_bytes_max = 32 * 1024,
	.periods_min = 2,
	.periods_max = W202_DMA_PERIODS,
	.buffer_bytes_max = W202_DMA_BUFFER_BYTES,
};

static void w202_pcm_release(struct w202_pcm *pcm)
{
	int i;

	WRITE_ONCE(pcm->running, false);
	for (i = 0; i < ARRAY_SIZE(pcm->channels); i++) {
		struct w202_pcm_channel *c = &pcm->channels[i];

		if (!c->chan)
			continue;
		dmaengine_terminate_sync(c->chan);
		dma_release_channel(c->chan);
		c->chan = NULL;
		c->desc = NULL;
		c->cookie = 0;
	}
}

static int w202_pcm_open(struct snd_soc_component *component,
			 struct snd_pcm_substream *substream)
{
	struct device *dev = component->dev;
	struct w202_pcm *pcm;
	int i, ret;

	snd_soc_set_runtime_hwparams(substream, &w202_pcm_hardware);
	ret = snd_pcm_hw_constraint_step(substream->runtime, 0,
				 SNDRV_PCM_HW_PARAM_PERIOD_BYTES, W202_PCM_PERIOD_STEP);
	if (ret < 0)
		return ret;
	ret = snd_pcm_hw_constraint_step(substream->runtime, 0,
				 SNDRV_PCM_HW_PARAM_BUFFER_BYTES, W202_PCM_PERIOD_STEP);
	if (ret < 0)
		return ret;
	ret = snd_pcm_hw_constraint_integer(substream->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	/* A positive return means that the constraint changed successfully. */
	if (ret < 0)
		return ret;
	pcm = kzalloc(sizeof(*pcm), GFP_KERNEL);
	if (!pcm)
		return -ENOMEM;
	pcm->substream = substream;
	for (i = 0; i < ARRAY_SIZE(pcm->channels); i++) {
		struct w202_pcm_channel *c = &pcm->channels[i];

		c->nodes = dma_alloc_coherent(dev, PAGE_SIZE, &c->nodes_dma, GFP_KERNEL);
		if (!c->nodes) {
			ret = -ENOMEM;
			goto free;
		}
	}
	substream->runtime->private_data = pcm;
	return 0;
free:
	while (i--) {
		struct w202_pcm_channel *c = &pcm->channels[i];

		dma_free_coherent(dev, PAGE_SIZE, c->nodes, c->nodes_dma);
	}
	kfree(pcm);
	return ret;
}

static int w202_pcm_close(struct snd_soc_component *component,
			  struct snd_pcm_substream *substream)
{
	struct w202_pcm *pcm = substream->runtime->private_data;
	int i;

	w202_pcm_release(pcm);
	for (i = 0; i < ARRAY_SIZE(pcm->channels); i++) {
		struct w202_pcm_channel *c = &pcm->channels[i];

		dma_free_coherent(component->dev, PAGE_SIZE, c->nodes, c->nodes_dma);
	}
	kfree(pcm);
	substream->runtime->private_data = NULL;
	return 0;
}

static void w202_pcm_elapsed(void *data)
{
	struct w202_pcm *pcm = data;

	if (READ_ONCE(pcm->running))
		snd_pcm_period_elapsed(pcm->substream);
}

static int w202_pcm_hw_params(struct snd_soc_component *component,
			      struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params)
{
	static const char * const names[2][2] = {
		[SNDRV_PCM_STREAM_PLAYBACK] = { "normal_p_l", "normal_p_r" },
		[SNDRV_PCM_STREAM_CAPTURE] = { "normal_c_l", "normal_c_r" },
	};
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct w202_pcm_dma_params *dma;
	struct w202_pcm *pcm = substream->runtime->private_data;
	int i, ret;

	w202_pcm_release(pcm);
	dma = snd_soc_dai_get_dma_data(snd_soc_rtd_to_cpu(rtd, 0), substream);
	if (!dma || params_channels(params) != 2 || params_rate(params) != 48000 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S16_LE)
		return -EINVAL;
	pcm->params = *dma;
	for (i = 0; i < ARRAY_SIZE(pcm->channels); i++) {
		struct w202_pcm_channel *c = &pcm->channels[i];

		c->chan = dma_request_chan(component->dev, names[substream->stream][i]);
		if (IS_ERR(c->chan)) {
			ret = PTR_ERR(c->chan);
			c->chan = NULL;
			w202_pcm_release(pcm);
			return ret;
		}
	}
	return 0;
}

static int w202_pcm_prepare(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct w202_pcm *pcm = runtime->private_data;
	bool playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	enum dma_transfer_direction dir = playback ? DMA_MEM_TO_DEV : DMA_DEV_TO_MEM;
	size_t bytes = snd_pcm_lib_buffer_bytes(substream) / 2;
	size_t period = snd_pcm_lib_period_bytes(substream) / 2;
	struct scatterlist *sg;
	unsigned int periods;
	int i, j, ret = 0;

	if (!period || bytes % period || bytes % (W202_PCM_FRAGMENT * sizeof(s16)))
		return -EINVAL;
	periods = bytes / period;
	if (periods < 2 || periods > W202_DMA_PERIODS)
		return -EINVAL;
	sg = kcalloc(periods, sizeof(*sg), GFP_KERNEL);
	if (!sg)
		return -ENOMEM;
	WRITE_ONCE(pcm->running, false);
	for (i = 0; i < ARRAY_SIZE(pcm->channels); i++) {
		struct w202_pcm_channel *c = &pcm->channels[i];
		dma_addr_t buffer = runtime->dma_addr + i * bytes;
		/* SPRD DMA takes the fragment length from src_maxburst in both directions. */
		struct dma_slave_config config = {
			.direction = dir,
			.src_addr = playback ? buffer : pcm->params.fifo[i],
			.dst_addr = playback ? pcm->params.fifo[i] : buffer,
			.src_addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES,
			.dst_addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES,
			.src_maxburst = W202_PCM_FRAGMENT * sizeof(s16),
		};
		struct sprd_dma_linklist link = {
			.virt_addr = (unsigned long)c->nodes,
			.phy_addr = c->nodes_dma,
		};
		unsigned long flags = SPRD_DMA_FLAGS(SPRD_DMA_CHN_MODE_NONE,
				SPRD_DMA_NO_TRG, SPRD_DMA_FRAG_REQ, SPRD_DMA_TRANS_INT);

		if (!c->chan) {
			ret = -ENODEV;
			break;
		}
		dmaengine_terminate_sync(c->chan);
		c->desc = NULL;
		c->cookie = 0;
		ret = dmaengine_slave_config(c->chan, &config);
		if (ret)
			break;
		sg_init_table(sg, periods);
		for (j = 0; j < periods; j++) {
			sg_dma_address(&sg[j]) = buffer + j * period;
			sg_dma_len(&sg[j]) = period;
		}
		c->desc = c->chan->device->device_prep_slave_sg(c->chan, sg, periods,
							 dir, flags, &link);
		if (!c->desc) {
			ret = -ENOMEM;
			break;
		}
		/* Notify once per stereo period, never once per channel. */
		if (!i) {
			c->desc->callback = w202_pcm_elapsed;
			c->desc->callback_param = pcm;
		}
	}
	kfree(sg);
	if (ret)
		w202_pcm_release(pcm);
	return ret;
}

static int w202_pcm_sync_stop(struct snd_soc_component *component,
			      struct snd_pcm_substream *substream)
{
	struct w202_pcm *pcm = substream->runtime->private_data;
	int i;

	for (i = 0; i < ARRAY_SIZE(pcm->channels); i++)
		if (pcm->channels[i].chan)
			dmaengine_terminate_sync(pcm->channels[i].chan);
	return 0;
}

static int w202_pcm_trigger(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream, int cmd)
{
	struct w202_pcm *pcm = substream->runtime->private_data;
	int i, ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		for (i = 0; i < ARRAY_SIZE(pcm->channels); i++)
			if (!pcm->channels[i].desc)
				return -EINVAL;
		for (i = 0; i < ARRAY_SIZE(pcm->channels); i++) {
			struct w202_pcm_channel *c = &pcm->channels[i];

			c->cookie = dmaengine_submit(c->desc);
			c->desc = NULL;
			ret = dma_submit_error(c->cookie);
			if (ret)
				goto stop;
		}
		WRITE_ONCE(pcm->running, true);
		for (i = 0; i < ARRAY_SIZE(pcm->channels); i++)
			dma_async_issue_pending(pcm->channels[i].chan);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		ret = 0;
stop:
		WRITE_ONCE(pcm->running, false);
		for (i = 0; i < ARRAY_SIZE(pcm->channels); i++) {
			struct w202_pcm_channel *c = &pcm->channels[i];

			if (c->chan)
				dmaengine_terminate_async(c->chan);
			c->desc = NULL;
		}
		return ret;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t w202_pcm_pointer(struct snd_soc_component *component,
					 struct snd_pcm_substream *substream)
{
	struct w202_pcm *pcm = substream->runtime->private_data;
	size_t bytes = snd_pcm_lib_buffer_bytes(substream) / 2;
	u32 pos[2];
	int i;

	if (!READ_ONCE(pcm->running))
		return 0;
	for (i = 0; i < ARRAY_SIZE(pcm->channels); i++) {
		struct w202_pcm_channel *c = &pcm->channels[i];
		struct dma_tx_state state = { };
		dma_addr_t start = substream->runtime->dma_addr + i * bytes;

		if (dmaengine_tx_status(c->chan, c->cookie, &state) == DMA_ERROR)
			return SNDRV_PCM_POS_XRUN;
		/* SPRD reports the current source address in residue. */
		if (state.residue < start || state.residue > start + bytes)
			return SNDRV_PCM_POS_XRUN;
		pos[i] = (state.residue - start) % bytes;
	}
	/* Choose the lagging channel, also when just one channel has wrapped. */
	return (max(pos[0], pos[1]) - min(pos[0], pos[1]) > bytes / 2 ?
		max(pos[0], pos[1]) : min(pos[0], pos[1])) / sizeof(s16);
}

static int w202_pcm_hw_free(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream)
{
	w202_pcm_release(substream->runtime->private_data);
	return 0;
}

static int w202_pcm_construct(struct snd_soc_component *component,
			      struct snd_soc_pcm_runtime *rtd)
{
	return snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV,
		component->dev, W202_DMA_BUFFER_BYTES, W202_DMA_BUFFER_BYTES);
}

static const struct snd_soc_component_driver w202_pcm_component = {
	.name = "w202-pcm",
	.open = w202_pcm_open,
	.close = w202_pcm_close,
	.hw_params = w202_pcm_hw_params,
	.prepare = w202_pcm_prepare,
	.hw_free = w202_pcm_hw_free,
	.trigger = w202_pcm_trigger,
	.sync_stop = w202_pcm_sync_stop,
	.pointer = w202_pcm_pointer,
	.pcm_construct = w202_pcm_construct,
};

static int w202_pcm_probe(struct platform_device *pdev)
{
	int ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));

	if (ret)
		return ret;
	return devm_snd_soc_register_component(&pdev->dev, &w202_pcm_component, NULL, 0);
}

static const struct of_device_id w202_pcm_match[] = {
	{ .compatible = "zte,w202ds-pcm" },
	{ }
};
MODULE_DEVICE_TABLE(of, w202_pcm_match);

static struct platform_driver w202_pcm_driver = {
	.probe = w202_pcm_probe,
	.driver = { .name = "w202-audio-pcm", .of_match_table = w202_pcm_match },
};
module_platform_driver(w202_pcm_driver);
MODULE_DESCRIPTION("W202DS stereo AP01 PCM transport");
MODULE_LICENSE("GPL");
