// SPDX-License-Identifier: GPL-2.0
/*
 * W200DS (Unisoc UMS9620) USB-C DisplayPort alt mode, phase 1: USB3 + 2-lane DP (TCA mux 3), HBR2.
 *
 * Platform driver on the "sprd,dptx" node (31890000.dptx). It owns:
 *  - the PD_DPU_DP power domain (PMU 0x3e4 bit25 / 0x548[20:16]), the DPTX/DPU1 clocks and the
 *    dispc1/usb31_dp/DPU1 glue writes of the shipped sprd-drm.ko dp_detect();
 *  - HPD injection into the DPTX through ipa-dispc1-glb-apb HPD_UPDATE (0x50);
 *  - the redrivers and GPIO120 (dp-gpios[1]);
 *  - a KMS-only drm_device "w202-dp": DPU1 CRTC/plane -> encoder -> dw-dp bridge -> w202 last
 *    bridge (power/detect gate, DPU1 run/stop) -> drm_bridge_connector (fwnode "w202-usbc-dp");
 *  - typec_switch/typec_mux on the "w202-usbc-mux" software node.
 */

#include <linux/bitfield.h>
#include <linux/iopoll.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/media-bus-format.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_graph.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-sprd-ums9620.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#include "w202_dp.h"

#define W202_DP_NAME		"w202-dp"
#define PMU_POLL_US		10
#define PMU_TIMEOUT_US		100000	/* stock: 50000 x ~2 us */

static bool aux_poll = true;
module_param(aux_poll, bool, 0444);
MODULE_PARM_DESC(aux_poll, "Poll AUX replies instead of the AUX_REPLY interrupt (stock IEN 0x95 has it off) [Y]");

int w202_sync_pol_vendor = 1;
module_param_named(sync_pol_vendor, w202_sync_pol_vendor, int, 0644);
MODULE_PARM_DESC(sync_pol_vendor,
		 "1: stock sync polarity (DPU1 DPI_CTRL=3, DPTX 0x30c bits set for negative sync); 0: DPI_CTRL from mode flags, dw-dp 0x30c untouched [1]");

bool w202_forward_irq_hpd = true;
module_param_named(forward_irq_hpd, w202_forward_irq_hpd, bool, 0644);
MODULE_PARM_DESC(forward_irq_hpd, "Inject IRQ_HPD pulses (0x50 = 0xb, 0xf); the shipped stack ignores IRQ_HPD [Y]");

bool w202_pi3dpx_reg08;
module_param_named(pi3dpx_reg08, w202_pi3dpx_reg08, bool, 0644);
MODULE_PARM_DESC(pi3dpx_reg08, "PI3DPX1217: write the last EQ value to reg 0x08 (EQ_RX1) instead of repeating reg 0x07 like stock [N]");

int w202_redriver_dir = -1;
module_param_named(redriver_dir, w202_redriver_dir, int, 0644);
MODULE_PARM_DESC(redriver_dir, "Orientation override for redriver/GPIO120: -1 auto (typec, else AON 0x2c bit10), 0 normal, 1 flipped [-1]");

static bool idle_shutdown = true;
module_param(idle_shutdown, bool, 0644);
MODULE_PARM_DESC(idle_shutdown, "Power PD_DPU_DP down (PMU 0x3e4 bit25) when DP is idle; N keeps it up after first use [Y]");

static bool restore_stream = true;
module_param(restore_stream, bool, 0644);
MODULE_PARM_DESC(restore_stream, "After HPD plug with the output still lit, restart a stopped DPTX video stream [Y]");

static unsigned int restore_delay_ms = 1000;
module_param(restore_delay_ms, uint, 0644);
MODULE_PARM_DESC(restore_delay_ms, "Delay from HPD plug to the stream check, ms [1000]");

static const char * const w202_dp_clk_names[] = {
	"clk_ipa_apb_dpu1_eb",
	"clk_ipa_apb_dptx_eb",
	"clk_ipa_apb_tca_eb",
	"clk_ipa_apb_usb31pll_eb",
};

struct w202_guard {
	struct drm_bridge base;
	struct w202_dp *dp;
};

static inline struct w202_dp *bridge_to_w202(struct drm_bridge *b)
{
	return container_of(b, struct w202_guard, base)->dp;
}

/* ------------------------------------------------------------------------------------------------
 * PD_DPU_DP power
 */

/* dw-dp register-access bracket (contract.md §4); nests, fails while the domain is down */
int w202_dp_hw_get(void *priv)
{
	struct w202_dp *dp = priv;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&dp->hw_lock, flags);
	if (!dp->powered || dp->hw_closing)
		ret = -ENODEV;
	else
		dp->hw_users++;
	spin_unlock_irqrestore(&dp->hw_lock, flags);
	return ret;
}

void w202_dp_hw_put(void *priv)
{
	struct w202_dp *dp = priv;
	unsigned long flags;

	spin_lock_irqsave(&dp->hw_lock, flags);
	if (!WARN_ON(dp->hw_users <= 0))
		dp->hw_users--;
	spin_unlock_irqrestore(&dp->hw_lock, flags);
	wake_up_all(&dp->hw_wq);
}

/*
 * The DPTX half of a TCA (orientation/mux) transaction, as the shipped dw_dptx.c TYPE_C interrupt does it: when the
 * TCA asserts typec_disable (TYPE_C_CTRL b1), put the DP lanes in P3, wait for the PHY interface, and raise
 * DISABLE_ACK (0x5); once the TCA releases it, drop the ack again (0x4). The TCA's xa ack waits for this, and an
 * ack left high from a previous transaction does not count. Polled by the combo PHY while it waits (the TYPE_C
 * interrupt stays masked); a powered-down DPTX has nothing to quiesce.
 */
static bool w202_dp_tca_ack(void *priv)
{
	struct w202_dp *dp = priv;
	u32 ctrl, phyif;

	if (w202_dp_hw_get(dp))
		return false;
	ctrl = readl(dp->dptx + DPTX_TYPE_C_CTRL);
	if ((ctrl & DPTX_TYPEC_DISABLE_STATUS) && !(ctrl & DPTX_TYPEC_DISABLE_ACK)) {
		phyif = readl(dp->dptx + DPTX_PHYIF_CTRL);
		phyif &= ~DPTX_PHYIF_LANE_PWRDOWN;
		phyif |= FIELD_PREP(DPTX_PHYIF_LANE_PWRDOWN, 3);
		writel(phyif, dp->dptx + DPTX_PHYIF_CTRL);
		if (readl_poll_timeout(dp->dptx + DPTX_PHYIF_CTRL, phyif, !(phyif & DPTX_PHYIF_BUSY), 10, 100000))
			dev_warn(dp->dev, "TCA disable: PHY busy 0x%08x\n", phyif);
		writel(DPTX_TYPEC_CTRL_ACK, dp->dptx + DPTX_TYPE_C_CTRL);
		writel(DPTX_IEN_TYPE_C, dp->dptx + DPTX_ISTS);
		dev_dbg(dp->dev, "TCA disable: TYPE_C_CTRL 0x%x, lanes in P3, acked\n", ctrl);
	} else if (!(ctrl & DPTX_TYPEC_DISABLE_STATUS) && (ctrl & DPTX_TYPEC_DISABLE_ACK)) {
		writel(DPTX_TYPEC_CTRL_RELEASE, dp->dptx + DPTX_TYPE_C_CTRL);
		writel(DPTX_IEN_TYPE_C, dp->dptx + DPTX_ISTS);
		dev_dbg(dp->dev, "TCA release: TYPE_C_CTRL 0x%x, ack dropped\n", ctrl);
		ctrl = DPTX_TYPEC_CTRL_RELEASE;
	}
	w202_dp_hw_put(dp);
	return ctrl & (DPTX_TYPEC_DISABLE_STATUS | DPTX_TYPEC_DISABLE_ACK);
}

static void w202_dp_tca_request(void *priv)
{
	struct w202_dp *dp = priv;

	queue_work(system_wq, &dp->tca_work);
}

static const struct sprd_ssphy_dp_ops w202_dp_ssphy_ops = {
	.tca_ack = w202_dp_tca_ack,
	.tca_request = w202_dp_tca_request,
};

static void w202_dp_ssphy_ops_unregister(void *data)
{
	struct w202_dp *dp = data;

	sprd_ssphy_set_dp_ops(dp->phy, NULL, NULL);
}

static void w202_dp_post_reset(void *priv)
{
	struct w202_dp *dp = priv;

	/* shipped dptx_core_init(): TYPE_C_CTRL = 5; shipped bind masks IEN bit7 (TYPE_C) */
	writel(0x5, dp->dptx + DPTX_TYPE_C_CTRL);
	writel(readl(dp->dptx + DPTX_IEN) & ~DPTX_IEN_TYPE_C, dp->dptx + DPTX_IEN);
}

static bool w202_dp_hw_idle(struct w202_dp *dp)
{
	bool idle;

	spin_lock_irq(&dp->hw_lock);
	idle = !dp->hw_users;
	spin_unlock_irq(&dp->hw_lock);
	return idle;
}

/* sprd-drm.ko sprd_dp_pd_event() + dp_detect() HOT_PLUG, shipped order (analysis-dp-video §1.2) */
static int w202_dp_hw_power_up(struct w202_dp *dp)
{
	u32 val;
	int ret;

	lockdep_assert_held(&dp->pwr_lock);

	/* step 0: PD_IPA must be on (0x548[12:8] == 0) */
	ret = regmap_read_poll_timeout(dp->pmu, PMU_PWR_STATUS_DBG_22, val,
				       !(val & PMU_PD_IPA_STATE), PMU_POLL_US, PMU_TIMEOUT_US);
	if (ret) {
		dev_err(dp->dev, "PD_IPA not on (PMU 0x548 = 0x%08x)\n", val);
		return ret;
	}
	/* step 1-2: PD_DPU_DP force-shutdown off, wait for the domain state machine */
	regmap_update_bits(dp->pmu, PMU_PD_DPU_DP_CFG_0, PMU_PD_DPU_DP_FORCE_SHUTDOWN, 0);
	ret = regmap_read_poll_timeout(dp->pmu, PMU_PWR_STATUS_DBG_22, val,
				       !(val & PMU_PD_DPU_DP_STATE), PMU_POLL_US, PMU_TIMEOUT_US);
	if (ret) {
		dev_err(dp->dev, "PD_DPU_DP did not power up (PMU 0x548 = 0x%08x)\n", val);
		regmap_update_bits(dp->pmu, PMU_PD_DPU_DP_CFG_0, PMU_PD_DPU_DP_FORCE_SHUTDOWN,
				   PMU_PD_DPU_DP_FORCE_SHUTDOWN);
		return ret;
	}

	ret = clk_bulk_prepare_enable(dp->num_dp_clks, dp->dp_clks);
	if (ret)
		goto err_pd;

	/* step 3: bypass HDCP; step 4: AUX PHY PWDNB; step 5: DPI polarity workaround */
	regmap_write(dp->dispc1, DISPC1_DPTX_CTRL, 0x1);
	regmap_update_bits(dp->usb31dp, USB31DP_AUX_PHY_CFG, USB31DP_AUX_PHY_PWDNB,
			   USB31DP_AUX_PHY_PWDNB);
	writel(0x3, dp->dpu + DPU_DPI_CTRL);

	/* step 7 (usbphy init in stock): DP reference on the combo PHY, never resets USB3 */
	ret = phy_power_on(dp->phy);
	if (ret) {
		dev_err(dp->dev, "DP PHY power on: %d\n", ret);
		goto err_clk;
	}
	dp->phy_on = true;

	spin_lock_irq(&dp->hw_lock);
	dp->powered = true;
	dp->hw_closing = false;
	spin_unlock_irq(&dp->hw_lock);

	/* step 6: DPTX core init (+ 0xc08 = 5 in post_reset); dw_dp_bind() does it the first time */
	if (dp->dwdp) {
		ret = dw_dp_hw_reinit(dp->dwdp);
		if (ret)
			dev_warn(dp->dev, "dw_dp_hw_reinit: %d\n", ret);
		if (!dp->dptx_irq_on) {
			enable_irq(dp->dptx_irq);
			dp->dptx_irq_on = true;
		}
	}
	dev_dbg(dp->dev, "PD_DPU_DP up\n");
	return 0;

err_clk:
	clk_bulk_disable_unprepare(dp->num_dp_clks, dp->dp_clks);
err_pd:
	regmap_update_bits(dp->pmu, PMU_PD_DPU_DP_CFG_0, PMU_PD_DPU_DP_FORCE_SHUTDOWN,
			   PMU_PD_DPU_DP_FORCE_SHUTDOWN);
	return ret;
}

static int w202_dp_hw_power_down(struct w202_dp *dp)
{
	lockdep_assert_held(&dp->pwr_lock);

	spin_lock_irq(&dp->hw_lock);
	dp->hw_closing = true;
	spin_unlock_irq(&dp->hw_lock);
	if (!wait_event_timeout(dp->hw_wq, w202_dp_hw_idle(dp), 5 * HZ)) {
		dev_err(dp->dev, "DPTX still in use after 5 s, PD_DPU_DP kept on\n");
		spin_lock_irq(&dp->hw_lock);
		dp->hw_closing = false;
		spin_unlock_irq(&dp->hw_lock);
		return -EBUSY;
	}

	if (dp->dptx_irq_on) {
		disable_irq(dp->dptx_irq);
		dp->dptx_irq_on = false;
	}
	/* stock unplug: IEN bit7 off, ISTS = 0xff, HPDSTS = 0x7 */
	writel(readl(dp->dptx + DPTX_IEN) & ~DPTX_IEN_TYPE_C, dp->dptx + DPTX_IEN);
	writel(0xff, dp->dptx + DPTX_ISTS);
	writel(0x7, dp->dptx + DPTX_HPDSTS);

	if (dp->phy_on) {
		phy_power_off(dp->phy);
		dp->phy_on = false;
	}
	clk_bulk_disable_unprepare(dp->num_dp_clks, dp->dp_clks);

	spin_lock_irq(&dp->hw_lock);
	dp->powered = false;
	dp->hw_closing = false;
	spin_unlock_irq(&dp->hw_lock);

	/* DPU1 dpu_power_domain(false) / stock: force-shutdown back on */
	regmap_update_bits(dp->pmu, PMU_PD_DPU_DP_CFG_0, PMU_PD_DPU_DP_FORCE_SHUTDOWN,
			   PMU_PD_DPU_DP_FORCE_SHUTDOWN);
	dp->pi3dpx.present = false;
	dp->ptn.present = false;
	dev_dbg(dp->dev, "PD_DPU_DP down\n");
	return 0;
}

static int w202_dp_power_get_locked(struct w202_dp *dp, enum w202_pwr_user user)
{
	int ret;

	lockdep_assert_held(&dp->pwr_lock);
	if (test_bit(user, &dp->pwr_users))
		return 0;
	if (!dp->powered) {
		ret = w202_dp_hw_power_up(dp);
		if (ret)
			return ret;
	}
	set_bit(user, &dp->pwr_users);
	return 0;
}

static void w202_dp_power_put_locked(struct w202_dp *dp, enum w202_pwr_user user)
{
	lockdep_assert_held(&dp->pwr_lock);
	clear_bit(user, &dp->pwr_users);
	if (!dp->pwr_users && dp->powered && idle_shutdown)
		w202_dp_hw_power_down(dp);
}

int w202_dp_power_get(struct w202_dp *dp, enum w202_pwr_user user)
{
	int ret;

	mutex_lock(&dp->pwr_lock);
	ret = w202_dp_power_get_locked(dp, user);
	mutex_unlock(&dp->pwr_lock);
	return ret;
}

void w202_dp_power_put(struct w202_dp *dp, enum w202_pwr_user user)
{
	mutex_lock(&dp->pwr_lock);
	w202_dp_power_put_locked(dp, user);
	mutex_unlock(&dp->pwr_lock);
}

/*
 * The TCA switch for a new plug orientation. Its handshake includes the DPTX (w202_dp_tca_ack()), and a DPTX that
 * is powered down cannot ack, so hold it up across the switch; never call into the PHY under pwr_lock (the
 * DPTX power-up takes the PHY's lock). Off TCPM's thread: a failed switch waits out the TCA's ~2.5 s timeout.
 */
static void w202_dp_tca_work(struct work_struct *work)
{
	struct w202_dp *dp = container_of(work, struct w202_dp, tca_work);
	enum typec_orientation o;
	int ret;

	mutex_lock(&dp->pwr_lock);
	o = dp->orientation;
	/* nothing plugged: the next attach routes it */
	ret = o == TYPEC_ORIENTATION_NONE ? -ENOTCONN : w202_dp_power_get_locked(dp, W202_PWR_TCA);
	mutex_unlock(&dp->pwr_lock);
	if (ret == -ENOTCONN)
		return;
	if (ret)
		dev_warn(dp->dev, "TCA switch with the DPTX down: %d\n", ret);
	sprd_ssphy_set_orientation(dp->phy, o);
	if (!ret)
		w202_dp_power_put(dp, W202_PWR_TCA);
}

/* ------------------------------------------------------------------------------------------------
 * Orientation, HPD
 */

static void w202_dp_apply_orientation_locked(struct w202_dp *dp)
{
	int typec_dir = -1, aon_dir = -1, dir, gpio;
	u32 boot_mode;

	lockdep_assert_held(&dp->pwr_lock);

	if (!regmap_read(dp->aon, AON_BOOT_MODE, &boot_mode))
		aon_dir = (boot_mode & AON_BOOT_MODE_CC_ORIENT) ? 0 : 1;
	if (dp->orientation == TYPEC_ORIENTATION_NORMAL)
		typec_dir = 0;
	else if (dp->orientation == TYPEC_ORIENTATION_REVERSE)
		typec_dir = 1;

	if (typec_dir >= 0 && aon_dir >= 0 && typec_dir != aon_dir)
		dev_warn(dp->dev, "orientation: typec says %s, AON BOOT_MODE 0x%08x bit10 says %s\n",
			 typec_dir ? "reverse" : "normal", boot_mode, aon_dir ? "reverse" : "normal");

	if (w202_redriver_dir >= 0)
		dir = w202_redriver_dir & 1;
	else if (typec_dir >= 0)
		dir = typec_dir;
	else
		dir = aon_dir > 0 ? 1 : 0;

	/* dp_detect() step 8: redriver mode 3 + direction, GPIO120 = 0 with redriver, else dir */
	w202_redriver_dp_on(dp, dir);
	gpio = w202_redriver_pi3dpx_present(dp) ? 0 : dir;
	gpiod_set_value_cansleep(dp->gpio_en2, gpio);
	dp->applied_dir = dir;
	dev_info(dp->dev, "orientation %s (typec %d, AON %d), GPIO120 = %d\n",
		 dir ? "flipped" : "normal", typec_dir, aon_dir, gpio);
}

void w202_dp_set_orientation(struct w202_dp *dp, enum typec_orientation o)
{
	mutex_lock(&dp->pwr_lock);
	dp->orientation = o;
	if (dp->hpd && dp->powered)
		w202_dp_apply_orientation_locked(dp);
	mutex_unlock(&dp->pwr_lock);
}

static void w202_dp_inject(struct w202_dp *dp, u32 level)
{
	/* global_dp.c dp_detect(): value, then value | HPD_UPDATE strobe */
	regmap_write(dp->dispc1, DISPC1_HPD_UPDATE, level);
	regmap_write(dp->dispc1, DISPC1_HPD_UPDATE, level | DISPC1_HPD_UPDATE_STROBE);
}

int w202_dp_hpd_event(struct w202_dp *dp, enum w202_hpd_event ev)
{
	int ret = 0;

	/* link training only on the TCA routing for this plug */
	if (ev == W202_HPD_PLUG)
		flush_work(&dp->tca_work);
	mutex_lock(&dp->pwr_lock);
	switch (ev) {
	case W202_HPD_PLUG:
		if (dp->hpd)
			break;
		ret = w202_dp_power_get_locked(dp, W202_PWR_HPD);
		if (ret)
			break;
		w202_dp_apply_orientation_locked(dp);
		w202_dp_inject(dp, DISPC1_DPTX_CONFIG_EN | DISPC1_HPD_STATE);		/* 0xa, 0xe */
		dp->hpd = true;
		dev_info(dp->dev, "HPD plug\n");
		mod_delayed_work(system_wq, &dp->hpd_notify_work, msecs_to_jiffies(20));
		if (restore_stream)
			mod_delayed_work(system_wq, &dp->restore_work,
					 msecs_to_jiffies(restore_delay_ms));
		break;
	case W202_HPD_IRQ:
		if (!dp->hpd || !w202_forward_irq_hpd)
			break;
		w202_dp_inject(dp, DISPC1_DPTX_CONFIG_EN | DISPC1_HPD_STATE |
			       DISPC1_HPD_IRQ);						/* 0xb, 0xf */
		dev_dbg(dp->dev, "IRQ_HPD\n");
		break;
	case W202_HPD_UNPLUG:
		if (!dp->hpd)
			break;
		w202_dp_inject(dp, DISPC1_DPTX_CONFIG_EN);				/* 0x8, 0xc */
		dp->hpd = false;
		cancel_delayed_work(&dp->restore_work);
		/* pi2dpx1217_restore_default_value() / ptn38003a rate+lanes 0 */
		w202_redriver_dp_off(dp);
		w202_dp_power_put_locked(dp, W202_PWR_HPD);
		dev_info(dp->dev, "HPD unplug\n");
		mod_delayed_work(system_wq, &dp->hpd_notify_work, msecs_to_jiffies(20));
		break;
	}
	mutex_unlock(&dp->pwr_lock);
	return ret;
}

/*
 * The HP Elite x3 Lap Dock (DP branch 00-22-b9 "7750") pulls HPD low for about a second a few seconds
 * after link-up. The connector returns unchanged, so userspace keeps its configuration and never
 * recommits, and the DPTX video stream stays off: link trained, sink never in sync, and the dock
 * keeps pulsing HPD. Restart the stream from the committed state once HPD has been back for a while.
 */
static void w202_dp_restore_work(struct work_struct *work)
{
	struct w202_dp *dp = container_of(to_delayed_work(work), struct w202_dp, restore_work);
	const struct drm_display_mode *m;
	bool streaming;
	u32 pol = 0;
	int ret;

	if (!READ_ONCE(dp->hpd) || w202_dp_hw_get(dp))
		return;
	streaming = readl(dp->dptx + DPTX_VSAMPLE_CTRL) & DPTX_VIDEO_STREAM_ENABLE;
	w202_dp_hw_put(dp);
	if (streaming)
		return;

	ret = dw_dp_restore_stream(dp->dwdp);
	if (ret) {
		if (ret != -ENOLINK)
			dev_warn(dp->dev, "video stream restore failed: %d\n", ret);
		return;
	}

	drm_modeset_lock(&dp->crtc.mutex, NULL);
	m = &dp->crtc.state->adjusted_mode;
	if (w202_sync_pol_vendor && dp->crtc.state->active && !w202_dp_hw_get(dp)) {
		/* same override as w202_guard_enable(), after dw-dp programmed the video */
		if (m->flags & DRM_MODE_FLAG_NHSYNC)
			pol |= DPTX_HSYNC_IN_POLARITY;
		if (m->flags & DRM_MODE_FLAG_NVSYNC)
			pol |= DPTX_VSYNC_IN_POLARITY;
		writel(pol, dp->dptx + DPTX_VINPUT_POLARITY_CTRL);
		w202_dp_hw_put(dp);
	}
	drm_modeset_unlock(&dp->crtc.mutex);
	dev_info(dp->dev, "video stream restored after HPD pulse\n");
}

static void w202_dp_hpd_notify_work(struct work_struct *work)
{
	struct w202_dp *dp = container_of(to_delayed_work(work), struct w202_dp,
					  hpd_notify_work);

	/* covers the manual-knob path and a DPTX HPD interrupt that never arrives */
	drm_bridge_hpd_notify(dp->guard, READ_ONCE(dp->hpd) ? connector_status_connected :
							      connector_status_disconnected);
}

/* ------------------------------------------------------------------------------------------------
 * Last bridge: gates detect/EDID on HPD (DPTX may be unpowered), powers the block from pre_enable
 * to post_disable, fixes sync polarity, runs/stops DPU1 after/before the DPTX like sprd_dp.c.
 */

static enum drm_connector_status w202_guard_detect(struct drm_bridge *bridge,
						   struct drm_connector *connector)
{
	struct w202_dp *dp = bridge_to_w202(bridge);

	if (!READ_ONCE(dp->hpd))
		return connector_status_disconnected;
	return drm_bridge_detect(dp->dw_bridge, connector);
}

static const struct drm_edid *w202_guard_edid_read(struct drm_bridge *bridge,
						   struct drm_connector *connector)
{
	struct w202_dp *dp = bridge_to_w202(bridge);

	if (!READ_ONCE(dp->hpd))
		return NULL;
	return drm_bridge_edid_read(dp->dw_bridge, connector);
}

static u32 *w202_guard_output_fmts(struct drm_bridge *bridge,
				   struct drm_bridge_state *bridge_state,
				   struct drm_crtc_state *crtc_state,
				   struct drm_connector_state *conn_state,
				   unsigned int *num)
{
	u32 *fmts = kmalloc(sizeof(*fmts), GFP_KERNEL);

	*num = 0;
	if (!fmts)
		return NULL;
	/* DPU1 DPI output: RGB 8 bpc, single pixel */
	fmts[0] = MEDIA_BUS_FMT_RGB888_1X24;
	*num = 1;
	return fmts;
}

static u32 *w202_guard_input_fmts(struct drm_bridge *bridge,
				  struct drm_bridge_state *bridge_state,
				  struct drm_crtc_state *crtc_state,
				  struct drm_connector_state *conn_state,
				  u32 output_fmt, unsigned int *num)
{
	return w202_guard_output_fmts(bridge, bridge_state, crtc_state, conn_state, num);
}

/*
 * dw-dp atomic_enable/disable touch the DPTX without hw_get. Allow a modeset that lights the
 * output only while PD_DPU_DP is already up (HPD high or force_power), so the commit itself never
 * has to power the block up and cannot leave dw-dp running against a dead domain.
 */
static int w202_guard_atomic_check(struct drm_bridge *bridge,
				   struct drm_bridge_state *bridge_state,
				   struct drm_crtc_state *crtc_state,
				   struct drm_connector_state *conn_state)
{
	struct w202_dp *dp = bridge_to_w202(bridge);

	if (crtc_state->active && drm_atomic_crtc_needs_modeset(crtc_state) &&
	    !READ_ONCE(dp->powered)) {
		drm_dbg_atomic(&dp->drm, "DP not plugged/powered: modeset refused\n");
		return -ENODEV;
	}
	return 0;
}

static void w202_guard_pre_enable(struct drm_bridge *bridge, struct drm_atomic_state *state)
{
	struct w202_dp *dp = bridge_to_w202(bridge);
	int ret = w202_dp_power_get(dp, W202_PWR_BRIDGE);

	if (ret)
		dev_err(dp->dev, "PD_DPU_DP power-up for the link failed: %d\n", ret);
}

static void w202_guard_enable(struct drm_bridge *bridge, struct drm_atomic_state *state)
{
	struct w202_dp *dp = bridge_to_w202(bridge);
	struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, &dp->crtc);
	u32 pol = 0;

	if (w202_sync_pol_vendor && cs && !w202_dp_hw_get(dp)) {
		/* vendor dp_video.c: VSAMPLE_POLARITY bits set for negative syncs */
		if (cs->adjusted_mode.flags & DRM_MODE_FLAG_NHSYNC)
			pol |= DPTX_HSYNC_IN_POLARITY;
		if (cs->adjusted_mode.flags & DRM_MODE_FLAG_NVSYNC)
			pol |= DPTX_VSYNC_IN_POLARITY;
		writel(pol, dp->dptx + DPTX_VINPUT_POLARITY_CTRL);
		w202_dp_hw_put(dp);
	}
	/* sprd_dp.c: sprd_dpu1_run() after the DPTX video is enabled */
	w202_dpu1_run(dp);
}

static void w202_guard_disable(struct drm_bridge *bridge, struct drm_atomic_state *state)
{
	w202_dpu1_stop(bridge_to_w202(bridge));
}

static void w202_guard_post_disable(struct drm_bridge *bridge, struct drm_atomic_state *state)
{
	struct w202_dp *dp = bridge_to_w202(bridge);

	mutex_lock(&dp->pwr_lock);
	if (test_bit(W202_PWR_BRIDGE, &dp->pwr_users))
		w202_dp_power_put_locked(dp, W202_PWR_BRIDGE);
	mutex_unlock(&dp->pwr_lock);
}

static const struct drm_bridge_funcs w202_guard_funcs = {
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.atomic_get_output_bus_fmts = w202_guard_output_fmts,
	.atomic_get_input_bus_fmts = w202_guard_input_fmts,
	.atomic_check = w202_guard_atomic_check,
	.atomic_pre_enable = w202_guard_pre_enable,
	.atomic_enable = w202_guard_enable,
	.atomic_disable = w202_guard_disable,
	.atomic_post_disable = w202_guard_post_disable,
	.detect = w202_guard_detect,
	.edid_read = w202_guard_edid_read,
};

/* ------------------------------------------------------------------------------------------------
 * sysfs knobs on 31890000.dptx
 */

static ssize_t force_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w202_dp *dp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", test_bit(W202_PWR_FORCE, &dp->pwr_users));
}

static ssize_t force_power_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct w202_dp *dp = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;
	if (on) {
		ret = w202_dp_power_get(dp, W202_PWR_FORCE);
		if (ret)
			return ret;
	} else {
		w202_dp_power_put(dp, W202_PWR_FORCE);
	}
	return count;
}
static DEVICE_ATTR_RW(force_power);

static ssize_t hpd_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w202_dp *dp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", READ_ONCE(dp->hpd) ? "plug" : "unplug");
}

static ssize_t hpd_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct w202_dp *dp = dev_get_drvdata(dev);
	int ret;

	if (sysfs_streq(buf, "plug"))
		ret = w202_dp_hpd_event(dp, W202_HPD_PLUG);
	else if (sysfs_streq(buf, "unplug"))
		ret = w202_dp_hpd_event(dp, W202_HPD_UNPLUG);
	else if (sysfs_streq(buf, "irq"))
		ret = w202_dp_hpd_event(dp, W202_HPD_IRQ);
	else
		return -EINVAL;
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(hpd);

static ssize_t hw_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w202_dp *dp = dev_get_drvdata(dev);
	u32 pmu_cfg = 0, pmu_sts = 0;
	ssize_t len;

	regmap_read(dp->pmu, PMU_PD_DPU_DP_CFG_0, &pmu_cfg);
	regmap_read(dp->pmu, PMU_PWR_STATUS_DBG_22, &pmu_sts);
	len = sysfs_emit(buf, "pmu_0x3e4 0x%08x\npmu_0x548 0x%08x\n", pmu_cfg, pmu_sts);
	/* DPTX/DPU1 registers only while PD_DPU_DP is up (a read of the unpowered block resets the SoC) */
	if (w202_dp_hw_get(dp))
		return len + sysfs_emit_at(buf, len, "PD_DPU_DP off: write 1 to force_power first\n");
	len += sysfs_emit_at(buf, len,
			     "dptx_version 0x%08x\ndptx_type 0x%08x\ndptx_id 0x%08x\ndptx_config1 0x%08x\n"
			     "dptx_type_c_ctrl 0x%08x\ndptx_hpdsts 0x%08x\n",
			     readl(dp->dptx + DPTX_VERSION_NUMBER), readl(dp->dptx + DPTX_VERSION_TYPE),
			     readl(dp->dptx + DPTX_ID), readl(dp->dptx + DPTX_CONFIG1),
			     readl(dp->dptx + DPTX_TYPE_C_CTRL), readl(dp->dptx + DPTX_HPDSTS));
	/* DPU1 registers additionally need its core clock: only while the CRTC is enabled */
	spin_lock_irq(&dp->dpu_lock);
	if (dp->dpu_enabled)
		len += sysfs_emit_at(buf, len, "dpu1_version 0x%08x\ndpu1_mmu_en 0x%08x\n",
				     readl(dp->dpu + DPU_VERSION), readl(dp->dpu + DPU_MMU_EN));
	else
		len += sysfs_emit_at(buf, len, "dpu1: CRTC off, not read\n");
	spin_unlock_irq(&dp->dpu_lock);
	w202_dp_hw_put(dp);
	return len;
}
static DEVICE_ATTR_RO(hw_id);

static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	static const char * const orient[] = { "none", "normal", "reverse" };
	struct w202_dp *dp = dev_get_drvdata(dev);
	u32 boot_mode = 0;
	ssize_t len;

	regmap_read(dp->aon, AON_BOOT_MODE, &boot_mode);
	mutex_lock(&dp->pwr_lock);
	len = sysfs_emit(buf,
			 "powered %d\nusers probe=%d hpd=%d crtc=%d bridge=%d force=%d tca=%d\nhpd %d\n"
			 "typec_orientation %s\napplied_dir %d\naon_boot_mode 0x%08x (bit10 %d)\n"
			 "dpu1_enabled %d running %d pclk %lu\n",
			 dp->powered, test_bit(W202_PWR_PROBE, &dp->pwr_users),
			 test_bit(W202_PWR_HPD, &dp->pwr_users), test_bit(W202_PWR_CRTC, &dp->pwr_users),
			 test_bit(W202_PWR_BRIDGE, &dp->pwr_users),
			 test_bit(W202_PWR_FORCE, &dp->pwr_users), test_bit(W202_PWR_TCA, &dp->pwr_users), dp->hpd,
			 orient[dp->orientation <= TYPEC_ORIENTATION_REVERSE ? dp->orientation : 0],
			 dp->applied_dir, boot_mode, !!(boot_mode & AON_BOOT_MODE_CC_ORIENT),
			 dp->dpu_enabled, dp->dpu_running, dp->pclk_hz);
	len = w202_redriver_show(dp, buf, len);
	mutex_unlock(&dp->pwr_lock);
	return len;
}
static DEVICE_ATTR_RO(status);

static struct attribute *w202_dp_attrs[] = {
	&dev_attr_force_power.attr,
	&dev_attr_hpd.attr,
	&dev_attr_hw_id.attr,
	&dev_attr_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(w202_dp);

/* ------------------------------------------------------------------------------------------------
 * probe/remove
 */

DEFINE_DRM_GEM_DMA_FOPS(w202_dp_fops);

static const struct drm_driver w202_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.fops = &w202_dp_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	.name = W202_DP_NAME,
	.desc = "W200DS USB-C DisplayPort (DPU1 + DW DPTX)",
	.major = 1,
	.minor = 0,
};

static const struct drm_mode_config_funcs w202_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static struct regmap *w202_syscon(struct w202_dp *dp, const char *prop)
{
	struct regmap *map = syscon_regmap_lookup_by_phandle(dp->dev->of_node, prop);

	if (IS_ERR(map))
		dev_err_probe(dp->dev, PTR_ERR(map), "syscon %s\n", prop);
	return map;
}

static void w202_dp_put_fwnodes(void *data)
{
	struct w202_dp *dp = data;

	fwnode_handle_put(dp->mux_fwnode);
	fwnode_handle_put(dp->conn_fwnode);
}

static void w202_dp_put_dpu_np(void *data)
{
	of_node_put(data);
}

static int w202_dp_find_swnodes(struct w202_dp *dp)
{
	const struct software_node *mux_sw, *dp_sw;

	mux_sw = software_node_find_by_name(NULL, "w202-usbc-mux");
	if (!mux_sw)
		return dev_err_probe(dp->dev, -EPROBE_DEFER,
				     "software node w202-usbc-mux missing (load ump9620_tcpc)\n");
	dp_sw = software_node_find_by_name(NULL, "w202-usbc-dp");
	if (!dp_sw) {
		fwnode_handle_put(software_node_fwnode(mux_sw));
		return dev_err_probe(dp->dev, -EPROBE_DEFER,
				     "software node w202-usbc-dp missing (load ump9620_tcpc)\n");
	}
	dp->mux_fwnode = software_node_fwnode(mux_sw);
	dp->conn_fwnode = software_node_fwnode(dp_sw);
	return devm_add_action_or_reset(dp->dev, w202_dp_put_fwnodes, dp);
}

static int w202_dp_get_resources(struct w202_dp *dp, struct platform_device *pdev)
{
	struct device *dev = dp->dev;
	struct resource *res, dpu_res;
	int i, ret;

	dp->pmu = w202_syscon(dp, "sprd,syscon-force-shutdown");
	if (IS_ERR(dp->pmu))
		return PTR_ERR(dp->pmu);
	dp->aon = w202_syscon(dp, "sprd,syscon-aon-apb");
	if (IS_ERR(dp->aon))
		return PTR_ERR(dp->aon);
	dp->dispc1 = w202_syscon(dp, "sprd,syscon-ipa-dispc1-glb-apb");
	if (IS_ERR(dp->dispc1))
		return PTR_ERR(dp->dispc1);
	dp->usb31dp = w202_syscon(dp, "sprd,syscon-usb31-dp-phy");
	if (IS_ERR(dp->usb31dp))
		return PTR_ERR(dp->usb31dp);

	/* DPTX: dw-dp requests the whole region; this unrequested alias only maps the glue registers */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	dp->dptx = devm_ioremap(dev, res->start, DPTX_MAP_SIZE);
	if (!dp->dptx)
		return -ENOMEM;
	dp->dptx_irq = platform_get_irq(pdev, 0);
	if (dp->dptx_irq < 0)
		return dp->dptx_irq;

	/* DPU1 via the OF graph: dptx port@1 -> dpu1 port */
	dp->dpu_np = of_graph_get_remote_node(dev->of_node, 1, -1);
	if (!dp->dpu_np)
		return dev_err_probe(dev, -ENODEV, "no dpu1 behind dptx port@1\n");
	ret = devm_add_action_or_reset(dev, w202_dp_put_dpu_np, dp->dpu_np);
	if (ret)
		return ret;
	if (of_address_to_resource(dp->dpu_np, 0, &dpu_res))
		return dev_err_probe(dev, -EINVAL, "dpu1 reg\n");
	dp->dpu = devm_ioremap(dev, dpu_res.start, min_t(resource_size_t,
							 resource_size(&dpu_res), DPU_MAP_SIZE));
	if (!dp->dpu)
		return -ENOMEM;

	dp->num_dp_clks = ARRAY_SIZE(w202_dp_clk_names);
	dp->dp_clks = devm_kcalloc(dev, dp->num_dp_clks, sizeof(*dp->dp_clks), GFP_KERNEL);
	if (!dp->dp_clks)
		return -ENOMEM;
	for (i = 0; i < dp->num_dp_clks; i++)
		dp->dp_clks[i].id = w202_dp_clk_names[i];
	ret = devm_clk_bulk_get(dev, dp->num_dp_clks, dp->dp_clks);
	if (ret)
		return dev_err_probe(dev, ret, "dptx clocks\n");

	/* shipped dp_glb_parse_dt(): only dp-gpios[1] ("GPIO_EN2", ap_gpio 120), output high */
	dp->gpio_en2 = devm_gpiod_get_index(dev, "dp", 1, GPIOD_OUT_HIGH);
	if (IS_ERR(dp->gpio_en2))
		return dev_err_probe(dev, PTR_ERR(dp->gpio_en2), "dp-gpios[1] (GPIO120)\n");

	dp->phy = devm_sprd_ssphy_get_dp_phy(dev);
	if (IS_ERR(dp->phy))
		return dev_err_probe(dev, PTR_ERR(dp->phy), "combo PHY (DP side)\n");

	sprd_ssphy_set_dp_ops(dp->phy, &w202_dp_ssphy_ops, dp);
	return devm_add_action_or_reset(dev, w202_dp_ssphy_ops_unregister, dp);
}

static int w202_dp_drm_init(struct w202_dp *dp)
{
	struct drm_device *drm = &dp->drm;
	struct drm_bridge *dw_bridge;
	struct w202_guard *guard;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = 16;
	drm->mode_config.min_height = 16;
	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.fb_modifiers_not_supported = true;
	drm->mode_config.funcs = &w202_mode_config_funcs;

	/* CRTC/plane need the mode_config IDR initialised above */
	ret = w202_dpu1_init(dp);
	if (ret)
		return ret;

	ret = drm_simple_encoder_init(drm, &dp->encoder, DRM_MODE_ENCODER_TMDS);
	if (ret)
		return ret;
	dp->encoder.possible_crtcs = drm_crtc_mask(&dp->crtc);

	dp->plat.max_link_rate = 540000;
	dp->plat.pixel_mode = DW_DP_PLAT_PIXEL_MODE_SINGLE;
	dp->plat.phy = dp->phy;
	dp->plat.ctrl_phy_rate_eq = true;
	dp->plat.hpd_status_bit8 = true;
	dp->plat.optional_clks_rst = true;
	dp->plat.aux_poll = aux_poll;
	dp->plat.full_reset = true;
	dp->plat.priv = dp;
	dp->plat.post_reset = w202_dp_post_reset;
	dp->plat.hw_get = w202_dp_hw_get;
	dp->plat.hw_put = w202_dp_hw_put;

	/* dw_dp_bind() programs the DPTX: PD_DPU_DP is held up by W202_PWR_PROBE here */
	dp->dwdp = dw_dp_bind(dp->dev, &dp->encoder, &dp->plat);
	if (IS_ERR(dp->dwdp))
		return dev_err_probe(dp->dev, PTR_ERR(dp->dwdp), "dw_dp_bind\n");
	/* with hw_get set dw-dp requests its IRQ IRQF_NO_AUTOEN; the domain is up here */
	enable_irq(dp->dptx_irq);
	dp->dptx_irq_on = true;

	dw_bridge = drm_bridge_chain_get_first_bridge(&dp->encoder);
	if (!dw_bridge)
		return -ENODEV;
	/* the chain keeps its own reference while attached */
	dp->dw_bridge = dw_bridge;
	drm_bridge_put(dw_bridge);

	guard = devm_drm_bridge_alloc(dp->dev, struct w202_guard, base, &w202_guard_funcs);
	if (IS_ERR(guard))
		return PTR_ERR(guard);
	guard->dp = dp;
	guard->base.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID | DRM_BRIDGE_OP_HPD;
	guard->base.type = DRM_MODE_CONNECTOR_DisplayPort;
	dp->guard = &guard->base;
	ret = devm_drm_bridge_add(dp->dev, dp->guard);
	if (ret)
		return ret;
	ret = drm_bridge_attach(&dp->encoder, dp->guard, dp->dw_bridge,
				DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret)
		return ret;

	dp->connector = drm_bridge_connector_init(drm, &dp->encoder);
	if (IS_ERR(dp->connector))
		return dev_err_probe(dp->dev, PTR_ERR(dp->connector), "bridge connector\n");
	/* drm_connector_oob_hotplug_event() from typec_displayport finds us by this fwnode */
	fwnode_handle_put(dp->connector->fwnode);
	dp->connector->fwnode = fwnode_handle_get(dp->conn_fwnode);
	ret = drm_connector_attach_encoder(dp->connector, &dp->encoder);
	if (ret)
		return ret;

	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);
	drm_kms_helper_poll_init(drm);
	return 0;
}

static int w202_dp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct w202_dp *dp;
	int ret;

	dp = devm_drm_dev_alloc(dev, &w202_drm_driver, struct w202_dp, drm);
	if (IS_ERR(dp))
		return PTR_ERR(dp);
	dp->dev = dev;
	platform_set_drvdata(pdev, dp);
	mutex_init(&dp->pwr_lock);
	spin_lock_init(&dp->hw_lock);
	init_waitqueue_head(&dp->hw_wq);
	INIT_DELAYED_WORK(&dp->hpd_notify_work, w202_dp_hpd_notify_work);
	INIT_DELAYED_WORK(&dp->restore_work, w202_dp_restore_work);
	INIT_WORK(&dp->tca_work, w202_dp_tca_work);
	dp->orientation = TYPEC_ORIENTATION_NONE;
	dp->applied_dir = -1;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ret = w202_dp_find_swnodes(dp);
	if (ret)
		return ret;
	ret = w202_dp_get_resources(dp, pdev);
	if (ret)
		return ret;
	w202_redriver_init(dp);

	ret = w202_dp_power_get(dp, W202_PWR_PROBE);
	if (ret) {
		dev_err(dev, "PD_DPU_DP power-up for bind failed: %d\n", ret);
		goto err_redriver;
	}
	ret = w202_dp_drm_init(dp);
	w202_dp_power_put(dp, W202_PWR_PROBE);
	if (ret)
		goto err_poll;

	ret = drm_dev_register(&dp->drm, 0);
	if (ret)
		goto err_poll;

	/* last: ump9620_tcpc's typec_register_port() defers until these exist */
	ret = w202_typec_register(dp);
	if (ret)
		goto err_unregister;

	dev_info(dev, "w202-dp ready (HBR2 x2, aux_poll=%d, idle_shutdown=%d)\n",
		 aux_poll, idle_shutdown);
	return 0;

err_unregister:
	drm_dev_unregister(&dp->drm);
err_poll:
	if (dp->drm.mode_config.poll_enabled)
		drm_kms_helper_poll_fini(&dp->drm);
	/* after dw_dp_bind() the PROBE power-put already disabled the DPTX IRQ; devres frees it */
err_redriver:
	w202_redriver_release(dp);
	return ret;
}

static void w202_dp_remove(struct platform_device *pdev)
{
	struct w202_dp *dp = platform_get_drvdata(pdev);

	w202_typec_unregister(dp);
	/* no more TCA requests from the PHY before the work goes */
	w202_dp_ssphy_ops_unregister(dp);
	cancel_work_sync(&dp->tca_work);
	cancel_delayed_work_sync(&dp->hpd_notify_work);
	cancel_delayed_work_sync(&dp->restore_work);
	drm_dev_unregister(&dp->drm);
	drm_kms_helper_poll_fini(&dp->drm);
	drm_atomic_helper_shutdown(&dp->drm);

	mutex_lock(&dp->pwr_lock);
	if (dp->hpd) {
		w202_dp_inject(dp, DISPC1_DPTX_CONFIG_EN);
		w202_redriver_dp_off(dp);
		dp->hpd = false;
	}
	dp->pwr_users = 0;
	if (dp->powered && w202_dp_hw_power_down(dp))
		dev_err(dp->dev, "PD_DPU_DP left on at unbind\n");
	mutex_unlock(&dp->pwr_lock);

	w202_redriver_release(dp);
}

static void w202_dp_shutdown(struct platform_device *pdev)
{
	struct w202_dp *dp = platform_get_drvdata(pdev);

	drm_atomic_helper_shutdown(&dp->drm);
}

static const struct of_device_id w202_dp_of_match[] = {
	{ .compatible = "sprd,dptx" },
	{ }
};
MODULE_DEVICE_TABLE(of, w202_dp_of_match);

static struct platform_driver w202_dp_driver = {
	.probe = w202_dp_probe,
	.remove = w202_dp_remove,
	.shutdown = w202_dp_shutdown,
	.driver = {
		.name = W202_DP_NAME,
		.of_match_table = w202_dp_of_match,
		.dev_groups = w202_dp_groups,
	},
};
module_platform_driver(w202_dp_driver);

MODULE_SOFTDEP("pre: ump9620_tcpc typec_displayport");
MODULE_DESCRIPTION("W200DS USB-C DisplayPort alt mode (DPU1 + DW DPTX + redrivers)");
MODULE_LICENSE("GPL");
