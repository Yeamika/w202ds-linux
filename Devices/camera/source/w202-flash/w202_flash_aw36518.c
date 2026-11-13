// SPDX-License-Identifier: GPL-2.0
/*
 * AW36518A torch LED on the ZTE W202DS rear camera.
 *
 * Register sequence follows the stock flash_ic_aw36518a_gpio.ko
 * (P720P01 BSP): init writes 0x07=0x89 and clears 0x01; torch-on writes
 * 0x69=0x02, 0x31=0x0c, the torch current to 0x05, ENABLE 0x01=0x13
 * (LED on, TORCH pin mode) and raises flash-torch-en-gpios; torch-off drops
 * the pin and clears 0x01. Without the pin, ENABLE 0x01=0x0b selects
 * I2C torch mode, as the stock driver does.
 *
 * Brightness 1..max maps linearly onto torch codes up to the stock level
 * (0x4a). Flash/preflash operation and thermal regulation are not implemented.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>

#define AW_REG_ENABLE		0x01
#define AW_REG_TORCH_CUR	0x05
#define AW_REG_TIMING		0x07
#define AW_REG_CHIP_ID		0x0c
#define AW_REG_VENDOR_31	0x31
#define AW_REG_VENDOR_69	0x69

#define AW_ENABLE_OFF		0x00
#define AW_ENABLE_TORCH_PIN	0x13
#define AW_ENABLE_TORCH_I2C	0x0b
#define AW_TIMING_STOCK		0x89
#define AW_TORCH_CODE_STOCK	0x4a
#define AW_TORCH_LEVELS		8

struct aw36518 {
	struct i2c_client *client;
	struct gpio_desc *torch_gpio;
	struct led_classdev cdev;
	struct mutex lock;	/* serialises register sequences */
};

static int aw_write(struct aw36518 *aw, u8 reg, u8 val)
{
	int ret = i2c_smbus_write_byte_data(aw->client, reg, val);

	if (ret)
		dev_err(&aw->client->dev, "write 0x%02x=0x%02x: %d\n", reg, val, ret);
	return ret;
}

static int aw_torch_off(struct aw36518 *aw)
{
	if (aw->torch_gpio)
		gpiod_set_value_cansleep(aw->torch_gpio, 0);
	return aw_write(aw, AW_REG_ENABLE, AW_ENABLE_OFF);
}

static int aw_torch_on(struct aw36518 *aw, enum led_brightness level)
{
	u8 code = DIV_ROUND_UP(AW_TORCH_CODE_STOCK * level, AW_TORCH_LEVELS);
	int ret;

	ret = aw_write(aw, AW_REG_VENDOR_69, 0x02);
	ret = ret ?: aw_write(aw, AW_REG_VENDOR_31, 0x0c);
	ret = ret ?: aw_write(aw, AW_REG_TORCH_CUR, code);
	ret = ret ?: aw_write(aw, AW_REG_ENABLE, aw->torch_gpio ?
			      AW_ENABLE_TORCH_PIN : AW_ENABLE_TORCH_I2C);
	if (ret) {
		/* Also stop a previously lit torch if a brightness update fails. */
		aw_torch_off(aw);
		return ret;
	}
	if (aw->torch_gpio)
		gpiod_set_value_cansleep(aw->torch_gpio, 1);
	return 0;
}

static int aw_brightness_set(struct led_classdev *cdev,
			     enum led_brightness level)
{
	struct aw36518 *aw = container_of(cdev, struct aw36518, cdev);
	int ret;

	mutex_lock(&aw->lock);
	ret = level ? aw_torch_on(aw, level) : aw_torch_off(aw);
	mutex_unlock(&aw->lock);
	return ret;
}

static int aw_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw36518 *aw;
	int id, ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return dev_err_probe(dev, -EOPNOTSUPP, "SMBus byte data required\n");

	aw = devm_kzalloc(dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;
	aw->client = client;
	mutex_init(&aw->lock);
	i2c_set_clientdata(client, aw);

	aw->torch_gpio = devm_gpiod_get_optional(dev, "flash-torch-en",
						 GPIOD_OUT_LOW);
	if (IS_ERR(aw->torch_gpio))
		return dev_err_probe(dev, PTR_ERR(aw->torch_gpio),
				     "torch enable gpio\n");

	usleep_range(2000, 3000);
	id = i2c_smbus_read_byte_data(client, AW_REG_CHIP_ID);
	if (id < 0)
		return dev_err_probe(dev, id, "chip id read\n");

	ret = aw_write(aw, AW_REG_TIMING, AW_TIMING_STOCK);
	ret = ret ?: aw_torch_off(aw);
	if (ret)
		return ret;

	aw->cdev.name = "white:torch";
	aw->cdev.max_brightness = AW_TORCH_LEVELS;
	aw->cdev.brightness_set_blocking = aw_brightness_set;
	aw->cdev.flags = LED_CORE_SUSPENDRESUME;

	ret = devm_led_classdev_register(dev, &aw->cdev);
	if (ret)
		return ret;

	dev_info(dev, "AW36518A id 0x%02x, torch %s mode\n", id,
		 aw->torch_gpio ? "pin" : "i2c");
	return 0;
}

static void aw_shutdown(struct i2c_client *client)
{
	struct aw36518 *aw = i2c_get_clientdata(client);

	mutex_lock(&aw->lock);
	aw_torch_off(aw);
	mutex_unlock(&aw->lock);
}

static void aw_remove(struct i2c_client *client)
{
	aw_shutdown(client);
}

static const struct of_device_id aw_of_match[] = {
	{ .compatible = "sprd,flash-aw36518a_gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw_of_match);

static struct i2c_driver aw_driver = {
	.driver = {
		.name = "w202-flash-aw36518",
		.of_match_table = aw_of_match,
	},
	.probe = aw_probe,
	.remove = aw_remove,
	.shutdown = aw_shutdown,
};
module_i2c_driver(aw_driver);

MODULE_DESCRIPTION("W202DS AW36518A torch LED");
MODULE_LICENSE("GPL");
