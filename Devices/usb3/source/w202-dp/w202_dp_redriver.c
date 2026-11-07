// SPDX-License-Identifier: GPL-2.0
/*
 * W200DS USB-C redrivers, driven over SMBus on their existing (driverless) DT i2c clients:
 *  - PI3DPX1217 @ i2c-2 0x52: vendor drivers/usb/phy/pidpx1217-i2c.c set_operating_mode() and
 *    restore_default_value(), as called by the shipped sprd-drm.ko dp_detect()/sprd_dp_pd_event().
 *  - PTN38003A @ i2c-6 0x30: vendor ptn38003a-i2c.c mode_usb32_set(1) + dp_mode_set(1) +
 *    dp_link_rate_set(HBR2) + dp_lane_set(2) (ssphy init and dptx_link_training in stock), and
 *    rate/lanes = 0 on unplug (dptx_threaded_irq).
 * Neither chip ACKs in the current kernel; they are only touched after PD_DPU_DP is up and a NACK
 * on the ID register marks the chip absent until the next power-up.
 */

#include <linux/i2c.h>
#include <linux/of.h>

#include "w202_dp.h"

#define PI3DPX_PATH		"/soc/ap-ahb/i2c@2260000/pidpx1217_redriver@52"
#define PTN_PATH		"/soc/ap-ahb/i2c@22a0000/btn38003a_redriver@30"

#define PI3DPX_VENDOR_ID	0x00
#define PI3DPX_DEVICE_ID	0x01
#define PI3DPX_MODE		0x03
#define PI3DPX_HPD		0x04
#define PI3DPX_EQ_RX2		0x05
#define PI3DPX_EQ_TX2		0x06
#define PI3DPX_EQ_TX1		0x07
#define PI3DPX_EQ_RX1		0x08
#define PI3DPX_MODE_DP2_USB	0x60	/* mode 3: 2-lane DP + USB3; | 0x10 flipped */
#define PI3DPX_MODE_FLIP	0x10
#define PI3DPX_MODE_SAFE	0xc0
#define PI3DPX_HPD_EN		0x06
#define PI3DPX_HPD_DIS		0x04
#define PI3DPX_EQ_DP		0x7c
#define PI3DPX_EQ_DEFAULT	0x08

#define PTN_CHIP_ID		0x00
#define PTN_MODE_CONTROL	0x04
#define PTN_MODE_MASK		0x07
#define PTN_MODE_USB_DP2	0x02
#define PTN_MODE_USB32		0x01
#define PTN_AUTO_ORIENT		BIT(7)
#define PTN_DP_LINK		0x06
#define PTN_DP_RATE_MASK	0x03
#define PTN_DP_RATE_HBR2	0x02
#define PTN_DP_LANES_MASK	0x0c
#define PTN_DP_LANES_2		0x08

static void w202_redriver_lookup(struct w202_redriver *rd)
{
	struct device_node *np;

	if (rd->client)
		return;
	np = of_find_node_by_path(rd->of_path);
	if (!np)
		return;
	/* NULL while the adapter has not been probed; retried at the next power-up */
	rd->client = of_find_i2c_device_by_node(np);
	of_node_put(np);
}

void w202_redriver_init(struct w202_dp *dp)
{
	dp->pi3dpx.of_path = PI3DPX_PATH;
	dp->ptn.of_path = PTN_PATH;
	w202_redriver_lookup(&dp->pi3dpx);
	w202_redriver_lookup(&dp->ptn);
}

void w202_redriver_release(struct w202_dp *dp)
{
	if (dp->pi3dpx.client)
		put_device(&dp->pi3dpx.client->dev);
	if (dp->ptn.client)
		put_device(&dp->ptn.client->dev);
	dp->pi3dpx.client = NULL;
	dp->ptn.client = NULL;
}

/* ID read; any SMBus error (NACK) = absent */
static bool w202_redriver_probe(struct w202_dp *dp, struct w202_redriver *rd,
				const char *name, u8 id_reg)
{
	int id;

	w202_redriver_lookup(rd);
	rd->present = false;
	if (!rd->client)
		return false;
	id = i2c_smbus_read_byte_data(rd->client, id_reg);
	if (id < 0) {
		dev_info(dp->dev, "%s (%s) not responding (%d): treated as absent\n",
			 name, dev_name(&rd->client->dev), id);
		return false;
	}
	rd->present = true;
	if (!rd->id_logged) {
		dev_info(dp->dev, "%s present, reg 0x%02x = 0x%02x\n", name, id_reg, id);
		rd->id_logged = true;
	}
	return true;
}

static void pi3dpx_write(struct w202_dp *dp, u8 reg, u8 val)
{
	int ret = i2c_smbus_write_byte_data(dp->pi3dpx.client, reg, val);

	/* vendor: log and continue with the sequence */
	if (ret)
		dev_warn(dp->dev, "pi3dpx1217 reg 0x%02x <- 0x%02x: %d\n", reg, val, ret);
}

static void ptn_update(struct w202_dp *dp, u8 reg, u8 mask, u8 val)
{
	int old = i2c_smbus_read_byte_data(dp->ptn.client, reg);
	int ret;

	if (old < 0) {
		dev_warn(dp->dev, "ptn38003a read 0x%02x: %d\n", reg, old);
		return;
	}
	ret = i2c_smbus_write_byte_data(dp->ptn.client, reg, (old & ~mask) | (val & mask));
	if (ret)
		dev_warn(dp->dev, "ptn38003a reg 0x%02x: %d\n", reg, ret);
}

/* dir: 0 = normal ("forward", AON bit10 set), 1 = flipped */
void w202_redriver_dp_on(struct w202_dp *dp, int dir)
{
	if (w202_redriver_probe(dp, &dp->pi3dpx, "PI3DPX1217", PI3DPX_VENDOR_ID)) {
		int dev_id = i2c_smbus_read_byte_data(dp->pi3dpx.client, PI3DPX_DEVICE_ID);

		dev_dbg(dp->dev, "PI3DPX1217 device id %d\n", dev_id);
		/* pi2dpx1217_set_operating_mode(3, dir), exact stock order (reg 07 twice) */
		pi3dpx_write(dp, PI3DPX_MODE, PI3DPX_MODE_DP2_USB | (dir ? PI3DPX_MODE_FLIP : 0));
		pi3dpx_write(dp, PI3DPX_HPD, PI3DPX_HPD_EN);
		pi3dpx_write(dp, PI3DPX_EQ_RX2, PI3DPX_EQ_DP);
		pi3dpx_write(dp, PI3DPX_EQ_TX2, PI3DPX_EQ_DP);
		pi3dpx_write(dp, PI3DPX_EQ_TX1, PI3DPX_EQ_DP);
		pi3dpx_write(dp, w202_pi3dpx_reg08 ? PI3DPX_EQ_RX1 : PI3DPX_EQ_TX1, PI3DPX_EQ_DP);
	}

	if (w202_redriver_probe(dp, &dp->ptn, "PTN38003A", PTN_CHIP_ID)) {
		/* mode_usb32_set(1) + dp_mode_set(1): USB3.2 + 2-lane DP, auto orientation */
		ptn_update(dp, PTN_MODE_CONTROL, PTN_AUTO_ORIENT | PTN_MODE_MASK,
			   PTN_AUTO_ORIENT | PTN_MODE_USB_DP2);
		/* dp_link_rate_set(HBR2) + dp_lane_set(2): phase-1 link maximum */
		ptn_update(dp, PTN_DP_LINK, PTN_DP_LANES_MASK | PTN_DP_RATE_MASK,
			   PTN_DP_LANES_2 | PTN_DP_RATE_HBR2);
	}
}

void w202_redriver_dp_off(struct w202_dp *dp)
{
	if (dp->pi3dpx.present) {
		/* pi2dpx1217_restore_default_value() */
		pi3dpx_write(dp, PI3DPX_MODE, PI3DPX_MODE_SAFE);
		pi3dpx_write(dp, PI3DPX_HPD, PI3DPX_HPD_DIS);
		pi3dpx_write(dp, PI3DPX_EQ_RX2, PI3DPX_EQ_DEFAULT);
		pi3dpx_write(dp, PI3DPX_EQ_TX2, PI3DPX_EQ_DEFAULT);
		pi3dpx_write(dp, PI3DPX_EQ_TX1, PI3DPX_EQ_DEFAULT);
		pi3dpx_write(dp, w202_pi3dpx_reg08 ? PI3DPX_EQ_RX1 : PI3DPX_EQ_TX1,
			     PI3DPX_EQ_DEFAULT);
	}
	if (dp->ptn.present) {
		/* dp_link_rate_set(0) + dp_lane_set(0); mode stays as stock leaves it */
		ptn_update(dp, PTN_DP_LINK, PTN_DP_LANES_MASK | PTN_DP_RATE_MASK, 0);
	}
}

bool w202_redriver_pi3dpx_present(struct w202_dp *dp)
{
	return dp->pi3dpx.present;
}

ssize_t w202_redriver_show(struct w202_dp *dp, char *buf, ssize_t len)
{
	return len + sysfs_emit_at(buf, len,
				   "pi3dpx1217: %s %s\nptn38003a: %s %s\n",
				   dp->pi3dpx.client ? dev_name(&dp->pi3dpx.client->dev) : "-",
				   dp->pi3dpx.present ? "present" : "absent/unprobed",
				   dp->ptn.client ? dev_name(&dp->ptn.client->dev) : "-",
				   dp->ptn.present ? "present" : "absent/unprobed");
}
