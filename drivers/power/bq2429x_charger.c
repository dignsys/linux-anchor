/*
 * BQ24296 battery charger
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 */

#include <linux/module.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/idr.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <asm/unaligned.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/power/bq2429x_charger.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>
#include <linux/of.h>
#include <linux/of_device.h>

struct bq24296_device_info *bq24296_di;
struct bq24296_board *bq24296_pdata;
static int bq24296_int;
int bq24296_mode;
int bq24296_chag_down;

/*
 * Common code for BQ24296 devices read
 */
static int bq24296_i2c_reg8_read(const struct i2c_client *client,
				 const char reg, char *buf, int count)
{
	struct i2c_adapter *adap = client->adapter;
	struct i2c_msg msgs[2];
	int ret;
	char reg_buf = reg;

	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags;
	msgs[0].len = 1;
	msgs[0].buf = &reg_buf;

	msgs[1].addr = client->addr;
	msgs[1].flags = client->flags | I2C_M_RD;
	msgs[1].len = count;
	msgs[1].buf = (char *)buf;

	ret = i2c_transfer(adap, msgs, 2);

	return (ret == 2) ? count : ret;
}
EXPORT_SYMBOL(bq24296_i2c_reg8_read);

static int bq24296_i2c_reg8_write(const struct i2c_client *client,
				  const char reg, const char *buf, int count)
{
	struct i2c_adapter *adap = client->adapter;
	struct i2c_msg msg;
	int ret;
	char *tx_buf = kmalloc(count + 1, GFP_KERNEL);

	if (!tx_buf)
		return -ENOMEM;
	tx_buf[0] = reg;
	memcpy(tx_buf + 1, buf, count);

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.len = count + 1;
	msg.buf = (char *)tx_buf;

	ret = i2c_transfer(adap, &msg, 1);
	kfree(tx_buf);
	return (ret == 1) ? count : ret;
}
EXPORT_SYMBOL(bq24296_i2c_reg8_write);

static int bq24296_read(struct i2c_client *client,
			u8 reg, u8 buf[], unsigned len)
{
	int ret;

	ret = bq24296_i2c_reg8_read(client, reg, buf, len);

	return ret;
}

static int bq24296_write(struct i2c_client *client,
			 u8 reg, u8 const buf[], unsigned len)
{
	int ret;

	ret = bq24296_i2c_reg8_write(client, reg, buf, (int)len);

	return ret;
}

static ssize_t bat_param_read(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct bq24296_device_info *di = bq24296_di;
	int i = 0, ret = 0;
	u8 buffer = 0;

	for (i = 0; i < 11; i++) {
		bq24296_read(di->client, i, &buffer, 1);
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"reg %02d value	%02x\n", i, buffer);
	}

	return ret;
}
DEVICE_ATTR(battparam, 0664, bat_param_read, NULL);

static const struct attribute *bq2429x_attrs[] = {
	&dev_attr_battparam.attr,
	NULL,
};

static int bq24296_update_reg(struct i2c_client *client,
			      int reg, u8 value, u8 mask)
{
	int ret = 0;
	u8 retval = 0;

	ret = bq24296_read(client, reg, &retval, 1);
	if (ret < 0) {
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);
		return ret;
	}

	if ((retval & mask) != value) {
		retval = ((retval & ~mask) | value) | value;
		ret = bq24296_write(client, reg, &retval, 1);
		if (ret < 0) {
			dev_err(&client->dev, "%s: err %d\n", __func__, ret);
			return ret;
		}
	}

	return ret;
}

static int bq24296_init_registers(void)
{
	int ret = 0;

	/* Disable the watchdog */
	ret = bq24296_update_reg(bq24296_di->client,
				 TERMINATION_TIMER_CONTROL_REGISTER,
				 WATCHDOG_DISABLE << WATCHDOG_OFFSET,
				 WATCHDOG_MASK << WATCHDOG_OFFSET);
	if (ret < 0) {
		dev_err(&bq24296_di->client->dev,
			"%s(): Failed to disable the watchdog\n", __func__);
		goto final;
	}

	/* Set Pre-Charge Current Limit as 128mA */
	ret = bq24296_update_reg(bq24296_di->client,
	 PRE_CHARGE_TERMINATION_CURRENT_CONTROL_REGISTER,
	 PRE_CHARGE_CURRENT_LIMIT_128MA << PRE_CHARGE_CURRENT_LIMIT_OFFSET,
	 PRE_CHARGE_CURRENT_LIMIT_MASK << PRE_CHARGE_CURRENT_LIMIT_OFFSET);
	if (ret < 0) {
		dev_err(&bq24296_di->client->dev,
			"%s(): Failed to set pre-charge limit 128mA\n",
				__func__);
		goto final;
	}

	/* Set Termination Current Limit as 128mA */
	ret = bq24296_update_reg(bq24296_di->client,
		 PRE_CHARGE_TERMINATION_CURRENT_CONTROL_REGISTER,
		 TERMINATION_CURRENT_LIMIT_128MA <<
		 TERMINATION_CURRENT_LIMIT_OFFSET,
		 TERMINATION_CURRENT_LIMIT_MASK <<
		 TERMINATION_CURRENT_LIMIT_OFFSET);
	if (ret < 0) {
		dev_err(&bq24296_di->client->dev,
			"%s(): Failed to set termination limit 128mA\n",
			__func__);
		goto final;
	}

final:
	return ret;
}

static int bq24296_get_limit_current(int value)
{
	u8 data;

	if (value < 120)
		data = 0;
	else if (value < 400)
		data = 1;
	else if (value < 700)
		data = 2;
	else if (value < 1000)
		data = 3;
	else if (value < 1200)
		data = 4;
	else if (value < 1600)
		data = 5;
	else if (value < 1800)
		data = 6;
	else
		data = 7;

	return data;
}

static int bq24296_get_chg_current(int value)
{
	u8 data;

	data = value / 64;
	data &= 0xff;

	return data;
}

static int bq24296_update_input_current_limit(u8 value)
{
	int ret = 0;

	ret = bq24296_update_reg(bq24296_di->client,
		 INPUT_SOURCE_CONTROL_REGISTER,
		 ((value << IINLIM_OFFSET) |
		  (EN_HIZ_DISABLE << EN_HIZ_OFFSET)),
		 ((IINLIM_MASK << IINLIM_OFFSET) |
		  (EN_HIZ_MASK << EN_HIZ_OFFSET)));
	if (ret < 0) {
		dev_err(&bq24296_di->client->dev,
			"%s(): Failed to set input current limit (0x%x)\n",
				__func__, value);
	}

	return ret;
}

static int bq24296_set_charge_current(u8 value)
{
	int ret = 0;

	ret = bq24296_update_reg(bq24296_di->client,
				 CHARGE_CURRENT_CONTROL_REGISTER,
				 value << CHARGE_CURRENT_OFFSET,
				 CHARGE_CURRENT_MASK << CHARGE_CURRENT_OFFSET);
	if (ret < 0) {
		dev_err(&bq24296_di->client->dev,
			"%s(): Failed to set charge current limit (0x%x)\n",
			__func__, value);
	}

	return ret;
}

static int bq24296_update_en_hiz_disable(void)
{
	int ret = 0;

	ret = bq24296_update_reg(bq24296_di->client,
				 INPUT_SOURCE_CONTROL_REGISTER,
				 EN_HIZ_DISABLE << EN_HIZ_OFFSET,
				 EN_HIZ_MASK << EN_HIZ_OFFSET);
	if (ret < 0) {
		dev_err(&bq24296_di->client->dev,
			"%s(): Failed to set en_hiz_disable\n", __func__);
	}
	return ret;
}

static int bq24296_update_charge_mode(u8 value)
{
	int ret = 0;

	ret = bq24296_update_reg(bq24296_di->client,
		 POWE_ON_CONFIGURATION_REGISTER,
		 value << CHARGE_MODE_CONFIG_OFFSET,
		 CHARGE_MODE_CONFIG_MASK << CHARGE_MODE_CONFIG_OFFSET);
	if (ret < 0) {
		dev_err(&bq24296_di->client->dev,
			"%s(): Failed to set charge mode(0x%x)\n",
			__func__, value);
	}

	return ret;
}

static int bq24296_update_vsys_min(u8 value)
{
	int ret = 0;

	ret = bq24296_update_reg(bq24296_di->client,
		 POWE_ON_CONFIGURATION_REGISTER,
		 value << VSYSMIN_OFFSET,
		 VSYSMIN_MASK << VSYSMIN_OFFSET);
	if (ret < 0) {
		dev_err(&bq24296_di->client->dev,
			"%s(): Failed to set VSYS_MIN(0x%x)\n",
			__func__, value);
	}

	return ret;
}

static int bq24296_update_otg_mode_current(u8 value)
{
	int ret = 0;

	ret = bq24296_update_reg(bq24296_di->client,
				 POWE_ON_CONFIGURATION_REGISTER,
				 value << OTG_MODE_CURRENT_CONFIG_OFFSET,
				 OTG_MODE_CURRENT_CONFIG_MASK <<
				 OTG_MODE_CURRENT_CONFIG_OFFSET);
	if (ret < 0) {
		dev_err(&bq24296_di->client->dev,
			"%s(): Failed to set otg current mode(0x%x)\n",
			__func__, value);
	}
	return ret;
}

static int bq24296_charge_mode_config(int on)
{
	if (!bq24296_int)
		return 0;

	if (1 == on) {
		bq24296_update_en_hiz_disable();
		mdelay(5);

		bq24296_update_charge_mode(CHARGE_MODE_CONFIG_OTG_OUTPUT);
		mdelay(10);

		bq24296_update_otg_mode_current(
				OTG_MODE_CURRENT_CONFIG_1300MA);
	} else
		bq24296_update_charge_mode(CHARGE_MODE_CONFIG_CHARGE_BATTERY);

	return 0;
}

static int previous_online;

static void usb_detect_work_func(struct work_struct *work)
{
	struct delayed_work *delayed_work =
		(struct delayed_work *)container_of(work,
						    struct delayed_work, work);
	struct bq24296_device_info *pi =
		(struct bq24296_device_info *)container_of(delayed_work,
				struct bq24296_device_info, usb_detect_work);
	u8 retval = 0;
	int ret;

	ret = bq24296_read(bq24296_di->client, 0x08, &retval, 1);
	if (ret < 0) {
		dev_err(&bq24296_di->client->dev, "%s: err %d\n", __func__,
				ret);
	}

	if (((retval >> VBUS_OFFSET) & VBUS_MASK) == 3)
		bq24296_chag_down = 1;
	else
		bq24296_chag_down = 0;

	dev_dbg(pi->dev, "retval = %08x bq24296_chag_down = %d\n", retval,
			bq24296_chag_down);

	mutex_lock(&pi->var_lock);

	if (gpio_is_valid(bq24296_pdata->dc_det_pin)) {
		ret = gpio_request(bq24296_pdata->dc_det_pin,
				"bq24296_dc_det");
		if (ret < 0) {
			dev_dbg(pi->dev,
				"Failed to request gpio %d with ret:%d\n",
				bq24296_pdata->dc_det_pin, ret);
		}

		gpio_direction_input(bq24296_pdata->dc_det_pin);
		ret = gpio_get_value(bq24296_pdata->dc_det_pin);
		if (ret == 0) {
			dev_dbg(pi->dev,
				"bq24296_update_input_current_limit = %d\n",
				bq24296_di->adp_input_current);
			bq24296_update_input_current_limit(
					bq24296_di->adp_input_current);
			bq24296_set_charge_current(CHARGE_CURRENT_2048MA);
			bq24296_charge_mode_config(0);
		} else {
			dev_dbg(pi->dev,
				"bq24296_update_input_current_limit = %d\n",
				IINLIM_500MA);
			bq24296_update_input_current_limit(IINLIM_500MA);
			bq24296_set_charge_current(CHARGE_CURRENT_512MA);
		}
		gpio_free(bq24296_pdata->dc_det_pin);
		dev_dbg(pi->dev, "bq24296_di->dc_det_pin=%x\n", ret);
	} else {
		/* detect VBUS being available */
		if (ret && !previous_online) { /* VBUS became available */
			dev_dbg(pi->dev, "VBUS became available\n");
			bq24296_update_input_current_limit(
					bq24296_di->usb_input_current);
		}
		previous_online = ret;
	}

	mutex_unlock(&pi->var_lock);
	schedule_delayed_work(&pi->usb_detect_work, HZ);
}

static void irq_work_func(struct work_struct *work)
{
}

static irqreturn_t chg_irq_func(int irq, void *dev_id)
{
	return IRQ_HANDLED;
}

#ifdef CONFIG_OF
static struct bq24296_board *bq24296_parse_dt(struct bq24296_device_info *di)
{
	struct bq24296_board *pdata;
	struct device_node *bq24296_np;

	bq24296_np = of_node_get(di->dev->of_node);
	if (!bq24296_np) {
		dev_err(di->dev, "could not find bq24296-node\n");
		return NULL;
	}

	pdata = devm_kzalloc(di->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return NULL;

	if (of_property_read_u32_array(bq24296_np, "ti,chg_current",
				       pdata->chg_current, 3)) {
		dev_err(di->dev, "charge current not specified\n");
		return NULL;
	}

	pdata->chg_irq_pin = of_get_named_gpio(bq24296_np, "gpios", 0);
	if (!gpio_is_valid(pdata->chg_irq_pin)) {
		dev_dbg(di->dev, "invalid gpio: %s/%d chg_irq_pin(%d)\n",
		       __func__, __LINE__, pdata->chg_irq_pin);
	}

	pdata->dc_det_pin = of_get_named_gpio(bq24296_np, "gpios", 1);
	if (!gpio_is_valid(pdata->dc_det_pin)) {
		dev_dbg(di->dev, "invalid gpio: %s/%d dc_det_pin(%d)\n",
		       __func__, __LINE__, pdata->dc_det_pin);
	}

	dev_dbg(di->dev, "gpio: %s/%d dc_det_pin(%d)\n", __func__, __LINE__,
			pdata->dc_det_pin);

	return pdata;
}

static const struct of_device_id bq24296_battery_of_match[] = {
	{ .compatible = "ti,bq24296"},
	{ .compatible = "ti,bq24297"},
	{ },
};
MODULE_DEVICE_TABLE(of, bq24296_battery_of_match);
#endif

#ifdef CONFIG_PM
static int bq24296_battery_suspend(struct device *dev)
{
	if (gpio_is_valid(bq24296_pdata->dc_det_pin))
		cancel_delayed_work_sync(&bq24296_di->usb_detect_work);

	return 0;
}

static int bq24296_battery_resume(struct device *dev)
{
	if (gpio_is_valid(bq24296_pdata->dc_det_pin))
		schedule_delayed_work(&bq24296_di->usb_detect_work,
			msecs_to_jiffies(50));
	return 0;
}

static const struct dev_pm_ops bq24296_charger_pm_ops = {
	.suspend = bq24296_battery_suspend,
	.resume = bq24296_battery_resume,
};
#endif

static int bq24296_battery_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct bq24296_device_info *di;
	u8 retval = 0;
	struct bq24296_board *pdev;
	struct device_node *bq24296_node;
	int ret = -EINVAL;

	dev_dbg(&client->dev, "%s\n", __func__);

	bq24296_node = of_node_get(client->dev.of_node);
	if (!bq24296_node) {
		dev_err(&client->dev, "could not find bq24296-node\n");
		ret = -ENXIO;
		goto batt_failed_2;
	}

	di = devm_kzalloc(&client->dev, sizeof(*di), GFP_KERNEL);
	if (!di) {
		ret = -ENOMEM;
		goto batt_failed_2;
	}

	dev_dbg(&client->dev, "bq24296 node : %s\n", bq24296_node->name);

	i2c_set_clientdata(client, di);
	di->dev = &client->dev;
	di->client = client;

	pdev = bq24296_parse_dt(di);
	if (!pdev) {
		dev_err(&client->dev, "failed to get platform data\n");
		return -EPROBE_DEFER;
	}

	bq24296_pdata = pdev;

	dev_dbg(di->dev,
		"chg_current=%d usb_input_current=%d adp_input_current=%d\n",
		pdev->chg_current[0], pdev->chg_current[1],
		pdev->chg_current[2]);

	/* get/set current */
	if (pdev->chg_current[0] && pdev->chg_current[1] &&
			pdev->chg_current[2]){
		di->chg_current =
			bq24296_get_chg_current(pdev->chg_current[0]);
		di->usb_input_current =
			bq24296_get_limit_current(pdev->chg_current[1]);
		di->adp_input_current  =
			bq24296_get_limit_current(pdev->chg_current[2]);
	} else {
		di->chg_current = bq24296_get_chg_current(1000);
		di->usb_input_current  = bq24296_get_limit_current(500);
		di->adp_input_current  = bq24296_get_limit_current(2000);
	}

	bq24296_di = di;
	/* get the vendor id */
	ret = bq24296_read(di->client, VENDOR_STATS_REGISTER, &retval, 1);
	if (ret < 0) {
		dev_err(di->dev, "Failed in reading register 0x%02x\n",
				VENDOR_STATS_REGISTER);
		goto batt_failed_2;
	}
	di->workqueue = create_singlethread_workqueue("bq24296_irq");
	INIT_WORK(&di->irq_work, irq_work_func);
	mutex_init(&di->var_lock);

	/* usb_detect_work is valid when we support usb changing and dc_det_pin */
	if (gpio_is_valid(bq24296_pdata->dc_det_pin)) {
		INIT_DELAYED_WORK(&di->usb_detect_work, usb_detect_work_func);
		schedule_delayed_work(&di->usb_detect_work, 0);
	}

	bq24296_init_registers();

	/* setting input current limit to 3A for DC charging */
	bq24296_update_input_current_limit(IINLIM_3000MA);

	/* Disable chagrer */
	bq24296_update_charge_mode(CHARGE_MODE_CONFIG_CHARGE_DISABLE);

	/* Set VSYS_MIN to 3.7V */
	bq24296_update_vsys_min(VSYSMIN_3P7);

	if (gpio_is_valid(pdev->chg_irq_pin)) {
		dev_info(di->dev, "chg irq pin : %d\n", pdev->chg_irq_pin);
		pdev->chg_irq = gpio_to_irq(pdev->chg_irq_pin);
		ret = request_threaded_irq(pdev->chg_irq, NULL, chg_irq_func,
					   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					   "bq24296_chg_irq", di);
		if (ret) {
			ret = -EINVAL;
			dev_err(di->dev,
					"failed to request bq24296_chg_irq\n");
			goto err_chgirq_failed;
		}
	} else
		dev_dbg(di->dev, "invalid gpio pin(%d)\n", pdev->chg_irq_pin);

	bq24296_int = 1;

	ret = sysfs_create_files(&client->dev.kobj, bq2429x_attrs);
	if (ret)
		dev_err(di->dev, "bq24296 create sysfs failed.\n");

	dev_info(di->dev, "bq24296_battery_probe ok\n");
	return 0;

err_chgirq_failed:
	cancel_delayed_work_sync(&di->usb_detect_work);
	destroy_workqueue(di->workqueue);
batt_failed_2:
	return ret;
}

static int bq24296_battery_remove(struct i2c_client *client)
{
	struct bq24296_device_info *di = i2c_get_clientdata(client);

	if (bq24296_pdata->chg_irq)
		free_irq(bq24296_pdata->chg_irq, di);

	sysfs_remove_files(&client->dev.kobj, bq2429x_attrs);
	cancel_delayed_work_sync(&di->usb_detect_work);
	destroy_workqueue(di->workqueue);

	return 0;
}

static const struct i2c_device_id bq24296_id[] = {
	{ "bq24296", 0 },
	{ "bq24297", 1 },
	{ }
};

MODULE_ALIAS("i2c:bq2429x");

static struct i2c_driver bq24296_battery_driver = {
	.driver = {
		.name = "bq2429x_charger",
		.owner = THIS_MODULE,
#ifdef CONFIG_PM
		.pm = &bq24296_charger_pm_ops,
#endif
#ifdef CONFIG_OF
		.of_match_table = of_match_ptr(bq24296_battery_of_match),
#endif
	},
	.probe = bq24296_battery_probe,
	.remove = bq24296_battery_remove,
	.id_table = bq24296_id,
};

static int __init bq24296_battery_init(void)
{
	int ret;

	ret = i2c_add_driver(&bq24296_battery_driver);
	if (ret)
		pr_err("Unable to register BQ24296 driver\n");

	return ret;
}
subsys_initcall(bq24296_battery_init);

static void __exit bq24296_battery_exit(void)
{
	i2c_del_driver(&bq24296_battery_driver);
}
module_exit(bq24296_battery_exit);

MODULE_AUTHOR("Rockchip");
MODULE_DESCRIPTION("BQ24296 battery monitor driver");
MODULE_LICENSE("GPL");
