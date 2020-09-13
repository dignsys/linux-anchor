/*
 * MIPI-DSI based ADV-TT80WX LCD panel driver.
 *
 * Copyright (c) 2017 Nexell Co., Ltd
 *
 * Sungwoo Park <swpark@nexell.co.kr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <drm/drmP.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <linux/of_gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>
#include <linux/backlight.h>

#define H_ACTIVE     		800
#define V_ACTIVE     		1280

#define H_FRONT_PORCH		(10+100)
#define H_BACK_PORCH		(30+100)
#define HS_LOW_PULSE_WIDTH	(8)

#define V_FRONT_PORCH		(2+10)
#define V_BACK_PORCH		(2+5)
#define VS_LOW_PULSE_WIDTH	(2+1)

#define VREFRESH		60
#define WIDTH_MM		107
#define HEIGHT_MM		172

#define WRDISBV			0x51
#define DISPOFF			0x28
#define SLPIN			0x10
#define SLPOUT			0x11
#define DISPON			0x29
#define DCSGETID1		0xDA
#define DCSGETID2		0xDB
#define DCSGETID3		0xDC

#define MODULEID1		0x83
#define MODULEID2       0x94
#define MODULEID3       0x0D

#define MAX_BACKLIGHT_BRIGHTNESS 175
#define MIN_BACKLIGHT_BRIGHTNESS 80
#define DFL_BACKLIGHT_BRIGHTNESS 175

#define TEST_CHECK_PANEL_MODULE	0

struct avdtt80wx {
	struct device *dev;
	struct drm_panel panel;

	struct regulator_bulk_data supplies[2];
	int reset_gpio;
	int enable_gpio;
	u32 power_on_delay;
	u32 reset_delay;
	u32 init_delay;
	struct videomode vm;
	u32 width_mm;
	u32 height_mm;
	bool is_power_on;
	int brightness;

	struct backlight_device *bl_dev;
	int error;
};

static void _dcs_write(struct avdtt80wx *ctx, const void *data, size_t len);
static int __maybe_unused _dcs_read(struct avdtt80wx *ctx, u8 cmd, void *data, size_t len);

static inline struct avdtt80wx *panel_to_avdtt80wx(struct drm_panel *panel)
{
	return container_of(panel, struct avdtt80wx, panel);
}

static int __maybe_unused avdtt80wx_clear_error(struct avdtt80wx *ctx)
{
	int ret = ctx->error;

	ctx->error = 0;
	return ret;
}

#if TEST_CHECK_PANEL_MODULE
static bool _check_panel_module(struct avdtt80wx *ctx)
{
	u8 cmd[3] = { DCSGETID1, DCSGETID2, DCSGETID3 };
	u8 id[3] = { MODULEID1, MODULEID2, MODULEID3 };
	u8 val;
	int i;
	int err;

	for (i = 0; i < 3; i++) {
		err = _dcs_read(ctx, cmd[i], &val, 1);
		printk("HSLEE:%s:%02x:%d\n", __FUNCTION__, val, err);
		if (err < 0)
			return false;

		if (val != id[i])
			return false;
	}

	return true;
}
#endif

static void _dcs_write(struct avdtt80wx *ctx, const void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;

	if (ctx->error < 0)
		return;

	ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %zd writing dcs seq: %*ph\n", ret,
			(int)len, data);
		ctx->error = ret;
	}
}

static int __maybe_unused _dcs_read(struct avdtt80wx *ctx, u8 cmd, void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (ctx->error < 0)
		return ctx->error;

	ret = mipi_dsi_dcs_read(dsi, cmd, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %d reading dcs seq(%#x)\n", ret, cmd);
		ctx->error = ret;
	}

	return ret;
}

#ifndef CONFIG_DRM_CHECK_PRE_INIT
#define avdtt80wx_dcs_write_seq(ctx, seq...) \
({ \
	const u8 d[] = { seq }; \
	BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > 64, \
			 "DCS sequence too big for stack"); \
	_dcs_write(ctx, d, ARRAY_SIZE(d)); \
})

#define avdtt80wx_dcs_write_seq_static(ctx, seq...) \
({ \
	static const u8 d[] = { seq }; \
	_dcs_write(ctx, d, ARRAY_SIZE(d)); \
})

static void _set_sequence(struct avdtt80wx *ctx)
{
	/* Datasheet: AVD-TT80WX-CN-046-A.pdf */

	static u8 d[193][8] = {
		{4,	0xFF, 0x98, 0x81, 0x03},
		{2,	0x01, 0x00},
		{2,	0x02, 0x00},
		{2,	0x03, 0x53},
		{2,	0x04, 0x53},
		{2,	0x05, 0x13},
		{2,	0x06, 0x04},
		{2,	0x07, 0x02},
		{2,	0x08, 0x02},
		{2,	0x09, 0x00},
		{2,	0x0a, 0x00},
		{2,	0x0b, 0x00},
		{2,	0x0c, 0x00},
		{2,	0x0d, 0x00},
		{2,	0x0e, 0x00},
		{2,	0x0f, 0x00},
		{2,	0x10, 0x00},
		{2,	0x11, 0x00},
		{2,	0x12, 0x00},
		{2,	0x13, 0x00},
		{2,	0x14, 0x00},
		{2,	0x15, 0x00},
		{2,	0x16, 0x00},
		{2,	0x17, 0x00},
		{2,	0x18, 0x00},
		{2,	0x19, 0x00},
		{2,	0x1a, 0x00},
		{2,	0x1b, 0x00},
		{2,	0x1c, 0x00},
		{2,	0x1d, 0x00},
		{2,	0x1e, 0xc0},
		{2,	0x1f, 0x80},
		{2,	0x20, 0x02},
		{2,	0x21, 0x09},
		{2,	0x22, 0x00},
		{2,	0x23, 0x00},
		{2,	0x24, 0x00},
		{2,	0x25, 0x00},
		{2,	0x26, 0x00},
		{2,	0x27, 0x00},
		{2,	0x28, 0x55},
		{2,	0x29, 0x03},
		{2,	0x2a, 0x00},
		{2,	0x2b, 0x00},
		{2,	0x2c, 0x00},
		{2,	0x2d, 0x00},
		{2,	0x2e, 0x00},
		{2,	0x2f, 0x00},
		{2,	0x30, 0x00},
		{2,	0x31, 0x00},
		{2,	0x32, 0x00},
		{2,	0x33, 0x00},
		{2,	0x34, 0x00},
		{2,	0x35, 0x00},
		{2,	0x36, 0x00},
		{2,	0x37, 0x00},
		{2,	0x38, 0x3C},
		{2,	0x39, 0x00},
		{2,	0x3a, 0x00},
		{2,	0x3b, 0x00},
		{2,	0x3c, 0x00},
		{2,	0x3d, 0x00},
		{2,	0x3e, 0x00},
		{2,	0x3f, 0x00},
		{2,	0x40, 0x00},
		{2,	0x41, 0x00},
		{2,	0x42, 0x00},
		{2,	0x43, 0x00},
		{2,	0x44, 0x00},
		{2,	0x50, 0x01},
		{2,	0x51, 0x23},
		{2,	0x52, 0x45},
		{2,	0x53, 0x67},
		{2,	0x54, 0x89},
		{2,	0x55, 0xab},
		{2,	0x56, 0x01},
		{2,	0x57, 0x23},
		{2,	0x58, 0x45},
		{2,	0x59, 0x67},
		{2,	0x5a, 0x89},
		{2,	0x5b, 0xab},
		{2,	0x5c, 0xcd},
		{2,	0x5d, 0xef},
		{2,	0x5e, 0x01},
		{2,	0x5f, 0x08},       
		{2,	0x60, 0x02},       
		{2,	0x61, 0x02},       
		{2,	0x62, 0x0A},       
		{2,	0x63, 0x15},     
		{2,	0x64, 0x14},      
		{2,	0x65, 0x02},    
		{2,	0x66, 0x11},      
		{2,	0x67, 0x10},    
		{2,	0x68, 0x02},   
		{2,	0x69, 0x0F},   
		{2,	0x6a, 0x0E},       
		{2,	0x6b, 0x02},      
		{2,	0x6c, 0x0D},     
		{2,	0x6d, 0x0C},     
		{2,	0x6e, 0x06},    
		{2,	0x6f, 0x02},      
		{2,	0x70, 0x02},   
		{2,	0x71, 0x02},    
		{2,	0x72, 0x02},      
		{2,	0x73, 0x02},    
		{2,	0x74, 0x02},   
		{2,	0x75, 0x06},      
		{2,	0x76, 0x02},   
		{2,	0x77, 0x02},  
		{2,	0x78, 0x0A},    
		{2,	0x79, 0x15},    
		{2,	0x7a, 0x14},     
		{2,	0x7b, 0x02},     
		{2,	0x7c, 0x10},    
		{2,	0x7d, 0x11},    
		{2,	0x7e, 0x02},     
		{2,	0x7f, 0x0C},    
		{2,	0x80, 0x0D},    
		{2,	0x81, 0x02},    
		{2,	0x82, 0x0E},   
		{2,	0x83, 0x0F},    
		{2,	0x84, 0x08},   
		{2,	0x85, 0x02}, 
		{2,	0x86, 0x02},     
		{2,	0x87, 0x02},   
		{2,	0x88, 0x02},    
		{2,	0x89, 0x02},      
		{2,	0x8A, 0x02},  
		{4,	0xFF, 0x98, 0x81, 0x04}, 
		{2,	0x6C, 0x15},
		{2,	0x6E, 0x30}, 
		{2,	0x6F, 0x33}, 
		{2,	0x8D, 0x1F},
		{2,	0x87, 0xBA},
		{2,	0x26, 0x76},
		{2,	0xB2, 0xD1},
		{2,	0x35, 0x1F},
		{2,	0x33, 0x14},
		{2,	0x3A, 0xA9},
		{2,	0x3B, 0x98},
		{2,	0x38, 0x01},
		{2,	0x39, 0x00},
		{4,	0xFF, 0x98, 0x81, 0x01}, //CMD_Page 1
		{2,	0x22, 0x0A},
		{2,	0x31, 0x00},   
		{2,	0x50, 0xC0},		
		{2,	0x51, 0xC0},	
		{2,	0x53, 0x47},          
		{2,	0x55, 0x7A},        
		{2,	0x60, 0x28},
		{2,	0x2E, 0xC8},
		{2,	0xA0, 0x01},
		{2,	0xA1, 0x10},   
		{2,	0xA2, 0x1B},   
		{2,	0xA3, 0x0C},   
		{2,	0xA4, 0x14},     
		{2,	0xA5, 0x25},
		{2,	0xA6, 0x1A},
		{2,	0xA7, 0x1D},
		{2,	0xA8, 0x68},    
		{2,	0xA9, 0x1B},    
		{2,	0xAA, 0x26},  
		{2,	0xAB, 0x5B},    
		{2,	0xAC, 0x1B},   
		{2,	0xAD, 0x17},    
		{2,	0xAE, 0x4F},      
		{2,	0xAF, 0x24},          
		{2,	0xB0, 0x2A}, 
		{2,	0xB1, 0x4E},   
		{2,	0xB2, 0x5F},     
		{2,	0xB3, 0x39},                                                         
		{2,	0xC0, 0x0F},	
		{2,	0xC1, 0x1B},          
		{2,	0xC2, 0x27},           
		{2,	0xC3, 0x16},         
		{2,	0xC4, 0x14},          
		{2,	0xC5, 0x28},    
		{2,	0xC6, 0x1D},            
		{2,	0xC7, 0x21},       
		{2,	0xC8, 0x6C},       
		{2,	0xC9, 0x1B},          
		{2,	0xCA, 0x26},        
		{2,	0xCB, 0x5B},           
		{2,	0xCC, 0x1B},         
		{2,	0xCD, 0x1B},              
		{2,	0xCE, 0x4F},              
		{2,	0xCF, 0x24},          
		{2,	0xD0, 0x2A},           
		{2,	0xD1, 0x4E},         
		{2,	0xD2, 0x5F},                  
		{2,	0xD3, 0x39}, 
		{4,	0xFF, 0x98, 0x81, 0x00}, 
		{2,	0x35, 0x00}
	};
	int i;
	u8 *pd;

	avdtt80wx_dcs_write_seq(ctx, DISPOFF);
	avdtt80wx_dcs_write_seq(ctx, SLPIN);
	msleep(150);

	for (i = 0; i < 193; ++i)
	{
		pd = &d[i][0];
		_dcs_write(ctx, pd + 1, pd[0]);
	}

	avdtt80wx_dcs_write_seq(ctx, SLPOUT);
	msleep(150);
	avdtt80wx_dcs_write_seq(ctx, DISPON);
}

#endif

static int avdtt80wx_power_on(struct avdtt80wx *ctx)
{
#ifndef CONFIG_DRM_CHECK_PRE_INIT
	int ret;

	if (ctx->is_power_on)
		return 0;

	gpio_direction_output(ctx->reset_gpio, 1);
	gpio_set_value(ctx->reset_gpio, 1);

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		return ret;

	msleep(ctx->power_on_delay);
	gpio_set_value(ctx->reset_gpio, 0);
	msleep(ctx->reset_delay);
	gpio_set_value(ctx->reset_gpio, 1);
	msleep(ctx->init_delay);
#endif
	ctx->is_power_on = true;

	return 0;
}

static int avdtt80wx_power_off(struct avdtt80wx *ctx)
{
	if (!ctx->is_power_on)
		return 0;

/*
	gpio_set_value(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
*/
	if (ctx->bl_dev) {
		ctx->bl_dev->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->bl_dev);
	}

	avdtt80wx_dcs_write_seq(ctx, DISPOFF);

	ctx->is_power_on = false;

	return 0;
}

static int avdtt80wx_enable(struct drm_panel *panel)
{

	struct avdtt80wx *ctx = panel_to_avdtt80wx(panel);

	if (ctx->bl_dev) {
		ctx->bl_dev->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ctx->bl_dev);
	}

	avdtt80wx_dcs_write_seq(ctx, DISPON);

	return 0;
}

static int avdtt80wx_disable(struct drm_panel *panel)
{
	struct avdtt80wx *ctx = panel_to_avdtt80wx(panel);

	if (ctx->bl_dev) {
		ctx->bl_dev->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->bl_dev);
	}

	avdtt80wx_dcs_write_seq(ctx, DISPOFF);

	return 0;
}

static int avdtt80wx_prepare(struct drm_panel *panel)
{
	struct avdtt80wx *ctx = panel_to_avdtt80wx(panel);
	int ret;

	ret = avdtt80wx_power_on(ctx);
	if (ret < 0) {
		dev_err(ctx->dev, "failed to power on\n");
		return ret;
	}

#ifndef CONFIG_DRM_CHECK_PRE_INIT
	_set_sequence(ctx);
	ret = ctx->error;
	if (ret < 0) {
		dev_err(ctx->dev, "failed to set_sequence\n");
		return ret;
	}
#endif


#if TEST_CHECK_PANEL_MODULE
	if (!_check_panel_module(ctx)) {
		return -ENOENT;
	}
#endif

	return 0;
}

static int avdtt80wx_unprepare(struct drm_panel *panel)
{
	struct avdtt80wx *ctx = panel_to_avdtt80wx(panel);

	return avdtt80wx_power_off(ctx);
}

static const struct drm_display_mode default_mode = {
	.clock		= (int)(((H_ACTIVE + H_BACK_PORCH + H_FRONT_PORCH + HS_LOW_PULSE_WIDTH) *
				(V_ACTIVE + V_BACK_PORCH + V_FRONT_PORCH + VS_LOW_PULSE_WIDTH) * VREFRESH) / 1000),

	.hdisplay	= H_ACTIVE,
	.hsync_start	= H_ACTIVE + H_BACK_PORCH, 					/* hactive + hbackporch */
	.hsync_end	= H_ACTIVE + H_BACK_PORCH + HS_LOW_PULSE_WIDTH, 		/* hsync_start + hsyncwidth */
	.htotal		= H_ACTIVE + H_BACK_PORCH + HS_LOW_PULSE_WIDTH + H_FRONT_PORCH, /* hsync_end + hfrontporch */

	.vdisplay	= V_ACTIVE,
	.vsync_start	= V_ACTIVE + V_BACK_PORCH, 					/* vactive + vbackporch */
	.vsync_end	= V_ACTIVE + V_BACK_PORCH + VS_LOW_PULSE_WIDTH, 		/* vsync_start + vsyncwidth */
	.vtotal		= V_ACTIVE + V_BACK_PORCH + VS_LOW_PULSE_WIDTH + V_FRONT_PORCH, /* vsync_end + vfrontporch */

	.vrefresh	= VREFRESH, 							/* Hz */

	.width_mm	= WIDTH_MM,
	.height_mm	= HEIGHT_MM,
};

/**
 * HACK
 * return value
 * 1: success
 * 0: failure
 */
static int avdtt80wx_get_modes(struct drm_panel *panel)
{
	struct drm_connector *connector = panel->connector;
	struct drm_display_mode *mode;

	mode = drm_mode_create(connector->dev);

	if (!mode) {
		DRM_ERROR("failed to create a new display mode\n");
		return 0;
	}

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	drm_mode_set_name(mode);
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs avdtt80wx_drm_funcs = {
	.enable = avdtt80wx_enable,
	.disable = avdtt80wx_disable,
	.prepare = avdtt80wx_prepare,
	.unprepare = avdtt80wx_unprepare,
	.get_modes = avdtt80wx_get_modes,
};

static int avdtt80wx_parse_dt(struct avdtt80wx *ctx)
{
	struct device *dev = ctx->dev;
	struct device_node *np = dev->of_node;

	of_property_read_u32(np, "power-on-delay", &ctx->power_on_delay);
	of_property_read_u32(np, "reset-delay", &ctx->reset_delay);
	of_property_read_u32(np, "init-delay", &ctx->init_delay);

	return 0;
}

static int avdtt80wx_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct avdtt80wx *ctx;
	struct device_node *backlight;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(struct avdtt80wx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;

	ctx->is_power_on = false;
	ctx->brightness = 0;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO
		| MIPI_DSI_MODE_VIDEO_HFP
		| MIPI_DSI_MODE_VIDEO_HBP
		| MIPI_DSI_MODE_VIDEO_HSA
		| MIPI_DSI_MODE_VSYNC_FLUSH
		;

	ret = avdtt80wx_parse_dt(ctx);
	if (ret)
		return ret;

	ctx->supplies[0].supply = "vci";
	ctx->supplies[1].supply = "vdd3";

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		dev_warn(dev, "failed to get regulators: %d\n", ret);

	ctx->reset_gpio = of_get_named_gpio(dev->of_node, "reset-gpio", 0);
	if (ctx->reset_gpio < 0) {
		dev_err(dev, "cannot get reset-gpio %d\n", ctx->reset_gpio);
		return -EINVAL;
	}

	ret = devm_gpio_request(dev, ctx->reset_gpio, "reset-gpio");
	if (ret) {
		dev_err(dev, "failed to request reset-gpio\n");
		return ret;
	}

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		ctx->bl_dev = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (IS_ERR(ctx->bl_dev)) {
			dev_err(dev, "failed to get backlight device\n");
			return PTR_ERR(ctx->bl_dev);
		}

        }

	ctx->width_mm = WIDTH_MM;
	ctx->height_mm = HEIGHT_MM;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &avdtt80wx_drm_funcs;

	ret = drm_panel_add(&ctx->panel);
	if (ret < 0) {
		dev_err(dev, "failed to add panel\n");
		return ret;
	}

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "failed to attach mipi dsi\n");
		drm_panel_remove(&ctx->panel);
	}

	return ret;
}

static int avdtt80wx_remove(struct mipi_dsi_device *dsi)
{
	struct avdtt80wx *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
	avdtt80wx_power_off(ctx);

	return 0;
}

static void avdtt80wx_shutdown(struct mipi_dsi_device *dsi)
{
	struct avdtt80wx *ctx = mipi_dsi_get_drvdata(dsi);

	avdtt80wx_power_off(ctx);
}

static const struct of_device_id avdtt80wx_of_match[] = {
	{ .compatible = "avdtt80wx" },
	{ }
};
MODULE_DEVICE_TABLE(of, avdtt80wx_of_match);

static struct mipi_dsi_driver avdtt80wx_driver = {
	.probe = avdtt80wx_probe,
	.remove = avdtt80wx_remove,
	.shutdown = avdtt80wx_shutdown,
	.driver = {
		.name = "panel-avdtt80wx",
		.of_match_table = avdtt80wx_of_match,
	},
};

module_mipi_dsi_driver(avdtt80wx_driver);

MODULE_DESCRIPTION("MIPI-SDI based avdtt80wx series LCD Panel Driver");
MODULE_AUTHOR("Hackseung Lee <lhs@dignsys.com>");
MODULE_LICENSE("GPL v2");
