/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mt6358-accdet.h  --  MT6358 ACCDET jack detection
 */

#ifndef __MT6358_ACCDET_H__
#define __MT6358_ACCDET_H__

#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>
#include <sound/jack.h>

#define MT6358_ACCDET_DEVNAME	"mt6358-accdet"

#define MT6358_ACCDET_NUM_BUTTONS	4

#define MT6358_ACCDET_JACK_MASK	(SND_JACK_HEADPHONE | \
				 SND_JACK_HEADSET | \
				 SND_JACK_BTN_0 | \
				 SND_JACK_BTN_1 | \
				 SND_JACK_BTN_2 | \
				 SND_JACK_BTN_3)

#define MT6358_ACCDET_BTN_MASK	(SND_JACK_BTN_0 | \
				 SND_JACK_BTN_1 | \
				 SND_JACK_BTN_2 | \
				 SND_JACK_BTN_3)

/* ACCDET register offsets (PMIC register space) */
#define MT6358_AUD_TOP_CKPDN_CON0	0x220c
#define MT6358_AUD_TOP_RST_CON0		0x2220
#define MT6358_AUD_TOP_INT_CON0		0x2228
#define MT6358_AUD_TOP_INT_MASK_CON0	0x222e
#define MT6358_AUD_TOP_INT_STATUS0	0x2234

#define MT6358_AUDENC_ANA_CON6		0x2394
#define MT6358_AUDENC_ANA_CON9		0x239a
#define MT6358_AUDENC_ANA_CON10		0x239c
#define MT6358_AUDENC_ANA_CON11		0x239e

#define MT6358_ACCDET_RSV		0x2508
#define MT6358_ACCDET_CTRL		0x250a
#define MT6358_ACCDET_STATE_SWCTRL	0x250c
#define MT6358_ACCDET_PWM_WIDTH		0x250e
#define MT6358_ACCDET_PWM_THRESH	0x2510
#define MT6358_ACCDET_EN_DELAY_NUM	0x2512
#define MT6358_ACCDET_DEBOUNCE0		0x2514
#define MT6358_ACCDET_DEBOUNCE1		0x2516
#define MT6358_ACCDET_DEBOUNCE2		0x2518
#define MT6358_ACCDET_DEBOUNCE3		0x251a
#define MT6358_ACCDET_DEBOUNCE4		0x251c
#define MT6358_ACCDET_DEFAULT_STATE_RG	0x251e
#define MT6358_ACCDET_IRQ_STS		0x2520
#define MT6358_ACCDET_CONTROL_RG	0x2522
#define MT6358_ACCDET_STATE_RG		0x2524
#define MT6358_ACCDET_EINT0_CTL		0x2526
#define MT6358_ACCDET_EINT0_PWM_DELAY	0x2528
#define MT6358_ACCDET_EINT0_STATE	0x252c
#define MT6358_ACCDET_HW_MODE_DFF	0x2538
#define MT6358_ACCDET_EINT1_CTL		0x253a
#define MT6358_ACCDET_EINT1_PWM_DELAY	0x253c
#define MT6358_ACCDET_EINT1_STATE	0x253e

/* AUD_TOP_CKPDN_CON0 */
#define MT6358_ACCDET_CK_PDN		BIT(0)

/* AUD_TOP_RST_CON0 */
#define MT6358_ACCDET_RST		BIT(1)

/* AUD_TOP_INT_CON0 / AUD_TOP_INT_STATUS0 */
#define MT6358_INT_ACCDET		BIT(5)
#define MT6358_INT_ACCDET_EINT0		BIT(6)
#define MT6358_INT_ACCDET_EINT1		BIT(7)

/* AUDENC_ANA_CON6 */
#define MT6358_ACCDET_FSTDSCHRG_ANALOG_DIR_EN	BIT(5)
#define MT6358_ACCDET_FSTDSCHRG_IMPR_EN		BIT(6)

/* AUDENC_ANA_CON10 */
#define MT6358_ACCDET_MICBIAS1_LOWP_EN		BIT(2)
#define MT6358_ACCDET_MICBIAS1_VREF_SFT		4
#define MT6358_ACCDET_MICBIAS1_VREF_MASK	(0x7 << 4)
#define MT6358_ACCDET_MICBIAS1_DCSW1PEN		BIT(8)

/* AUDENC_ANA_CON11 */
#define MT6358_ACCDET_MODE_ANA11_MODE1		0x0807
#define MT6358_ACCDET_MODE_ANA11_MODE2		0x0887
#define MT6358_ACCDET_MODE_ANA11_MODE6		0x0887

/* ACCDET_CONTROL_RG (CON13) */
#define MT6358_ACCDET_CMP0_SWSEL		BIT(1)
#define MT6358_ACCDET_VTH_SWSEL			BIT(2)
#define MT6358_ACCDET_MBIAS_SWSEL		BIT(3)
#define MT6358_ACCDET_CMP0_EN_SW		BIT(12)
#define MT6358_ACCDET_VTH_EN_SW			BIT(13)
#define MT6358_ACCDET_MBIAS_EN_SW		BIT(14)
#define MT6358_ACCDET_PWM_EN_SW			BIT(15)

/* ACCDET_CTRL */
#define MT6358_ACCDET_ENABLE_B0			BIT(0)
#define MT6358_ACCDET_SEQ_INIT_EN_B1		BIT(1)
#define MT6358_ACCDET_EINT0_EN_B2		BIT(2)
#define MT6358_ACCDET_EINT0_SEQ_INIT_EN_B3	BIT(3)
#define MT6358_ACCDET_EINT1_EN_B4		BIT(4)
#define MT6358_ACCDET_EINT1_SEQ_INIT_EN_B5	BIT(5)
#define MT6358_ACCDET_ANASWCTRL_SEL_B6		BIT(6)

/* ACCDET_STATE_SWCTRL */
#define MT6358_ACCDET_CMP_PWM_EN_B0		BIT(0)
#define MT6358_ACCDET_VTH_PWM_EN_B1		BIT(1)
#define MT6358_ACCDET_MICBIAS_PWM_EN_B2		BIT(2)
#define MT6358_ACCDET_EINT0_PWM_EN_B3		BIT(3)
#define MT6358_ACCDET_EINT1_PWM_EN_B4		BIT(4)
#define MT6358_ACCDET_PWM_EN			(0x7)
#define MT6358_ACCDET_EINT0_PWM_IDLE_B11	BIT(11)
#define MT6358_ACCDET_EINT1_PWM_IDLE_B12	BIT(12)
#define MT6358_ACCDET_PWM_IDLE			(0x7 << 8)

/* ACCDET_IRQ_STS */
#define MT6358_ACCDET_IRQ_B0			BIT(0)
#define MT6358_ACCDET_EINT0_IRQ_B2		BIT(2)
#define MT6358_ACCDET_EINT1_IRQ_B3		BIT(3)
#define MT6358_ACCDET_IRQ_CLR_B8		BIT(8)
#define MT6358_ACCDET_EINT0_IRQ_CLR_B10		BIT(10)
#define MT6358_ACCDET_EINT1_IRQ_CLR_B11		BIT(11)
#define MT6358_ACCDET_EINT0_IRQ_POL_B14		BIT(14)
#define MT6358_ACCDET_EINT1_IRQ_POL_B15		BIT(15)

/* ACCDET_STATE_RG */
#define MT6358_ACCDET_STATE_MEM_IN_OFFSET	6
#define MT6358_ACCDET_STATE_AB_MASK		0x3

/* ACCDET_EINT0_CTL / ACCDET_EINT1_CTL */
#define MT6358_ACCDET_EINT0_DEB_SEL		BIT(0)
#define MT6358_ACCDET_EINT0_DEB_BYPASS		(0x0 << 3)
#define MT6358_ACCDET_EINT0_DEB_OUT_012		(0x1 << 3)
#define MT6358_ACCDET_EINT0_DEB_IN_32		(0xa << 3)
#define MT6358_ACCDET_EINT0_DEB_IN_64		(0xc << 3)
#define MT6358_ACCDET_EINT0_DEB_IN_256		(0xe << 3)
#define MT6358_ACCDET_EINT0_DEB_512		(0xf << 3)
#define MT6358_ACCDET_EINT0_DEB_CLR		(0xf << 3)
#define MT6358_ACCDET_EINT0_PWM_THRSH_MASK	(0x7 << 8)
#define MT6358_ACCDET_EINT0_PWM_WIDTH_MASK	(0x3 << 12)
#define MT6358_ACCDET_EINT0_PWM_THRSH		(0x6 << 8)
#define MT6358_ACCDET_EINT0_PWM_WIDTH		(0x2 << 12)

#define MT6358_ACCDET_EINT1_DEB_SEL		BIT(0)
#define MT6358_ACCDET_EINT1_DEB_BYPASS		(0x0 << 3)
#define MT6358_ACCDET_EINT1_DEB_OUT_012		(0x1 << 3)
#define MT6358_ACCDET_EINT1_DEB_IN_32		(0xa << 3)
#define MT6358_ACCDET_EINT1_DEB_IN_64		(0xc << 3)
#define MT6358_ACCDET_EINT1_DEB_IN_256		(0xe << 3)
#define MT6358_ACCDET_EINT1_DEB_512		(0xf << 3)
#define MT6358_ACCDET_EINT1_DEB_CLR		(0xf << 3)
#define MT6358_ACCDET_EINT1_PWM_THRSH_MASK	(0x7 << 8)
#define MT6358_ACCDET_EINT1_PWM_WIDTH_MASK	(0x3 << 12)
#define MT6358_ACCDET_EINT1_PWM_THRSH		(0x6 << 8)
#define MT6358_ACCDET_EINT1_PWM_WIDTH		(0x2 << 12)

/* ACCDET_HW_MODE_DFF */
#define MT6358_ACCDET_HWEN_SEL_0		0x0
#define MT6358_ACCDET_HWEN_SEL_1		0x1
#define MT6358_ACCDET_HWEN_SEL_0_OR_1		0x2
#define MT6358_ACCDET_HWEN_SEL_0_AND_1		0x3
#define MT6358_ACCDET_HWMODE_SEL		BIT(2)
#define MT6358_ACCDET_EINT_DEB_OUT_DFF		BIT(3)
#define MT6358_ACCDET_FAST_DISCHARGE		BIT(4)
#define MT6358_ACCDET_FAST_DISCHARGE_EN		0xc01c
#define MT6358_ACCDET_FAST_DISCHARGE_DIS	0xc00c
#define MT6358_ACCDET_FAST_DISCHARGE_REVISE	0x000c
#define MT6358_ACCDET_EINT0_REVERSE		BIT(14)
#define MT6358_ACCDET_EINT1_REVERSE		BIT(15)
#define MT6358_ACCDET_EINT_REVERSE		(BIT(14) | BIT(15))

#define MT6358_ACCDET_REGISTER_VAL(x)	((x) - 1)

enum mt6358_accdet_debounce_state {
	MT6358_ACCDET_STATE_000 = 0,
	MT6358_ACCDET_STATE_001,
	MT6358_ACCDET_STATE_010,
	MT6358_ACCDET_STATE_011,
	MT6358_ACCDET_STATE_AUXADC,
	MT6358_ACCDET_EINT0_STATE_000,
	MT6358_ACCDET_EINT0_STATE_011,
	MT6358_ACCDET_EINT1_STATE_000,
	MT6358_ACCDET_EINT1_STATE_011,
};

struct mt6358_pwm_deb_settings {
	unsigned int pwm_width;
	unsigned int pwm_thresh;
	unsigned int fall_delay;
	unsigned int rise_delay;
	unsigned int debounce0;
	unsigned int debounce1;
	unsigned int debounce3;
	unsigned int debounce4;
	unsigned int eint_pwm_width;
	unsigned int eint_pwm_thresh;
	unsigned int eint_debounce0;
	unsigned int eint_debounce1;
	unsigned int eint_debounce2;
	unsigned int eint_debounce3;
	unsigned int eint_inverter_debounce;
};

struct mt6358_three_key_threshold {
	unsigned int mid;
	unsigned int up;
	unsigned int down;
};

struct mt6358_four_key_threshold {
	unsigned int mid;
	unsigned int voice;
	unsigned int up;
	unsigned int down;
};

struct mt6358_accdet_data {
	unsigned int mic_vol;
	unsigned int mic_mode;
	unsigned int plugout_deb;
	unsigned int eint_pol;
	unsigned int eint_detect_mode;
	unsigned int eint_use_ext_res;
	unsigned int eint_comp_vth;
	unsigned int eint_num;
	unsigned int key_mode;
	struct mt6358_pwm_deb_settings pwm_deb;
	struct mt6358_three_key_threshold three_key;
	struct mt6358_four_key_threshold four_key;
};

struct mt6358_accdet {
	struct device *dev;
	struct regmap *regmap;
	struct snd_soc_jack *jack;
	struct mt6358_accdet_data data;

	int accdet_irq;
	int accdet_eint0;
	int accdet_eint1;
	unsigned int caps;

	struct mutex res_lock; /* protects register sequences and jack state */
	unsigned int eint_pin_state; /* current EINT pin state, see enum */
	unsigned int jack_type;
	unsigned int btn_type;
	unsigned int jd_sts;
	bool jack_plugged;

	struct work_struct accdet_work;
	struct work_struct eint_work;
	struct workqueue_struct *accdet_workqueue;
	struct workqueue_struct *eint_workqueue;
};

#if IS_ENABLED(CONFIG_SND_SOC_MT6358_ACCDET)
int mt6358_accdet_enable_jack_detect(struct snd_soc_component *component,
				     struct snd_soc_jack *jack);
#else
static inline int
mt6358_accdet_enable_jack_detect(struct snd_soc_component *component,
				 struct snd_soc_jack *jack)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* __MT6358_ACCDET_H__ */
