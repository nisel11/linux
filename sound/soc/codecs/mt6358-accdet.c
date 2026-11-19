// SPDX-License-Identifier: GPL-2.0
/*
 * mt6358-accdet.c  --  MT6358 ACCDET jack detection
 *
 * ACCDET (Accessory Detection) is the PMIC block used to detect headphone
 * and headset plug/unplug events on MediaTek platforms.
 */

#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#include <sound/jack.h>
#include <sound/soc.h>
#include <sound/soc-component.h>

#include <linux/mfd/mt6397/core.h>

#include "mt6358-accdet.h"

/* ACCDET capabilities */
#define ACCDET_PMIC_EINT_IRQ		BIT(0)
#define ACCDET_AP_GPIO_EINT		BIT(1)
#define ACCDET_PMIC_EINT0		BIT(2)
#define ACCDET_PMIC_EINT1		BIT(3)
#define ACCDET_PMIC_BI_EINT		BIT(4)
#define ACCDET_THREE_KEY		BIT(8)
#define ACCDET_FOUR_KEY			BIT(9)
#define ACCDET_TRI_KEY_CDD		BIT(10)

enum mt6358_accdet_eint_pin_state {
	MT6358_EINT_PIN_PLUG_OUT = 0,
	MT6358_EINT_PIN_PLUG_IN,
};

static struct platform_driver mt6358_accdet_driver;

static void mt6358_accdet_set_debounce(struct mt6358_accdet *priv, int state,
				       unsigned int debounce)
{
	switch (state) {
	case MT6358_ACCDET_STATE_000:
		regmap_write(priv->regmap, MT6358_ACCDET_DEBOUNCE0, debounce);
		break;
	case MT6358_ACCDET_STATE_001:
		regmap_write(priv->regmap, MT6358_ACCDET_DEBOUNCE1, debounce);
		break;
	case MT6358_ACCDET_STATE_010:
		regmap_write(priv->regmap, MT6358_ACCDET_DEBOUNCE2, debounce);
		break;
	case MT6358_ACCDET_STATE_011:
		regmap_write(priv->regmap, MT6358_ACCDET_DEBOUNCE3, debounce);
		break;
	case MT6358_ACCDET_STATE_AUXADC:
		regmap_write(priv->regmap, MT6358_ACCDET_DEBOUNCE4, debounce);
		break;
	case MT6358_ACCDET_EINT0_STATE_000:
	case MT6358_ACCDET_EINT0_STATE_011:
		regmap_update_bits(priv->regmap, MT6358_ACCDET_EINT0_CTL,
				   MT6358_ACCDET_EINT0_DEB_CLR,
				   debounce << 3);
		break;
	case MT6358_ACCDET_EINT1_STATE_000:
	case MT6358_ACCDET_EINT1_STATE_011:
		regmap_update_bits(priv->regmap, MT6358_ACCDET_EINT1_CTL,
				   MT6358_ACCDET_EINT1_DEB_CLR,
				   debounce << 3);
		break;
	default:
		dev_warn(priv->dev, "invalid debounce state %d\n", state);
		break;
	}
}

static void mt6358_accdet_jack_report(struct mt6358_accdet *priv)
{
	int report;

	if (!priv->jack)
		return;

	report = priv->jack_type | priv->btn_type;
	snd_soc_jack_report(priv->jack, report, MT6358_ACCDET_JACK_MASK);
}

static void mt6358_accdet_clear_accdet_irq(struct mt6358_accdet *priv)
{
	unsigned int val;
	int ret;

	regmap_update_bits(priv->regmap, MT6358_ACCDET_IRQ_STS,
			   MT6358_ACCDET_IRQ_CLR_B8,
			   MT6358_ACCDET_IRQ_CLR_B8);
	ret = regmap_read_poll_timeout(priv->regmap, MT6358_ACCDET_IRQ_STS,
				       val,
				       !(val & MT6358_ACCDET_IRQ_B0),
				       0, 1000);
	if (ret)
		dev_err(priv->dev, "failed to clear ACCDET IRQ: %d\n", ret);

	regmap_update_bits(priv->regmap, MT6358_ACCDET_IRQ_STS,
			   MT6358_ACCDET_IRQ_CLR_B8, 0);
	regmap_write(priv->regmap, MT6358_AUD_TOP_INT_STATUS0,
		     MT6358_INT_ACCDET);
}

static void mt6358_accdet_clear_eint_irq(struct mt6358_accdet *priv, bool eint0)
{
	unsigned int clr = eint0 ? MT6358_ACCDET_EINT0_IRQ_CLR_B10 :
				   MT6358_ACCDET_EINT1_IRQ_CLR_B11;
	unsigned int sts = eint0 ? MT6358_ACCDET_EINT0_IRQ_B2 :
				   MT6358_ACCDET_EINT1_IRQ_B3;
	unsigned int top = eint0 ? MT6358_INT_ACCDET_EINT0 :
				   MT6358_INT_ACCDET_EINT1;
	unsigned int val;
	int ret;

	regmap_update_bits(priv->regmap, MT6358_ACCDET_IRQ_STS, clr, clr);
	ret = regmap_read_poll_timeout(priv->regmap, MT6358_ACCDET_IRQ_STS,
				       val, !(val & sts), 0, 1000);
	if (ret)
		dev_err(priv->dev, "failed to clear EINT%s IRQ: %d\n",
			eint0 ? "0" : "1", ret);

	regmap_update_bits(priv->regmap, MT6358_ACCDET_IRQ_STS, clr, 0);
	regmap_write(priv->regmap, MT6358_AUD_TOP_INT_STATUS0, top);
}

static void mt6358_accdet_reverse_eint_polarity(struct mt6358_accdet *priv,
						bool eint0)
{
	unsigned int pol = eint0 ? MT6358_ACCDET_EINT0_IRQ_POL_B14 :
				   MT6358_ACCDET_EINT1_IRQ_POL_B15;
	bool level_low = (priv->data.eint_pol == IRQ_TYPE_LEVEL_LOW);
	unsigned int val;

	regmap_read(priv->regmap, MT6358_ACCDET_IRQ_STS, &val);

	/*
	 * The EINT line is level triggered. Flip its polarity after every
	 * interrupt so that the opposite pin state can trigger the next one.
	 */
	if (priv->eint_pin_state == MT6358_EINT_PIN_PLUG_OUT) {
		if (level_low)
			val |= pol;
		else
			val &= ~pol;
	} else {
		if (level_low)
			val &= ~pol;
		else
			val |= pol;
	}

	regmap_write(priv->regmap, MT6358_ACCDET_IRQ_STS, val);
}

static void mt6358_accdet_set_eint_debounce(struct mt6358_accdet *priv,
					    bool eint0, unsigned int debounce)
{
	unsigned int reg = eint0 ? MT6358_ACCDET_EINT0_CTL :
				   MT6358_ACCDET_EINT1_CTL;
	unsigned int mask = eint0 ? MT6358_ACCDET_EINT0_DEB_CLR :
				    MT6358_ACCDET_EINT1_DEB_CLR;

	regmap_update_bits(priv->regmap, reg, mask, debounce);
}

static void mt6358_accdet_check_type(struct mt6358_accdet *priv)
{
	unsigned int val, ab, type;

	if (regmap_read(priv->regmap, MT6358_ACCDET_STATE_RG, &val))
		return;

	ab = (val >> MT6358_ACCDET_STATE_MEM_IN_OFFSET) &
	     MT6358_ACCDET_STATE_AB_MASK;

	switch (ab) {
	case 0:
		/*
		 * AB=00 is a plain headphone on plug-in, but it is also the
		 * state seen while a headset button is pressed. Keep the
		 * headset type once it has been detected.
		 */
		type = (priv->jack_type == SND_JACK_HEADSET) ?
			SND_JACK_HEADSET : SND_JACK_HEADPHONE;
		break;
	case 1:
		type = SND_JACK_HEADSET;
		break;
	default:
		/* AB=11 is the idle/plug-out state, nothing to report here. */
		return;
	}

	if (type != priv->jack_type) {
		priv->jack_type = type;
		mt6358_accdet_jack_report(priv);
	}
}

static void mt6358_accdet_accdet_work(struct work_struct *work)
{
	struct mt6358_accdet *priv =
		container_of(work, struct mt6358_accdet, accdet_work);

	guard(mutex)(&priv->res_lock);

	if (!priv->jack_plugged)
		return;

	mt6358_accdet_check_type(priv);
}

static void mt6358_accdet_eint_work(struct work_struct *work)
{
	struct mt6358_accdet *priv =
		container_of(work, struct mt6358_accdet, eint_work);

	guard(mutex)(&priv->res_lock);

	if (priv->eint_pin_state == MT6358_EINT_PIN_PLUG_IN) {
		priv->jack_plugged = true;

		/* Re-initialise the FSM and debounce before enabling it. */
		regmap_update_bits(priv->regmap, MT6358_ACCDET_CTRL,
				   MT6358_ACCDET_SEQ_INIT_EN_B1,
				   MT6358_ACCDET_SEQ_INIT_EN_B1);
		mdelay(2);
		regmap_update_bits(priv->regmap, MT6358_ACCDET_CTRL,
				   MT6358_ACCDET_SEQ_INIT_EN_B1, 0);
		mdelay(1);

		mt6358_accdet_set_debounce(priv, MT6358_ACCDET_STATE_000,
					   priv->data.pwm_deb.debounce0);
		mt6358_accdet_set_debounce(priv, MT6358_ACCDET_STATE_001,
					   priv->data.pwm_deb.debounce1);
		mt6358_accdet_set_debounce(priv, MT6358_ACCDET_STATE_011,
					   priv->data.pwm_deb.debounce3);
		mt6358_accdet_set_debounce(priv, MT6358_ACCDET_STATE_AUXADC,
					   priv->data.pwm_deb.debounce4);

		regmap_update_bits(priv->regmap, MT6358_ACCDET_STATE_SWCTRL,
				   MT6358_ACCDET_PWM_IDLE,
				   MT6358_ACCDET_PWM_IDLE);
		regmap_update_bits(priv->regmap, MT6358_ACCDET_STATE_SWCTRL,
				   MT6358_ACCDET_EINT0_PWM_IDLE_B11 |
				   MT6358_ACCDET_PWM_EN,
				   MT6358_ACCDET_EINT0_PWM_IDLE_B11 |
				   MT6358_ACCDET_PWM_EN);
		regmap_update_bits(priv->regmap, MT6358_ACCDET_CTRL,
				   MT6358_ACCDET_ENABLE_B0,
				   MT6358_ACCDET_ENABLE_B0);

		/* Give the accessory FSM time to settle before sampling AB. */
		msleep(20);
		mt6358_accdet_check_type(priv);
	} else {
		priv->jack_plugged = false;

		regmap_update_bits(priv->regmap, MT6358_ACCDET_STATE_SWCTRL,
				   MT6358_ACCDET_PWM_IDLE, 0);

		mt6358_accdet_clear_accdet_irq(priv);
		regmap_update_bits(priv->regmap, MT6358_ACCDET_CTRL,
				   MT6358_ACCDET_ENABLE_B0, 0);
		regmap_update_bits(priv->regmap, MT6358_ACCDET_STATE_SWCTRL,
				   MT6358_ACCDET_PWM_EN, 0);

		priv->jack_type = 0;
		priv->btn_type = 0;
		mt6358_accdet_jack_report(priv);
	}
}

static irqreturn_t mt6358_accdet_irq(int irq, void *data)
{
	struct mt6358_accdet *priv = data;
	bool eint0;
	unsigned int irq_val;
	int ret;

	guard(mutex)(&priv->res_lock);

	ret = regmap_read(priv->regmap, MT6358_ACCDET_IRQ_STS, &irq_val);
	if (ret)
		return IRQ_NONE;

	eint0 = irq_val & MT6358_ACCDET_EINT0_IRQ_B2;

	if (eint0 || (irq_val & MT6358_ACCDET_EINT1_IRQ_B3)) {
		mt6358_accdet_reverse_eint_polarity(priv, eint0);
		mt6358_accdet_clear_eint_irq(priv, eint0);

		if (priv->eint_pin_state == MT6358_EINT_PIN_PLUG_OUT) {
			mt6358_accdet_set_eint_debounce(priv, eint0,
							MT6358_ACCDET_EINT0_DEB_OUT_012);
			priv->eint_pin_state = MT6358_EINT_PIN_PLUG_IN;
		} else {
			mt6358_accdet_set_eint_debounce(priv, eint0,
							MT6358_ACCDET_EINT0_DEB_IN_256);
			mt6358_accdet_set_debounce(priv, MT6358_ACCDET_STATE_011,
						   priv->data.pwm_deb.debounce3);
			priv->eint_pin_state = MT6358_EINT_PIN_PLUG_OUT;
		}

		queue_work(priv->eint_workqueue, &priv->eint_work);
	} else if (irq_val & MT6358_ACCDET_IRQ_B0) {
		mt6358_accdet_clear_accdet_irq(priv);
		queue_work(priv->accdet_workqueue, &priv->accdet_work);
	}

	return IRQ_HANDLED;
}

static void mt6358_accdet_set_defaults(struct mt6358_accdet *priv)
{
	struct mt6358_accdet_data *data = &priv->data;

	data->mic_vol = 8;
	data->mic_mode = 2;
	data->plugout_deb = 1;
	data->eint_pol = IRQ_TYPE_LEVEL_LOW;
	data->eint_detect_mode = 4;
	data->eint_use_ext_res = 0;
	data->eint_comp_vth = 0;
	data->eint_num = 0;
	data->key_mode = 0;

	data->pwm_deb.pwm_width = 0x500;
	data->pwm_deb.pwm_thresh = 0x500;
	data->pwm_deb.fall_delay = 0x1;
	data->pwm_deb.rise_delay = 0x1f0;
	data->pwm_deb.debounce0 = 0x800;
	data->pwm_deb.debounce1 = 0x800;
	data->pwm_deb.debounce3 = 0x20;
	data->pwm_deb.debounce4 = 0x44;
	data->pwm_deb.eint_pwm_width = 0x4;
	data->pwm_deb.eint_pwm_thresh = 0x1;
	data->pwm_deb.eint_debounce0 = 0xe;
	data->pwm_deb.eint_debounce1 = 0xe;
	data->pwm_deb.eint_debounce2 = 0xe;
	data->pwm_deb.eint_debounce3 = 0xe;
	data->pwm_deb.eint_inverter_debounce = 0xe;
}

static int mt6358_accdet_parse_dt(struct mt6358_accdet *priv)
{
	struct device *dev = priv->dev;
	struct device_node *node = dev->of_node;
	unsigned int pwm_deb[15];
	int three_key[4];
	int four_key[5];
	unsigned int tmp;
	int ret;

	if (!node)
		return -EINVAL;

	mt6358_accdet_set_defaults(priv);

	of_property_read_u32(node, "mediatek,mic-vol", &priv->data.mic_vol);
	of_property_read_u32(node, "mediatek,plugout-debounce",
			     &priv->data.plugout_deb);
	of_property_read_u32(node, "mediatek,mic-mode", &priv->data.mic_mode);
	of_property_read_u32(node, "mediatek,eint-level-pol",
			     &priv->data.eint_pol);
	of_property_read_u32(node, "mediatek,eint-detect-mode",
			     &priv->data.eint_detect_mode);
	of_property_read_u32(node, "mediatek,eint-use-ext-res",
			     &priv->data.eint_use_ext_res);
	of_property_read_u32(node, "mediatek,eint-comp-vth",
			     &priv->data.eint_comp_vth);

	ret = of_property_read_u32_array(node, "mediatek,pwm-deb-setting",
					 pwm_deb, ARRAY_SIZE(pwm_deb));
	if (!ret) {
		priv->data.pwm_deb.pwm_width = pwm_deb[0];
		priv->data.pwm_deb.pwm_thresh = pwm_deb[1];
		priv->data.pwm_deb.fall_delay = pwm_deb[2];
		priv->data.pwm_deb.rise_delay = pwm_deb[3];
		priv->data.pwm_deb.debounce0 = pwm_deb[4];
		priv->data.pwm_deb.debounce1 = pwm_deb[5];
		priv->data.pwm_deb.debounce3 = pwm_deb[6];
		priv->data.pwm_deb.debounce4 = pwm_deb[7];
		priv->data.pwm_deb.eint_pwm_width = pwm_deb[8];
		priv->data.pwm_deb.eint_pwm_thresh = pwm_deb[9];
		priv->data.pwm_deb.eint_debounce0 = pwm_deb[10];
		priv->data.pwm_deb.eint_debounce1 = pwm_deb[11];
		priv->data.pwm_deb.eint_debounce2 = pwm_deb[12];
		priv->data.pwm_deb.eint_debounce3 = pwm_deb[13];
		priv->data.pwm_deb.eint_inverter_debounce = pwm_deb[14];
	}

	priv->caps = ACCDET_PMIC_EINT_IRQ;

	ret = of_property_read_u32(node, "mediatek,eint-use-ap", &tmp);
	if (!ret && tmp == 1)
		priv->caps |= ACCDET_AP_GPIO_EINT;

	ret = of_property_read_u32(node, "mediatek,eint-num", &tmp);
	if (!ret && tmp == 1) {
		priv->data.eint_num = 1;
		priv->caps |= ACCDET_PMIC_EINT1;
	} else if (!ret && tmp == 2) {
		priv->data.eint_num = 2;
		priv->caps |= ACCDET_PMIC_BI_EINT;
	} else {
		priv->data.eint_num = 0;
		priv->caps |= ACCDET_PMIC_EINT0;
	}

	ret = of_property_read_u32(node, "mediatek,key-mode", &tmp);
	if (!ret) {
		priv->data.key_mode = tmp;
		if (tmp == 1) {
			priv->caps |= ACCDET_FOUR_KEY;
			ret = of_property_read_u32_array(node,
							 "mediatek,four-key-thr",
							 four_key,
							 ARRAY_SIZE(four_key));
			if (!ret) {
				priv->data.four_key.mid = four_key[1];
				priv->data.four_key.voice = four_key[2];
				priv->data.four_key.up = four_key[3];
				priv->data.four_key.down = four_key[4];
			}
		} else if (tmp == 2) {
			priv->caps |= ACCDET_TRI_KEY_CDD;
		} else {
			priv->caps |= ACCDET_THREE_KEY;
			ret = of_property_read_u32_array(node,
							 "mediatek,three-key-thr",
							 three_key,
							 ARRAY_SIZE(three_key));
			if (!ret) {
				priv->data.three_key.mid = three_key[1];
				priv->data.three_key.up = three_key[2];
				priv->data.three_key.down = three_key[3];
			}
		}
	}

	dev_dbg(dev, "accdet caps=0x%x mic_vol=%u mic_mode=%u\n",
		priv->caps, priv->data.mic_vol, priv->data.mic_mode);

	return 0;
}

static void mt6358_accdet_hw_init(struct mt6358_accdet *priv)
{
	/* Power on the ACCDET clock (bit is active low). */
	regmap_update_bits(priv->regmap, MT6358_AUD_TOP_CKPDN_CON0,
			   MT6358_ACCDET_CK_PDN, 0);

	/* Reset the ACCDET unit. */
	regmap_update_bits(priv->regmap, MT6358_AUD_TOP_RST_CON0,
			   MT6358_ACCDET_RST, MT6358_ACCDET_RST);
	regmap_update_bits(priv->regmap, MT6358_AUD_TOP_RST_CON0,
			   MT6358_ACCDET_RST, 0);

	/*
	 * The codec writes these bits once in its probe, but the reset above
	 * (and probe ordering) can wipe them.  Without the analogue switches
	 * enabled the EINT comparator never toggles and no interrupt is ever
	 * raised, so re-apply them here.
	 */
	regmap_update_bits(priv->regmap, MT6358_ACCDET_CONTROL_RG,
			   MT6358_ACCDET_CMP0_SWSEL |
			   MT6358_ACCDET_VTH_SWSEL |
			   MT6358_ACCDET_MBIAS_SWSEL |
			   MT6358_ACCDET_CMP0_EN_SW |
			   MT6358_ACCDET_VTH_EN_SW |
			   MT6358_ACCDET_MBIAS_EN_SW,
			   MT6358_ACCDET_CMP0_SWSEL |
			   MT6358_ACCDET_VTH_SWSEL |
			   MT6358_ACCDET_MBIAS_SWSEL |
			   MT6358_ACCDET_CMP0_EN_SW |
			   MT6358_ACCDET_VTH_EN_SW |
			   MT6358_ACCDET_MBIAS_EN_SW);

	/* AUD IRQs are unmasked at reset, but make sure of it. */
	regmap_update_bits(priv->regmap, MT6358_AUD_TOP_INT_MASK_CON0,
			   MT6358_INT_ACCDET | MT6358_INT_ACCDET_EINT0 |
			   MT6358_INT_ACCDET_EINT1, 0);

	/* PWM frequency, duty and rise/fall delay. */
	regmap_write(priv->regmap, MT6358_ACCDET_PWM_WIDTH,
		     MT6358_ACCDET_REGISTER_VAL(priv->data.pwm_deb.pwm_width));
	regmap_write(priv->regmap, MT6358_ACCDET_PWM_THRESH,
		     MT6358_ACCDET_REGISTER_VAL(priv->data.pwm_deb.pwm_thresh));
	regmap_write(priv->regmap, MT6358_ACCDET_EN_DELAY_NUM,
		     (priv->data.pwm_deb.fall_delay << 15) |
		     priv->data.pwm_deb.rise_delay);

	/* Microphone bias voltage. */
	regmap_update_bits(priv->regmap, MT6358_AUDENC_ANA_CON10,
			   MT6358_ACCDET_MICBIAS1_VREF_MASK,
			   priv->data.mic_vol <<
			   MT6358_ACCDET_MICBIAS1_VREF_SFT);
	regmap_update_bits(priv->regmap, MT6358_AUDENC_ANA_CON10,
			   MT6358_ACCDET_MICBIAS1_LOWP_EN,
			   MT6358_ACCDET_MICBIAS1_LOWP_EN);

	/* Microphone mode. */
	switch (priv->data.mic_mode) {
	case 1:
		regmap_update_bits(priv->regmap, MT6358_AUDENC_ANA_CON11,
				   MT6358_ACCDET_MODE_ANA11_MODE1,
				   MT6358_ACCDET_MODE_ANA11_MODE1);
		break;
	case 2:
		regmap_update_bits(priv->regmap, MT6358_AUDENC_ANA_CON11,
				   MT6358_ACCDET_MODE_ANA11_MODE2,
				   MT6358_ACCDET_MODE_ANA11_MODE2);
		break;
	case 6:
		regmap_update_bits(priv->regmap, MT6358_AUDENC_ANA_CON11,
				   MT6358_ACCDET_MODE_ANA11_MODE6,
				   MT6358_ACCDET_MODE_ANA11_MODE6);
		regmap_update_bits(priv->regmap, MT6358_AUDENC_ANA_CON10,
				   MT6358_ACCDET_MICBIAS1_DCSW1PEN,
				   MT6358_ACCDET_MICBIAS1_DCSW1PEN);
		break;
	default:
		dev_warn(priv->dev, "unsupported mic-mode %u\n",
			 priv->data.mic_mode);
		break;
	}

	/* Analogue fast discharge is used with level-low EINT polarity. */
	if (priv->data.eint_pol == IRQ_TYPE_LEVEL_LOW)
		regmap_update_bits(priv->regmap, MT6358_AUDENC_ANA_CON6,
				   MT6358_ACCDET_FSTDSCHRG_ANALOG_DIR_EN |
				   MT6358_ACCDET_FSTDSCHRG_IMPR_EN,
				   MT6358_ACCDET_FSTDSCHRG_ANALOG_DIR_EN |
				   MT6358_ACCDET_FSTDSCHRG_IMPR_EN);
	else
		regmap_update_bits(priv->regmap, MT6358_AUDENC_ANA_CON6,
				   MT6358_ACCDET_FSTDSCHRG_ANALOG_DIR_EN |
				   MT6358_ACCDET_FSTDSCHRG_IMPR_EN, 0);

	/* SW mode, with fast discharge enabled. */
	regmap_update_bits(priv->regmap, MT6358_ACCDET_HW_MODE_DFF,
			   MT6358_ACCDET_HWMODE_SEL |
			   MT6358_ACCDET_FAST_DISCHARGE,
			   MT6358_ACCDET_FAST_DISCHARGE);

	/* EINT PWM width/threshold and debounce. */
	regmap_update_bits(priv->regmap, MT6358_ACCDET_EINT0_CTL,
			   MT6358_ACCDET_EINT0_PWM_THRSH_MASK |
			   MT6358_ACCDET_EINT0_PWM_WIDTH_MASK,
			   MT6358_ACCDET_EINT0_PWM_THRSH |
			   MT6358_ACCDET_EINT0_PWM_WIDTH);
	mt6358_accdet_set_eint_debounce(priv, true,
					MT6358_ACCDET_EINT0_DEB_OUT_012);

	regmap_update_bits(priv->regmap, MT6358_ACCDET_STATE_SWCTRL,
			   MT6358_ACCDET_EINT0_PWM_EN_B3 |
			   MT6358_ACCDET_EINT0_PWM_IDLE_B11,
			   MT6358_ACCDET_EINT0_PWM_EN_B3 |
			   MT6358_ACCDET_EINT0_PWM_IDLE_B11);

	/* Initial debounce values. */
	mt6358_accdet_set_debounce(priv, MT6358_ACCDET_STATE_000,
				   priv->data.pwm_deb.debounce0);
	mt6358_accdet_set_debounce(priv, MT6358_ACCDET_STATE_001,
				   priv->data.pwm_deb.debounce1);
	mt6358_accdet_set_debounce(priv, MT6358_ACCDET_STATE_011,
				   priv->data.pwm_deb.debounce3);
	mt6358_accdet_set_debounce(priv, MT6358_ACCDET_STATE_AUXADC,
				   priv->data.pwm_deb.debounce4);

	/*
	 * The reset also clears the EINT edge selection.  With no jack the
	 * pin is idle, so configure the edge that will catch the first
	 * insertion: falling for an active-low line, rising for active-high.
	 */
	if (priv->caps & ACCDET_PMIC_EINT0) {
		bool level_low = priv->data.eint_pol == IRQ_TYPE_LEVEL_LOW;

		regmap_update_bits(priv->regmap, MT6358_ACCDET_IRQ_STS,
				   MT6358_ACCDET_EINT0_IRQ_POL_B14,
				   level_low ? 0 :
				   MT6358_ACCDET_EINT0_IRQ_POL_B14);
	}

	/* Make sure no stale IRQ is pending, then enable EINT0 detection. */
	mt6358_accdet_clear_accdet_irq(priv);
	mt6358_accdet_clear_eint_irq(priv, true);

	regmap_update_bits(priv->regmap, MT6358_ACCDET_CTRL,
			   MT6358_ACCDET_EINT0_EN_B2,
			   MT6358_ACCDET_EINT0_EN_B2);
}

/* Temporary debug helper, remove once jack detection is verified. */
static const unsigned int mt6358_accdet_debug_regs[] = {
	MT6358_AUD_TOP_CKPDN_CON0,
	MT6358_AUD_TOP_INT_CON0,
	MT6358_AUD_TOP_INT_MASK_CON0,
	MT6358_AUD_TOP_INT_STATUS0,
	0x019e, /* MT6358_TOP_INT_STATUS0 */
	MT6358_AUDENC_ANA_CON6,
	MT6358_AUDENC_ANA_CON10,
	MT6358_AUDENC_ANA_CON11,
	MT6358_ACCDET_CTRL,
	MT6358_ACCDET_STATE_SWCTRL,
	MT6358_ACCDET_IRQ_STS,
	MT6358_ACCDET_CONTROL_RG,
	MT6358_ACCDET_STATE_RG,
	MT6358_ACCDET_EINT0_CTL,
	MT6358_ACCDET_EINT0_STATE,
	MT6358_ACCDET_HW_MODE_DFF,
};

static ssize_t regs_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct mt6358_accdet *priv = dev_get_drvdata(dev);
	unsigned int i, val;
	int n = 0;

	for (i = 0; i < ARRAY_SIZE(mt6358_accdet_debug_regs); i++) {
		if (regmap_read(priv->regmap, mt6358_accdet_debug_regs[i], &val))
			continue;
		n += scnprintf(buf + n, PAGE_SIZE - n, "0x%04x: 0x%04x\n",
			       mt6358_accdet_debug_regs[i], val);
	}

	return n;
}
static DEVICE_ATTR_RO(regs);

static struct attribute *mt6358_accdet_attrs[] = {
	&dev_attr_regs.attr,
	NULL,
};

static const struct attribute_group mt6358_accdet_group = {
	.attrs = mt6358_accdet_attrs,
};

static int mt6358_accdet_set_jack(struct snd_soc_component *component,
				  struct snd_soc_jack *jack, void *data)
{
	struct mt6358_accdet *priv = snd_soc_component_get_drvdata(component);

	if (!jack) {
		guard(mutex)(&priv->res_lock);
		priv->jack = NULL;
		return 0;
	}

	snd_jack_set_key(jack->jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_1, KEY_VOLUMEDOWN);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_2, KEY_VOLUMEUP);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_3, KEY_VOICECOMMAND);

	guard(mutex)(&priv->res_lock);
	priv->jack = jack;
	mt6358_accdet_jack_report(priv);

	return 0;
}

int mt6358_accdet_enable_jack_detect(struct snd_soc_component *component,
				     struct snd_soc_jack *jack)
{
	return mt6358_accdet_set_jack(component, jack, NULL);
}
EXPORT_SYMBOL_GPL(mt6358_accdet_enable_jack_detect);

static const struct snd_soc_component_driver mt6358_accdet_soc_driver = {
	.name = MT6358_ACCDET_DEVNAME,
	.set_jack = mt6358_accdet_set_jack,
};

static void mt6358_accdet_destroy_workqueues(void *data)
{
	struct mt6358_accdet *priv = data;

	destroy_workqueue(priv->eint_workqueue);
	destroy_workqueue(priv->accdet_workqueue);
}

static int mt6358_accdet_probe(struct platform_device *pdev)
{
	struct mt6358_accdet *priv;
	struct mt6397_chip *mt6397 = dev_get_drvdata(pdev->dev.parent);
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	if (!mt6397)
		return dev_err_probe(&pdev->dev, -ENODEV, "no parent MT6397 chip\n");

	priv->dev = &pdev->dev;
	priv->regmap = mt6397->regmap;
	if (IS_ERR(priv->regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->regmap),
				     "failed to get parent regmap\n");

	mutex_init(&priv->res_lock);

	ret = mt6358_accdet_parse_dt(priv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to parse DT\n");

	priv->accdet_workqueue =
		create_singlethread_workqueue("mt6358_accdet");
	if (!priv->accdet_workqueue)
		return -ENOMEM;

	priv->eint_workqueue =
		create_singlethread_workqueue("mt6358_accdet_eint");
	if (!priv->eint_workqueue) {
		destroy_workqueue(priv->accdet_workqueue);
		return -ENOMEM;
	}

	ret = devm_add_action_or_reset(&pdev->dev,
				       mt6358_accdet_destroy_workqueues, priv);
	if (ret)
		return ret;

	INIT_WORK(&priv->accdet_work, mt6358_accdet_accdet_work);
	INIT_WORK(&priv->eint_work, mt6358_accdet_eint_work);

	priv->eint_pin_state = MT6358_EINT_PIN_PLUG_OUT;
	priv->jack_type = 0;
	priv->btn_type = 0;
	priv->jack_plugged = false;

	platform_set_drvdata(pdev, priv);

	ret = devm_device_add_group(&pdev->dev, &mt6358_accdet_group);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to add sysfs attributes\n");

	mt6358_accdet_hw_init(priv);

	priv->accdet_irq = platform_get_irq_byname(pdev, "accdet_irq");
	if (priv->accdet_irq < 0)
		return dev_err_probe(&pdev->dev, priv->accdet_irq,
				     "failed to get ACCDET IRQ\n");

	ret = devm_request_threaded_irq(&pdev->dev, priv->accdet_irq, NULL,
					mt6358_accdet_irq,
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
					"ACCDET_IRQ", priv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request ACCDET IRQ\n");

	if (priv->caps & ACCDET_PMIC_EINT0) {
		priv->accdet_eint0 = platform_get_irq_byname(pdev, "accdet_eint0");
		if (priv->accdet_eint0 < 0)
			return dev_err_probe(&pdev->dev, priv->accdet_eint0,
					     "failed to get ACCDET EINT0 IRQ\n");

		ret = devm_request_threaded_irq(&pdev->dev, priv->accdet_eint0,
						NULL, mt6358_accdet_irq,
						IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
						"ACCDET_EINT0", priv);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "failed to request ACCDET EINT0 IRQ\n");
	}

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &mt6358_accdet_soc_driver,
					      NULL, 0);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register component\n");

	mt6358_accdet_jack_report(priv);

	dev_info(&pdev->dev, "MT6358 ACCDET ready (caps=0x%x)\n", priv->caps);

	return 0;
}

static const struct of_device_id mt6358_accdet_of_match[] = {
	{ .compatible = "mediatek,mt6358-accdet" },
	{}
};
MODULE_DEVICE_TABLE(of, mt6358_accdet_of_match);

static struct platform_driver mt6358_accdet_driver = {
	.driver = {
		.name = "mt6358-accdet",
		.of_match_table = mt6358_accdet_of_match,
	},
	.probe = mt6358_accdet_probe,
};
module_platform_driver(mt6358_accdet_driver);

MODULE_DESCRIPTION("MT6358 ACCDET ALSA SoC jack driver");
MODULE_LICENSE("GPL");
