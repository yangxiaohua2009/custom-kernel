// SPDX-License-Identifier: GPL-2.0-only
//
// es8326.c -- es8326 ALSA SoC audio driver
// Copyright Everest Semiconductor Co., Ltd
//
// Authors: David Yang <yangxiaohua@everest-semi.com>
//

#include <linux/clk.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include "es8326.h"

static int chip_state = 0;
struct es8326_priv {
	struct clk *mclk;
	struct snd_pcm_hw_constraint_list *sysclk_constraints;
	struct i2c_client *i2c;
	struct regmap *regmap;
	struct snd_soc_component *component;
	struct delayed_work jack_detect_work;
	struct delayed_work pcm_pop_work;
	bool amic;
	bool start;
	bool muted;
	bool hp_inserted;
	bool spk_gpio_level;
	bool hp_det_level;
	struct snd_soc_jack *jack;
	int irq;
	int chip_state;
	/* The lock protects the situation that an irq is generated
	 * while enabling or disabling irq.
	 */
	struct mutex lock;
	u8 mic1_src;
	u8 mic2_src;
	u8 jack_pol;
	bool jd_inverted;
	unsigned int sysclk;
};

static void es8326_jack_detect_handler(struct work_struct *work)
{
	struct es8326_priv *es8326 =
		container_of(work, struct es8326_priv, jack_detect_work.work);
	struct snd_soc_component *comp = es8326->component;
	unsigned int iface;

	iface = snd_soc_component_read(comp, ES8326_HP_DECTECT_FB);
	dev_dbg(comp->dev, "gpio flag %#04x", iface);
	if ((iface & ES8326_HPINSERT_FLAG) == 0) {
		dev_dbg(comp->dev, "No headset detected");
		snd_soc_jack_report(es8326->jack, 0, SND_JACK_HEADSET);
		snd_soc_component_write(comp, ES8326_ADC1_SRC_2A, es8326->mic2_src);
		snd_soc_component_write(comp, ES8326_ANA_MICBIAS_1B, 0x70);
	} else if ((iface & ES8326_HPINSERT_FLAG) == ES8326_HPINSERT_FLAG) {
		if ((iface & ES8326_HPBUTTON_FLAG) == 0x00) {
			dev_dbg(comp->dev, "Headset detected");
			snd_soc_jack_report(es8326->jack, SND_JACK_HEADSET, SND_JACK_HEADSET);
			snd_soc_component_write(comp, ES8326_ADC1_SRC_2A, es8326->mic1_src);
		} else {
			dev_dbg(comp->dev, "Headphone detected");
			snd_soc_jack_report(es8326->jack, SND_JACK_HEADPHONE, SND_JACK_HEADSET);
		}
	}
}

static irqreturn_t es8326_irq(int irq, void *dev_id)
{
	struct es8326_priv *es8326 = dev_id;
	struct snd_soc_component *comp = es8326->component;

	snd_soc_component_write(comp, ES8326_ANA_MICBIAS_1B, 0x7c);

	queue_delayed_work(system_wq, &es8326->jack_detect_work,
			   msecs_to_jiffies(300));

	return IRQ_HANDLED;
}

static const SNDRV_CTL_TLVD_DECLARE_DB_SCALE(dac_vol_tlv, -9550, 50, 0);
static const SNDRV_CTL_TLVD_DECLARE_DB_SCALE(adc_vol_tlv, -9550, 50, 0);
static const SNDRV_CTL_TLVD_DECLARE_DB_SCALE(adc_analog_pga_tlv, 0, 300, 0);
static const SNDRV_CTL_TLVD_DECLARE_DB_SCALE(adc_pga_tlv, 0, 600, 0);
static const SNDRV_CTL_TLVD_DECLARE_DB_SCALE(softramp_rate, 0, 100, 0);
static const SNDRV_CTL_TLVD_DECLARE_DB_SCALE(drc_target_tlv, -3200, 200, 0);
static const SNDRV_CTL_TLVD_DECLARE_DB_SCALE(drc_recovery_tlv, -125, 250, 0);

static const char *const winsize[] = {
	"0.25db/2  LRCK",
	"0.25db/4  LRCK",
	"0.25db/8  LRCK",
	"0.25db/16  LRCK",
	"0.25db/32  LRCK",
	"0.25db/64  LRCK",
	"0.25db/128  LRCK",
	"0.25db/256  LRCK",
	"0.25db/512  LRCK",
	"0.25db/1024  LRCK",
	"0.25db/2048  LRCK",
	"0.25db/4096  LRCK",
	"0.25db/8192  LRCK",
	"0.25db/16384  LRCK",
	"0.25db/32768  LRCK",
	"0.25db/65536  LRCK",
};

static const char *const dacpol_txt[] =	{
	"Normal", "R Invert", "L Invert", "L + R Invert" };
static const struct soc_enum dacpol =
	SOC_ENUM_SINGLE(0x4d, 4, 4, dacpol_txt);
static const struct soc_enum alc_winsize =
	SOC_ENUM_SINGLE(0x2e, 4, 16, winsize);
static const struct soc_enum drc_winsize =
	SOC_ENUM_SINGLE(0x54, 4, 16, winsize);
static const struct snd_kcontrol_new es8326_snd_controls[] = {
	SOC_SINGLE_TLV("DAC Playback Volume", ES8326_DAC_VOL_50, 0, 0xbf, 0, dac_vol_tlv),
	SOC_ENUM("Playback Polarity", dacpol),
	SOC_SINGLE_TLV("DAC Ramp Rate", ES8326_DAC_RAMPRATE_4E, 0, 0x0f, 0, softramp_rate),
	SOC_SINGLE_TLV("DRC Recovery Level", ES8326_DRC_RECOVERY_53, 0, 4, 0, drc_recovery_tlv),
	SOC_ENUM("DRC Winsize", drc_winsize),
	SOC_SINGLE_TLV("DRC Target Level", ES8326_DRC_WINSIZE_54, 0, 0x0f, 0, drc_target_tlv),

	SOC_DOUBLE_R_TLV("ADC Capture Volume", ES8326_ADC1_VOL_2C, ES8326_ADC2_VOL_2D, 0, 0xff, 0,
			 adc_vol_tlv),
	SOC_DOUBLE_TLV("ADC PGA Volume", ES8326_ADC_SCALE_29, 4, 0, 5, 0, adc_pga_tlv),
	SOC_SINGLE_TLV("ADC PGA Gain Volume", ES8326_PAGGAIN_23, 0, 10, 0, adc_analog_pga_tlv),
	SOC_SINGLE_TLV("ADC Ramp Rate", ES8326_ADC_RAMPRATE_2E, 0, 0x0f, 0, softramp_rate),
	SOC_SINGLE("ALC Capture Switch", ES8326_ALC_RECOVERY_32, 3, 1, 0),
	SOC_SINGLE_TLV("ALC Capture Recovery Level", ES8326_ALC_LEVEL_33, 0, 4, 0, drc_recovery_tlv),
	SOC_ENUM("ALC Capture Winsize", alc_winsize),
	SOC_SINGLE_TLV("ALC Capture Target Level", ES8326_ALC_LEVEL_33, 0, 0x0f, 0, drc_target_tlv),
};

static const struct snd_soc_dapm_widget es8326_dapm_widgets[] = {
	SND_SOC_DAPM_INPUT("MIC1"),
	SND_SOC_DAPM_INPUT("MIC2"),

	SND_SOC_DAPM_ADC("Right ADC", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_ADC("Left ADC", NULL, SND_SOC_NOPM, 0, 0),

	/* Digital Interface */
	SND_SOC_DAPM_AIF_OUT("I2S OUT", "I2S1 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("I2S IN", "I2S1 Playback", 0, SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_DAC("Right DAC", NULL, ES8326_ANA_PDN_16, 0, 1),
	SND_SOC_DAPM_DAC("Left DAC", NULL, ES8326_ANA_PDN_16, 1, 1),
	SND_SOC_DAPM_PGA("LHPMIX", ES8326_DAC2HPMIX_25, 7, 0, NULL, 0),
	SND_SOC_DAPM_PGA("RHPMIX", ES8326_DAC2HPMIX_25, 3, 0, NULL, 0),
	//SND_SOC_DAPM_SUPPLY("HPOR Cal", ES8326_HP_CAL_27, 7, 1, NULL, 0),
	//SND_SOC_DAPM_SUPPLY("HPOL Cal", ES8326_HP_CAL_27, 3, 1, NULL, 0),
	//SND_SOC_DAPM_REG(snd_soc_dapm_supply, "HPOR Supply", ES8326_HP_CAL_27,
	//		 ES8326_HPOR_SHIFT, 7, 7, 0),
	//SND_SOC_DAPM_REG(snd_soc_dapm_supply, "HPOL Supply", ES8326_HP_CAL_27,
	//		 0, 7, 7, 0),
	SND_SOC_DAPM_OUTPUT("HPOL"),
	SND_SOC_DAPM_OUTPUT("HPOR"),
};

static const struct snd_soc_dapm_route es8326_dapm_routes[] = {
	{"Left ADC", NULL, "MIC1"},
	{"Right ADC", NULL, "MIC2"},

	{"I2S OUT", NULL, "Left ADC"},
	{"I2S OUT", NULL, "Right ADC"},

	{"Right DAC", NULL, "I2S IN"},
	{"Left DAC", NULL, "I2S IN"},

	{"LHPMIX", NULL, "Left DAC"},
	{"RHPMIX", NULL, "Right DAC"},

	//{"HPOR", NULL, "HPOR Cal"},
	//{"HPOL", NULL, "HPOL Cal"},
	//{"HPOR", NULL, "HPOR Supply"},
	//HPOL", NULL, "HPOL Supply"},

	{"HPOL", NULL, "LHPMIX"},
	{"HPOR", NULL, "RHPMIX"},
};
static bool es8326_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0xfb:
	case 0xfc:
	case 0x27:
		return true;
	default:
		return false;
	}
}
const struct regmap_config es8326_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
	.volatile_reg = es8326_volatile_register,
	.cache_type = REGCACHE_RBTREE,
};

struct _coeff_div {
	u16 fs;
	u32 rate;
	u32 mclk;
	u8 reg4;
	u8 reg5;
	u8 reg6;
	u8 reg7;
	u8 reg8;
	u8 reg9;
	u8 rega;
	u8 regb;
};

/* codec hifi mclk clock divider coefficients */
/* {ratio, LRCK, MCLK, REG04, REG05, REG06, REG07, REG08, REG09, REG10, REG11} */
static const struct _coeff_div coeff_div[] = {
	{32, 8000, 256000, 0x60, 0x00, 0x0F, 0x75, 0x0A, 0x1B, 0x1F, 0x7F},
	{32, 16000, 512000, 0x20, 0x00, 0x0D, 0x75, 0x0A, 0x1B, 0x1F, 0x3F},
	{32, 44100, 1411200, 0x00, 0x00, 0x13, 0x2D, 0x0A, 0x0A, 0x1F, 0x1F},
	{32, 48000, 1536000, 0x00, 0x00, 0x13, 0x2D, 0x0A, 0x0A, 0x1F, 0x1F},
	{36, 8000, 288000, 0x20, 0x00, 0x0D, 0x75, 0x0A, 0x1B, 0x23, 0x47},
	{36, 16000, 576000, 0x20, 0x00, 0x0D, 0x75, 0x0A, 0x1B, 0x23, 0x47},
	{48, 8000, 384000, 0x60, 0x02, 0x1F, 0x75, 0x0A, 0x1B, 0x1F, 0x7F},
	{48, 16000, 768000, 0x20, 0x02, 0x0F, 0x75, 0x0A, 0x1B, 0x1F, 0x3F},
	{48, 48000, 2304000, 0x00, 0x02, 0x0D, 0x2D, 0x0A, 0x0A, 0x1F, 0x1F},
	{64, 8000, 512000, 0x60, 0x00, 0x0D, 0x75, 0x0A, 0x1B, 0x1F, 0x7F},
	{64, 16000, 1024000, 0x20, 0x00, 0x05, 0x75, 0x0A, 0x1B, 0x1F, 0x3F},

	{64, 44100, 2822400, 0x00, 0x00, 0x11, 0x2D, 0x0A, 0x0A, 0x1F, 0x1F},
	{64, 48000, 3072000, 0x00, 0x00, 0x11, 0x2D, 0x0A, 0x0A, 0x1F, 0x1F},
	{72, 8000, 576000, 0x20, 0x00, 0x13, 0x35, 0x0A, 0x1B, 0x23, 0x47},
	{72, 16000, 1152000, 0x20, 0x00, 0x05, 0x75, 0x0A, 0x1B, 0x23, 0x47},
	{96, 8000, 768000, 0x60, 0x02, 0x1D, 0x75, 0x0A, 0x1B, 0x1F, 0x7F},
	{96, 16000, 1536000, 0x20, 0x02, 0x0D, 0x75, 0x0A, 0x1B, 0x1F, 0x3F},
	{100, 48000, 4800000, 0x04, 0x04, 0x3F, 0x6D, 0x38, 0x08, 0x4f, 0x1f},
	{125, 48000, 6000000, 0x04, 0x04, 0x1F, 0x2D, 0x0A, 0x0A, 0x27, 0x27},
	{128, 8000, 1024000, 0x60, 0x00, 0x13, 0x35, 0x0A, 0x1B, 0x1F, 0x7F},
	{128, 16000, 2048000, 0x20, 0x00, 0x11, 0x35, 0x0A, 0x1B, 0x1F, 0x3F},

	{128, 44100, 5644800, 0x00, 0x00, 0x01, 0x2D, 0x0A, 0x0A, 0x1F, 0x1F},
	{128, 48000, 6144000, 0x00, 0x00, 0x01, 0x2D, 0x0A, 0x0A, 0x1F, 0x1F},
	{144, 8000, 1152000, 0x20, 0x00, 0x03, 0x35, 0x0A, 0x1B, 0x23, 0x47},
	{144, 16000, 2304000, 0x20, 0x00, 0x11, 0x35, 0x0A, 0x1B, 0x23, 0x47},
	{192, 8000, 1536000, 0x60, 0x02, 0x0D, 0x75, 0x0A, 0x1B, 0x1F, 0x7F},
	{192, 16000, 3072000, 0x20, 0x02, 0x05, 0x75, 0x0A, 0x1B, 0x1F, 0x3F},
	{200, 48000, 9600000, 0x04, 0x04, 0x0F, 0x2D, 0x0A, 0x0A, 0x1F, 0x1F},
	{250, 48000, 12000000, 0x04, 0x04, 0x0F, 0x2D, 0x0A, 0x0A, 0x27, 0x27},
	{256, 8000, 2048000, 0x60, 0x00, 0x11, 0x35, 0x0A, 0x1B, 0x1F, 0x7F},
	{256, 16000, 4096000, 0x20, 0x00, 0x01, 0x35, 0x0A, 0x1B, 0x1F, 0x3F},

	{256, 44100, 11289600, 0x00, 0x00, 0x10, 0x2D, 0x0A, 0x0A, 0x1F, 0x1F},
	{256, 48000, 12288000, 0x00, 0x00, 0x30, 0x2D, 0x0A, 0x0A, 0x1F, 0x1F},
	{288, 8000, 2304000, 0x20, 0x00, 0x01, 0x35, 0x0A, 0x1B, 0x23, 0x47},
	{384, 8000, 3072000, 0x60, 0x02, 0x05, 0x75, 0x0A, 0x1B, 0x1F, 0x7F},
	{384, 16000, 6144000, 0x20, 0x02, 0x03, 0x35, 0x0A, 0x1B, 0x1F, 0x3F},
	{384, 48000, 18432000, 0x00, 0x02, 0x01, 0x2D, 0x0A, 0x0A, 0x1F, 0x1F},
	{400, 48000, 19200000, 0x09, 0x04, 0x0f, 0x6d, 0x3a, 0x0A, 0x4F, 0x1F},
	{500, 48000, 24000000, 0x18, 0x04, 0x1F, 0x2D, 0x0A, 0x0A, 0x1F, 0x1F},
	{512, 8000, 4096000, 0x60, 0x00, 0x01, 0x35, 0x0A, 0x1B, 0x1F, 0x7F},
	{512, 16000, 8192000, 0x20, 0x00, 0x10, 0x35, 0x0A, 0x1B, 0x1F, 0x3F},

	{512, 44100, 22579200, 0x00, 0x00, 0x00, 0x2D, 0x0A, 0x0A, 0x1F, 0x1F},
	{512, 48000, 24576000, 0x00, 0x00, 0x00, 0x2D, 0x0A, 0x0A, 0x1F, 0x1F},
	{768, 8000, 6144000, 0x60, 0x02, 0x11, 0x35, 0x0A, 0x1B, 0x1F, 0x7F},
	{768, 16000, 12288000, 0x20, 0x02, 0x01, 0x35, 0x0A, 0x1B, 0x1F, 0x3F},
	{800, 48000, 38400000, 0x00, 0x18, 0x13, 0x2D, 0x0A, 0x0A, 0x1F, 0x1F},
	{1024, 8000, 8192000, 0x60, 0x00, 0x10, 0x35, 0x0A, 0x1B, 0x1F, 0x7F},
	{1024, 16000, 16384000, 0x20, 0x00, 0x00, 0x35, 0x0A, 0x1B, 0x1F, 0x3F},
	{1152, 16000, 18432000, 0x20, 0x08, 0x11, 0x35, 0x0A, 0x1B, 0x1F, 0x3F},
	{1536, 8000, 12288000, 0x60, 0x02, 0x01, 0x35, 0x0A, 0x1B, 0x1F, 0x7F},

	{1536, 16000, 24576000, 0x20, 0x02, 0x10, 0x35, 0x0A, 0x1B, 0x1F, 0x3F},
	{1625, 8000, 13000000, 0x0C, 0x18, 0x1F, 0x2D, 0x0A, 0x0A, 0x27, 0x27},
	{1625, 16000, 26000000, 0x0C, 0x18, 0x1F, 0x2D, 0x0A, 0x0A, 0x27, 0x27},
	{2048, 8000, 16384000, 0x60, 0x00, 0x00, 0x35, 0x0A, 0x1B, 0x1F, 0x7F},
	{2304, 8000, 18432000, 0x40, 0x02, 0x10, 0x35, 0x0A, 0x1B, 0x1F, 0x5F},
	{3072, 8000, 24576000, 0x60, 0x02, 0x10, 0x35, 0x0A, 0x1B, 0x1F, 0x7F},
	{3250, 8000, 26000000, 0x0C, 0x18, 0x0F, 0x2D, 0x0A, 0x0A, 0x27, 0x27},

};

static inline int get_coeff(int mclk, int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(coeff_div); i++) {
		if (coeff_div[i].rate == rate && coeff_div[i].mclk == mclk)
			return i;
	}

	return -EINVAL;
}

static unsigned int es8326_rates[] = {
	8000, 12000, 16000, 24000, 32000, 48000, 96000
};

static struct snd_pcm_hw_constraint_list es8326_constraints = {
	.count = ARRAY_SIZE(es8326_rates),
	.list = es8326_rates,
};

/*
 * Note that this should be called from init rather than from hw_params.
 */
static int es8326_set_dai_sysclk(struct snd_soc_dai *codec_dai,
				 int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_component *codec = codec_dai->component;
	struct es8326_priv *es8326 = snd_soc_component_get_drvdata(codec);

	es8326->sysclk = freq;

	if (freq == 0) {
		es8326->sysclk_constraints->list = NULL;
		es8326->sysclk_constraints->count = 0;
		return 0;
	}

	es8326->sysclk_constraints = &es8326_constraints;
	return 0;
}

static int es8326_set_dai_fmt(struct snd_soc_dai *codec_dai, unsigned int fmt)
{
	struct snd_soc_component *component = codec_dai->component;
	u8 iface = 0;

	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_CBP_CFP:
		snd_soc_component_update_bits(component, ES8326_RESET_00,
					      ES8326_MASTER_MODE_EN, ES8326_MASTER_MODE_EN);
		break;
	case SND_SOC_DAIFMT_CBC_CFC:
		break;
	default:
		return -EINVAL;
	}

	/* interface format */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		dev_err(component->dev, "Codec driver does not support right justified\n");
		return -EINVAL;
	case SND_SOC_DAIFMT_LEFT_J:
		iface |= ES8326_DAIFMT_LEFT_J;
		break;
	case SND_SOC_DAIFMT_DSP_A:
		iface |= ES8326_DAIFMT_DSP_A;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		iface |= ES8326_DAIFMT_DSP_B;
		break;
	default:
		return -EINVAL;
	}

	snd_soc_component_update_bits(component, ES8326_FMT_13, ES8326_DAIFMT_MASK, iface);

	return 0;
}
static int es8326_resume(struct snd_soc_component *component)
{
	struct es8326_priv *es8326 = snd_soc_component_get_drvdata(component);

	regmap_write(es8326->regmap, ES8326_CLK_CTL_01, ES8326_CLK_ON);
	/* Two channel ADC */
	regmap_write(es8326->regmap, ES8326_PULLUP_CTL_F9, 0x02);
	regmap_write(es8326->regmap, ES8326_CLK_INV_02, 0x00);
	regmap_write(es8326->regmap, ES8326_CLK_DIV_CPC_0C, 0x1F);
	regmap_write(es8326->regmap, ES8326_CLK_VMIDS1_10, 0xC8);
	regmap_write(es8326->regmap, ES8326_CLK_VMIDS2_11, 0x88);
	regmap_write(es8326->regmap, ES8326_CLK_CAL_TIME_12, 0x20);
	regmap_write(es8326->regmap, ES8326_DAC_MUTE_14, 0x00);
	regmap_write(es8326->regmap, ES8326_ANA_LOWPOWER_19, 0xF0);
	regmap_write(es8326->regmap, ES8326_SYS_BIAS_1D, 0x08);
	regmap_write(es8326->regmap, ES8326_DAC2HPMIX_25, 0x22);
	regmap_write(es8326->regmap, ES8326_ADC1_SRC_2A, es8326->mic1_src);
	regmap_write(es8326->regmap, ES8326_ADC2_SRC_2B, es8326->mic2_src);
	regmap_write(es8326->regmap, ES8326_HPJACK_TIMER_56, 0x88);
	regmap_write(es8326->regmap, ES8326_HP_DET_57,
	//	     ES8326_HP_DET_SRC_PIN9 | es8326->jack_pol);
	0x17);
	regmap_write(es8326->regmap, ES8326_INT_SOURCE_58, 0x08);
	regmap_write(es8326->regmap, ES8326_INTOUT_IO_59, 0x45);
	regmap_write(es8326->regmap, ES8326_RESET_00, ES8326_CSM_ON);
	snd_soc_component_update_bits(component, ES8326_PAGGAIN_23,
				      ES8326_MIC_SEL_MASK, ES8326_MIC1_SEL);
#if 1
	regmap_write(es8326->regmap, ES8326_SDINOUT1_IO_5A,
		    (ES8326_IO_DMIC_CLK << ES8326_SDINOUT1_SHIFT));
	regmap_write(es8326->regmap, ES8326_SDINOUT23_IO_5B, ES8326_IO_INPUT);
	regmap_write(es8326->regmap, ES8326_CLK_RESAMPLE_03, 0x05);
	regmap_write(es8326->regmap, ES8326_HP_DRVIER_24, 0x00);
	regmap_write(es8326->regmap, ES8326_VMIDSEL_18, 0x02);
	regmap_write(es8326->regmap, ES8326_ANA_PDN_16, 0x00);
	regmap_write(es8326->regmap, ES8326_PGA_PDN_17, 0x40);
	regmap_write(es8326->regmap, ES8326_DAC2HPMIX_25, 0xAA);
	regmap_write(es8326->regmap, ES8326_ADC_MUTE_15, 0x00);
	regmap_write(es8326->regmap, 0x23, 0x1a);
	regmap_write(es8326->regmap, 0x29, 0x55);
	regmap_write(es8326->regmap, 0x2c, 0xe1);
	regmap_write(es8326->regmap, 0x2d, 0xe1);
	//regmap_write(es8326->regmap, 0x50, 0x00);
	regmap_write(es8326->regmap, 0x51, 0x1a);
	regmap_write(es8326->regmap, 0x4e, 0xe1);
#endif
        es8326->chip_state = 1;
	es8326_irq(es8326->irq, es8326);
	return 0;
}
static int es8326_pcm_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct snd_soc_component *codec = dai->component;
	struct es8326_priv *es8326 = snd_soc_component_get_drvdata(codec);
	u8 srate = 0;
	int coeff;

	if(es8326->chip_state == 0)
	        es8326_resume(codec);
	regmap_write(es8326->regmap, 0x50, 0xbf);//new
	regmap_write(es8326->regmap, 0x4e, 0xe2);//new
	queue_delayed_work(system_wq, &es8326->pcm_pop_work, msecs_to_jiffies(100));
	coeff = get_coeff(es8326->sysclk, params_rate(params));
        //coeff = get_coeff(4800000, params_rate(params));
	/* bit size */
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		srate |= ES8326_S16_LE;
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
		srate |= ES8326_S20_3_LE;
		break;
	case SNDRV_PCM_FORMAT_S18_3LE:
		srate |= ES8326_S18_LE;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		srate |= ES8326_S24_LE;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		srate |= ES8326_S32_LE;
		break;
	default:
		break;
	}

	/* set iface & srate */
	snd_soc_component_update_bits(codec, ES8326_FMT_13, ES8326_DATA_LEN_MASK, srate);

	if (coeff >= 0) {
		regmap_write(es8326->regmap,  ES8326_CLK_DIV1_04,
			     coeff_div[coeff].reg4);
		regmap_write(es8326->regmap,  ES8326_CLK_DIV2_05,
			     coeff_div[coeff].reg5);
		regmap_write(es8326->regmap,  ES8326_CLK_DLL_06,
			     coeff_div[coeff].reg6);
		regmap_write(es8326->regmap,  ES8326_CLK_MUX_07,
			     coeff_div[coeff].reg7);
		regmap_write(es8326->regmap,  ES8326_CLK_ADC_SEL_08,
			     coeff_div[coeff].reg8);
		regmap_write(es8326->regmap,  ES8326_CLK_DAC_SEL_09,
			     coeff_div[coeff].reg9);
		regmap_write(es8326->regmap,  ES8326_CLK_ADC_OSR_0A,
			     coeff_div[coeff].rega);
		regmap_write(es8326->regmap,  ES8326_CLK_DAC_OSR_0B,
			     coeff_div[coeff].regb);
	}

	return 0;
}

static int es8326_set_bias_level(struct snd_soc_component *codec,
				 enum snd_soc_bias_level level)
{
	struct es8326_priv *es8326 = snd_soc_component_get_drvdata(codec);
	int ret;

	switch (level) {
	case SND_SOC_BIAS_ON:		
	if (!IS_ERR(es8326->mclk)) {
			if (snd_soc_component_get_bias_level(codec) == SND_SOC_BIAS_ON) {
				clk_disable_unprepare(es8326->mclk);
			} else {
				ret = clk_prepare_enable(es8326->mclk);
				if (ret)
					return ret;
			}
		}
		//regmap_write(es8326->regmap, ES8326_CLK_CTL_01, ES8326_CLK_ON);
		//regmap_write(es8326->regmap, ES8326_RESET_00, ES8326_PWRUP_SEQ_EN);
		regmap_write(es8326->regmap, 0x50, 0xbf);//new
		regmap_write(es8326->regmap, 0x4e, 0xe1);//new

		//regmap_write(es8326->regmap, ES8326_INTOUT_IO_59, 0x45);
		//regmap_write(es8326->regmap, ES8326_SDINOUT1_IO_5A,
		//	    (ES8326_IO_DMIC_CLK << ES8326_SDINOUT1_SHIFT));
		//regmap_write(es8326->regmap, ES8326_SDINOUT23_IO_5B, ES8326_IO_INPUT);
		//regmap_write(es8326->regmap, ES8326_CLK_RESAMPLE_03, 0x05);
		//regmap_write(es8326->regmap, ES8326_HP_DRVIER_24, 0x00);
		//regmap_write(es8326->regmap, ES8326_VMIDSEL_18, 0x02);
		//regmap_write(es8326->regmap, ES8326_ANA_PDN_16, 0x00);
		//regmap_write(es8326->regmap, ES8326_PGA_PDN_17, 0x40);
		//regmap_write(es8326->regmap, ES8326_DAC2HPMIX_25, 0xAA);
		//regmap_write(es8326->regmap, ES8326_ADC_MUTE_15, 0x00);
		//regmap_write(es8326->regmap, ES8326_RESET_00, ES8326_CSM_ON);

		break;
	case SND_SOC_BIAS_PREPARE:
		break;
	case SND_SOC_BIAS_STANDBY:
		break;
	case SND_SOC_BIAS_OFF:
	        break;
		if (es8326->mclk)
			clk_disable_unprepare(es8326->mclk);
		regmap_write(es8326->regmap, ES8326_ADC_MUTE_15, 0x1F);
		regmap_write(es8326->regmap, ES8326_DAC2HPMIX_25, 0x11);
		regmap_write(es8326->regmap, ES8326_RESET_00, ES8326_CSM_OFF);
		regmap_write(es8326->regmap, ES8326_PGA_PDN_17, 0xF8);
		regmap_write(es8326->regmap, ES8326_ANA_PDN_16, 0xFB);
		regmap_write(es8326->regmap, ES8326_VMIDSEL_18, 0x00);
		regmap_write(es8326->regmap, ES8326_HP_DRVIER_24, 0x0F);
		regmap_write(es8326->regmap, ES8326_INT_SOURCE_58, 0x08);
		regmap_write(es8326->regmap, ES8326_SDINOUT1_IO_5A, ES8326_IO_INPUT);
		regmap_write(es8326->regmap, ES8326_SDINOUT23_IO_5B, ES8326_IO_INPUT);
		regmap_write(es8326->regmap, ES8326_RESET_00,
			     ES8326_CODEC_RESET | ES8326_PWRUP_SEQ_EN);
		//regmap_write(es8326->regmap, ES8326_CLK_CTL_01, ES8326_CLK_OFF);

		break;
	}

	return 0;
}

static void es8326_pcm_shutdown(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *dai)
{
	struct snd_soc_component* component = dai->component;
	struct es8326_priv *es8326 = snd_soc_component_get_drvdata(component);	
        
	cancel_delayed_work(&es8326->pcm_pop_work); 
	//snd_soc_component_write(component, 0x27, 0x00);
	regmap_write(es8326->regmap, 0x50, 0x00);//new
	regmap_write(es8326->regmap, 0x4e, 0xe0);//new
regmap_write(es8326->regmap, 0x28, 0x87);
regmap_write(es8326->regmap, 0x25, 0x11);
regmap_write(es8326->regmap, 0x24, 0x0f);
	chip_state = 0;	
}

static void pcm_pop_work_events(struct work_struct *work)
{
	struct es8326_priv *es8326 =
		container_of(work, struct es8326_priv, pcm_pop_work.work);
	struct snd_soc_component *comp = es8326->component;	
	unsigned int muteflag;
	unsigned int flag_27;

#if 1
	muteflag = snd_soc_component_read(comp, 0xfc);
	if((muteflag & 0x01)&&(muteflag & 0x40)){
	        printk("es8326 pcm mute flag %#04x",muteflag);
		if(chip_state==0){
			regmap_write(es8326->regmap, ES8326_INTOUT_IO_59, 0x45);
			regmap_write(es8326->regmap, ES8326_SDINOUT1_IO_5A,
				    (ES8326_IO_DMIC_CLK << ES8326_SDINOUT1_SHIFT));
			regmap_write(es8326->regmap, ES8326_SDINOUT23_IO_5B, ES8326_IO_INPUT);
			regmap_write(es8326->regmap, ES8326_CLK_RESAMPLE_03, 0x05);
			regmap_write(es8326->regmap, ES8326_HP_DRVIER_24, 0x20);
			regmap_write(es8326->regmap, 0x28, 0xa7);
			regmap_write(es8326->regmap, 0x28, 0x97);
			regmap_write(es8326->regmap, ES8326_VMIDSEL_18, 0x02);
			regmap_write(es8326->regmap, ES8326_ANA_PDN_16, 0x00);
			regmap_write(es8326->regmap, ES8326_PGA_PDN_17, 0x40);
			regmap_write(es8326->regmap, ES8326_DAC2HPMIX_25, 0xAA);
			regmap_write(es8326->regmap, ES8326_ADC_MUTE_15, 0x00);
			regmap_write(es8326->regmap, ES8326_RESET_00, ES8326_CSM_ON);

			regmap_write(es8326->regmap, 0x4d, 0x00);
			regmap_write(es8326->regmap, 0x27, 0x77);
		}
		chip_state++;
		if(chip_state==4){
			regmap_write(es8326->regmap,  0x27, 0x00);
			printk("es8326 pcm 27 00");
			chip_state=1;
		}
	}else{
		regmap_write(es8326->regmap, ES8326_INTOUT_IO_59, 0x45);
		regmap_write(es8326->regmap, ES8326_SDINOUT1_IO_5A,
			    (ES8326_IO_DMIC_CLK << ES8326_SDINOUT1_SHIFT));
		regmap_write(es8326->regmap, ES8326_SDINOUT23_IO_5B, ES8326_IO_INPUT);
		regmap_write(es8326->regmap, ES8326_CLK_RESAMPLE_03, 0x05);
		regmap_write(es8326->regmap, ES8326_HP_DRVIER_24, 0x20);
		regmap_write(es8326->regmap, 0x28, 0xa7);
		regmap_write(es8326->regmap, 0x28, 0x97);
		regmap_write(es8326->regmap, ES8326_VMIDSEL_18, 0x02);
		regmap_write(es8326->regmap, ES8326_ANA_PDN_16, 0x00);
		regmap_write(es8326->regmap, ES8326_PGA_PDN_17, 0x40);
		regmap_write(es8326->regmap, ES8326_DAC2HPMIX_25, 0xAA);
		regmap_write(es8326->regmap, ES8326_ADC_MUTE_15, 0x00);
		regmap_write(es8326->regmap, ES8326_RESET_00, ES8326_CSM_ON);

		regmap_write(es8326->regmap,  0x4d, 0x00);
		regmap_write(es8326->regmap,  0x27, 0x77);
		chip_state=1;
		printk("es8326 pcm normal flag %#04x",muteflag);
	}
	queue_delayed_work(system_wq, &es8326->pcm_pop_work, msecs_to_jiffies(50));
#endif
}

static void __exit pcm_pop_work_exit(struct es8326_priv *es8326)  
{  
    int ret;  
    ret = cancel_delayed_work(&es8326->pcm_pop_work);   
} 
#define es8326_RATES SNDRV_PCM_RATE_8000_96000

#define es8326_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE |\
	SNDRV_PCM_FMTBIT_S24_LE)

static const struct snd_soc_dai_ops es8326_ops = {
	.hw_params = es8326_pcm_hw_params,
	.shutdown = es8326_pcm_shutdown,
	.set_fmt = es8326_set_dai_fmt,
	.set_sysclk = es8326_set_dai_sysclk,
};

static struct snd_soc_dai_driver es8326_dai = {
	.name = "ES8326 HiFi",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = es8326_RATES,
		.formats = es8326_FORMATS,
		},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = es8326_RATES,
		.formats = es8326_FORMATS,
		},
	.ops = &es8326_ops,
	.symmetric_rate = 1,
};


static int es8326_probe(struct snd_soc_component *component)
{
	struct es8326_priv *es8326 = snd_soc_component_get_drvdata(component);
	int ret;
	u8 reg;
	
        es8326->chip_state = 0;
	es8326->component = component;
	es8326->jd_inverted = device_property_read_bool(component->dev,
							"everest,jack-detect-inverted");

	ret = device_property_read_u8(component->dev, "everest,mic1-src", &es8326->mic1_src);
	if (ret != 0) {
		dev_dbg(component->dev, "mic1-src return %d", ret);
		es8326->mic1_src = ES8326_ADC_AMIC;
	}
	dev_dbg(component->dev, "mic1-src %x", es8326->mic1_src);

	ret = device_property_read_u8(component->dev, "everest,mic2-src", &es8326->mic2_src);
	if (ret != 0) {
		dev_dbg(component->dev, "mic2-src return %d", ret);
		es8326->mic2_src = ES8326_ADC_DMIC;
	}
	dev_dbg(component->dev, "mic2-src %x", es8326->mic2_src);

	ret = device_property_read_u8(component->dev, "everest,jack-pol", &es8326->jack_pol);
	if (ret != 0) {
		dev_dbg(component->dev, "jack-pol return %d", ret);
		es8326->jack_pol = ES8326_HP_DET_BUTTON_POL | ES8326_HP_TYPE_OMTP;
	}
	dev_dbg(component->dev, "jack-pol %x", es8326->jack_pol);
	
	reg = snd_soc_component_read(component, ES8326_CHIP_VERSION_FF);
	if((reg && ES8326_VERSION_B) == 1)
	{
		regmap_write(es8326->regmap, ES8326_ANA_VSEL_1C, 0x7F);
		regmap_write(es8326->regmap, ES8326_VMIDLOW_22, 0x0F);
		regmap_write(es8326->regmap, ES8326_DAC2HPMIX_25, 0xAA);
		regmap_write(es8326->regmap, ES8326_HP_DRVIER_24, 0x20);
	}
	INIT_DELAYED_WORK(&es8326->pcm_pop_work,
				pcm_pop_work_events);

	return 0;
}

static void es8326_enable_jack_detect(struct snd_soc_component *component,
				struct snd_soc_jack *jack)
{
	struct es8326_priv *es8326 = snd_soc_component_get_drvdata(component);

	dev_dbg(component->dev, "Enter into %s\n", __func__);

	mutex_lock(&es8326->lock);
	if (es8326->jd_inverted)
		snd_soc_component_update_bits(component, ES8326_HP_DET_57,
					      ES8326_HP_DET_JACK_POL, ~es8326->jack_pol);
	es8326->jack = jack;

	mutex_unlock(&es8326->lock);
	/* Enable irq and sync initial jack state */
	enable_irq(es8326->irq);
	es8326_irq(es8326->irq, es8326);
}
static int es8326_suspend(struct snd_soc_component *component){
	struct es8326_priv *es8326 = snd_soc_component_get_drvdata(component);

	//cancel_delayed_work(&es8326->pcm_pop_work);
	return 0;
}
static void es8326_disable_jack_detect(struct snd_soc_component *component)
{
	struct es8326_priv *es8326 = snd_soc_component_get_drvdata(component);

	dev_dbg(component->dev, "Enter into %s\n", __func__);
	if (!es8326->jack)
		return; /* Already disabled (or never enabled) */

	disable_irq(es8326->irq);
	cancel_delayed_work_sync(&es8326->jack_detect_work);
	pcm_pop_work_exit(es8326);
	mutex_lock(&es8326->lock);
	if (es8326->jack->status & SND_JACK_MICROPHONE)
		snd_soc_jack_report(es8326->jack, 0, SND_JACK_BTN_0);

	es8326->jack = NULL;
	mutex_unlock(&es8326->lock);
}

static int es8326_set_jack(struct snd_soc_component *component,
			struct snd_soc_jack *jack, void *data)
{
	if (jack)
		es8326_enable_jack_detect(component, jack);
	else
		es8326_disable_jack_detect(component);

	return 0;
}

static void es8326_remove(struct snd_soc_component *component)
{
	es8326_disable_jack_detect(component);
	es8326_set_bias_level(component, SND_SOC_BIAS_OFF);
}

static const struct snd_soc_component_driver soc_component_dev_es8326 = {
	.probe = es8326_probe,
	.remove = es8326_remove,
	.resume = es8326_resume,
	.suspend = es8326_suspend,
	.set_bias_level = es8326_set_bias_level,
	.set_jack = es8326_set_jack,

	.dapm_widgets = es8326_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(es8326_dapm_widgets),
	.dapm_routes = es8326_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(es8326_dapm_routes),
	.controls = es8326_snd_controls,
	.num_controls = ARRAY_SIZE(es8326_snd_controls),
};

static int es8326_i2c_probe(struct i2c_client *i2c,
			    const struct i2c_device_id *id)
{
	struct es8326_priv *es8326;
	int ret;

	es8326 = devm_kzalloc(&i2c->dev, sizeof(struct es8326_priv), GFP_KERNEL);
	if (!es8326)
		return -ENOMEM;

	i2c_set_clientdata(i2c, es8326);
	es8326->i2c = i2c;
	mutex_init(&es8326->lock);
	es8326->regmap = devm_regmap_init_i2c(i2c, &es8326_regmap_config);
	if (IS_ERR(es8326->regmap)) {
		ret = PTR_ERR(es8326->regmap);
		dev_err(&i2c->dev, "Failed to init regmap: %d\n", ret);
		return ret;
	}

	es8326->irq = i2c->irq;
	/* ES8316 is level-based while ES8326 is edge-based */
	ret = devm_request_threaded_irq(&i2c->dev, es8326->irq, NULL, es8326_irq,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"es8326", es8326);
	if (ret == 0)
		disable_irq(es8326->irq);
	else
		dev_err(&i2c->dev, "Failed to reguest IRQ: %d\n", ret);
	INIT_DELAYED_WORK(&es8326->jack_detect_work,
			  es8326_jack_detect_handler);

	es8326->mclk = devm_clk_get_optional(&i2c->dev, "mclk");
	if (IS_ERR(es8326->mclk)) {
		dev_err(&i2c->dev, "unable to get mclk\n");
		return PTR_ERR(es8326->mclk);
	}
	if (!es8326->mclk)
		dev_warn(&i2c->dev, "assuming static mclk\n");

	ret = clk_prepare_enable(es8326->mclk);
	if (ret) {
		dev_err(&i2c->dev, "unable to enable mclk\n");
		return ret;
	}
	return devm_snd_soc_register_component(&i2c->dev,
					&soc_component_dev_es8326,
					&es8326_dai, 1);
}

static const struct i2c_device_id es8326_i2c_id[] = {
	{"es8326", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, es8326_i2c_id);

#ifdef CONFIG_OF
static const struct of_device_id es8326_of_match[] = {
	{ .compatible = "everest, es8326", },
	{}
};
MODULE_DEVICE_TABLE(of, es8326_of_match);
#endif

#ifdef CONFIG_ACPI
static const struct acpi_device_id es8326_acpi_match[] = {
	{"ESSX8326", 0},
	{},
};
MODULE_DEVICE_TABLE(acpi, es8326_acpi_match);
#endif

static struct i2c_driver es8326_i2c_driver = {
	.driver = {
		.name = "es8326",
		.acpi_match_table = ACPI_PTR(es8326_acpi_match),
		.of_match_table = of_match_ptr(es8326_of_match),
	},
	.probe = es8326_i2c_probe,
};
module_i2c_driver(es8326_i2c_driver);

MODULE_DESCRIPTION("ASoC es8326 driver");
MODULE_AUTHOR("David Yang <yangxiaohua@everest-semi.com>");
MODULE_LICENSE("GPL");



