// SPDX-License-Identifier: GPL-2.0
/*
 * ASoC machine driver for the Khadas VIM3 HDMI playback path under ACPI.
 *
 * The in-tree machine driver (axg-card) is structurally OF-only: it walks
 * DT child nodes, parses phandles and reads of_-only card properties.
 * This driver is its ACPI replacement for exactly one topology - the
 * playback subset of the VIM3 DT sound card (meson-khadas-vim3.dtsi):
 *
 *   FE  frddr_a "FRDDR"          (DPCM front end, playback only)
 *   BE  tdmif_a "TDM Pad"        i2s, cpu clock provider, mclk-fs 256,
 *                                2 slots on every lane (DT masks <1 1>)
 *       -> codec tohdmitx "I2S IN A"
 *   C2C tohdmitx "I2S OUT" -> hdmi-codec "i2s-hifi" (dw-hdmi child)
 *
 * It binds to the SSDT CARD device (PRP0001,
 * "khadas,vim3-acpi-hdmi-sound") and resolves every component at probe
 * time by scanning the platform bus - PRP0001 device names
 * ("PRP0001:NN") are enumeration-order dependent and must never be
 * hardcoded.  Anything not yet registered simply defers the card.
 *
 * Name prefixes are applied through codec_conf (which overrides the DT
 * sound-name-prefix path in the core), reproducing the DT control names
 * so the amixer routing recipe is identical in both modes:
 *   FRDDR_A SINK 1 SEL = OUT 0, FRDDR_A SRC 1 EN = on,
 *   TDMOUT_A SRC SEL = IN 0, TOHDMITX I2S SRC = I2S A, TOHDMITX = on.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#include "axg-tdm.h"

#define VIM3_HDMI_SND_MCLK_FS	256

static const struct snd_soc_pcm_stream vim3_hdmi_snd_c2c_params = {
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.rate_min = 5525,
	.rate_max = 192000,
	.channels_min = 1,
	.channels_max = 8,
};

/* DT dai-tdm-slot-tx-mask-0..3 = <1 1> on every lane */
static u32 vim3_hdmi_snd_tx_mask[AXG_TDM_NUM_LANES] = { 0x3, 0x3, 0x3, 0x3 };

static int vim3_hdmi_snd_be_hw_params(struct snd_pcm_substream *substream,
				      struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	unsigned int mclk = params_rate(params) * VIM3_HDMI_SND_MCLK_FS;
	struct snd_soc_dai *codec_dai;
	int ret, i;

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		ret = snd_soc_dai_set_sysclk(codec_dai, 0, mclk,
					     SND_SOC_CLOCK_IN);
		if (ret && ret != -ENOTSUPP)
			return ret;
	}

	ret = snd_soc_dai_set_sysclk(snd_soc_rtd_to_cpu(rtd, 0), 0, mclk,
				     SND_SOC_CLOCK_OUT);
	if (ret && ret != -ENOTSUPP)
		return ret;

	return 0;
}

static const struct snd_soc_ops vim3_hdmi_snd_be_ops = {
	.hw_params = vim3_hdmi_snd_be_hw_params,
};

static int vim3_hdmi_snd_tdm_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	int ret;

	/*
	 * Deliberately NO snd_soc_dai_set_tdm_slot() on the codec side.
	 *
	 * tohdmitx is transparent glue with no .set_tdm_slot op, so such a
	 * call can only ever return -ENOTSUPP -- but it is not free: the
	 * core stores the mask first, and snd_soc_xlate_tdm_slot_mask()
	 * expands a zero mask to (1 << slots) - 1.  __soc_pcm_hw_params()
	 * then sees a non-zero tdm_mask and pins the codec's channel count
	 * to hweight(mask) for *every* stream (soc-pcm.c
	 * soc_pcm_codec_params_fixup()).  With slots = 2 that is a hard
	 * stereo clamp: meson_codec_glue_input_hw_params() records
	 * channels_min/max = 2, meson_codec_glue_output_startup() copies
	 * that onto the codec-to-codec link, hdmi-codec is told 2, and
	 * dw-hdmi enables a single I2S lane (AUD_CONF0 = 0x21) no matter
	 * what userspace opened.  Verified on silicon: an 8-channel stream
	 * reached the frontend intact and still left AUD_CONF0 at 0x21.
	 *
	 * The CPU-side call below is the one that matters -- it is what
	 * publishes the real 8-slot capability.
	 */

	/* slots = fls(0x3) = 2, slot_width 0 = driver default (32) */
	ret = axg_tdm_set_tdm_slots(snd_soc_rtd_to_cpu(rtd, 0),
				    vim3_hdmi_snd_tx_mask, NULL, 2, 0);
	if (ret) {
		dev_err(snd_soc_rtd_to_cpu(rtd, 0)->dev,
			"setting tdm link slots failed\n");
		return ret;
	}

	return 0;
}

/* Routes replicate the playback strings of meson-khadas-vim3.dtsi */
static const struct snd_soc_dapm_route vim3_hdmi_snd_routes[] = {
	{ "TDMOUT_A IN 0", NULL, "FRDDR_A OUT 0" },
	{ "TDM_A Playback", NULL, "TDMOUT_A OUT" },
};

static int vim3_hdmi_snd_match_compatible(struct device *dev, const void *data)
{
	return fwnode_property_match_string(dev_fwnode(dev), "compatible",
					    data) >= 0;
}

static int vim3_hdmi_snd_match_name_prefix(struct device *dev, const void *data)
{
	return str_has_prefix(dev_name(dev), data) > 0;
}

/*
 * Resolve one component device and return its name as a devm-owned
 * string; -EPROBE_DEFER while the device has not been registered yet.
 */
static const char *vim3_hdmi_snd_component_name(struct device *dev,
						const void *key,
						device_match_t match)
{
	struct device *found;
	const char *name;

	found = bus_find_device(&platform_bus_type, NULL, key, match);
	if (!found)
		return ERR_PTR(-EPROBE_DEFER);

	name = devm_kstrdup(dev, dev_name(found), GFP_KERNEL);
	put_device(found);

	return name ? name : ERR_PTR(-ENOMEM);
}

struct vim3_hdmi_snd_names {
	const char *frddr;
	const char *tdmif;
	const char *tdmout;
	const char *tohdmitx;
	const char *hdmi_codec;
};

static int vim3_hdmi_snd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vim3_hdmi_snd_names n;
	struct snd_soc_dai_link_component *dlc;
	struct snd_soc_dai_link *links;
	struct snd_soc_codec_conf *conf;
	struct snd_soc_aux_dev *aux;
	struct snd_soc_card *card;
	int i;

	static const struct {
		const char *compatible;
		size_t offset;
	} parts[] = {
		{ "amlogic,g12a-frddr",    offsetof(struct vim3_hdmi_snd_names, frddr) },
		{ "amlogic,axg-tdm-iface", offsetof(struct vim3_hdmi_snd_names, tdmif) },
		{ "amlogic,g12a-tdmout",   offsetof(struct vim3_hdmi_snd_names, tdmout) },
		{ "amlogic,g12a-tohdmitx", offsetof(struct vim3_hdmi_snd_names, tohdmitx) },
	};

	for (i = 0; i < ARRAY_SIZE(parts); i++) {
		const char *name = vim3_hdmi_snd_component_name(dev,
					parts[i].compatible,
					vim3_hdmi_snd_match_compatible);
		if (IS_ERR(name))
			return dev_err_probe(dev, PTR_ERR(name), "waiting for %s\n",
					     parts[i].compatible);
		*(const char **)((char *)&n + parts[i].offset) = name;
	}

	/* dw-hdmi's audio child; registered once meson-drm has bound */
	n.hdmi_codec = vim3_hdmi_snd_component_name(dev, "hdmi-audio-codec.",
					vim3_hdmi_snd_match_name_prefix);
	if (IS_ERR(n.hdmi_codec))
		return dev_err_probe(dev, PTR_ERR(n.hdmi_codec),
				     "waiting for hdmi-audio-codec\n");

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	links = devm_kcalloc(dev, 3, sizeof(*links), GFP_KERNEL);
	dlc = devm_kcalloc(dev, 5, sizeof(*dlc), GFP_KERNEL);
	conf = devm_kcalloc(dev, 4, sizeof(*conf), GFP_KERNEL);
	aux = devm_kzalloc(dev, sizeof(*aux), GFP_KERNEL);
	if (!card || !links || !dlc || !conf || !aux)
		return -ENOMEM;

	/* FE: frddr_a */
	dlc[0].name = n.frddr;
	dlc[0].dai_name = "FRDDR";
	links[0].name = "fe-frddr-a";
	links[0].stream_name = "fe-frddr-a";
	links[0].cpus = &dlc[0];
	links[0].num_cpus = 1;
	links[0].codecs = &snd_soc_dummy_dlc;
	links[0].num_codecs = 1;
	links[0].nonatomic = true;
	links[0].dynamic = 1;
	links[0].dpcm_merged_format = 1;
	links[0].dpcm_merged_chan = 1;
	links[0].dpcm_merged_rate = 1;
	links[0].playback_only = 1;

	/* BE: tdmif_a pad -> tohdmitx i2s input */
	dlc[1].name = n.tdmif;
	dlc[1].dai_name = "TDM Pad";
	dlc[2].name = n.tohdmitx;
	dlc[2].dai_name = "I2S IN A";
	links[1].name = "be-tdm-pad-a";
	links[1].stream_name = "be-tdm-pad-a";
	links[1].cpus = &dlc[1];
	links[1].num_cpus = 1;
	links[1].codecs = &dlc[2];
	links[1].num_codecs = 1;
	links[1].nonatomic = true;
	links[1].no_pcm = 1;
	links[1].playback_only = 1;
	links[1].dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBC_CFC;
	links[1].ops = &vim3_hdmi_snd_be_ops;
	links[1].init = vim3_hdmi_snd_tdm_dai_init;

	/* C2C glue: tohdmitx output -> dw-hdmi codec */
	dlc[3].name = n.tohdmitx;
	dlc[3].dai_name = "I2S OUT";
	dlc[4].name = n.hdmi_codec;
	dlc[4].dai_name = "i2s-hifi";
	links[2].name = "c2c-tohdmitx";
	links[2].stream_name = "c2c-tohdmitx";
	links[2].cpus = &dlc[3];
	links[2].num_cpus = 1;
	links[2].codecs = &dlc[4];
	links[2].num_codecs = 1;
	links[2].nonatomic = true;
	links[2].c2c_params = &vim3_hdmi_snd_c2c_params;
	links[2].num_c2c_params = 1;

	/* The formatter is an aux component, exactly as audio-aux-devs */
	aux->dlc.name = n.tdmout;

	conf[0].dlc.name = n.frddr;
	conf[0].name_prefix = "FRDDR_A";
	conf[1].dlc.name = n.tdmif;
	conf[1].name_prefix = "TDM_A";
	conf[2].dlc.name = n.tdmout;
	conf[2].name_prefix = "TDMOUT_A";
	conf[3].dlc.name = n.tohdmitx;
	conf[3].name_prefix = "TOHDMITX";

	card->name = "KHADAS-VIM3";
	card->dev = dev;
	card->owner = THIS_MODULE;
	card->dai_link = links;
	card->num_links = 3;
	card->aux_dev = aux;
	card->num_aux_devs = 1;
	card->codec_conf = conf;
	card->num_configs = 4;
	card->dapm_routes = vim3_hdmi_snd_routes;
	card->num_dapm_routes = ARRAY_SIZE(vim3_hdmi_snd_routes);

	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id vim3_hdmi_snd_of_match[] = {
	{ .compatible = "khadas,vim3-acpi-hdmi-sound" },
	{}
};
MODULE_DEVICE_TABLE(of, vim3_hdmi_snd_of_match);

static struct platform_driver vim3_hdmi_snd_driver = {
	.probe = vim3_hdmi_snd_probe,
	.driver = {
		.name = "vim3-hdmi-snd",
		.of_match_table = vim3_hdmi_snd_of_match,
	},
};
module_platform_driver(vim3_hdmi_snd_driver);

MODULE_DESCRIPTION("Khadas VIM3 ACPI HDMI sound card");
MODULE_AUTHOR("Gus Bourg <gus@bourg.net>");
MODULE_LICENSE("GPL");