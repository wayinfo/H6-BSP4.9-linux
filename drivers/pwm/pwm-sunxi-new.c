/*
 * drivers/pwm/pwm-sunxi.c
 *
 * Allwinnertech pulse-width-modulation controller driver
 *
 * Copyright (C) 2015 AllWinner
 *
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */
#include <linux/types.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/pwm.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/pinctrl/pinconf.h>
/*#include <linux/sunxi-gpio.h>*/
#include <linux/pinctrl/consumer.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/of_iommu.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>

#include <linux/io.h>
#include <linux/clk.h>

#define PWM_DEBUG 0
#define PWM_NUM_MAX 4
#define PWM_BIND_NUM 2
#define PWM_PIN_STATE_ACTIVE "active"
#define PWM_PIN_STATE_SLEEP "sleep"

#define SETMASK(width, shift)   ((width?((-1U) >> (32-width)):0)  << (shift))
#define CLRMASK(width, shift)   (~(SETMASK(width, shift)))
#define GET_BITS(shift, width, reg)     \
	    (((reg) & SETMASK(width, shift)) >> (shift))
#define SET_BITS(shift, width, reg, val) \
	    (((reg) & CLRMASK(width, shift)) | (val << (shift)))

#if PWM_DEBUG
#define pwm_debug(msg...) pr_info
#else
#define pwm_debug(msg...)
#endif

#if ((defined CONFIG_ARCH_SUN8IW12P1) ||\
			(defined CONFIG_ARCH_SUN8IW17P1) ||\
			(defined CONFIG_ARCH_SUN50IW6P1) ||\
			(defined CONFIG_ARCH_SUN50IW3P1))
#define CLK_GATE_SUPPORT
#endif

struct sunxi_pwm_config {
	unsigned int reg_peci_offset;
	unsigned int reg_peci_shift;
	unsigned int reg_peci_width;

	unsigned int reg_pis_offset;
	unsigned int reg_pis_shift;
	unsigned int reg_pis_width;

	unsigned int reg_crie_offset;
	unsigned int reg_crie_shift;
	unsigned int reg_crie_width;

	unsigned int reg_cfie_offset;
	unsigned int reg_cfie_shift;
	unsigned int reg_cfie_width;

	unsigned int reg_cris_offset;
	unsigned int reg_cris_shift;
	unsigned int reg_cris_width;

	unsigned int reg_cfis_offset;
	unsigned int reg_cfis_shift;
	unsigned int reg_cfis_width;

	unsigned int reg_clk_src_offset;
	unsigned int reg_clk_src_shift;
	unsigned int reg_clk_src_width;

	unsigned int reg_bypass_offset;
	unsigned int reg_bypass_shift;
	unsigned int reg_bypass_width;

	unsigned int reg_clk_gating_offset;
	unsigned int reg_clk_gating_shift;
	unsigned int reg_clk_gating_width;

	unsigned int reg_clk_div_m_offset;
	unsigned int reg_clk_div_m_shift;
	unsigned int reg_clk_div_m_width;

	unsigned int reg_pdzintv_offset;
	unsigned int reg_pdzintv_shift;
	unsigned int reg_pdzintv_width;

	unsigned int reg_dz_en_offset;
	unsigned int reg_dz_en_shift;
	unsigned int reg_dz_en_width;

	unsigned int reg_enable_offset;
	unsigned int reg_enable_shift;
	unsigned int reg_enable_width;

	unsigned int reg_cap_en_offset;
	unsigned int reg_cap_en_shift;
	unsigned int reg_cap_en_width;

	unsigned int reg_period_rdy_offset;
	unsigned int reg_period_rdy_shift;
	unsigned int reg_period_rdy_width;

	unsigned int reg_pul_start_offset;
	unsigned int reg_pul_start_shift;
	unsigned int reg_pul_start_width;

	unsigned int reg_mode_offset;
	unsigned int reg_mode_shift;
	unsigned int reg_mode_width;

	unsigned int reg_act_sta_offset;
	unsigned int reg_act_sta_shift;
	unsigned int reg_act_sta_width;

	unsigned int reg_prescal_offset;
	unsigned int reg_prescal_shift;
	unsigned int reg_prescal_width;

	unsigned int reg_entire_offset;
	unsigned int reg_entire_shift;
	unsigned int reg_entire_width;

	unsigned int reg_active_offset;
	unsigned int reg_active_shift;
	unsigned int reg_active_width;

	unsigned int reg_busy_offset;
	unsigned int reg_busy_shift;

	unsigned int dead_time;
	unsigned int bind_pwm;

};

static int sunxi_pwm_get_config_base(struct platform_device *pdev,
				struct sunxi_pwm_config *config);
static int sunxi_pwm_get_config_enh(struct platform_device *pdev,
				struct sunxi_pwm_config *config);
static int sunxi_pwm_config_base(struct pwm_chip *chip, struct pwm_device *pwm,
				int duty_ns, int period_ns);

static int sunxi_pwm_config_enh(struct pwm_chip *chip, struct pwm_device *pwm,
				int duty_ns, int period_ns);

struct sunxi_pwm_chip {
	struct pwm_chip chip;
	void __iomem *base;
	struct sunxi_pwm_config *config;
	int (*sunxi_pwm_get_config)(struct platform_device *pdev,
				struct sunxi_pwm_config *config);
	int (*sunxi_pwm_config)(struct pwm_chip *chip, struct pwm_device *pwm,
				int duty_ns, int period_ns);
#if defined(CLK_GATE_SUPPORT)
	struct clk	*pwm_clk;
#endif
};

static struct sunxi_pwm_chip pwm_config_param[] = {
	[0] = {
		.sunxi_pwm_get_config = sunxi_pwm_get_config_base,
		.sunxi_pwm_config = sunxi_pwm_config_base,
	},

	[1] = {
		.sunxi_pwm_get_config = sunxi_pwm_get_config_enh,
		.sunxi_pwm_config = sunxi_pwm_config_enh,
	},

};

static inline struct sunxi_pwm_chip *to_sunxi_pwm_chip(struct pwm_chip *chip)
{
	return container_of(chip, struct sunxi_pwm_chip, chip);
}

static inline u32 sunxi_pwm_readl(struct pwm_chip *chip, u32 offset)
{
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);
	u32 value = 0;

	value = readl(pc->base + offset);

	return value;
}

static inline u32 sunxi_pwm_writel(struct pwm_chip *chip, u32 offset, u32 value)
{
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);

	writel(value, pc->base + offset);

	return 0;
}

static int sunxi_pwm_pin_set_state(struct device *dev, char *name)
{
	struct pinctrl *pctl;
	struct pinctrl_state *state;
	int ret = -1;

	pctl = pinctrl_get(dev);
	if (IS_ERR(pctl)) {
		dev_err(dev, "pinctrl_get failed!\n");
		ret = PTR_ERR(pctl);
		goto exit;
	}

	state = pinctrl_lookup_state(pctl, name);
	if (IS_ERR(state)) {
		dev_err(dev, "pinctrl_lookup_state(%s) failed!\n", name);
		ret = PTR_ERR(state);
		goto exit;
	}

	ret = pinctrl_select_state(pctl, state);
	if (ret < 0) {
		dev_err(dev, "pinctrl_select_state(%s) failed!\n", name);
		goto exit;
	}
	ret = 0;

exit:
	return ret;
}

#if !defined(CONFIG_OF)
struct platform_device sunxi_pwm_device = {
	.name = "sunxi_pwm",
	.id = -1,
};
#else
static const struct of_device_id sunxi_pwm_match[] = {
	{ .compatible = "allwinner,sunxi-pwm", .data = &pwm_config_param[1] },
	{ .compatible = "allwinner,sunxi-s_pwm", .data = &pwm_config_param[1] },
	{ .compatible = "allwinner,sun8iw12-s_pwm",
						.data = &pwm_config_param[0] },
	{},
};
MODULE_DEVICE_TABLE(of, sunxi_pwm_match);
#endif

static int sunxi_pwm_get_config_base(struct platform_device *pdev,
			struct sunxi_pwm_config *config)
{
	struct device_node *np = pdev->dev.of_node;
	int ret = 0;

	/* read register config */
	ret = of_property_read_u32(np,
			"reg_busy_offset", &config->reg_busy_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_busy_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_busy_shift", &config->reg_busy_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_busy_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_enable_offset", &config->reg_enable_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_enable_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_enable_shift", &config->reg_enable_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_enable_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_clk_gating_offset",
			&config->reg_clk_gating_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_clk_gating_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_clk_gating_shift", &config->reg_clk_gating_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_clk_gating_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_bypass_offset", &config->reg_bypass_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_bypass_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_bypass_shift", &config->reg_bypass_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_bypass_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_pulse_start_offset",
			&config->reg_pul_start_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_bypass_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_pulse_start_shift", &config->reg_pul_start_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_pulse_start_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_mode_offset", &config->reg_mode_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_mode_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_mode_shift", &config->reg_mode_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_mode_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_polarity_offset", &config->reg_act_sta_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_polarity_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_polarity_shift", &config->reg_act_sta_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_polarity_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_period_offset", &config->reg_entire_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_period_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_period_shift", &config->reg_entire_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_period_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_period_width", &config->reg_entire_width);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_period_width! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_active_offset", &config->reg_active_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_duty_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_active_shift", &config->reg_active_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_duty_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_active_width", &config->reg_active_width);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_duty_width! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_prescal_offset", &config->reg_prescal_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_duty_width! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_prescal_shift", &config->reg_prescal_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_duty_width! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_prescal_width", &config->reg_prescal_width);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_duty_width! err=%d\n", ret);
		goto err;
	}

	config->bind_pwm = 255;
err:

	of_node_put(np);

	return ret;
}

static int sunxi_pwm_get_config_enh(struct platform_device *pdev,
				struct sunxi_pwm_config *config)
{
	struct device_node *np = pdev->dev.of_node;
	int ret = 0;
	/* read register config */
	ret = of_property_read_u32(np,
			"reg_peci_offset", &config->reg_peci_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_peci_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_peci_shift", &config->reg_peci_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_peci_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			 "reg_peci_width", &config->reg_peci_width);
	if (ret < 0) {
		dev_err(&pdev->dev,
			 "failed to get reg_peci_width! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			 "reg_pis_offset", &config->reg_pis_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			 "failed to get reg_pis_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			 "reg_pis_shift", &config->reg_pis_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			 "failed to get reg_pis_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			 "reg_pis_width", &config->reg_pis_width);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_pis_width! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_crie_offset", &config->reg_crie_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_crie_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_crie_shift", &config->reg_crie_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_crie_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
		"reg_bypass_shift", &config->reg_bypass_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_bypass_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_crie_width", &config->reg_crie_width);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_crie_width! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_cfie_offset", &config->reg_cfie_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_cfie_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_cfie_shift", &config->reg_cfie_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_cfie_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_cfie_width", &config->reg_cfie_width);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_cfie_width! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_cris_offset", &config->reg_cris_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_cris_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_cris_shift", &config->reg_cris_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_cris_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_cris_width", &config->reg_cris_width);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_cris_width! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_cfis_offset", &config->reg_cfis_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_cfis_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_cfis_shift", &config->reg_cfis_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_cfis_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_cfis_width", &config->reg_cfis_width);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_cfis_width! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
		"reg_clk_src_offset", &config->reg_clk_src_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_clk_src_offset! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
		"reg_clk_src_shift", &config->reg_clk_src_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_clk_src_shift! err=%d\n", ret);
		goto err;
	}

	ret = of_property_read_u32(np,
			"reg_clk_src_width",
			&config->reg_clk_src_width);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_clk_src_width! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
		"reg_bypass_offset", &config->reg_bypass_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_bypass_offset! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
		"reg_bypass_shift", &config->reg_bypass_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_bypass_shift! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_bypass_width", &config->reg_bypass_width);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_bypass_width! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_clk_gating_offset",
			&config->reg_clk_gating_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_clk_gating_offset! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_clk_gating_shift",
			&config->reg_clk_gating_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_clk_gating_shift! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_clk_gating_width",
			&config->reg_clk_gating_width);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_clk_gating_width! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_clk_div_m_offset",
			&config->reg_clk_div_m_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_clk_div_m_offset! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_clk_div_m_shift",
			&config->reg_clk_div_m_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_clk_div_m_shift! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_clk_div_m_width",
			&config->reg_clk_div_m_width);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_clk_div_m_width! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_pdzintv_offset",
			&config->reg_pdzintv_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_pdzintv_offset! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_pdzintv_shift",
			&config->reg_pdzintv_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_pdzintv_shift! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_pdzintv_width",
			&config->reg_pdzintv_width);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_pdzintv_width! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_dz_en_offset",
			&config->reg_dz_en_offset);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_dz_en_offset! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_dz_en_shift", &config->reg_dz_en_shift);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_dz_en_shift! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_dz_en_width", &config->reg_dz_en_width);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get reg_dz_en_width! err=%d\n", ret);
		goto err;
	}
	ret = of_property_read_u32(np,
			"reg_enable_offset",
			&config->reg_enable_offset);

	ret = of_property_read_u32(np,
			"reg_enable_shift",
			&config->reg_enable_shift);

	ret = of_property_read_u32(np,
			"reg_enable_width",
			&config->reg_enable_width);

	ret = of_property_read_u32(np,
			"reg_cap_en_offset",
			&config->reg_cap_en_offset);

	ret = of_property_read_u32(np,
			"reg_cap_en_shift",
			&config->reg_cap_en_shift);

	ret = of_property_read_u32(np,
			"reg_cap_en_width",
			&config->reg_cap_en_width);

	ret = of_property_read_u32(np,
			"reg_period_rdy_offset",
			&config->reg_period_rdy_offset);

	ret = of_property_read_u32(np,
			"reg_period_rdy_shift",
			&config->reg_period_rdy_shift);

	ret = of_property_read_u32(np,
			"reg_period_rdy_width",
			&config->reg_period_rdy_width);

	ret = of_property_read_u32(np,
			"reg_pul_start_offset",
			&config->reg_pul_start_offset);

	ret = of_property_read_u32(np,
			"reg_pul_start_shift",
			&config->reg_pul_start_shift);

	ret = of_property_read_u32(np,
			"reg_pul_start_width",
			&config->reg_pul_start_width);

	ret = of_property_read_u32(np,
			"reg_mode_offset", &config->reg_mode_offset);

	ret = of_property_read_u32(np,
			"reg_mode_shift", &config->reg_mode_shift);

	ret = of_property_read_u32(np,
			"reg_mode_width", &config->reg_mode_width);

	ret = of_property_read_u32(np,
			"reg_act_sta_offset",
			&config->reg_act_sta_offset);

	ret = of_property_read_u32(np,
			"reg_act_sta_shift",
			&config->reg_act_sta_shift);

	ret = of_property_read_u32(np,
			"reg_act_sta_width",
			&config->reg_act_sta_width);

	ret = of_property_read_u32(np,
			"reg_prescal_offset",
			&config->reg_prescal_offset);

	ret = of_property_read_u32(np,
			"reg_prescal_shift",
			&config->reg_prescal_shift);

	ret = of_property_read_u32(np,
			"reg_prescal_width",
			&config->reg_prescal_width);

	ret = of_property_read_u32(np,
			"reg_entire_offset",
			&config->reg_entire_offset);

	ret = of_property_read_u32(np,
			"reg_entire_shift",
			&config->reg_entire_shift);

	ret = of_property_read_u32(np,
			"reg_entire_width",
			&config->reg_entire_width);

	ret = of_property_read_u32(np,
			"reg_active_offset",
			&config->reg_active_offset);

	ret = of_property_read_u32(np,
			"reg_active_shift",
			&config->reg_active_shift);

	ret = of_property_read_u32(np,
			"reg_active_width",
			&config->reg_active_width);

	ret = of_property_read_u32(np,
			"bind_pwm", &config->bind_pwm);
	if (ret < 0) {
		/*if there is no bind pwm,set 255, dual pwm invalid!*/
		config->bind_pwm = 255;
		ret = 0;
	}

	ret = of_property_read_u32(np,
			"dead_time", &config->dead_time);
	if (ret < 0) {
		/*if there is  bind pwm, but not set dead time,
		 * set bind pwm 255,dual pwm invalid!
		 */
		config->bind_pwm = 255;
		ret = 0;
	}

err:

	of_node_put(np);

	return ret;
}

static int sunxi_pwm_set_polarity_single(struct pwm_chip *chip,
		struct pwm_device *pwm, enum pwm_polarity polarity)
{
	u32 temp;
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);
	unsigned int reg_offset, reg_shift;

	reg_offset = pc->config[pwm->pwm - chip->base].reg_act_sta_offset;
	reg_shift = pc->config[pwm->pwm - chip->base].reg_act_sta_shift;
	temp = sunxi_pwm_readl(chip, reg_offset);
	if (polarity == PWM_POLARITY_NORMAL)
		temp = SET_BITS(reg_shift, 1, temp, 1);
	else
		temp = SET_BITS(reg_shift, 1, temp, 0);

	sunxi_pwm_writel(chip, reg_offset, temp);

	return 0;
}

static int sunxi_pwm_set_polarity_dual(struct pwm_chip *chip,
					struct pwm_device *pwm,
					enum pwm_polarity polarity,
					int bind_num)
{
	u32 temp[2];
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);
	unsigned int reg_offset[2], reg_shift[2];

	/* config current pwm*/
	reg_offset[0] = pc->config[pwm->pwm - chip->base].reg_act_sta_offset;
	reg_shift[0] = pc->config[pwm->pwm - chip->base].reg_act_sta_shift;
	temp[0] = sunxi_pwm_readl(chip, reg_offset[0]);
	if (polarity == PWM_POLARITY_NORMAL)
		temp[0] = SET_BITS(reg_shift[0], 1, temp[0], 1);
	else
		temp[0] = SET_BITS(reg_shift[0], 1, temp[0], 0);
	/* config bind pwm*/
	reg_offset[1] = pc->config[bind_num - chip->base].reg_act_sta_offset;
	reg_shift[1] = pc->config[bind_num - chip->base].reg_act_sta_shift;
	temp[1] = sunxi_pwm_readl(chip, reg_offset[1]);

	/*bind pwm's polarity is reverse compare with the  current pwm*/
	if (polarity == PWM_POLARITY_NORMAL)
		temp[1] = SET_BITS(reg_shift[1], 1, temp[1], 0);
	else
		temp[1] = SET_BITS(reg_shift[1], 1, temp[1], 1);
	/*config register at the same time*/
	sunxi_pwm_writel(chip, reg_offset[0], temp[0]);
	sunxi_pwm_writel(chip, reg_offset[1], temp[1]);

	return 0;

}

static int sunxi_pwm_set_polarity(struct pwm_chip *chip,
		struct pwm_device *pwm, enum pwm_polarity polarity)
{
	int bind_num;
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);

	bind_num = pc->config[pwm->pwm - chip->base].bind_pwm;
	if (bind_num == 255)
		sunxi_pwm_set_polarity_single(chip, pwm, polarity);
	else
		sunxi_pwm_set_polarity_dual(chip, pwm, polarity, bind_num);

	return 0;
}

static int sunxi_pwm_config_base(struct pwm_chip *chip, struct pwm_device *pwm,
		int duty_ns, int period_ns)
{
	u32 pre_scal[11][2] = {
		/* reg_value  clk_pre_div */
		{15, 1},
		{0, 120},
		{1, 180},
		{2, 240},
		{3, 360},
		{4, 480},
		{8, 12000},
		{9, 24000},
		{10, 36000},
		{11, 48000},
		{12, 72000}
		};
	u32 freq;
	u32 pre_scal_id = 0;
	u32 entire_cycles = 256;
	u32 active_cycles = 192;
	u32 entire_cycles_max = 65536;
	u32 temp;
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);
	unsigned int reg_offset, reg_shift, reg_width;

	reg_offset = pc->config[pwm->pwm - chip->base].reg_bypass_offset;


	reg_shift = pc->config[pwm->pwm - chip->base].reg_bypass_shift;

	if (period_ns < 42) {
		/* if freq lt 24M, then direct output 24M clock */
		temp = sunxi_pwm_readl(chip, reg_offset);
		temp = SET_BITS(reg_shift, 1, temp, 1);
		sunxi_pwm_writel(chip, reg_offset, temp);
		return 0;
	}
	/* disable bypass function */
	temp = sunxi_pwm_readl(chip, reg_offset);
	temp = SET_BITS(reg_shift, 1, temp, 0);
	sunxi_pwm_writel(chip, reg_offset, temp);

	if (period_ns < 10667)
		freq = 93747;
	else if (period_ns > 1000000000)
		freq = 1;
	else
		freq = 1000000000 / period_ns;

	/* clock source rate is  24Mhz */
	entire_cycles = 24000000 / freq / pre_scal[pre_scal_id][1];

	while (entire_cycles > entire_cycles_max) {
		pre_scal_id++;

		if (pre_scal_id > 10)
			break;

		entire_cycles = 24000000 / freq / pre_scal[pre_scal_id][1];
	}

	if (period_ns < 5*100*1000)
		active_cycles = (duty_ns * entire_cycles +
						(period_ns/2)) / period_ns;
	else if (period_ns >= 5*100*1000 && period_ns < 6553500)
		active_cycles =
			((duty_ns / 100) * entire_cycles +
			(period_ns / 2 / 100)) / (period_ns/100);
	else
		active_cycles =
			((duty_ns / 10000) * entire_cycles +
			 (period_ns / 2 / 10000)) / (period_ns/10000);

	/* config prescal */
	reg_offset = pc->config[pwm->pwm - chip->base].reg_prescal_offset;
	reg_shift = pc->config[pwm->pwm - chip->base].reg_prescal_shift;
	reg_width = pc->config[pwm->pwm - chip->base].reg_prescal_width;

	temp = sunxi_pwm_readl(chip, reg_offset);
	temp = SET_BITS(reg_shift, reg_width, temp, (pre_scal[pre_scal_id][0]));
	sunxi_pwm_writel(chip, reg_offset, temp);

	/* config active cycles */
	reg_offset = pc->config[pwm->pwm - chip->base].reg_active_offset;
	reg_shift = pc->config[pwm->pwm - chip->base].reg_active_shift;
	reg_width = pc->config[pwm->pwm - chip->base].reg_active_width;

	temp = sunxi_pwm_readl(chip, reg_offset);
	temp = SET_BITS(reg_shift, reg_width, temp, (active_cycles));
	sunxi_pwm_writel(chip, reg_offset, temp);

	/* config period cycles */
	reg_offset = pc->config[pwm->pwm - chip->base].reg_entire_offset;
	reg_shift = pc->config[pwm->pwm - chip->base].reg_entire_shift;
	reg_width = pc->config[pwm->pwm - chip->base].reg_entire_width;
	temp = sunxi_pwm_readl(chip, reg_offset);
	temp = SET_BITS(reg_shift, reg_width, temp, (entire_cycles - 1));

	sunxi_pwm_writel(chip, reg_offset, temp);

	return 0;
}
#define PRESCALE_MAX 256

static int sunxi_pwm_config_enh_single(struct pwm_chip *chip,
		struct pwm_device *pwm, int duty_ns, int period_ns)
{
	unsigned int temp;
	unsigned long long c = 0;
	unsigned long entire_cycles = 256, active_cycles = 192;
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);
	struct sunxi_pwm_config *config_pwm;
	unsigned int reg_offset, reg_shift, reg_width;
	unsigned int pre_scal_id = 0, div_m = 0, prescale = 0;
	u32 pre_scal[][2] = {
		/* reg_value  clk_pre_div */
		{0, 1},
		{1, 2},
		{2, 4},
		{3, 8},
		{4, 16},
		{5, 32},
		{6, 64},
		{7, 128},
		{8, 256},
	};
	config_pwm = &(pc->config[pwm->pwm - chip->base]);
	reg_offset = config_pwm->reg_bypass_offset;
	reg_shift = config_pwm->reg_bypass_shift;
	reg_width = config_pwm->reg_bypass_width;

	if (period_ns > 0 && period_ns <= 10) {
		/*if freq lt 100M, then direct output 100M clock,set by pass.*/
		c = 100000000;
		temp = sunxi_pwm_readl(chip, reg_offset);
		temp = SET_BITS(reg_shift, reg_width, temp, 1);
		sunxi_pwm_writel(chip, reg_offset, temp);

		reg_offset = config_pwm->reg_clk_src_offset;
		reg_shift = config_pwm->reg_clk_src_shift;
		reg_width = config_pwm->reg_clk_src_width;

		temp = sunxi_pwm_readl(chip, reg_offset);
		temp = SET_BITS(reg_shift, reg_width, temp, 1);
		sunxi_pwm_writel(chip, reg_offset, temp);

		return 0;
	} else if (period_ns > 10 && period_ns <= 334) {
		/* if freq between 3M~100M, then select 100M as clock */
		c = 100000000;
		reg_offset = config_pwm->reg_clk_src_offset;
		reg_shift = config_pwm->reg_clk_src_shift;
		reg_width = config_pwm->reg_clk_src_width;

		temp = sunxi_pwm_readl(chip, reg_offset);
		temp = SET_BITS(reg_shift, reg_width, temp, 1);
		sunxi_pwm_writel(chip, reg_offset, temp);
	} else if (period_ns > 334) {
		/* if freq < 3M, then select 24M clock */
		c = 24000000;
		reg_offset = config_pwm->reg_clk_src_offset;
		reg_shift = config_pwm->reg_clk_src_shift;
		reg_width = config_pwm->reg_clk_src_width;

		temp = sunxi_pwm_readl(chip, reg_offset);
		temp = SET_BITS(reg_shift, reg_width, temp, 0);
		sunxi_pwm_writel(chip, reg_offset, temp);
	}
	pwm_debug("duty_ns=%d period_ns=%d c =%llu.\n", duty_ns, period_ns, c);

	c = c * period_ns;
	do_div(c, 1000000000);
	entire_cycles = (unsigned long)c;

	for (pre_scal_id = 0; pre_scal_id < 9; pre_scal_id++) {
		if (entire_cycles <= 65536)
			break;
		for (prescale = 0; prescale < PRESCALE_MAX+1; prescale++) {
			entire_cycles = (entire_cycles/pre_scal[pre_scal_id][1])
								/(prescale + 1);
			if (entire_cycles <= 65536) {
				div_m = pre_scal[pre_scal_id][0];
				break;
			}
		}
	}

	c = (unsigned long long)entire_cycles * duty_ns;
	do_div(c, period_ns);
	active_cycles = c;
	if (entire_cycles == 0)
		entire_cycles++;

	/* config  clk div_m*/
	reg_offset = config_pwm->reg_clk_div_m_offset;
	reg_shift = config_pwm->reg_clk_div_m_shift;
	reg_width = config_pwm->reg_clk_div_m_width;
	temp = sunxi_pwm_readl(chip, reg_offset);
	temp = SET_BITS(reg_shift, reg_width, temp, div_m);
	sunxi_pwm_writel(chip, reg_offset, temp);

	/* config prescal */
	reg_offset = config_pwm->reg_prescal_offset;
	reg_shift = config_pwm->reg_prescal_shift;
	reg_width = config_pwm->reg_prescal_width;
	temp = sunxi_pwm_readl(chip, reg_offset);
	temp = SET_BITS(reg_shift, reg_width, temp, prescale);
	sunxi_pwm_writel(chip, reg_offset, temp);

	/* config active cycles */
	reg_offset = config_pwm->reg_active_offset;
	reg_shift = config_pwm->reg_active_shift;
	reg_width = config_pwm->reg_active_width;
	temp = sunxi_pwm_readl(chip, reg_offset);
	temp = SET_BITS(reg_shift, reg_width, temp, active_cycles);
	sunxi_pwm_writel(chip, reg_offset, temp);

	/* config period cycles */
	reg_offset = config_pwm->reg_entire_offset;
	reg_shift = config_pwm->reg_entire_shift;
	reg_width = config_pwm->reg_entire_width;
	temp = sunxi_pwm_readl(chip, reg_offset);
	temp = SET_BITS(reg_shift, reg_width, temp, (entire_cycles - 1));

	sunxi_pwm_writel(chip, reg_offset, temp);

	pwm_debug("active_cycles=%lu entire_cycles=%lu prescale=%u div_m=%u\n",
			active_cycles, entire_cycles, prescale, div_m);
	return 0;
}

static int sunxi_pwm_config_enh_dual(struct pwm_chip *chip,
				     struct pwm_device *pwm,
				     int duty_ns, int period_ns, int bind_num)
{
	u32 value[2] = {0};
	unsigned int temp;
	unsigned long long c = 0, clk = 0, clk_temp = 0;
	unsigned long entire_cycles = 256, active_cycles = 192;
	unsigned int reg_offset[2], reg_shift[2], reg_width[2];
	unsigned int pre_scal_id = 0, div_m = 0, prescale = 0;
	int src_clk_sel = 0;
	int i = 0;
	unsigned int dead_time = 0, duty = 0;
	u32 pre_scal[][2] = {
	/* reg_value  clk_pre_div */
		{0, 1},
		{1, 2},
		{2, 4},
		{3, 8},
		{4, 16},
		{5, 32},
		{6, 64},
		{7, 128},
		{8, 256},
	};
	unsigned int pwm_index[2] = {0};
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);

	pwm_index[0] = pwm->pwm - chip->base;
	pwm_index[1] = bind_num - chip->base;

	/* if duty time < dead time,it is wrong. */
	dead_time = pc->config[pwm_index[0]].dead_time;
	duty = (unsigned int)duty_ns;
	/* judge if the pwm eanble dead zone */
	reg_offset[0] = pc->config[pwm_index[0]].reg_dz_en_offset;
	reg_shift[0] = pc->config[pwm_index[0]].reg_dz_en_shift;
	reg_width[0] = pc->config[pwm_index[0]].reg_dz_en_width;

	value[0] = sunxi_pwm_readl(chip, reg_offset[0]);
	value[0] = SET_BITS(reg_shift[0], reg_width[0], value[0], 1);
	sunxi_pwm_writel(chip, reg_offset[0], value[0]);

	temp = sunxi_pwm_readl(chip, reg_offset[0]);
	temp &=  (1u << reg_shift[0]);
	if (duty < dead_time || temp == 0) {
		pr_err("[PWM]duty time or dead zone error.\n");
		return -EINVAL;
	}

	for (i = 0; i < PWM_BIND_NUM; i++) {
		reg_offset[i] = pc->config[pwm_index[i]].reg_bypass_offset;
		reg_shift[i] = pc->config[pwm_index[i]].reg_bypass_shift;
		reg_width[i] = pc->config[pwm_index[i]].reg_bypass_width;
	}

	if (period_ns > 0 && period_ns <= 10) {
		/* if freq lt 100M, then direct output 100M clock,set by pass*/
		clk = 100000000;
		src_clk_sel = 1;

		/* config the two pwm bypass */
		for (i = 0; i < PWM_BIND_NUM; i++) {
			temp = sunxi_pwm_readl(chip, reg_offset[i]);
			temp = SET_BITS(reg_shift[i], reg_width[i], temp, 1);
			sunxi_pwm_writel(chip, reg_offset[i], temp);

			reg_offset[i] =
				pc->config[pwm_index[i]].reg_clk_src_offset;
			reg_shift[i] =
				pc->config[pwm_index[i]].reg_clk_src_shift;
			reg_width[i] =
				pc->config[pwm_index[i]].reg_clk_src_width;
			temp = sunxi_pwm_readl(chip, reg_offset[i]);
			temp = SET_BITS(reg_shift[i], reg_width[i], temp, 1);
			sunxi_pwm_writel(chip, reg_offset[i], temp);
		}

		return 0;
	} else if (period_ns > 10 && period_ns <= 334) {
		clk = 100000000;
		src_clk_sel = 1;
	} else if (period_ns > 334) {
		/* if freq < 3M, then select 24M clock */
		clk = 24000000;
		src_clk_sel = 0;
	}

	for (i = 0; i < PWM_BIND_NUM; i++) {
		reg_offset[i] = pc->config[pwm_index[i]].reg_clk_src_offset;
		reg_shift[i] = pc->config[pwm_index[i]].reg_clk_src_shift;
		reg_width[i] = pc->config[pwm_index[i]].reg_clk_src_width;

		temp = sunxi_pwm_readl(chip, reg_offset[i]);
		temp = SET_BITS(reg_shift[i], reg_width[i], temp, src_clk_sel);
		sunxi_pwm_writel(chip, reg_offset[i], temp);
	}

	c = clk;
	c *= period_ns;
	do_div(c, 1000000000);
	entire_cycles = (unsigned long)c;

	/* get div_m and prescale,which satisfy:
	 * deat_val <= 256, entire <= 65536
	 */
	for (pre_scal_id = 0; pre_scal_id < 9; pre_scal_id++) {
		for (prescale = 0; prescale < PRESCALE_MAX+1; prescale++) {
			entire_cycles = (entire_cycles/pre_scal[pre_scal_id][1])
								/(prescale + 1);
			clk_temp = clk;
			do_div(clk_temp,
				pre_scal[pre_scal_id][1] * (prescale + 1));
			clk_temp *= dead_time;
			do_div(clk_temp, 1000000000);
			if (entire_cycles <= 65536 && clk_temp <= 256) {
				div_m = pre_scal[pre_scal_id][0];
				break;
			}
		}
		if (entire_cycles <= 65536 && clk_temp <= 256)
			break;
		pr_err("%s:cfg dual err.entire_cycles=%lu,dead_zone_val=%llu",
					__func__, entire_cycles, clk_temp);
			return -EINVAL;
	}

	c = (unsigned long long)entire_cycles * duty_ns;
	do_div(c,  period_ns);
	active_cycles = c;
	if (entire_cycles == 0)
		entire_cycles++;

	/* config  clk div_m*/
	for (i = 0; i < PWM_BIND_NUM; i++) {
		reg_offset[i] = pc->config[pwm_index[i]].reg_clk_div_m_offset;
		reg_shift[i] = pc->config[pwm_index[i]].reg_clk_div_m_shift;
		reg_width[i] = pc->config[pwm_index[i]].reg_clk_div_m_width;
		temp = sunxi_pwm_readl(chip, reg_offset[i]);
		temp = SET_BITS(reg_shift[i], reg_width[i], temp, div_m);
		sunxi_pwm_writel(chip, reg_offset[i], temp);
	}

	/* config prescal */
	for (i = 0; i < PWM_BIND_NUM; i++) {
		reg_offset[i] = pc->config[pwm_index[i]].reg_prescal_offset;
		reg_shift[i] = pc->config[pwm_index[i]].reg_prescal_shift;
		reg_width[i] = pc->config[pwm_index[i]].reg_prescal_width;
		temp = sunxi_pwm_readl(chip, reg_offset[i]);
		temp = SET_BITS(reg_shift[i], reg_width[i], temp, prescale);
		sunxi_pwm_writel(chip, reg_offset[i], temp);
	}

	/* config active cycles */
	for (i = 0; i < PWM_BIND_NUM; i++) {
		reg_offset[i] = pc->config[pwm_index[i]].reg_active_offset;
		reg_shift[i] = pc->config[pwm_index[i]].reg_active_shift;
		reg_width[i] = pc->config[pwm_index[i]].reg_active_width;
		temp = sunxi_pwm_readl(chip, reg_offset[i]);
		temp = SET_BITS(reg_shift[i], reg_width[i],
				temp, active_cycles);
		sunxi_pwm_writel(chip, reg_offset[i], temp);
	}

	/* config period cycles */
	for (i = 0; i < PWM_BIND_NUM; i++) {
		reg_offset[i] = pc->config[pwm_index[i]].reg_entire_offset;
		reg_shift[i] = pc->config[pwm_index[i]].reg_entire_shift;
		reg_width[i] = pc->config[pwm_index[i]].reg_entire_width;
		temp = sunxi_pwm_readl(chip, reg_offset[i]);
		temp = SET_BITS(reg_shift[i], reg_width[i], temp,
				(entire_cycles - 1));
		sunxi_pwm_writel(chip, reg_offset[i], temp);
	}

	pwm_debug("active_cycles=%lu entire_cycles=%lu prescale=%u div_m=%u\n",
			active_cycles, entire_cycles, prescale, div_m);

	/* config dead zone, one config for two pwm */
	reg_offset[0] = pc->config[pwm_index[0]].reg_pdzintv_offset;
	reg_shift[0] = pc->config[pwm_index[0]].reg_pdzintv_shift;
	reg_width[0] = pc->config[pwm_index[0]].reg_pdzintv_width;
	temp = sunxi_pwm_readl(chip, reg_offset[0]);
	temp = SET_BITS(reg_shift[0], reg_width[0], temp,
			(unsigned int)clk_temp);
	sunxi_pwm_writel(chip, reg_offset[0], temp);

	return 0;
}

static int sunxi_pwm_config_enh(struct pwm_chip *chip, struct pwm_device *pwm,
		int duty_ns, int period_ns)
{
	int bind_num;

	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);

	bind_num = pc->config[pwm->pwm - chip->base].bind_pwm;
	if (bind_num == 255)
		sunxi_pwm_config_enh_single(chip, pwm, duty_ns, period_ns);
	else
		sunxi_pwm_config_enh_dual(chip, pwm, duty_ns,
					  period_ns, bind_num);

	return 0;
}

static int sunxi_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
				int duty_ns, int period_ns)
{
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);

	if (pc->sunxi_pwm_config)
		pc->sunxi_pwm_config(chip,
				pwm, duty_ns, period_ns);
		return 0;
}

static int sunxi_pwm_enable_single(struct pwm_chip *chip,
				struct pwm_device *pwm)
{
	unsigned int value = 0, index = 0;
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);
	unsigned int reg_offset, reg_shift;
	struct device_node *sub_np;
	struct platform_device *pwm_pdevice;

	index = pwm->pwm - chip->base;
	sub_np = of_parse_phandle(chip->dev->of_node, "pwms", index);
	if (IS_ERR_OR_NULL(sub_np)) {
		pr_err("%s: can't parse \"pwms\" property\n", __func__);
			return -ENODEV;
	}
	pwm_pdevice = of_find_device_by_node(sub_np);
	if (IS_ERR_OR_NULL(pwm_pdevice)) {
		pr_err("%s: can't parse pwm device\n", __func__);
		return -ENODEV;
	}
	sunxi_pwm_pin_set_state(&pwm_pdevice->dev, PWM_PIN_STATE_ACTIVE);

	/* enable clk for pwm controller */
	reg_offset = pc->config[pwm->pwm - chip->base].reg_clk_gating_offset;
	reg_shift = pc->config[pwm->pwm - chip->base].reg_clk_gating_shift;
	value = sunxi_pwm_readl(chip, reg_offset);
	value = SET_BITS(reg_shift, 1, value, 1);
	sunxi_pwm_writel(chip, reg_offset, value);

	/* enable pwm controller */
	reg_offset = pc->config[pwm->pwm - chip->base].reg_enable_offset;
	reg_shift = pc->config[pwm->pwm - chip->base].reg_enable_shift;
	value = sunxi_pwm_readl(chip, reg_offset);
	value = SET_BITS(reg_shift, 1, value, 1);
	sunxi_pwm_writel(chip, reg_offset, value);

	return 0;
}

unsigned long long sunxi_get_clk_freq(struct pwm_chip *chip, int pwm)
{
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);
	unsigned int reg_offset, reg_shift, reg_width;
	unsigned long long c = 0;
	unsigned int temp;
	unsigned int index = 0;
	u32 pre_scal[][2] = {
		/* reg_value  clk_pre_div */
		{0, 1},
		{1, 2},
		{2, 4},
		{3, 8},
		{4, 16},
		{5, 32},
		{6, 64},
		{7, 128},
		{8, 256},
	};

	index = pwm - chip->base;
	reg_offset = pc->config[index].reg_clk_src_offset;
	reg_shift = pc->config[index].reg_clk_src_shift;
	reg_width = pc->config[index].reg_clk_src_width;

	temp = sunxi_pwm_readl(chip, reg_offset);
	temp = temp >> reg_shift;
	temp = temp & ((1u << reg_width) - 1);
	if (temp == 0)
		c = 24000000;
	else if (temp == 1)
		c = 100000000;
	/* check if clk is bypass*/
	reg_offset = pc->config[index].reg_bypass_offset;
	reg_shift = pc->config[index].reg_bypass_shift;
	reg_width = pc->config[index].reg_bypass_width;
	temp = sunxi_pwm_readl(chip, reg_offset);
	temp = temp >> reg_shift;
	temp = temp & ((1u << reg_width) - 1);
	if (temp == 1)
		return c;

	/* check clk div m */
	reg_offset = pc->config[index].reg_clk_div_m_offset;
	reg_shift = pc->config[index].reg_clk_div_m_shift;
	reg_width = pc->config[index].reg_clk_div_m_width;
	temp = sunxi_pwm_readl(chip, reg_offset);
	temp = temp >> reg_shift;
	temp = temp & ((1u << reg_width) - 1);
	do_div(c, pre_scal[temp][1]);

	/* check clk prescal */
	reg_offset = pc->config[index].reg_prescal_offset;
	reg_shift = pc->config[index].reg_prescal_shift;
	reg_width = pc->config[index].reg_prescal_width;
	temp = sunxi_pwm_readl(chip, reg_offset);
	temp = temp >> reg_shift;
	temp = temp & ((1u << reg_width) - 1);
	do_div(c, temp + 1);

	return c;
}

static int sunxi_pwm_enable_dual(struct pwm_chip *chip,
				struct pwm_device *pwm, int bind_num)
{
	u32 value[2] = {0};
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);
	unsigned int reg_offset[2], reg_shift[2], reg_width[2];
	struct device_node *sub_np[2];
	struct platform_device *pwm_pdevice[2];
	int i = 0;
	unsigned int pwm_index[2] = {0};

	pwm_index[0] = pwm->pwm - chip->base;
	pwm_index[1] = bind_num - chip->base;

	/*set current pwm pin state*/
	sub_np[0] = of_parse_phandle(chip->dev->of_node, "pwms", pwm_index[0]);
	if (IS_ERR_OR_NULL(sub_np[0])) {
		pr_err("%s: can't parse \"pwms\" property\n", __func__);
		return -ENODEV;
	}
	pwm_pdevice[0] = of_find_device_by_node(sub_np[0]);
	if (IS_ERR_OR_NULL(pwm_pdevice[0])) {
		pr_err("%s: can't parse pwm device\n", __func__);
		return -ENODEV;
	}

	/*set bind pwm pin state*/
	sub_np[1] = of_parse_phandle(chip->dev->of_node, "pwms", pwm_index[1]);
	if (IS_ERR_OR_NULL(sub_np[1])) {
		pr_err("%s: can't parse \"pwms\" property\n", __func__);
		return -ENODEV;
	}
	pwm_pdevice[1] = of_find_device_by_node(sub_np[1]);
	if (IS_ERR_OR_NULL(pwm_pdevice[1])) {
		pr_err("%s: can't parse pwm device\n", __func__);
		return -ENODEV;
	}

	sunxi_pwm_pin_set_state(&pwm_pdevice[0]->dev, PWM_PIN_STATE_ACTIVE);
	sunxi_pwm_pin_set_state(&pwm_pdevice[1]->dev, PWM_PIN_STATE_ACTIVE);

	/* enable clk for pwm controller */
	for (i = 0; i < PWM_BIND_NUM; i++) {
		reg_offset[i] = pc->config[pwm_index[i]].reg_clk_gating_offset;
		reg_shift[i] = pc->config[pwm_index[i]].reg_clk_gating_shift;
		reg_width[i] = pc->config[pwm_index[i]].reg_clk_gating_width;
		value[i] = sunxi_pwm_readl(chip, reg_offset[i]);
		value[i] = SET_BITS(reg_shift[i], reg_width[i], value[i], 1);
		sunxi_pwm_writel(chip, reg_offset[i], value[i]);
	}

	/* enable pwm controller */
	for (i = 0; i < PWM_BIND_NUM; i++) {
		reg_offset[i] = pc->config[pwm_index[i]].reg_enable_offset;
		reg_shift[i] = pc->config[pwm_index[i]].reg_enable_shift;
		reg_width[i] = pc->config[pwm_index[i]].reg_enable_width;
		value[i] = sunxi_pwm_readl(chip, reg_offset[i]);
		value[i] = SET_BITS(reg_shift[i], reg_width[i], value[i], 1);
		sunxi_pwm_writel(chip, reg_offset[i], value[i]);
	}

	return 0;

}

static int sunxi_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	int bind_num;
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);

	bind_num = pc->config[pwm->pwm - chip->base].bind_pwm;
	if (bind_num == 255)
		sunxi_pwm_enable_single(chip, pwm);
	else
		sunxi_pwm_enable_dual(chip, pwm, bind_num);

	return 0;
}


static void sunxi_pwm_disable_single(struct pwm_chip *chip,
					struct pwm_device *pwm)
{
	u32 value = 0, index = 0;
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);
	unsigned int reg_offset, reg_shift;
	struct device_node *sub_np;
	struct platform_device *pwm_pdevice;

	index = pwm->pwm - chip->base;
	sub_np = of_parse_phandle(chip->dev->of_node, "pwms", index);
	if (IS_ERR_OR_NULL(sub_np)) {
		pr_err("%s: can't parse \"pwms\" property\n", __func__);
		return;
	}
	pwm_pdevice = of_find_device_by_node(sub_np);
	if (IS_ERR_OR_NULL(pwm_pdevice)) {
		pr_err("%s: can't parse pwm device\n", __func__);
		return;
	}

	/* disable pwm controller */
	reg_offset = pc->config[pwm->pwm - chip->base].reg_enable_offset;
	reg_shift = pc->config[pwm->pwm - chip->base].reg_enable_shift;
	value = sunxi_pwm_readl(chip, reg_offset);
	value = SET_BITS(reg_shift, 1, value, 0);
	sunxi_pwm_writel(chip, reg_offset, value);

	/* disable pwm controller */
	reg_offset = pc->config[pwm->pwm - chip->base].reg_clk_gating_offset;
	reg_shift = pc->config[pwm->pwm - chip->base].reg_clk_gating_shift;
	value = sunxi_pwm_readl(chip, reg_offset);
	value = SET_BITS(reg_shift, 1, value, 0);
	sunxi_pwm_writel(chip, reg_offset, value);

	sunxi_pwm_pin_set_state(&pwm_pdevice->dev, PWM_PIN_STATE_SLEEP);
}

static void sunxi_pwm_disable_dual(struct pwm_chip *chip,
				struct pwm_device *pwm, int bind_num)
{
	u32 value[2] = {0};
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);
	unsigned int reg_offset[2], reg_shift[2], reg_width[2];
	struct device_node *sub_np[2];
	struct platform_device *pwm_pdevice[2];
	int i = 0;
	unsigned int pwm_index[2] = {0};

	pwm_index[0] = pwm->pwm - chip->base;
	pwm_index[1] = bind_num - chip->base;

	/* get current index pwm device */
	sub_np[0] = of_parse_phandle(chip->dev->of_node, "pwms", pwm_index[0]);
	if (IS_ERR_OR_NULL(sub_np[0])) {
		pr_err("%s: can't parse \"pwms\" property\n", __func__);
		return;
	}
	pwm_pdevice[0] = of_find_device_by_node(sub_np[0]);
	if (IS_ERR_OR_NULL(pwm_pdevice[0])) {
		pr_err("%s: can't parse pwm device\n", __func__);
		return;
	}
	/* get bind pwm device */
	sub_np[1] = of_parse_phandle(chip->dev->of_node, "pwms", pwm_index[1]);
	if (IS_ERR_OR_NULL(sub_np[1])) {
		pr_err("%s: can't parse \"pwms\" property\n", __func__);
		return;
	}
	pwm_pdevice[1] = of_find_device_by_node(sub_np[1]);
	if (IS_ERR_OR_NULL(pwm_pdevice[1])) {
		pr_err("%s: can't parse pwm device\n", __func__);
		return;
	}

	/* disable pwm controller */
	for (i = 0; i < PWM_BIND_NUM; i++) {
		reg_offset[i] = pc->config[pwm_index[i]].reg_enable_offset;
		reg_shift[i] = pc->config[pwm_index[i]].reg_enable_shift;
		reg_width[i] = pc->config[pwm_index[i]].reg_enable_width;
		value[i] = sunxi_pwm_readl(chip, reg_offset[i]);
		value[i] = SET_BITS(reg_shift[i], reg_width[i], value[i], 0);
		sunxi_pwm_writel(chip, reg_offset[i], value[i]);
	}

	/* disable pwm clk gating */
	for (i = 0; i < PWM_BIND_NUM; i++) {
		reg_offset[i] = pc->config[pwm_index[i]].reg_clk_gating_offset;
		reg_shift[i] = pc->config[pwm_index[i]].reg_clk_gating_shift;
		reg_width[i] = pc->config[pwm_index[i]].reg_clk_gating_width;
		value[i] = sunxi_pwm_readl(chip, reg_offset[i]);
		value[i] = SET_BITS(reg_shift[i], reg_width[i], value[i], 0);
		sunxi_pwm_writel(chip, reg_offset[i], value[i]);
	}

	/* disable pwm dead zone,one for the two pwm */
	reg_offset[0] = pc->config[pwm->pwm - chip->base].reg_dz_en_offset;
	reg_shift[0] = pc->config[pwm->pwm - chip->base].reg_dz_en_shift;
	reg_width[0] = pc->config[pwm->pwm - chip->base].reg_dz_en_width;
	value[0] = sunxi_pwm_readl(chip, reg_offset[0]);
	value[0] = SET_BITS(reg_shift[0], reg_width[0], value[0], 0);
	sunxi_pwm_writel(chip, reg_offset[0], value[0]);

	/* config pin sleep */
	sunxi_pwm_pin_set_state(&pwm_pdevice[0]->dev, PWM_PIN_STATE_SLEEP);
	sunxi_pwm_pin_set_state(&pwm_pdevice[1]->dev, PWM_PIN_STATE_SLEEP);
}

static void sunxi_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	int bind_num;
	struct sunxi_pwm_chip *pc = to_sunxi_pwm_chip(chip);

	bind_num = pc->config[pwm->pwm - chip->base].bind_pwm;
	if (bind_num == 255)
		sunxi_pwm_disable_single(chip, pwm);
	else
		sunxi_pwm_disable_dual(chip, pwm, bind_num);
}


static struct pwm_ops sunxi_pwm_ops = {
	.config = sunxi_pwm_config,
	.enable = sunxi_pwm_enable,
	.disable = sunxi_pwm_disable,
	.set_polarity = sunxi_pwm_set_polarity,
	.owner = THIS_MODULE,
};

static int sunxi_pwm_probe(struct platform_device *pdev)
{
	int ret;
	struct sunxi_pwm_chip *pwm;
	struct device_node *np = pdev->dev.of_node;
	int i;
	struct platform_device *pwm_pdevice;
	struct device_node *sub_np;
	const struct of_device_id *of_id;

	of_id = of_match_device(sunxi_pwm_match, &pdev->dev);
	if (!of_id) {
		dev_err(&pdev->dev, "Unable to setup pwm data\n");
		return -ENODEV;
	}

	pwm = devm_kzalloc(&pdev->dev, sizeof(*pwm), GFP_KERNEL);
	if (!pwm) {
		ret = -EINVAL;
		dev_err(&pdev->dev, "failed to allocate memory!\n");
		return ret;
	}

	pwm = (struct sunxi_pwm_chip *)of_id->data;

	/* io map pwm base */
	pwm->base = (void __iomem *)of_iomap(pdev->dev.of_node, 0);
	if (!pwm->base) {
		dev_err(&pdev->dev, "unable to map pwm registers\n");
		ret = -EINVAL;
		goto err_iomap;
	}

	/* read property pwm-number */
	ret = of_property_read_u32(np, "pwm-number", &pwm->chip.npwm);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get pwm number: %d, force to one!\n", ret);
		/* force to one pwm if read property fail */
		pwm->chip.npwm = 1;
	}

	/* read property pwm-base */
	ret = of_property_read_u32(np, "pwm-base", &pwm->chip.base);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get pwm-base: %d, force to -1 !\n", ret);
		/* force to one pwm if read property fail */
		pwm->chip.base = -1;
	}
	pwm->chip.dev = &pdev->dev;
	pwm->chip.ops = &sunxi_pwm_ops;

	/* add pwm chip to pwm-core */
	ret = pwmchip_add(&pwm->chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "pwmchip_add() failed: %d\n", ret);
		goto err_add;
	}
	platform_set_drvdata(pdev, pwm);

	pwm->config = devm_kzalloc(&pdev->dev,
			sizeof(*pwm->config) * pwm->chip.npwm, GFP_KERNEL);
	if (!pwm->config) {
		ret = -ENOMEM;
		dev_err(&pdev->dev, "failed to allocate memory!\n");
		goto err_alloc;
	}

	pwm->config = devm_kzalloc(&pdev->dev,
			sizeof(*pwm->config) * pwm->chip.npwm, GFP_KERNEL);

	if (!pwm->config) {
		ret = -ENOMEM;
		dev_err(&pdev->dev, "failed to allocate memory!\n");
		goto err_alloc;
	}

	for (i = 0; i < pwm->chip.npwm; i++) {
		sub_np = of_parse_phandle(np, "pwms", i);
		if (IS_ERR_OR_NULL(sub_np)) {
			pr_err("%s: can't parse \"pwms\" property\n", __func__);
			return -EINVAL;
		}

		pwm_pdevice = of_find_device_by_node(sub_np);
		if (pwm->sunxi_pwm_get_config) {
			ret = pwm->sunxi_pwm_get_config(pwm_pdevice,
						&pwm->config[i]);
			if (ret)
				goto err_get_config;
		}
	}
#if defined(CLK_GATE_SUPPORT)
	pwm->pwm_clk = of_clk_get(pdev->dev.of_node, 0);
	if (IS_ERR_OR_NULL(pwm->pwm_clk)) {
		pr_err("%s: can't get pwm clk\n", __func__);
		return -EINVAL;
	}
	clk_prepare_enable(pwm->pwm_clk);
#endif
	return 0;

err_get_config:
err_alloc:
	pwmchip_remove(&pwm->chip);
err_add:
	iounmap(pwm->base);
err_iomap:
	return ret;
}

static int sunxi_pwm_remove(struct platform_device *pdev)
{
	struct sunxi_pwm_chip *pwm = platform_get_drvdata(pdev);
#if defined CLK_GATE_SUPPORT
	clk_disable(pwm->pwm_clk);
#endif
	return pwmchip_remove(&pwm->chip);
}

static int sunxi_pwm_suspend(struct platform_device *pdev, pm_message_t state)
{
	return 0;
}

static int sunxi_pwm_resume(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver sunxi_pwm_driver = {
	.probe = sunxi_pwm_probe,
	.remove = sunxi_pwm_remove,
	.suspend = sunxi_pwm_suspend,
	.resume = sunxi_pwm_resume,
	.driver = {
		.name = "sunxi_pwm",
		.owner  = THIS_MODULE,
		.of_match_table = sunxi_pwm_match,
	 },
};

static int __init pwm_module_init(void)
{
	int ret = 0;

	pr_info("pwm module init!\n");

#if !defined(CONFIG_OF)
	ret = platform_device_register(&sunxi_pwm_device);
#endif
	if (ret == 0)
		ret = platform_driver_register(&sunxi_pwm_driver);

	return ret;
}

static void __exit pwm_module_exit(void)
{
	pr_info("pwm module exit!\n");

	platform_driver_unregister(&sunxi_pwm_driver);
#if !defined(CONFIG_OF)
	platform_device_unregister(&sunxi_pwm_device);
#endif
}

subsys_initcall(pwm_module_init);
module_exit(pwm_module_exit);

MODULE_AUTHOR("zengqi");
MODULE_AUTHOR("liuli");
MODULE_DESCRIPTION("pwm driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:sunxi-pwm");
