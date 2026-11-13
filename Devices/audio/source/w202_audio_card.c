// SPDX-License-Identifier: GPL-2.0-only
/* W202DS card: VBC AP01 to four AW88166 speakers, and UMP9620 main mic to VBC. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/soc.h>

#define W202_AMPS 4

struct w202_card {
	struct snd_soc_card card;
	struct snd_soc_dai_link links[2];
	struct snd_soc_dai_link_component cpu;
	struct snd_soc_dai_link_component pcm;
	struct snd_soc_dai_link_component amps[W202_AMPS];
	struct snd_soc_dai_link_component mic_cpu;
	struct snd_soc_dai_link_component mic;
	struct snd_soc_codec_conf conf[W202_AMPS];
};

static const struct snd_soc_dapm_widget w202_speakers[] = {
	SND_SOC_DAPM_SPK("Speaker 0", NULL),
	SND_SOC_DAPM_SPK("Speaker 1", NULL),
	SND_SOC_DAPM_SPK("Speaker 2", NULL),
	SND_SOC_DAPM_SPK("Speaker 3", NULL),
};

static const struct snd_soc_dapm_route w202_routes[] = {
	{ "Speaker 0", NULL, "AMP0 DAC Output" },
	{ "Speaker 1", NULL, "AMP1 DAC Output" },
	{ "Speaker 2", NULL, "AMP2 DAC Output" },
	{ "Speaker 3", NULL, "AMP3 DAC Output" },
};

static void w202_card_put_nodes(void *data)
{
	struct w202_card *c = data;
	int i;

	of_node_put(c->cpu.of_node);
	of_node_put(c->pcm.of_node);
	of_node_put(c->mic.of_node);
	for (i = 0; i < W202_AMPS; i++)
		of_node_put(c->amps[i].of_node);
}

static int w202_card_probe(struct platform_device *pdev)
{
	static const char * const prefixes[] = { "AMP0", "AMP1", "AMP2", "AMP3" };
	struct device *dev = &pdev->dev;
	struct w202_card *c;
	int i, ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, w202_card_put_nodes, c);
	if (ret)
		return ret;
	c->cpu.of_node = of_parse_phandle(dev->of_node, "zte,audio-cpu", 0);
	c->cpu.dai_name = "w202-vbc-ap01";
	c->pcm.of_node = of_parse_phandle(dev->of_node, "zte,audio-platform", 0);
	if (!c->cpu.of_node || !c->pcm.of_node ||
	    of_count_phandle_with_args(dev->of_node, "zte,audio-codecs", NULL) != W202_AMPS)
		return -EINVAL;
	for (i = 0; i < W202_AMPS; i++) {
		c->amps[i].of_node = of_parse_phandle(dev->of_node, "zte,audio-codecs", i);
		if (!c->amps[i].of_node)
			return -EINVAL;
		c->amps[i].dai_name = "aw88166-aif";
		c->conf[i].dlc.of_node = c->amps[i].of_node;
		c->conf[i].name_prefix = prefixes[i];
	}
	/* A playback-only DT remains usable while the audio bundle is updated. */
	if (of_property_present(dev->of_node, "zte,audio-mic")) {
		c->mic.of_node = of_parse_phandle(dev->of_node, "zte,audio-mic", 0);
		if (!c->mic.of_node || !of_device_is_available(c->mic.of_node))
			return -EINVAL;
		c->mic.dai_name = "w202-mic-ap01";
		c->mic_cpu.of_node = c->cpu.of_node;
		c->mic_cpu.dai_name = "w202-vbc-ap01-capture";
	}
	c->links[0].name = "AP01 Speakers";
	c->links[0].stream_name = "Speakers";
	c->links[0].cpus = &c->cpu;
	c->links[0].num_cpus = 1;
	c->links[0].platforms = &c->pcm;
	c->links[0].num_platforms = 1;
	c->links[0].codecs = c->amps;
	c->links[0].num_codecs = W202_AMPS;
	c->links[0].playback_only = 1;
	c->links[0].dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC;
	/* Internal AGCP codec to VBC ADC0 (IIS1); both ends are fixed by AUDCP. */
	c->links[1].name = "AP01 Microphone";
	c->links[1].stream_name = "Microphone";
	c->links[1].cpus = &c->mic_cpu;
	c->links[1].num_cpus = 1;
	c->links[1].platforms = &c->pcm;
	c->links[1].num_platforms = 1;
	c->links[1].codecs = &c->mic;
	c->links[1].num_codecs = 1;
	c->links[1].capture_only = 1;
	/* Default order arms DMA before enabling VBC requests, stops VBC first. */
	c->card.name = "W202DS Speakers";
	c->card.owner = THIS_MODULE;
	c->card.dev = dev;
	c->card.dai_link = c->links;
	c->card.num_links = c->mic.of_node ? ARRAY_SIZE(c->links) : 1;
	c->card.codec_conf = c->conf;
	c->card.num_configs = W202_AMPS;
	c->card.dapm_widgets = w202_speakers;
	c->card.num_dapm_widgets = ARRAY_SIZE(w202_speakers);
	c->card.dapm_routes = w202_routes;
	c->card.num_dapm_routes = ARRAY_SIZE(w202_routes);
	platform_set_drvdata(pdev, c);
	return devm_snd_soc_register_card(dev, &c->card);
}

static const struct of_device_id w202_card_match[] = {
	{ .compatible = "zte,w202ds-speakers" }, { }
};
MODULE_DEVICE_TABLE(of, w202_card_match);

static struct platform_driver w202_card_driver = {
	.probe = w202_card_probe,
	.driver = { .name = "w202-audio-card", .of_match_table = w202_card_match },
};
module_platform_driver(w202_card_driver);
MODULE_DESCRIPTION("W202DS VBC speaker and main microphone card");
MODULE_LICENSE("GPL");
