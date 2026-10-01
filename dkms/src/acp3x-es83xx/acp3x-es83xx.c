// SPDX-License-Identifier: GPL-2.0+
//
// Machine driver for AMD ACP Audio engine using ES8336 codec.
//
// Copyright 2023 Marian Postevca <posteuca@mutex.one>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc-dapm.h>
#include <sound/jack.h>
#include <sound/soc-acpi.h>
#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/delay.h>
#include <linux/string_choices.h>
#include "../acp-mach.h"
#include "acp3x-es83xx.h"

#define get_mach_priv(card) ((struct acp3x_es83xx_private *)((acp_get_drvdata(card))->mach_priv))

#define DUAL_CHANNEL	2

#define ES83XX_ENABLE_DMIC	BIT(4)
#define ES83XX_48_MHZ_MCLK	BIT(5)
#define ES83XX_HP_LOW		BIT(6)

/*
 * Speaker amp sequencing. The speakers are fed from the codec HP outputs
 * through an external amp gated by the speakers-enable GPIO. The amp must
 * only be live while the codec output stage is fully up and settled,
 * otherwise it amplifies the power-up/power-down transient (pop).
 */
#define ES83XX_MAX_DELAY_MS	500U

static unsigned int spk_on_delay_ms = 50;
module_param(spk_on_delay_ms, uint, 0644);
MODULE_PARM_DESC(spk_on_delay_ms,
		 "Wait after the codec output powers up before enabling the speaker amp (ms, default 50)");

static unsigned int spk_off_delay_ms = 20;
module_param(spk_off_delay_ms, uint, 0644);
MODULE_PARM_DESC(spk_off_delay_ms,
		 "Wait after disabling the speaker amp before the codec output powers down (ms, default 20)");

static void acp3x_es83xx_spk_delay(unsigned int ms)
{
	ms = min(ms, ES83XX_MAX_DELAY_MS);
	if (ms)
		msleep(ms);
}

struct acp3x_es83xx_private {
	bool speaker_on;
	bool headphone_on;
	unsigned long quirk;
	struct snd_soc_component *codec;
	struct device *codec_dev;
	struct gpio_desc *gpio_speakers, *gpio_headphone;
	struct acpi_gpio_params enable_spk_gpio, enable_hp_gpio;
	struct acpi_gpio_mapping gpio_mapping[3];
	struct snd_soc_dapm_route mic_map[2];
};

static const unsigned int channels[] = {
	DUAL_CHANNEL,
};

static const struct snd_pcm_hw_constraint_list constraints_channels = {
	.count = ARRAY_SIZE(channels),
	.list = channels,
	.mask = 0,
};

#define ES83xx_12288_KHZ_MCLK_FREQ   (48000 * 256)
#define ES83xx_48_MHZ_MCLK_FREQ      (48000 * 1000)

static int acp3x_es83xx_headphone_power_event(struct snd_soc_dapm_widget *w,
					    struct snd_kcontrol *kcontrol, int event);
static int acp3x_es83xx_speaker_power_event(struct snd_soc_dapm_widget *w,
					    struct snd_kcontrol *kcontrol, int event);

static int acp3x_es83xx_codec_startup(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime;
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai *codec_dai;
	struct acp3x_es83xx_private *priv;
	unsigned int freq;
	int ret;

	runtime = substream->runtime;
	rtd = snd_soc_substream_to_rtd(substream);
	codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	priv = get_mach_priv(rtd->card);

	if (priv->quirk & ES83XX_48_MHZ_MCLK) {
		dev_dbg(priv->codec_dev, "using a 48Mhz MCLK\n");
		freq = ES83xx_48_MHZ_MCLK_FREQ;
	} else {
		dev_dbg(priv->codec_dev, "using a 12.288Mhz MCLK\n");
		freq = ES83xx_12288_KHZ_MCLK_FREQ;
	}

	ret = snd_soc_dai_set_sysclk(codec_dai, 0, freq, SND_SOC_CLOCK_OUT);
	if (ret < 0) {
		dev_err(rtd->dev, "can't set codec sysclk: %d\n", ret);
		return ret;
	}

	runtime->hw.channels_max = DUAL_CHANNEL;
	snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
				   &constraints_channels);

	return 0;
}

static struct snd_soc_jack es83xx_jack;

static struct snd_soc_jack_pin es83xx_jack_pins[] = {
	{
		.pin	= "Headphone",
		.mask	= SND_JACK_HEADPHONE,
	},
	{
		.pin	= "Headset Mic",
		.mask	= SND_JACK_MICROPHONE,
	},
};

static const struct snd_soc_dapm_widget acp3x_es83xx_widgets[] = {
	/*
	 * DAPM powers speaker endpoints up last and down first, so the amp
	 * GPIO is driven from the Speaker widget itself: POST_PMU runs after
	 * the codec output stage is up, PRE_PMD before it goes down.
	 */
	SND_SOC_DAPM_SPK("Speaker", acp3x_es83xx_speaker_power_event),
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Internal Mic", NULL),

	SND_SOC_DAPM_SUPPLY("Headphone Power", SND_SOC_NOPM, 0, 0,
			    acp3x_es83xx_headphone_power_event,
			    SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),
};

static const struct snd_soc_dapm_route acp3x_es83xx_audio_map[] = {
	{"Headphone", NULL, "HPOL"},
	{"Headphone", NULL, "HPOR"},
	{"Headphone", NULL, "Headphone Power"},

	/*
	 * There is no separate speaker output instead the speakers are muxed to
	 * the HP outputs. The mux is controlled Speaker and/or headphone switch.
	 */
	{"Speaker", NULL, "HPOL"},
	{"Speaker", NULL, "HPOR"},
};


static const struct snd_kcontrol_new acp3x_es83xx_controls[] = {
	SOC_DAPM_PIN_SWITCH("Speaker"),
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("Internal Mic"),
};

static int acp3x_es83xx_configure_widgets(struct snd_soc_card *card)
{
	card->dapm_widgets = acp3x_es83xx_widgets;
	card->num_dapm_widgets = ARRAY_SIZE(acp3x_es83xx_widgets);
	card->controls = acp3x_es83xx_controls;
	card->num_controls = ARRAY_SIZE(acp3x_es83xx_controls);
	card->dapm_routes = acp3x_es83xx_audio_map;
	card->num_dapm_routes = ARRAY_SIZE(acp3x_es83xx_audio_map);

	return 0;
}

static int acp3x_es83xx_headphone_power_event(struct snd_soc_dapm_widget *w,
					      struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_card *card = snd_soc_dapm_to_card(w->dapm);
	struct acp3x_es83xx_private *priv = get_mach_priv(card);

	dev_dbg(priv->codec_dev, "headphone power event = %d\n", event);
	if (SND_SOC_DAPM_EVENT_ON(event))
		priv->headphone_on = true;
	else
		priv->headphone_on = false;

	/*
	 * Only touch the headphone amp. The speaker amp is sequenced by the
	 * Speaker widget; rewriting it from here could turn it back on at a
	 * point where the codec output is not up (e.g. during suspend).
	 */
	gpiod_set_value_cansleep(priv->gpio_headphone, priv->headphone_on);

	return 0;
}

static int acp3x_es83xx_speaker_power_event(struct snd_soc_dapm_widget *w,
					    struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_card *card = snd_soc_dapm_to_card(w->dapm);
	struct acp3x_es83xx_private *priv = get_mach_priv(card);

	dev_dbg(priv->codec_dev, "speaker power event: %d\n", event);
	if (SND_SOC_DAPM_EVENT_ON(event)) {
		/* Codec output stage is already powered, let it settle first. */
		acp3x_es83xx_spk_delay(spk_on_delay_ms);
		priv->speaker_on = true;
		gpiod_set_value_cansleep(priv->gpio_speakers, 1);
	} else {
		/* Silence the amp before the codec output stage collapses. */
		priv->speaker_on = false;
		gpiod_set_value_cansleep(priv->gpio_speakers, 0);
		acp3x_es83xx_spk_delay(spk_off_delay_ms);
	}

	return 0;
}

static int acp3x_es83xx_suspend_pre(struct snd_soc_card *card)
{
	struct acp3x_es83xx_private *priv = get_mach_priv(card);

	/* We need to disable the jack in the machine driver suspend
	 * callback so that the CODEC suspend callback actually gets
	 * called. Without doing it, the CODEC suspend/resume
	 * callbacks do not get called if headphones are plugged in.
	 * This is because plugging in headphones keeps some supplies
	 * active, this in turn means that the lowest bias level
	 * that the CODEC can go to is SND_SOC_BIAS_STANDBY.
	 * If components do not set idle_bias_on to true then
	 * their suspend/resume callbacks do not get called.
	 */
	dev_dbg(priv->codec_dev, "card suspend\n");

	/*
	 * suspend_pre runs before the core mutes the DAC and powers down
	 * DAPM, so cut the amp here. speaker_on is left alone, it keeps
	 * tracking the DAPM state and resume_post uses it to restore the amp.
	 */
	gpiod_set_value_cansleep(priv->gpio_speakers, 0);
	acp3x_es83xx_spk_delay(spk_off_delay_ms);

	snd_soc_component_set_jack(priv->codec, NULL, NULL);
	return 0;
}

static int acp3x_es83xx_resume_post(struct snd_soc_card *card)
{
	struct acp3x_es83xx_private *priv = get_mach_priv(card);

	/* We disabled jack detection in suspend callback,
	 * enable it back.
	 */
	dev_dbg(priv->codec_dev, "card resume\n");
	snd_soc_component_set_jack(priv->codec, &es83xx_jack, NULL);

	/*
	 * If DAPM kept the speaker path up across suspend, no POST_PMU event
	 * fires on resume, so turn the amp back on here. Userspace is already
	 * running again, so hold the DAPM lock: a stream stopping right now
	 * must not have its PRE_PMD undone by us.
	 */
	snd_soc_dapm_mutex_lock(card);
	if (priv->speaker_on) {
		acp3x_es83xx_spk_delay(spk_on_delay_ms);
		gpiod_set_value_cansleep(priv->gpio_speakers, 1);
	}
	snd_soc_dapm_mutex_unlock(card);
	return 0;
}

void acp3x_es83xx_shutdown(struct snd_soc_card *card)
{
	struct acp3x_es83xx_private *priv = get_mach_priv(card);

	if (!priv)
		return;

	/* Amps off before the rails drop, otherwise the collapse pops. */
	gpiod_set_value_cansleep(priv->gpio_speakers, 0);
	gpiod_set_value_cansleep(priv->gpio_headphone, 0);
	acp3x_es83xx_spk_delay(spk_off_delay_ms);
}

static int acp3x_es83xx_configure_gpios(struct acp3x_es83xx_private *priv)
{

	priv->enable_spk_gpio.crs_entry_index = 0;
	priv->enable_hp_gpio.crs_entry_index = 1;

	priv->enable_spk_gpio.active_low = false;

	if (priv->quirk & ES83XX_HP_LOW)
		priv->enable_hp_gpio.active_low = true;
	else
		priv->enable_hp_gpio.active_low = false;

	priv->gpio_mapping[0].name = "speakers-enable-gpios";
	priv->gpio_mapping[0].data = &priv->enable_spk_gpio;
	priv->gpio_mapping[0].size = 1;
	priv->gpio_mapping[0].quirks = ACPI_GPIO_QUIRK_ONLY_GPIOIO;

	priv->gpio_mapping[1].name = "headphone-enable-gpios";
	priv->gpio_mapping[1].data = &priv->enable_hp_gpio;
	priv->gpio_mapping[1].size = 1;
	priv->gpio_mapping[1].quirks = ACPI_GPIO_QUIRK_ONLY_GPIOIO;

	dev_info(priv->codec_dev, "speaker gpio %d active %s, headphone gpio %d active %s\n",
		 priv->enable_spk_gpio.crs_entry_index,
		 str_low_high(priv->enable_spk_gpio.active_low),
		 priv->enable_hp_gpio.crs_entry_index,
		 str_low_high(priv->enable_hp_gpio.active_low));
	return 0;
}

static void acp3x_es83xx_put_gpios(void *data)
{
	struct acp3x_es83xx_private *priv = data;

	if (priv->gpio_speakers) {
		gpiod_set_value_cansleep(priv->gpio_speakers, 0);
		gpiod_put(priv->gpio_speakers);
		priv->gpio_speakers = NULL;
	}
	if (priv->gpio_headphone) {
		gpiod_put(priv->gpio_headphone);
		priv->gpio_headphone = NULL;
	}
}

/*
 * Take ownership of the amp GPIOs and drive the speaker amp off. This has to
 * happen from the card probe op, i.e. before the codec component probes:
 * the codec resets and ramps its reference voltage during registration, and
 * doing that with the amp still in its firmware default state pops.
 *
 * Card registration can still return -EPROBE_DEFER after this, so the lines
 * are released through a devm action on the card device. Otherwise they
 * would stay requested and the retried probe would fail with -EBUSY.
 */
static int acp3x_es83xx_get_gpios(struct device *dev,
				  struct acp3x_es83xx_private *priv)
{
	struct acpi_device *adev = ACPI_COMPANION(priv->codec_dev);
	struct gpio_desc *gpio;
	int ret;

	acp3x_es83xx_configure_gpios(priv);

	/* The name -> _CRS index mapping is only needed for these lookups. */
	if (acpi_dev_add_driver_gpios(adev, priv->gpio_mapping))
		dev_warn(priv->codec_dev, "failed to add speaker gpio\n");

	gpio = gpiod_get_optional(priv->codec_dev, "speakers-enable", GPIOD_OUT_LOW);
	if (IS_ERR(gpio)) {
		ret = PTR_ERR(gpio);
		goto out;
	}
	priv->gpio_speakers = gpio;

	gpio = gpiod_get_optional(priv->codec_dev, "headphone-enable",
				  priv->enable_hp_gpio.active_low ? GPIOD_OUT_LOW : GPIOD_OUT_HIGH);
	if (IS_ERR(gpio)) {
		ret = PTR_ERR(gpio);
		goto out;
	}
	priv->gpio_headphone = gpio;
	ret = 0;

out:
	acpi_dev_remove_driver_gpios(adev);
	if (ret) {
		acp3x_es83xx_put_gpios(priv);
		return dev_err_probe(dev, ret, "could not get amp enable GPIOs\n");
	}

	return devm_add_action_or_reset(dev, acp3x_es83xx_put_gpios, priv);
}

static int acp3x_es83xx_configure_mics(struct acp3x_es83xx_private *priv)
{
	int num_routes = 0;
	int i;

	if (!(priv->quirk & ES83XX_ENABLE_DMIC)) {
		priv->mic_map[num_routes].sink = "MIC1";
		priv->mic_map[num_routes].source = "Internal Mic";
		num_routes++;
	}

	priv->mic_map[num_routes].sink = "MIC2";
	priv->mic_map[num_routes].source = "Headset Mic";
	num_routes++;

	for (i = 0; i < num_routes; i++)
		dev_info(priv->codec_dev, "%s is %s\n",
			 priv->mic_map[i].source, priv->mic_map[i].sink);

	return num_routes;
}

static int acp3x_es83xx_init(struct snd_soc_pcm_runtime *runtime)
{
	struct snd_soc_component *codec = snd_soc_rtd_to_codec(runtime, 0)->component;
	struct snd_soc_card *card = runtime->card;
	struct acp3x_es83xx_private *priv = get_mach_priv(card);
	int ret = 0;
	int num_routes;

	ret = snd_soc_card_jack_new_pins(card, "Headset",
					 SND_JACK_HEADSET | SND_JACK_BTN_0,
					 &es83xx_jack, es83xx_jack_pins,
					 ARRAY_SIZE(es83xx_jack_pins));
	if (ret) {
		dev_err(card->dev, "jack creation failed %d\n", ret);
		return ret;
	}

	snd_jack_set_key(es83xx_jack.jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);

	snd_soc_component_set_jack(codec, &es83xx_jack, NULL);

	priv->codec = codec;

	num_routes = acp3x_es83xx_configure_mics(priv);
	if (num_routes > 0) {
		struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(card);

		ret = snd_soc_dapm_add_routes(dapm, priv->mic_map, num_routes);
		if (ret != 0)
			device_remove_software_node(priv->codec_dev);
	}

	return ret;
}

static const struct snd_soc_ops acp3x_es83xx_ops = {
	.startup = acp3x_es83xx_codec_startup,
};


SND_SOC_DAILINK_DEF(codec,
		    DAILINK_COMP_ARRAY(COMP_CODEC("i2c-ESSX8336:00", "ES8316 HiFi")));

static const struct dmi_system_id acp3x_es83xx_dmi_table[] = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "HUAWEI"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "KLVL-WXXW"),
			DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "M1010"),
		},
		.driver_data = (void *)(ES83XX_ENABLE_DMIC),
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "HUAWEI"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "KLVL-WXX9"),
			DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "M1010"),
		},
		.driver_data = (void *)(ES83XX_ENABLE_DMIC),
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "HUAWEI"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "BOM-WXX9"),
			DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "M1010"),
		},
		.driver_data = (void *)(ES83XX_ENABLE_DMIC | ES83XX_48_MHZ_MCLK | ES83XX_HP_LOW),
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "HUAWEI"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "HVY-WXX9"),
			DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "M1010"),
		},
		.driver_data = (void *)(ES83XX_ENABLE_DMIC),
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "HUAWEI"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "HVY-WXX9"),
			DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "M1020"),
		},
		.driver_data = (void *)(ES83XX_ENABLE_DMIC),
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "HUAWEI"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "HVY-WXX9"),
			DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "M1040"),
		},
		.driver_data = (void *)(ES83XX_ENABLE_DMIC),
	},
	{}
};

static int acp3x_es83xx_configure_link(struct snd_soc_card *card, struct snd_soc_dai_link *link)
{
	link->codecs = codec;
	link->num_codecs = ARRAY_SIZE(codec);
	link->init = acp3x_es83xx_init;
	link->ops = &acp3x_es83xx_ops;
	link->dai_fmt = SND_SOC_DAIFMT_I2S
		| SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBP_CFP;

	return 0;
}

static int acp3x_es83xx_probe(struct snd_soc_card *card)
{
	int ret = 0;
	struct device *dev = card->dev;
	const struct dmi_system_id *dmi_id;

	dmi_id = dmi_first_match(acp3x_es83xx_dmi_table);
	if (dmi_id && dmi_id->driver_data) {
		struct acp3x_es83xx_private *priv;
		struct acp_card_drvdata *acp_drvdata;
		struct acpi_device *adev;
		struct device *codec_dev;

		acp_drvdata = (struct acp_card_drvdata *)card->drvdata;

		dev_info(dev, "matched DMI table with this system, trying to register sound card\n");

		adev = acpi_dev_get_first_match_dev(acp_drvdata->acpi_mach->id, NULL, -1);
		if (!adev) {
			dev_err(dev, "Error cannot find '%s' dev\n", acp_drvdata->acpi_mach->id);
			return -ENXIO;
		}

		codec_dev = acpi_get_first_physical_node(adev);
		acpi_dev_put(adev);
		if (!codec_dev) {
			dev_warn(dev, "Error cannot find codec device, will defer probe\n");
			return -EPROBE_DEFER;
		}

		priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
		if (!priv) {
			put_device(codec_dev);
			return -ENOMEM;
		}

		priv->codec_dev = codec_dev;
		priv->quirk = (unsigned long)dmi_id->driver_data;

		/* Amp off before the codec component gets probed. */
		ret = acp3x_es83xx_get_gpios(dev, priv);
		if (ret)
			return ret;

		acp_drvdata->mach_priv = priv;
		dev_info(dev, "successfully probed the sound card\n");
	} else {
		ret = -ENODEV;
		dev_warn(dev, "this system has a ES83xx codec defined in ACPI, but the driver doesn't have this system registered in DMI table\n");
	}
	return ret;
}


void acp3x_es83xx_init_ops(struct acp_mach_ops *ops)
{
	ops->probe = acp3x_es83xx_probe;
	ops->configure_widgets = acp3x_es83xx_configure_widgets;
	ops->configure_link = acp3x_es83xx_configure_link;
	ops->suspend_pre = acp3x_es83xx_suspend_pre;
	ops->resume_post = acp3x_es83xx_resume_post;
}
