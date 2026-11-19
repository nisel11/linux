// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

static const char * const ft8057_regulator_names[] = {
	"vddio",
	"vddpos",
	"vddneg",
};

static const unsigned long ft8057_regulator_enable_loads[] = {
	62000,
	100000,
	100000,
};

struct ft8057_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;

	struct regulator_bulk_data supplies[ARRAY_SIZE(ft8057_regulator_names)];

	struct gpio_desc *reset;
};

static inline struct ft8057_panel *to_ft8057_panel(struct drm_panel *panel)
{
	return container_of(panel, struct ft8057_panel, panel);
}

static const struct drm_display_mode ft8057_mode = {
	.clock = 95000,
	.hdisplay = 720,
	.hsync_start = 720 + 40,
	.hsync_end = 720 + 40 + 14,
	.htotal = 720 + 40 + 14 + 25,
	.vdisplay = 1650,
	.vsync_start = 1650 + 172,
	.vsync_end = 1650 + 172 + 2,
	.vtotal = 1650 + 172 + 2 + 28,
	.width_mm = 68,
	.height_mm = 156,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int ft8057_panel_prepare(struct drm_panel *panel)
{
	struct ft8057_panel *ctx = to_ft8057_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret)
		return ret;

	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(5);
	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(5);
	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(5);
	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(15);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 90);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	if (dsi_ctx.accum_err) {
		dev_err(panel->dev, "failed to initialize panel: %d\n",
			dsi_ctx.accum_err);
		gpiod_set_value_cansleep(ctx->reset, 0);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	}

	return dsi_ctx.accum_err;
}

static int ft8057_panel_unprepare(struct drm_panel *panel)
{
	struct ft8057_panel *ctx = to_ft8057_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	gpiod_set_value_cansleep(ctx->reset, 0);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return 0;
}

static int ft8057_panel_get_modes(struct drm_panel *panel,
				  struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &ft8057_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	connector->display_info.width_mm = ft8057_mode.width_mm;
	connector->display_info.height_mm = ft8057_mode.height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs ft8057_panel_funcs = {
	.prepare = ft8057_panel_prepare,
	.unprepare = ft8057_panel_unprepare,
	.get_modes = ft8057_panel_get_modes,
};

static int ft8057_panel_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ft8057_panel *ctx;
	int i, ret;

	ctx = devm_drm_panel_alloc(dev, struct ft8057_panel, panel,
				   &ft8057_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	for (i = 0; i < ARRAY_SIZE(ctx->supplies); i++) {
		ctx->supplies[i].supply = ft8057_regulator_names[i];
		ctx->supplies[i].init_load_uA = ft8057_regulator_enable_loads[i];
	}

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset),
				     "failed to get reset gpio\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO |
			  MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach to DSI host\n");
	}

	return 0;
}

static void ft8057_panel_remove(struct mipi_dsi_device *dsi)
{
	struct ft8057_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ft8057_panel_of_match[] = {
	{ .compatible = "focaltech,ft8057hd" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ft8057_panel_of_match);

static struct mipi_dsi_driver ft8057_panel_driver = {
	.probe = ft8057_panel_probe,
	.remove = ft8057_panel_remove,
	.driver = {
		.name = "panel-focaltech-ft8057",
		.of_match_table = ft8057_panel_of_match,
	},
};
module_mipi_dsi_driver(ft8057_panel_driver);

MODULE_AUTHOR("Roman Vivchar <rva333@protonmail.com>");
MODULE_DESCRIPTION("FocalTech FT8057 MIPI-DSI panel driver");
MODULE_LICENSE("GPL");
