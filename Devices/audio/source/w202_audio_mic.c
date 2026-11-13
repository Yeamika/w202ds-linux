// SPDX-License-Identifier: GPL-2.0-only
/*
 * W202DS microphones: UMP9620 analog codec (PMIC) and AGCP digital codec.
 * MIC1 (main) -> ADC_1 and MIC3 (back) -> ADC_3 -> ADD0 (MIC13) -> digital
 * ADC -> VBC ADC0 (IIS1). Register map, DAPM graph, events and supplies follow
 * the factory snd-soc-sprd-codec-ump9620.ko and snd-soc-sprd-codec-ump9620-power.ko;
 * see PROTOCOL.md.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

/* Component register space of sprd_codec_read/write (0x125128/0x125318). */
#define W202_DIG_FIRST		0x1000	/* AGCP digital codec, MMIO */
#define W202_DIG_LAST		0x12f4
#define W202_ANA_FIRST		0x2100	/* PMIC TOPA/CFGA, reg - 0x3000 + DT reg */
#define W202_ANA_LAST		0x3290
#define W202_ANA_BASE		0x3000

/* Digital codec. */
#define W202_DIG_TOP_CTL	0x1000
#define W202_DIG_I2S_CTL	0x1008
#define W202_DIG_DA_FS		0x100c
#define W202_DIG_1010		0x1010
#define W202_DIG_AD_FS		0x1018
#define W202_DIG_ADC1_CTL	0x1034
#define W202_DIG_1084		0x1084
#define W202_DIG_1088		0x1088

/* Analog codec (component addresses). */
#define W202_TOPA_CLK_EN	0x2100
#define W202_TOPA_SOFT_RST	0x2104
#define W202_TOPA_DIG_ADC	0x2108
#define W202_TOPA_2140		0x2140
#define W202_ANA_PMU0		0x3000
#define W202_ANA_PMU1		0x3004
#define W202_ANA_305C		0x305c
#define W202_ANA_CLK0		0x3068
#define W202_ANA_CDC0		0x3074
#define W202_ANA_CDC1		0x3078
#define W202_ANA_CDC2		0x307c
#define W202_ANA_DCL0		0x30fc
#define W202_ANA_DCL1		0x3100

/* PMIC global registers, absolute ADI addresses. */
#define W202_PMIC_MODULE_EN0	0x2008
#define W202_PMIC_ARM_CLK_EN0	0x200c
#define W202_PMIC_SOFT_RST0	0x2014
#define W202_PMIC_ADD0_SEL	0x234c
#define W202_PMIC_XTL_WAIT	0x2378
#define W202_PMIC_AUD_2394	0x2394

#define W202_MIC_RATE		48000
#define W202_MIC_VOLUME		6	/* codec.xml Handsfree Record adc1/adc3 */

enum { W202_VREG, W202_VB, W202_BG, W202_BIAS, W202_DCL, W202_MICBIAS1,
	W202_MICBIAS3, W202_CLK_DCL_32K, W202_SUPPLIES };

struct w202_mic {
	struct device *dev;
	struct regmap *pmic;
	u32 ana_base;
	void __iomem *dig;
	struct clk_bulk_data clocks[2];
	struct regmap *sinout;
	u32 sinout_reg, sinout_mask;
	struct regulator *vreg, *vb;
	struct regulator_desc desc[W202_SUPPLIES];
	struct regulator_consumer_supply consumer[W202_SUPPLIES];
	struct regulator_init_data init[W202_SUPPLIES];
	bool vreg_on, vreg_reset;
	bool lrdat_sel;
	unsigned int add0_sel, adc_lrclk, adc1_lrclk, ad_rate;
};

static u32 w202_mic_pmic(struct w202_mic *m, unsigned int reg)
{
	return reg - W202_ANA_BASE + m->ana_base;
}

static unsigned int w202_mic_read(struct snd_soc_component *component, unsigned int reg)
{
	struct w202_mic *m = snd_soc_component_get_drvdata(component);
	unsigned int val = 0;

	if (reg >= W202_ANA_FIRST && reg <= W202_ANA_LAST) {
		regmap_read(m->pmic, w202_mic_pmic(m, reg), &val);
	} else if (reg >= W202_DIG_FIRST && reg <= W202_DIG_LAST) {
		/* arch_audio_codec_digital_reg_enable_v2 around every access. */
		if (clk_bulk_prepare_enable(ARRAY_SIZE(m->clocks), m->clocks))
			return 0;
		val = readl(m->dig + reg - W202_DIG_FIRST);
		clk_bulk_disable_unprepare(ARRAY_SIZE(m->clocks), m->clocks);
	}
	return val;
}

static int w202_mic_write(struct snd_soc_component *component, unsigned int reg,
			  unsigned int val)
{
	struct w202_mic *m = snd_soc_component_get_drvdata(component);
	int ret;

	if (reg >= W202_ANA_FIRST && reg <= W202_ANA_LAST)
		return regmap_write(m->pmic, w202_mic_pmic(m, reg), val);
	if (reg < W202_DIG_FIRST || reg > W202_DIG_LAST)
		return 0;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(m->clocks), m->clocks);
	if (ret)
		return ret;
	writel(val, m->dig + reg - W202_DIG_FIRST);
	clk_bulk_disable_unprepare(ARRAY_SIZE(m->clocks), m->clocks);
	return 0;
}

/* sprd_codec_set_ad_sample_rate: 0x1018 bits 3:0 ADC, 7:4 ADC1, rate / 4000. */
static void w202_mic_rate(struct snd_soc_component *component)
{
	struct w202_mic *m = snd_soc_component_get_drvdata(component);
	unsigned int fs = m->ad_rate / 4000;

	if (m->ad_rate)
		snd_soc_component_update_bits(component, W202_DIG_AD_FS, 0xff, fs << 4 | fs);
}

static void w202_mic_sinout(struct w202_mic *m, bool on)
{
	regmap_update_bits(m->sinout, m->sinout_reg, m->sinout_mask, on ? m->sinout_mask : 0);
}

static void w202_mic_cfga_reset(struct snd_soc_component *component)
{
	snd_soc_component_update_bits(component, W202_TOPA_SOFT_RST, 0x6, 0x6);
	udelay(10);
	snd_soc_component_update_bits(component, W202_TOPA_SOFT_RST, 0x6, 0);
	snd_soc_component_update_bits(component, W202_TOPA_2140, 0x10, 0);
}

/* analog_power_event 0x125ff4 */
static int w202_mic_analog_event(struct snd_soc_dapm_widget *w,
				 struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct w202_mic *m = snd_soc_component_get_drvdata(component);

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		w202_mic_sinout(m, true);
		regmap_update_bits(m->pmic, W202_PMIC_ARM_CLK_EN0, 0x83, 0x83);
		regmap_update_bits(m->pmic, W202_PMIC_AUD_2394, 0x2, 0x2);
		regmap_update_bits(m->pmic, W202_PMIC_XTL_WAIT, 0x100, 0x100);
		snd_soc_component_update_bits(component, W202_TOPA_CLK_EN, 0x2, 0x2);
		w202_mic_cfga_reset(component);
		return regulator_set_mode(m->vb, REGULATOR_MODE_NORMAL);
	}
	regmap_update_bits(m->pmic, W202_PMIC_ARM_CLK_EN0, 0x83, 0);
	regmap_update_bits(m->pmic, W202_PMIC_AUD_2394, 0x2, 0);
	snd_soc_component_update_bits(component, W202_TOPA_CLK_EN, 0x2, 0);
	w202_mic_cfga_reset(component);
	regulator_set_mode(m->vb, REGULATOR_MODE_STANDBY);
	w202_mic_sinout(m, false);
	return 0;
}

/* digital_power_event 0x125d80: digital codec clocks stay on while powered. */
static int w202_mic_digital_event(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct w202_mic *m = snd_soc_component_get_drvdata(component);
	int ret;

	if (!SND_SOC_DAPM_EVENT_ON(event)) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(m->clocks), m->clocks);
		return 0;
	}
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(m->clocks), m->clocks);
	if (ret)
		return ret;
	snd_soc_component_update_bits(component, W202_DIG_DA_FS, 0x8000, 0);
	w202_mic_rate(component);
	snd_soc_component_update_bits(component, W202_DIG_1010, 0xffff, 0);
	if (m->adc_lrclk)
		snd_soc_component_update_bits(component, W202_DIG_I2S_CTL, 0x4, 0x4);
	if (m->adc1_lrclk)
		snd_soc_component_update_bits(component, W202_DIG_ADC1_CTL, 0x1, 0x1);
	snd_soc_component_update_bits(component, W202_DIG_1084, 0xffff, 0x3303);
	snd_soc_component_update_bits(component, W202_DIG_1088, 0x1, 0);
	return 0;
}

/* audio_dcl_event 0x126418, POST_PMU of DIG_CLK_DRV_SOFT. */
static int w202_mic_dcl_event(struct snd_soc_dapm_widget *w,
			      struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct w202_mic *m = snd_soc_component_get_drvdata(component);

	snd_soc_component_update_bits(component, W202_ANA_DCL0, 0x8601, 0x8601);
	snd_soc_component_update_bits(component, W202_ANA_DCL1, 0x400, 0x400);
	usleep_range(180, 200);
	if (m->lrdat_sel)
		snd_soc_component_update_bits(component, W202_ANA_305C, 0x300, 0x300);
	return 0;
}

/* adc_1_event 0x126cb4 */
static int w202_mic_adc1_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	if (event == SND_SOC_DAPM_PRE_PMU) {
		snd_soc_component_update_bits(component, W202_ANA_CDC0, 0x2, 0x2);
		snd_soc_component_update_bits(component, W202_ANA_CDC0, 0x2, 0);
		snd_soc_component_update_bits(component, W202_ANA_CDC0, 0x8, 0);
		udelay(4);
		snd_soc_component_update_bits(component, W202_ANA_CDC0, 0x8, 0x8);
		udelay(4);
	} else {
		snd_soc_component_update_bits(component, W202_ANA_CDC0, 0x8, 0);
		msleep(60);
	}
	return 0;
}

/* adc_3_event 0x126fac */
static int w202_mic_adc3_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	if (event == SND_SOC_DAPM_PRE_PMU) {
		snd_soc_component_update_bits(component, W202_ANA_CDC1, 0x8000, 0x8000);
		snd_soc_component_update_bits(component, W202_ANA_CDC1, 0x8000, 0);
		snd_soc_component_update_bits(component, W202_ANA_CDC0, 0x8000, 0);
		udelay(4);
		snd_soc_component_update_bits(component, W202_ANA_CDC0, 0x8000, 0x8000);
		udelay(4);
	} else {
		snd_soc_component_update_bits(component, W202_ANA_CDC0, 0x8000, 0);
		msleep(60);
	}
	return 0;
}

/* chan_event 0x1272a4: sprd_codec_sample_rate_setting. */
static int w202_mic_chan_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol, int event)
{
	w202_mic_rate(snd_soc_dapm_to_component(w->dapm));
	return 0;
}

static const char * const w202_add0_text[] = {
	"ADD0_DATA_MIC12", "ADD0_DATA_MIC13", "ADD0_DATA_MIC23",
};
static const unsigned int w202_add0_value[] = { 0, 0x100, 0x180 };	/* 0x119ae8 */
static SOC_ENUM_SINGLE_EXT_DECL(w202_add0_enum, w202_add0_text);
static const char * const w202_lrclk_text[] = { "normal", "invert" };

static int w202_mic_add0_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct w202_mic *m = snd_soc_component_get_drvdata(snd_kcontrol_chip(kcontrol));

	ucontrol->value.enumerated.item[0] = m->add0_sel;
	return 0;
}

/* sprd_codec_add0_data_sel_put 0x12be88: PMIC 0x234c mask 0x180. */
static int w202_mic_add0_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct w202_mic *m = snd_soc_component_get_drvdata(snd_kcontrol_chip(kcontrol));
	unsigned int item = ucontrol->value.enumerated.item[0];
	int ret;

	if (item >= ARRAY_SIZE(w202_add0_value))
		return -EINVAL;
	ret = regmap_update_bits(m->pmic, W202_PMIC_ADD0_SEL, 0x180, w202_add0_value[item]);
	if (ret)
		return ret;
	if (m->add0_sel == item)
		return 0;
	m->add0_sel = item;
	return 1;
}

/* sprd_codec_adc_lrclk_sel_put 0x12afac / adc1 0x12b120 */
static int w202_mic_lrclk_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct w202_mic *m = snd_soc_component_get_drvdata(snd_kcontrol_chip(kcontrol));

	ucontrol->value.enumerated.item[0] = kcontrol->private_value ? m->adc1_lrclk : m->adc_lrclk;
	return 0;
}

static int w202_mic_lrclk_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct w202_mic *m = snd_soc_component_get_drvdata(component);
	unsigned int item = ucontrol->value.enumerated.item[0];
	unsigned int *state = kcontrol->private_value ? &m->adc1_lrclk : &m->adc_lrclk;
	int ret;

	if (item > 1)
		return -EINVAL;
	if (kcontrol->private_value)
		ret = snd_soc_component_update_bits(component, W202_DIG_ADC1_CTL, 0x1, item);
	else
		ret = snd_soc_component_update_bits(component, W202_DIG_I2S_CTL, 0x4, item << 2);
	if (ret < 0)
		return ret;
	if (*state == item)
		return 0;
	*state = item;
	return 1;
}

static int w202_mic_lrclk_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(w202_lrclk_text), w202_lrclk_text);
}

static const struct snd_kcontrol_new w202_mic_controls[] = {
	SOC_SINGLE("ADC_1 Capture Volume", W202_ANA_CDC2, 3, 7, 0),
	SOC_SINGLE("ADC_3 Capture Volume", W202_ANA_CDC2, 6, 7, 0),
	SOC_ENUM_EXT("ADD0 DATA SEL", w202_add0_enum, w202_mic_add0_get, w202_mic_add0_put),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "ADC LRCLK Select",
		.info = w202_mic_lrclk_info, .get = w202_mic_lrclk_get,
		.put = w202_mic_lrclk_put, .private_value = 0,
	}, {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "ADC1 LRCLK Select",
		.info = w202_mic_lrclk_info, .get = w202_mic_lrclk_get,
		.put = w202_mic_lrclk_put, .private_value = 1,
	},
};

static const struct snd_kcontrol_new w202_adc1_mixer[] = {
	SOC_DAPM_SINGLE("MIC1PGA_1 Switch", W202_ANA_CDC1, 1, 1, 0),
};

static const struct snd_kcontrol_new w202_adc3_mixer[] = {
	SOC_DAPM_SINGLE("MIC3PGA_3 Switch", W202_ANA_CDC1, 4, 1, 0),
};

static const char * const w202_dig_adc_text[] = { "ADC", "DAC", "DMIC" };
static SOC_ENUM_SINGLE_DECL(w202_dig_adc_enum, W202_DIG_TOP_CTL, 8, w202_dig_adc_text);
static const struct snd_kcontrol_new w202_dig_adc_mux = SOC_DAPM_ENUM("Route", w202_dig_adc_enum);

#define PRE_POST_PMU (SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU)
#define PRE_PMU_POST_PMD (SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD)

/* Factory widget table: types, registers and subsequences. */
static const struct snd_soc_dapm_widget w202_mic_widgets[] = {
	SND_SOC_DAPM_SUPPLY_S("Digital Power", 1, SND_SOC_NOPM, 0, 0,
			      w202_mic_digital_event, PRE_PMU_POST_PMD),
	SND_SOC_DAPM_SUPPLY_S("Analog Power", 0, SND_SOC_NOPM, 0, 0,
			      w202_mic_analog_event, PRE_PMU_POST_PMD),
	SND_SOC_DAPM_REGULATOR_SUPPLY("BG", 0, 0),
	SND_SOC_DAPM_REGULATOR_SUPPLY("BIAS", 0, 0),
	SND_SOC_DAPM_REGULATOR_SUPPLY("MICBIAS1", 0, 0),
	SND_SOC_DAPM_REGULATOR_SUPPLY("MICBIAS3", 0, 0),
	SND_SOC_DAPM_REGULATOR_SUPPLY("DCL", 0, 0),
	SND_SOC_DAPM_REGULATOR_SUPPLY("CLK_DCL_32K", 0, 0),
	SND_SOC_DAPM_SUPPLY_S("ANA_CLK", 1, W202_ANA_CLK0, 15, 0, NULL, 0),
	/* clk_26m_in_sel_event writes only for the non-default "1" selection. */
	SND_SOC_DAPM_SUPPLY_S("CLK 26M IN SEL", 1, SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("CLK_DCL_6M5", 1, W202_ANA_CLK0, 12, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("CLK_ADC", 1, W202_ANA_CLK0, 9, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("CLK_DIG_6M5", 1, W202_ANA_CLK0, 11, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("CLK_ADC_Norm", 1, SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("DIG_CLK_DRV_SOFT", 5, W202_ANA_DCL1, 1, 0,
			      w202_mic_dcl_event, SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_SUPPLY_S("ADC_1_CLK", 5, W202_ANA_CDC0, 5, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("ADC_3_CLK", 5, W202_ANA_CDC0, 14, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("ADC_IBIAS", 3, W202_ANA_CDC0, 11, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("ADC_VREF_BUF", 3, W202_ANA_CDC0, 10, 0, NULL, 0),
	SND_SOC_DAPM_INPUT("MIC Pin"),
	SND_SOC_DAPM_INPUT("MIC3 Pin"),
	SND_SOC_DAPM_MIXER("ADC_1 Mixer", SND_SOC_NOPM, 0, 0,
			   w202_adc1_mixer, ARRAY_SIZE(w202_adc1_mixer)),
	SND_SOC_DAPM_MIXER("ADC_3 Mixer", SND_SOC_NOPM, 0, 0,
			   w202_adc3_mixer, ARRAY_SIZE(w202_adc3_mixer)),
	SND_SOC_DAPM_PGA("ADC_1 Gain", W202_ANA_CDC0, 9, 0, NULL, 0),
	SND_SOC_DAPM_PGA("ADC_3 Gain", W202_ANA_CDC0, 12, 0, NULL, 0),
	SND_SOC_DAPM_PGA_E("ADC_1 Switch", W202_ANA_CDC0, 7, 0, NULL, 0,
			   w202_mic_adc1_event, PRE_POST_PMU),
	SND_SOC_DAPM_PGA_E("ADC_3 Switch", W202_ANA_CDC0, 13, 0, NULL, 0,
			   w202_mic_adc3_event, PRE_POST_PMU),
	SND_SOC_DAPM_PGA_S("ADie Digital ADCL Switch", 2, W202_TOPA_DIG_ADC, 3, 0, NULL, 0),
	SND_SOC_DAPM_MUX("Digital ADC In Sel", SND_SOC_NOPM, 0, 0, &w202_dig_adc_mux),
	SND_SOC_DAPM_PGA_S("Digital ADCL Switch", 4, W202_DIG_TOP_CTL, 1, 0, NULL, 0),
	SND_SOC_DAPM_PGA_S("Digital ADCR Switch", 4, W202_DIG_TOP_CTL, 3, 0, NULL, 0),
	SND_SOC_DAPM_ADC_E("ADC", "Normal-Capture-AP01", SND_SOC_NOPM, 0, 0,
			   w202_mic_chan_event, PRE_PMU_POST_PMD),
};

/*
 * Factory route table, MIC1/MIC3 subset. The factory "AUD ADC0L/R" virtual
 * switches only gate the HAL's choice of ADC pair; ADC0 is the only pair here,
 * so its lanes connect directly and capture works without userspace routing.
 */
static const struct snd_soc_dapm_route w202_mic_routes[] = {
	{ "ANA_CLK", NULL, "Analog Power" },
	{ "CLK 26M IN SEL", NULL, "ANA_CLK" },
	{ "CLK_DCL_32K", NULL, "CLK 26M IN SEL" },
	{ "CLK_DCL_6M5", NULL, "CLK_DCL_32K" },
	{ "CLK_DIG_6M5", NULL, "CLK_DCL_6M5" },
	{ "ADC_IBIAS", NULL, "ADC_VREF_BUF" },
	{ "CLK_ADC", NULL, "CLK_DIG_6M5" },
	{ "CLK_ADC", NULL, "DCL" },
	{ "CLK_ADC", NULL, "DIG_CLK_DRV_SOFT" },
	{ "CLK_ADC", NULL, "Digital Power" },
	{ "CLK_ADC_Norm", NULL, "CLK_ADC" },
	{ "CLK_ADC_Norm", NULL, "ADC_IBIAS" },
	{ "BIAS", NULL, "BG" },
	{ "ADC_1_CLK", NULL, "BIAS" },
	{ "ADC_3_CLK", NULL, "BIAS" },
	{ "ADC_1 Switch", NULL, "ADC_1_CLK" },
	{ "ADC_3 Switch", NULL, "ADC_3_CLK" },
	{ "ADC_1 Gain", NULL, "ADC_1 Mixer" },
	{ "ADC_1 Switch", NULL, "ADC_1 Gain" },
	{ "ADC_3 Gain", NULL, "ADC_3 Mixer" },
	{ "ADC_3 Switch", NULL, "ADC_3 Gain" },
	{ "ADie Digital ADCL Switch", NULL, "ADC_1 Switch" },
	{ "ADie Digital ADCL Switch", NULL, "ADC_3 Switch" },
	{ "Digital ADC In Sel", "ADC", "ADie Digital ADCL Switch" },
	{ "Digital ADCL Switch", NULL, "Digital ADC In Sel" },
	{ "Digital ADCR Switch", NULL, "Digital ADC In Sel" },
	{ "ADC", NULL, "Digital ADCL Switch" },
	{ "ADC", NULL, "Digital ADCR Switch" },
	{ "ADC", NULL, "CLK_ADC_Norm" },
	{ "MIC Pin", NULL, "MICBIAS1" },
	{ "ADC_1 Mixer", "MIC1PGA_1 Switch", "MIC Pin" },
	{ "MIC3 Pin", NULL, "MICBIAS3" },
	{ "ADC_3 Mixer", "MIC3PGA_3 Switch", "MIC3 Pin" },
};

/*
 * Factory sprd_codec_probe cycles VB once (vreg_enable resets the analog block,
 * then VB is left in standby). The PMIC analog registers (0x1000-0x1290) read
 * back as zero while VREG (0x2008 bit 4) is off, so VREG stays on while the
 * component exists: DAPM then reads the real mixer state at card init and the
 * controls keep their values between streams. Defaults are the ODM
 * mainmic_auxmic2 route (MIC1 + MIC3 on ADD0, LRCLK inverted) and the
 * codec.xml record gains.
 */
static int w202_mic_component_probe(struct snd_soc_component *component)
{
	struct w202_mic *m = snd_soc_component_get_drvdata(component);
	int ret;

	ret = regulator_enable(m->vreg);
	if (ret)
		return ret;
	ret = regulator_enable(m->vb);
	if (ret)
		goto err;
	regulator_set_mode(m->vb, REGULATOR_MODE_STANDBY);
	regulator_disable(m->vb);
	snd_soc_component_update_bits(component, W202_ANA_CDC1, BIT(1) | BIT(4),
				      BIT(1) | BIT(4));
	snd_soc_component_update_bits(component, W202_ANA_CDC2, 0x7 << 3 | 0x7 << 6,
				      W202_MIC_VOLUME << 3 | W202_MIC_VOLUME << 6);
	m->add0_sel = 1;
	ret = regmap_update_bits(m->pmic, W202_PMIC_ADD0_SEL, 0x180, w202_add0_value[1]);
	if (ret)
		goto err;
	m->adc_lrclk = 1;
	m->adc1_lrclk = 1;
	snd_soc_component_update_bits(component, W202_DIG_I2S_CTL, 0x4, 0x4);
	snd_soc_component_update_bits(component, W202_DIG_ADC1_CTL, 0x1, 0x1);
	return 0;
err:
	regulator_disable(m->vreg);
	return ret;
}

static void w202_mic_component_remove(struct snd_soc_component *component)
{
	struct w202_mic *m = snd_soc_component_get_drvdata(component);

	regulator_disable(m->vreg);
}

static const struct snd_soc_component_driver w202_mic_component = {
	.name = "w202-mic",
	.probe = w202_mic_component_probe,
	.remove = w202_mic_component_remove,
	.read = w202_mic_read,
	.write = w202_mic_write,
	.controls = w202_mic_controls,
	.num_controls = ARRAY_SIZE(w202_mic_controls),
	.dapm_widgets = w202_mic_widgets,
	.num_dapm_widgets = ARRAY_SIZE(w202_mic_widgets),
	.dapm_routes = w202_mic_routes,
	.num_dapm_routes = ARRAY_SIZE(w202_mic_routes),
	.idle_bias_on = 0,
	.endianness = 1,
};

/* sprd_codec_pcm_hw_params 0x124aac, capture: both AD rates. */
static int w202_mic_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct w202_mic *m = snd_soc_component_get_drvdata(dai->component);

	if (params_rate(params) != W202_MIC_RATE)
		return -EINVAL;
	m->ad_rate = params_rate(params);
	w202_mic_rate(dai->component);
	return 0;
}

static const struct snd_soc_dai_ops w202_mic_dai_ops = {
	.hw_params = w202_mic_hw_params,
};

static struct snd_soc_dai_driver w202_mic_dai = {
	.name = "w202-mic-ap01",
	.capture = {
		.stream_name = "Normal-Capture-AP01",
		.channels_min = 1, .channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	},
	.ops = &w202_mic_dai_ops,
};

/* vreg_enable 0x104614 / vreg_disable 0x104728 */
static int w202_vreg_enable(struct regulator_dev *rdev)
{
	struct w202_mic *m = rdev_get_drvdata(rdev);
	int ret;

	ret = regmap_update_bits(m->pmic, W202_PMIC_MODULE_EN0, BIT(4), BIT(4));
	if (ret)
		return ret;
	if (!m->vreg_reset) {
		ret = regmap_update_bits(m->pmic, W202_PMIC_SOFT_RST0, 0x3100, 0x3100);
		udelay(10);
		if (!ret)
			ret = regmap_update_bits(m->pmic, W202_PMIC_SOFT_RST0, 0x3100, 0);
		if (ret)
			return ret;
		m->vreg_reset = true;
	}
	m->vreg_on = true;
	return 0;
}

static int w202_vreg_disable(struct regulator_dev *rdev)
{
	struct w202_mic *m = rdev_get_drvdata(rdev);

	m->vreg_on = false;
	return regmap_update_bits(m->pmic, W202_PMIC_MODULE_EN0, BIT(4), 0);
}

static int w202_vreg_is_enabled(struct regulator_dev *rdev)
{
	struct w202_mic *m = rdev_get_drvdata(rdev);

	return m->vreg_on;
}

static const struct regulator_ops w202_vreg_ops = {
	.enable = w202_vreg_enable,
	.disable = w202_vreg_disable,
	.is_enabled = w202_vreg_is_enabled,
};

/* power_sleep 0x104098 for VB: ANA_PMU0 bit 10 in standby. */
static int w202_vb_set_mode(struct regulator_dev *rdev, unsigned int mode)
{
	struct w202_mic *m = rdev_get_drvdata(rdev);

	if (mode != REGULATOR_MODE_NORMAL && mode != REGULATOR_MODE_STANDBY)
		return -EINVAL;
	return regmap_update_bits(m->pmic, w202_mic_pmic(m, W202_ANA_PMU0), BIT(10),
				  mode == REGULATOR_MODE_STANDBY ? BIT(10) : 0);
}

static unsigned int w202_vb_get_mode(struct regulator_dev *rdev)
{
	struct w202_mic *m = rdev_get_drvdata(rdev);
	unsigned int val;

	if (regmap_read(m->pmic, w202_mic_pmic(m, W202_ANA_PMU0), &val))
		return REGULATOR_MODE_INVALID;
	return val & BIT(10) ? REGULATOR_MODE_STANDBY : REGULATOR_MODE_NORMAL;
}

static const struct regulator_ops w202_vb_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.set_mode = w202_vb_set_mode,
	.get_mode = w202_vb_get_mode,
};

static const struct regulator_ops w202_switch_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

/* SPRD_AUDIO_POWER_INFO_* table of the factory power module. */
static const struct {
	const char *name, *parent;
	unsigned int reg, mask;
} w202_supplies[W202_SUPPLIES] = {
	[W202_VREG] = { "VREG", NULL, 0, 0 },
	[W202_VB] = { "VB", "VREG", W202_ANA_PMU0, BIT(13) },
	[W202_BG] = { "BG", "VB", W202_ANA_PMU0, BIT(9) },
	[W202_BIAS] = { "BIAS", "VB", W202_ANA_PMU0, BIT(4) },
	[W202_DCL] = { "DCL", "VB", W202_ANA_DCL1, BIT(9) },
	[W202_MICBIAS1] = { "MICBIAS1", "BIAS", W202_ANA_PMU1, BIT(0) },
	[W202_MICBIAS3] = { "MICBIAS3", "BIAS", W202_ANA_PMU1, BIT(2) },
	[W202_CLK_DCL_32K] = { "CLK_DCL_32K", "DCL", W202_ANA_CLK0, BIT(13) },
};

static int w202_mic_regulators(struct w202_mic *m)
{
	struct regulator_config config = { .dev = m->dev, .driver_data = m, .regmap = m->pmic };
	int i;

	for (i = 0; i < W202_SUPPLIES; i++) {
		struct regulator_desc *desc = &m->desc[i];
		struct regulator_init_data *init = &m->init[i];
		struct regulator_dev *rdev;

		m->consumer[i].dev_name = dev_name(m->dev);
		m->consumer[i].supply = w202_supplies[i].name;
		init->consumer_supplies = &m->consumer[i];
		init->num_consumer_supplies = 1;
		init->constraints.valid_ops_mask = REGULATOR_CHANGE_STATUS;
		desc->name = devm_kasprintf(m->dev, GFP_KERNEL, "w202-mic-%s",
					    w202_supplies[i].name);
		if (!desc->name)
			return -ENOMEM;
		desc->supply_name = w202_supplies[i].parent;
		desc->id = i;
		desc->type = REGULATOR_VOLTAGE;
		desc->owner = THIS_MODULE;
		if (i == W202_VREG) {
			desc->ops = &w202_vreg_ops;
		} else {
			desc->ops = i == W202_VB ? &w202_vb_ops : &w202_switch_ops;
			desc->enable_reg = w202_mic_pmic(m, w202_supplies[i].reg);
			desc->enable_mask = w202_supplies[i].mask;
		}
		if (i == W202_VB) {
			init->constraints.valid_ops_mask |= REGULATOR_CHANGE_MODE;
			init->constraints.valid_modes_mask = REGULATOR_MODE_NORMAL |
							     REGULATOR_MODE_STANDBY;
		}
		config.init_data = init;
		rdev = devm_regulator_register(m->dev, desc, &config);
		if (IS_ERR(rdev))
			return dev_err_probe(m->dev, PTR_ERR(rdev), "%s supply\n", desc->name);
	}
	return 0;
}

static int w202_mic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *dig;
	struct w202_mic *m;
	unsigned int args[2];
	int ret;

	m = devm_kzalloc(dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->dev = dev;
	m->pmic = dev_get_regmap(dev->parent, NULL);
	if (!m->pmic)
		return -EPROBE_DEFER;
	ret = of_property_read_u32(dev->of_node, "reg", &m->ana_base);
	if (ret)
		return ret;
	/* sprd_codec_probe: u32 "lrdat-sel", absent means 0. */
	m->lrdat_sel = !of_property_read_u32(dev->of_node, "lrdat-sel", &args[0]) && args[0];
	m->sinout = syscon_regmap_lookup_by_phandle_args(dev->of_node, "clk26m_sinout_pmic_en",
							 2, args);
	if (IS_ERR(m->sinout))
		return dev_err_probe(dev, PTR_ERR(m->sinout), "26 MHz sinout\n");
	m->sinout_reg = args[0];
	m->sinout_mask = args[1];
	dig = of_parse_phandle(dev->of_node, "digital-codec", 0);
	if (!dig)
		return -EINVAL;
	m->dig = devm_of_iomap(dev, dig, 0, NULL);
	of_node_put(dig);
	if (IS_ERR(m->dig))
		return PTR_ERR(m->dig);
	m->clocks[0].id = "aud";
	m->clocks[1].id = "audif";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(m->clocks), m->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "digital codec clocks\n");
	ret = w202_mic_regulators(m);
	if (ret)
		return ret;
	m->vreg = devm_regulator_get(dev, "VREG");
	if (IS_ERR(m->vreg))
		return PTR_ERR(m->vreg);
	m->vb = devm_regulator_get(dev, "VB");
	if (IS_ERR(m->vb))
		return PTR_ERR(m->vb);
	platform_set_drvdata(pdev, m);
	return devm_snd_soc_register_component(dev, &w202_mic_component, &w202_mic_dai, 1);
}

static const struct of_device_id w202_mic_match[] = {
	{ .compatible = "zte,w202ds-mic-codec" }, { }
};
MODULE_DEVICE_TABLE(of, w202_mic_match);

static struct platform_driver w202_mic_driver = {
	.probe = w202_mic_probe,
	.driver = { .name = "w202-audio-mic", .of_match_table = w202_mic_match },
};
module_platform_driver(w202_mic_driver);
MODULE_DESCRIPTION("W202DS UMP9620/AGCP main microphone codec");
MODULE_LICENSE("GPL");
