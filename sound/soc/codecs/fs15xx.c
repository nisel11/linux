// SPDX-License-Identifier: GPL-2.0-only
/* GPIO pulse control for FourSemi FS15xx and SPC1910 speaker amplifiers. */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <sound/soc.h>

enum fs15xx_mode {
	FS15XX_OFF,
	FS15XX_RCV1,
	FS15XX_RCV2,
	FS15XX_RCV3,
	FS15XX_SPK1,
	FS15XX_SPK2,
	FS15XX_SPK3,
};

struct fs15xx {
	struct gpio_desc *cmd;
	struct gpio_desc *id;
	struct gpio_desc *mod;
	struct mutex lock;
	spinlock_t pulse_lock;
	unsigned int type;
	unsigned int mode;
	bool enabled;
};

static const char * const fs15xx_mode_text[] = {
	"OFF", "MODE_RCV1", "MODE_RCV2", "MODE_RCV3",
	"MODE_SPK1", "MODE_SPK2", "MODE_SPK3",
};
static const char * const fs15xx_switch_text[] = { "Off", "On" };
static const char * const fs15xx_type_text[] = { "SPC1910", "Foursemi" };

static SOC_ENUM_SINGLE_EXT_DECL(fs15xx_mode_enum, fs15xx_mode_text);
static SOC_ENUM_SINGLE_EXT_DECL(fs15xx_switch_enum, fs15xx_switch_text);
static SOC_ENUM_SINGLE_EXT_DECL(fs15xx_type_enum, fs15xx_type_text);

static void fs15xx_off(struct fs15xx *amp)
{
	gpiod_set_value(amp->cmd, 0);
	if (amp->mod)
		gpiod_set_value_cansleep(amp->mod, 1);
	/* The command input needs at least 5 ms low to reset its decoder. */
	usleep_range(5000, 6000);
}

static unsigned int fs15xx_spc_pulses(unsigned int mode)
{
	switch (mode) {
	case FS15XX_RCV1: return 8;
	case FS15XX_RCV2: return 9;
	case FS15XX_SPK1: return 1;
	case FS15XX_SPK2: return 10;
	default: return 0;
	}
}

static int fs15xx_pulse(struct gpio_desc *cmd, bool high_first)
{
	ktime_t edge;

	gpiod_set_value(cmd, high_first);
	edge = ktime_get();
	udelay(10);
	if (ktime_us_delta(ktime_get(), edge) > 140)
		return -ETIMEDOUT;
	gpiod_set_value(cmd, !high_first);
	edge = ktime_get();
	udelay(10);
	if (ktime_us_delta(ktime_get(), edge) > 140)
		return -ETIMEDOUT;
	return 0;
}

static int fs15xx_start(struct fs15xx *amp)
{
	unsigned long flags;
	unsigned int i, pulses;
	ktime_t edge;
	int ret = 0;

	if (!amp->mode)
		return 0;
	if (!amp->type && !fs15xx_spc_pulses(amp->mode))
		return -EINVAL;

	if (amp->mod)
		gpiod_set_value_cansleep(amp->mod, 0);

	/* cmd is a local, non-sleeping GPIO. Keep IRQs out of the 10 us
	 * edges; GPIO controller latency still contributes to each interval.
	 */
	spin_lock_irqsave(&amp->pulse_lock, flags);
	if (amp->type) {
		gpiod_set_value(amp->cmd, 1);
		edge = ktime_get();
		udelay(300); /* Header high: 200..1000 us. */
		if (ktime_us_delta(ktime_get(), edge) > 1000)
			ret = -ETIMEDOUT;
		for (i = 0; i < amp->mode && !ret; i++)
			ret = fs15xx_pulse(amp->cmd, false);
	} else {
		for (i = 0; i < 15 && !ret; i++)
			ret = fs15xx_pulse(amp->cmd, true);
		if (!ret)
			udelay(60);
		pulses = fs15xx_spc_pulses(amp->mode);
		for (i = 0; i < pulses && !ret; i++)
			ret = fs15xx_pulse(amp->cmd, true);
		if (!ret) {
			udelay(600);
			gpiod_set_value(amp->cmd, 1);
		}
	}
	if (!ret)
		udelay(300); /* Final high latch; leave cmd high while enabled. */
	spin_unlock_irqrestore(&amp->pulse_lock, flags);
	if (ret)
		fs15xx_off(amp);
	return ret;
}

static int fs15xx_mode_get(struct snd_kcontrol *control,
			   struct snd_ctl_elem_value *value)
{
	struct fs15xx *amp = snd_soc_component_get_drvdata(snd_kcontrol_chip(control));

	value->value.enumerated.item[0] = amp->mode;
	return 0;
}

static int fs15xx_mode_put(struct snd_kcontrol *control,
			   struct snd_ctl_elem_value *value)
{
	struct fs15xx *amp = snd_soc_component_get_drvdata(snd_kcontrol_chip(control));
	unsigned int mode = value->value.enumerated.item[0];
	int ret = 0;

	if (mode > FS15XX_SPK3 || (!amp->type && mode &&
				   !fs15xx_spc_pulses(mode)))
		return -EINVAL;
	mutex_lock(&amp->lock);
	if (mode == amp->mode)
		goto out;
	if (amp->enabled)
		fs15xx_off(amp);
	amp->mode = mode;
	if (amp->enabled)
		ret = fs15xx_start(amp);
	if (ret)
		amp->enabled = false;
	else
		ret = 1;
out:
	mutex_unlock(&amp->lock);
	return ret;
}

static int fs15xx_switch_get(struct snd_kcontrol *control,
			     struct snd_ctl_elem_value *value)
{
	struct fs15xx *amp = snd_soc_component_get_drvdata(snd_kcontrol_chip(control));

	value->value.enumerated.item[0] = amp->enabled;
	return 0;
}

static int fs15xx_switch_put(struct snd_kcontrol *control,
			     struct snd_ctl_elem_value *value)
{
	struct fs15xx *amp = snd_soc_component_get_drvdata(snd_kcontrol_chip(control));
	unsigned int enabled = value->value.enumerated.item[0];
	int ret = 0;

	if (enabled > 1)
		return -EINVAL;
	mutex_lock(&amp->lock);
	if (enabled == amp->enabled)
		goto out;
	if (enabled)
		ret = fs15xx_start(amp);
	else
		fs15xx_off(amp);
	if (!ret) {
		amp->enabled = enabled;
		ret = 1;
	}
out:
	mutex_unlock(&amp->lock);
	return ret;
}

static int fs15xx_type_get(struct snd_kcontrol *control,
			   struct snd_ctl_elem_value *value)
{
	struct fs15xx *amp = snd_soc_component_get_drvdata(snd_kcontrol_chip(control));

	value->value.enumerated.item[0] = amp->type;
	return 0;
}

static const struct snd_kcontrol_new fs15xx_controls[] = {
	SOC_ENUM_EXT("FSM_AMP_SWITCH", fs15xx_switch_enum,
		     fs15xx_switch_get, fs15xx_switch_put),
	SOC_ENUM_EXT("FSM_AMP_MODE", fs15xx_mode_enum,
		     fs15xx_mode_get, fs15xx_mode_put),
	SOC_ENUM_EXT_ACC("FSM_AMP_TYPE", fs15xx_type_enum,
			 fs15xx_type_get, NULL, SNDRV_CTL_ELEM_ACCESS_READ),
};

static const struct snd_soc_component_driver fs15xx_component = {
	.controls = fs15xx_controls,
	.num_controls = ARRAY_SIZE(fs15xx_controls),
};

static int fs15xx_select_states(struct device *dev)
{
	static const char * const names[] = {
		"fs15xx_id_default", "fs15xx_cmd_default",
		"fs15xx_mod_default",
	};
	struct pinctrl *pinctrl;
	struct pinctrl_state *state;
	int i, ret;

	if (!of_property_present(dev->of_node, "pinctrl-0"))
		return 0;
	pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(pinctrl))
		return PTR_ERR(pinctrl);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (of_property_match_string(dev->of_node, "pinctrl-names",
					     names[i]) < 0)
			continue;
		state = pinctrl_lookup_state(pinctrl, names[i]);
		if (IS_ERR(state))
			return PTR_ERR(state);
		ret = pinctrl_select_state(pinctrl, state);
		if (ret)
			return ret;
	}
	return 0;
}

static int fs15xx_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fs15xx *amp;
	int ret, level;

	amp = devm_kzalloc(dev, sizeof(*amp), GFP_KERNEL);
	if (!amp)
		return -ENOMEM;
	ret = device_property_read_u32(dev, "fsm,dev-type", &amp->type);
	if (ret || amp->type > 1)
		return dev_err_probe(dev, -EINVAL, "invalid fsm,dev-type\n");
	ret = fs15xx_select_states(dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to select pinctrl states\n");

	/* gpiolib-of appends -gpio(s) to the consumer ID, including its comma. */
	amp->cmd = devm_gpiod_get(dev, "fsm,cmd", GPIOD_OUT_LOW);
	if (IS_ERR(amp->cmd))
		return dev_err_probe(dev, PTR_ERR(amp->cmd), "failed to get cmd GPIO\n");
	if (gpiod_cansleep(amp->cmd))
		return dev_err_probe(dev, -EINVAL, "cmd GPIO must not sleep\n");
	amp->id = devm_gpiod_get_optional(dev, "fsm,id", GPIOD_OUT_LOW);
	if (IS_ERR(amp->id))
		return dev_err_probe(dev, PTR_ERR(amp->id), "failed to get id GPIO\n");
	amp->mod = devm_gpiod_get_optional(dev, "fsm,mod", GPIOD_OUT_HIGH);
	if (IS_ERR(amp->mod))
		return dev_err_probe(dev, PTR_ERR(amp->mod), "failed to get mod GPIO\n");
	if (amp->id) {
		gpiod_set_value_cansleep(amp->id, 1);
		udelay(1);
		level = gpiod_get_value_cansleep(amp->id);
		gpiod_set_value_cansleep(amp->id, 0);
		if (level < 0)
			return dev_err_probe(dev, level, "failed to read id GPIO\n");
		dev_dbg(dev, "id GPIO read %d\n", level);
	}
	mutex_init(&amp->lock);
	spin_lock_init(&amp->pulse_lock);
	amp->mode = FS15XX_SPK1;
	platform_set_drvdata(pdev, amp);
	ret = devm_snd_soc_register_component(dev, &fs15xx_component, NULL, 0);
	if (!ret)
		dev_info(dev, "FS15xx GPIO amplifier ready (type %u)\n", amp->type);
	return ret;
}

static void fs15xx_remove(struct platform_device *pdev)
{
	struct fs15xx *amp = platform_get_drvdata(pdev);

	fs15xx_off(amp);
}

static const struct of_device_id fs15xx_of_match[] = {
	{ .compatible = "foursemi,fs15xx" },
	{ }
};
MODULE_DEVICE_TABLE(of, fs15xx_of_match);

static struct platform_driver fs15xx_driver = {
	.probe = fs15xx_probe,
	.remove = fs15xx_remove,
	.driver = {
		.name = "fs15xx",
		.of_match_table = fs15xx_of_match,
	},
};
module_platform_driver(fs15xx_driver);

MODULE_DESCRIPTION("FourSemi FS15xx GPIO pulse amplifier");
MODULE_LICENSE("GPL");
