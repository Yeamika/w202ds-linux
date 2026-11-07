// SPDX-License-Identifier: GPL-2.0-only
/*
 * Unisoc UMS9620 (qogirn6pro) USB 3.1 / USB 2.0 combo PHY for DWC3, device mode.
 * Ported from the Unisoc 5.4 kernel (drivers/usb/phy/phy-sprd-usb31-qogirn6pro.c). The Type-C redriver (ptn38003a)
 * and the usbm cross-PHY bookkeeping are left out. On the board with a battery (the U30 Air) D+/D- go to the PMIC's
 * BC1.2 detector first and to the PHY once it found a USB port (SDP, CDP), as 5.4's PHY and dwc3 hand them over
 * (ums9620-bc12.h, kanoqwq b4748534); the boards without one (F50, V50) switch them to the PHY at probe, as before.
 *
 * The combo PHY also carries DisplayPort (the TCA runs USB3 + 2-lane DP, mux 3, from the USB init on): a generic
 * PHY for the DPTX takes a DP reference beside dwc3's usb_phy one, so the PHY is shut down only when both are gone
 * and is never reset under a live USB3 link, and sets the DP swing/pre-emphasis overrides (Unisoc's dp_link.c).
 *
 * Copyright (C) 2020 Spreadtrum Communications Inc.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/extcon.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-sprd-ums9620.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/usb/otg.h>
#include <linux/usb/phy.h>
#include <linux/usb/typec.h>
#include <linux/workqueue.h>
#include <dt-bindings/soc/sprd,qogirn6pro-mask.h>
#include <dt-bindings/soc/sprd,qogirn6pro-regs.h>

struct sprd_ssphy {
	struct device		*dev;
	struct usb_phy		phy;
	void __iomem		*base;
	struct regmap		*aon_apb;
	struct regmap		*ipa_apb;
	struct regmap		*ipa_dispc1_glb_apb;
	struct regmap		*ipa_usb31_dp;
	struct regmap		*ipa_usb31_dptx;
	struct regmap		*ana_g0l;
	struct regmap		*pmic;
	struct regulator	*vdd;
	u32			vdd_vol;
	u32			host_eye_pattern;
	u32			device_eye_pattern;
	u32			ssphy_ctl5_eye_pattern;
	u32			ssphy_ctl6_eye_pattern;
	atomic_t		reset;
	atomic_t		inited;
	bool			is_host;
	bool			bc12_enabled, bc12_ready, bc12_stopping;
	bool			vbus_present;
	atomic_t		bc12_generation;
	u32			bc12_seen;
	unsigned int		bc12_samples;
	bool			bc12_retry;
	struct mutex		dpdm_lock;
	struct regmap		*pmu_apb;
	struct phy		*dp_phy;
	struct mutex		pwr_lock;	/* the USB and DP users, PHY init and shutdown */
	bool			usb_user, dp_user, pmu_forced;
	enum typec_orientation	orientation;	/* last from TCPM; NONE: use AON BOOT_MODE bit10 */
	bool			tca_valid;	/* the TCA routes by @tca_flip */
	bool			tca_flip;	/* TCPC/SYSMODE b2 as last programmed */
	const struct sprd_ssphy_dp_ops *dp_ops;	/* the DPTX side of the TCA */
	void			*dp_data;
	spinlock_t		bc12_event_lock;
	struct delayed_work	bc12_work;
};

#define PHY_INIT_TIMEOUT 500

static bool pmu_pll_force = true;
module_param(pmu_pll_force, bool, 0644);
MODULE_PARM_DESC(pmu_pll_force,
		 "PMU USB31 PLL force-on and PHY power-stable at PHY init, cleared at shutdown (Unisoc; next init)");

#define DISPC1_GLB_APB_EB		 (0x0)
#define DISPC1_GLB_APB_RST		 (0x4)

#define PHY0_SRAM_BYPASS		(0X48)
#define PHY_SRAM_INIT_CHECK_DONE	(0x74)
#define TYPEC_DISABLE_ACK		(0xc08)

#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_VBUSVALID       0x02000000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TESTCLK         0x01000000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TESTDATAIN      0xff0000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TESTADDR        0xf000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TESTDATAOUTSEL  0x800
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TESTDATAOUT     0x380
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_BIST_MODE       0x7c
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_T2RCOMP         0x2
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_LPBK_END        0x1
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_DATABUS16_8     0x10000000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_SUSPENDM        0x08000000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_PORN            0x04000000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_RESET           0x02000000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_RXERROR         0x01000000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_BYPASS_DRV_DP   0x00800000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_BYPASS_DRV_DM   0x00400000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_BYPASS_FS       0x00200000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_BYPASS_IN_DP    0x00100000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_BYPASS_IN_DM    0x80000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_BYPASS_OUT_DP   0x40000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_BYPASS_OUT_DM   0x20000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_VBUSVLDEXT      0x10000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_RESERVED        0x0000FFFF
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_REXTENABLE      0x4
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_DMPULLUP        0x2
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_SAMPLER_SEL     0x1
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_DPPULLDOWN      0x10
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_DMPULLDOWN      0x8
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TXBITSTUFFENABLE 0x4
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TXBITSTUFFENABLEH 0x2
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_SLEEPM          0x1
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TUNEHSAMP       0x06000000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TFREGRES        0x01f80000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TFHSRES         0x7c000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TUNERISE        0x3000
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TUNEOTG         0xe00
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TUNEDSC         0x180
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TUNESQ          0x78
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TUNEEQ          0x7
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_TUNEPLLS        0x7800
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_PLL_PFD_DEADZONE 0x300
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_PLL_PFD_DELAY   0xc0
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_PLL_CP_IOFFSET_EN 0x20
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_PLL_CP_IOFFSET  0x1e
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_PLL_REF_DOUBLER_EN 0x1
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_BISTRAM_EN      0x2
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_BIST_MODE_EN    0x1
#define BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_ISO_SW_EN       0x1
#define BIT_ANLG_PHY_G0L_DBG_SEL_ANALOG_USB20_USB20_SUSPENDM 0x100
#define BIT_ANLG_PHY_G0L_DBG_SEL_ANALOG_USB20_USB20_PORN    0x80
#define BIT_ANLG_PHY_G0L_DBG_SEL_ANALOG_USB20_USB20_RESET   0x40
#define BIT_ANLG_PHY_G0L_DBG_SEL_ANALOG_USB20_USB20_BYPASS_FS 0x20
#define BIT_ANLG_PHY_G0L_DBG_SEL_ANALOG_USB20_USB20_BYPASS_IN_DM 0x10
#define BIT_ANLG_PHY_G0L_DBG_SEL_ANALOG_USB20_USB20_DPPULLDOWN 0x8
#define BIT_ANLG_PHY_G0L_DBG_SEL_ANALOG_USB20_USB20_DMPULLDOWN 0x4
#define BIT_ANLG_PHY_G0L_DBG_SEL_ANALOG_USB20_USB20_SLEEPM 0x2
#define BIT_ANLG_PHY_G0L_DBG_SEL_ANALOG_USB20_USB20_ISO_SW_EN 0x1

#define REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_TEST_PIN        0x0000
#define REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_UTMI_CTL1       0x0004
#define REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_BATTER_PLL      0x0008
#define REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_UTMI_CTL2       0x000C
#define REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_TRIMMING        0x0010
#define REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_PLL_CTRL        0x0014
#define REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_PHY_BIST_TEST   0x0018
#define REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_PHY             0x001C
#define REG_ANLG_PHY_G0L_ANALOG_USB20_REG_SEL_CFG_0         0x0020

#define DEFAULT_DEVICE_EYE_PATTERN			0x067bd1c0
#define DEFAULT_HOST_EYE_PATTERN			0x067bd1c0

#define CHGR_DET_FGU_CTRL		0x23a0
#define DP_DM_FS_ENB			BIT(14)
#define DP_DM_BC_ENB			BIT(0)

#define DEFAULT_SSPHY_CTL5_EYE_PATTERN			0x1e40a000
#define DEFAULT_SSPHY_CTL6_EYE_PATTERN			0x5d000083

static int sc27xx_dpdm_switch_to_phy(struct regmap *regmap, bool enable)
{
	/* Do not overwrite the detector delay fields in this shared register. */
	return regmap_update_bits(regmap, CHGR_DET_FGU_CTRL,
		DP_DM_FS_ENB | DP_DM_BC_ENB,
		enable ? DP_DM_FS_ENB | DP_DM_BC_ENB : 0);
}

static void sprd_ssphy_bc12_role(struct sprd_ssphy *phy);

static inline void sprd_ssphy_reset_core(struct sprd_ssphy *phy)
{
	u32 reg, msk;
	int ret = 0;

	dev_dbg(phy->phy.dev, "%s ipa usb&phy rst!\n", __func__);

	/* Purpose: To soft-reset USB control */
	msk = MASK_IPA_APB_USB_SOFT_RST | MASK_IPA_APB_PAM_U3_SOFT_RST;
	reg = msk;
	regmap_update_bits(phy->ipa_apb, REG_IPA_APB_IPA_RST, msk, reg);

	/* Reset USB2 PHY */
	if (true) {
		msk = MASK_AON_APB_OTG_PHY_SOFT_RST | MASK_AON_APB_OTG_UTMI_SOFT_RST;
		reg = msk;
		regmap_update_bits(phy->aon_apb, REG_AON_APB_APB_RST1, msk, reg);
	}

	/* Reset USB31 PHY */
	ret |= regmap_read(phy->ipa_dispc1_glb_apb, DISPC1_GLB_APB_RST, &reg);
	reg |= BIT(4);
	ret |= regmap_write(phy->ipa_dispc1_glb_apb, DISPC1_GLB_APB_RST, reg);
	usleep_range(1, 10);
	/* ssphy power on @0x64900d14*/
	msk = MASK_AON_APB_PHY_TEST_POWERDOWN;
	regmap_update_bits(phy->aon_apb, REG_AON_APB_USB31DPCOMBPHY_CTRL, msk, 0);

	/*
	 *Reset signal should hold on for a while
	 *to issue resret process reliable.
	 */
	usleep_range(20000, 30000);
	msk = MASK_IPA_APB_USB_SOFT_RST | MASK_IPA_APB_PAM_U3_SOFT_RST;
	regmap_update_bits(phy->ipa_apb, REG_IPA_APB_IPA_RST, msk, 0);
	msk = MASK_AON_APB_OTG_PHY_SOFT_RST | MASK_AON_APB_OTG_UTMI_SOFT_RST;
	regmap_update_bits(phy->aon_apb, REG_AON_APB_APB_RST1, msk, 0);
	ret |= regmap_read(phy->ipa_dispc1_glb_apb, DISPC1_GLB_APB_RST, &reg);
	reg &= ~BIT(4);
	ret |= regmap_write(phy->ipa_dispc1_glb_apb, DISPC1_GLB_APB_RST, reg);
}

static int sprd_ssphy_set_vbus(struct usb_phy *x, int on)
{
	struct sprd_ssphy *phy = container_of(x, struct sprd_ssphy, phy);
	u32 reg, msk;
	int ret = 0;
	bool was_host;

	/* (always: bc12_enabled may still go false at the end of the probe, after dwc3 got the PHY) */
	mutex_lock(&phy->dpdm_lock);
	was_host = phy->is_host;
	if (on) {
		regmap_write(phy->ana_g0l, REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_TRIMMING,
					phy->host_eye_pattern);
		/* set USB connector type is A-type*/
		msk = MASK_AON_APB_USB2_PHY_IDDIG;
		ret |= regmap_update_bits(phy->aon_apb,
			REG_AON_APB_OTG_PHY_CTRL, msk, 0);

		msk = BIT_ANLG_PHY_G0L_DBG_SEL_ANALOG_USB20_USB20_DMPULLDOWN |
			BIT_ANLG_PHY_G0L_DBG_SEL_ANALOG_USB20_USB20_DPPULLDOWN;
		ret |= regmap_update_bits(phy->ana_g0l,
			REG_ANLG_PHY_G0L_ANALOG_USB20_REG_SEL_CFG_0,
			msk, msk);

		/* the pull down resistance on D-/D+ enable */
		msk = BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_DMPULLDOWN |
			BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_DPPULLDOWN;
		ret |= regmap_update_bits(phy->ana_g0l,
			REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_UTMI_CTL2,
			msk, msk);

		reg = 0x200;
		msk = BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_RESERVED;
		ret |= regmap_update_bits(phy->ana_g0l,
			REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_UTMI_CTL1,
			msk, reg);
		phy->is_host = true;
	} else {
		regmap_write(phy->ana_g0l, REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_TRIMMING,
					phy->device_eye_pattern);

		if (true) {
			reg = msk = MASK_AON_APB_USB2_PHY_IDDIG;
			ret |= regmap_update_bits(phy->aon_apb,
				REG_AON_APB_OTG_PHY_CTRL, msk, reg);

			msk = BIT_ANLG_PHY_G0L_DBG_SEL_ANALOG_USB20_USB20_DMPULLDOWN |
				BIT_ANLG_PHY_G0L_DBG_SEL_ANALOG_USB20_USB20_DPPULLDOWN;
			ret |= regmap_update_bits(phy->ana_g0l,
				REG_ANLG_PHY_G0L_ANALOG_USB20_REG_SEL_CFG_0,
				msk, msk);

			/* the pull down resistance on D-/D+ enable */
			msk = BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_DMPULLDOWN |
				BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_DPPULLDOWN;
			ret |= regmap_update_bits(phy->ana_g0l,
				REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_UTMI_CTL2,
				msk, 0);

			msk = BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_RESERVED;
			ret |= regmap_update_bits(phy->ana_g0l,
				REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_UTMI_CTL1,
				msk, 0);
		}
		phy->is_host = false;
	}

	if (READ_ONCE(phy->bc12_enabled) && was_host != phy->is_host)
		sprd_ssphy_bc12_role(phy);
	mutex_unlock(&phy->dpdm_lock);
	return ret;
}

/* Requirement from usb analog compliance tests */
static void usb_config_4_cts(struct sprd_ssphy *phy)
{
	u32	reg;

	dev_dbg(phy->phy.dev, "%s enter\n", __func__);

	regmap_read(phy->ipa_usb31_dp, 0x20, &reg);
	/*enable cr apb*/
	reg |= BIT(4);
	regmap_write(phy->ipa_usb31_dp, 0x20, reg);
	/*enable cr clk*/
	reg |= BIT(3);
	regmap_write(phy->ipa_usb31_dp, 0x20, reg);

	/*phy reg addr 0x0021 SUP_DIG_LVL_OVER_IN*/
	reg = 0x10021;
	regmap_write(phy->ipa_usb31_dp, 0x8, reg);
	regmap_read(phy->ipa_usb31_dp, 0xc, &reg);
	/*bit7: TX_VBOOST_LVL_EN; bit6L4=5 TX_TXVBOOST_LVL*/
	reg &= ~(BIT(6) | BIT(5) | BIT(4));
	reg |= BIT(7) | BIT(6) | BIT(4);
	regmap_write(phy->ipa_usb31_dp, 0xc, reg);

	/*cfg for enter gen2*/
	reg = 0x10063;
	regmap_write(phy->ipa_usb31_dp, 0x8, reg);
	regmap_read(phy->ipa_usb31_dp, 0xc, &reg);
	reg |= BIT(8) | BIT(7) | BIT(1) | BIT(0);
	regmap_write(phy->ipa_usb31_dp, 0xc, reg);

	/* Configure usb31 phy eye-pattern */
	regmap_write(phy->ipa_usb31_dp, 0x44, phy->ssphy_ctl5_eye_pattern);
	regmap_write(phy->ipa_usb31_dp, 0x48, phy->ssphy_ctl6_eye_pattern);
}

#define TCA_INTR_STS		0x08
#define TCA_XA_ACK		BIT(0)
#define TCA_XA_TIMEOUT		BIT(1)
#define TCA_TCPC		0x14
#define TCA_TCPC_MUX		GENMASK(1, 0)
#define TCA_TCPC_MUX_NONE	0
#define TCA_TCPC_MUX_USB_DP2	3
#define TCA_TCPC_FLIP		BIT(2)
#define TCA_TCPC_LOW_POWER	BIT(3)
#define TCA_TCPC_VALID		BIT(4)
#define TCA_SYSMODE_CFG		0x18
#define TCA_SYSMODE_FLIP	BIT(2)

#define TCA_XACT_TIMEOUT_MS	3000	/* > the TCA's own xa timeout */
#define TCA_DP_RELEASE_MS	100

/*
 * One TCPC transaction: write @tcpc (VALID set) and wait for the TCA's xa ack or its own timeout event (~2.5 s with
 * CTRLSYNCMODE_CFG1 0x3d090). Never stop waiting before the TCA does: a request written while the previous one is
 * still pending wedged the TCA (no event at all, every later request failed too). Returns INTR_STS, 0 if no event.
 *
 * Controller-synced mode also waits for the DPTX: the TCA asserts typec_disable in its TYPE_C_CTRL and needs the
 * DP lanes quiesced and DISABLE_ACK raised, then dropped once it releases. The DP driver's hook does that; keep
 * calling it until the handshake is back to idle.
 */
static u32 sprd_ssphy_tca_xact(struct sprd_ssphy *phy, u32 tcpc)
{
	unsigned long deadline;
	u32 sts;

	writel_relaxed(0xffff, phy->base + TCA_INTR_STS);
	if (phy->dp_ops)
		phy->dp_ops->tca_ack(phy->dp_data);
	writel_relaxed(tcpc | TCA_TCPC_VALID, phy->base + TCA_TCPC);
	deadline = jiffies + msecs_to_jiffies(TCA_XACT_TIMEOUT_MS);
	for (;;) {
		usleep_range(1000, 2000);
		sts = readl_relaxed(phy->base + TCA_INTR_STS);
		if (sts & (TCA_XA_ACK | TCA_XA_TIMEOUT))
			break;
		if (time_after(jiffies, deadline)) {
			sts = 0;
			break;
		}
		if (phy->dp_ops)
			phy->dp_ops->tca_ack(phy->dp_data);
	}
	writel_relaxed(0xffff, phy->base + TCA_INTR_STS);

	deadline = jiffies + msecs_to_jiffies(TCA_DP_RELEASE_MS);
	while (phy->dp_ops && phy->dp_ops->tca_ack(phy->dp_data) && time_before(jiffies, deadline))
		usleep_range(1000, 2000);
	return sts;
}

/*
 * TCA TCPC (0x14): mux[1:0] 3 = USB3 + DP 2-lane, b4 = VALID (starts the transaction); TCPC b2 and SYSMODE_CFG
 * (0x18) b2 = @flip, set for a reversed plug. Unisoc sets them when AON BOOT_MODE bit10 is clear, which our
 * Type-C port reports as TYPEC_ORIENTATION_REVERSE; a normal plug (bit10 set) runs with both clear, as measured
 * with USB3 SuperSpeed and DP link training both up.
 *
 * The orientation only changes from "no connection": going from a flipped mux 3 straight to a normal mux 3
 * never got the xa ack (timed out every time, the TCA kept the flipped routing). So when the flip changes on a
 * connected mux, pass through mux 0 first. Interrupt enables and the controller-synced mode are set up by
 * sprd_ssphy_hw_init(). pwr_lock held.
 */
static int sprd_ssphy_tca_program(struct sprd_ssphy *phy, bool flip)
{
	u32 tcpc = readl_relaxed(phy->base + TCA_TCPC), sys, sts;

	if (FIELD_GET(TCA_TCPC_MUX, tcpc) != TCA_TCPC_MUX_NONE && !!(tcpc & TCA_TCPC_FLIP) != flip) {
		tcpc &= ~(TCA_TCPC_MUX | TCA_TCPC_LOW_POWER | TCA_TCPC_VALID);
		sts = sprd_ssphy_tca_xact(phy, tcpc | TCA_TCPC_MUX_NONE);
		if (!(sts & TCA_XA_ACK))
			dev_warn(phy->phy.dev, "TCA disconnect before flip: INTR_STS 0x%x\n", sts);
	}

	sys = readl_relaxed(phy->base + TCA_SYSMODE_CFG);
	if (flip)
		sys |= TCA_SYSMODE_FLIP;
	else
		sys &= ~TCA_SYSMODE_FLIP;
	writel_relaxed(sys, phy->base + TCA_SYSMODE_CFG);

	tcpc &= ~(TCA_TCPC_MUX | TCA_TCPC_FLIP | TCA_TCPC_LOW_POWER | TCA_TCPC_VALID);
	tcpc |= TCA_TCPC_MUX_USB_DP2;
	if (flip)
		tcpc |= TCA_TCPC_FLIP;
	sts = sprd_ssphy_tca_xact(phy, tcpc);
	if (sts & TCA_XA_ACK) {
		phy->tca_flip = flip;
		phy->tca_valid = true;
		return 0;
	}
	/* routing unknown: the next orientation report programs it again */
	phy->tca_valid = false;
	dev_dbg(phy->phy.dev, "TCA %s: INTR_STS 0x%x\n", flip ? "flipped" : "normal", sts);
	return -ETIMEDOUT;
}

/* The full PHY bring-up and the TCA in USB3 + DP 2-lane (mux 3); pwr_lock held, PHY not inited. */
static int sprd_ssphy_hw_init(struct sprd_ssphy *phy)
{
	struct usb_phy *x = &phy->phy;
	u32	reg, msk;
	int	ret = 0;
	int timeout;
	bool flip;

	
	/*
	 * Due to chip design, some chips may turn on vddusb by default,
	 * We MUST avoid turning it on twice.
	 */
	if (phy->vdd) {
		ret = regulator_enable(phy->vdd);
		if (ret) {
			dev_err(x->dev,
				"Failed to enable phy->vdd: %d\n", ret);
			return ret;
		}
	}

	
	/* select the IPA_SYS USB controller */
	msk = MASK_AON_APB_USB20_CTRL_MUX_REG;
	reg = msk;
	ret |= regmap_update_bits(phy->aon_apb, REG_AON_APB_AON_SOC_USB_CTRL,
			 msk, reg);

	/* enable analog */
	msk = MASK_AON_APB_CGM_OTG_REF_EN | MASK_AON_APB_CGM_DPHY_REF_EN;
	reg = msk;
	ret |= regmap_update_bits(phy->aon_apb, REG_AON_APB_CGM_REG1, msk, reg);

	/*enable analog:0x64900004*/
	reg = MASK_AON_APB_AON_USB2_TOP_EB | MASK_AON_APB_OTG_PHY_EB |
						MASK_AON_APB_ANA_EB;
	msk = reg;
	ret |= regmap_update_bits(phy->aon_apb, REG_AON_APB_APB_EB1, msk, reg);

	/* utmisrp_bvalid  sys vbus valid:0x64900D14*/
	reg = MASK_AON_APB_SYS_VBUSVALID;
	msk = reg;
	ret |= regmap_update_bits(phy->aon_apb, REG_AON_APB_USB31DPCOMBPHY_CTRL, msk, reg);

	/*  usb eb and usb ref eb :0x25000004*/
	ret |= regmap_read(phy->ipa_apb, REG_IPA_APB_IPA_EB, &reg);
	reg |= MASK_IPA_APB_USB_EB | MASK_IPA_APB_USB_REF_EB;
	ret |= regmap_write(phy->ipa_apb, REG_IPA_APB_IPA_EB, reg);

	/* usb suspend eb :0x64900138*/
	reg = MASK_AON_APB_CGM_USB_SUSPEND_EN;
	msk = reg;
	ret |= regmap_update_bits(phy->aon_apb, REG_AON_APB_CGM_REG1, msk, reg);

	/*
	 * USB2 PHY power on: set pd_l/pd_s firstly, then set iso_sw.
	 * poweroff sequence is opposite
	 */
	msk = MASK_AON_APB_LVDSRF_PD_PD_L | MASK_AON_APB_LVDSRF_PS_PD_S;
	regmap_update_bits(phy->aon_apb, REG_AON_APB_MIPI_CSI_POWER_CTRL, msk, 0);

	ret |= regmap_update_bits(phy->ana_g0l,
		REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_PHY,
		BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_ISO_SW_EN, 0);

	/* enable usb20 ISO AVDD1V8_USB*/
	msk = MASK_AON_APB_USB20_ISO_SW_EN;
	ret |= regmap_update_bits(phy->aon_apb, REG_AON_APB_AON_SOC_USB_CTRL,
			 msk, 0);

	/* ssphy power on @0x64900d14*/
	msk = MASK_AON_APB_PHY_TEST_POWERDOWN;
	regmap_update_bits(phy->aon_apb, REG_AON_APB_USB31DPCOMBPHY_CTRL, msk, 0);

	/*hsphy vbus valid */
	msk = MASK_AON_APB_OTG_VBUS_VALID_PHYREG;
	reg = msk;
	regmap_update_bits(phy->aon_apb, REG_AON_APB_OTG_PHY_TEST, msk, reg);
	/*ssphy vbus valid */
	msk = MASK_AON_APB_SYS_VBUSVALID;
	reg = msk;
	regmap_update_bits(phy->aon_apb, REG_AON_APB_USB31DPCOMBPHY_CTRL, msk, reg);

	msk = BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_VBUSVLDEXT;
	reg = msk;
	ret |= regmap_update_bits(phy->ana_g0l,
			REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_UTMI_CTL1,	msk, reg);

	regmap_write(phy->ana_g0l, REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_TRIMMING,
					phy->device_eye_pattern);

	/* Reset PHY */
	sprd_ssphy_reset_core(phy);

	/* wait phy_sram init down check bit2=1 */
	timeout = PHY_INIT_TIMEOUT;
	while (1) {
		ret |= regmap_read(phy->ipa_usb31_dp, PHY_SRAM_INIT_CHECK_DONE, &reg);
		if ((reg & BIT(2)) == BIT(2)) {
			break;
		} else {
			msleep(1);
			if (!--timeout) {
				dev_err(x->dev, "%s phy sram init failure\n", __func__);
				break;
			}
		}
	}

	usb_config_4_cts(phy);

	/* REG_TYPEC_CTRL: TYPEC_DISABLE_ACK */
	ret |= regmap_read(phy->ipa_usb31_dptx, TYPEC_DISABLE_ACK, &reg);
	reg |= BIT(0);
	ret |= regmap_write(phy->ipa_usb31_dptx, TYPEC_DISABLE_ACK, reg);

	/* TCA GCFG
	bit1:0 => 2b01 op_mode = controller synced mode
	bit3:2 => 2b00 reserved
	bit4 = 1 => USB device mode
	Bit31:5 => 0x0
	*/
	reg = readl_relaxed(phy->base + 0x010);
	reg &= ~(0xffffffc0);
	writel_relaxed(reg, phy->base + 0x010);
	reg &= ~(BIT(0) | BIT(4));
	if (phy->is_host)
		reg |= BIT(0);
	else
		reg |= BIT(0) | BIT(4);
	writel_relaxed(reg, phy->base + 0x010);

	/* TCA CTRLSYNCMODE CFG0 */
	reg = readl_relaxed(phy->base + 0x020);
	reg |= BIT(1) | BIT(2);
	writel_relaxed(reg, phy->base + 0x020);

	/*  TCA CTRLSYNCMODE CFG1 */
	reg = readl_relaxed(phy->base + 0x024);
	reg = 0x3d090;
	writel_relaxed(reg, phy->base + 0x024);

	/* clear TCA int status */
	reg = readl_relaxed(phy->base + 0x08);
	reg = 0xffff;
	writel_relaxed(reg, phy->base + 0x08);

	/* TCA INT EN
	xa_ack_event_en =1
	xa_timeout_event_en=1
	*/
	reg = readl_relaxed(phy->base + 0x04);
	reg |= BIT(0) | BIT(1);
	writel_relaxed(reg, phy->base + 0x04);

	/* usb3 switch port: USB3 + DP 2-lane (remove usbonly, add combophy for dp/usb) */
	if (phy->orientation != TYPEC_ORIENTATION_NONE) {
		flip = phy->orientation == TYPEC_ORIENTATION_REVERSE;
	} else {
		ret |= regmap_read(phy->aon_apb, REG_AON_APB_BOOT_MODE, &reg);
		flip = !(reg & BIT(10));
	}
	phy->tca_valid = false;
	if (phy->dp_ops) {
		/*
		 * With the DP driver bound the DPTX is down here (resume), and the TCA waits for its handshake: the
		 * transaction timed out after ~2.5 s and left USB3 and DP unrouted. Let the DP driver switch the TCA
		 * with the DPTX powered (sprd_ssphy_set_orientation()); its HPD waits for that before link training.
		 */
		phy->dp_ops->tca_request(phy->dp_data);
	} else if (sprd_ssphy_tca_program(phy, flip)) {
		dev_warn(x->dev, "init: TCA %s not acked\n", flip ? "flipped" : "normal");
	}

	if (!phy->pmic) {
		/*
		 *In FPGA platform, Disable low power will take some time
		 *before the DWC3 Core register is accessible.
		 */
		usleep_range(1000, 2000);
	}

	/* USB31 PLL forced on, PHY PCS/UPCS power stable (Unisoc's init, last) */
	if (phy->pmu_apb && READ_ONCE(pmu_pll_force)) {
		regmap_update_bits(phy->pmu_apb, REG_PMU_APB_USB31PLLV_REL_CFG,
				   MASK_PMU_APB_USB31PLLV_FRC_ON, MASK_PMU_APB_USB31PLLV_FRC_ON);
		regmap_update_bits(phy->pmu_apb, REG_PMU_APB_SNPS_PHY_PWR_STABLE,
				   MASK_PMU_APB_REG_USB31_PHY_UPCS_PWR_STABLE |
				   MASK_PMU_APB_REG_USB31_PHY_PCS_PWR_STABLE,
				   MASK_PMU_APB_REG_USB31_PHY_UPCS_PWR_STABLE |
				   MASK_PMU_APB_REG_USB31_PHY_PCS_PWR_STABLE);
		phy->pmu_forced = true;
	}

	atomic_set(&phy->inited, 1);

	return ret;
}

/* Turn off PHY and core; pwr_lock held */
static void sprd_ssphy_hw_shutdown(struct sprd_ssphy *phy)
{
	struct usb_phy *x = &phy->phy;
	u32 msk = 0, reg = 0;

	if (!atomic_read(&phy->inited)) {
		dev_dbg(x->dev, "%s is already shut down\n", __func__);
		return;
	}

		if (true) {
		msk = BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_ISO_SW_EN;
		reg = msk;
		regmap_update_bits(phy->ana_g0l, REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_PHY,
			msk, reg);

		/*hsphy vbus invalid */
		msk = MASK_AON_APB_OTG_VBUS_VALID_PHYREG;
		regmap_update_bits(phy->aon_apb, REG_AON_APB_OTG_PHY_TEST, msk, 0);

		/* disable usb20 ISO*/
		msk = MASK_AON_APB_USB20_ISO_SW_EN;
		reg = msk;
		regmap_update_bits(phy->aon_apb, REG_AON_APB_AON_SOC_USB_CTRL,
				 msk, reg);

		/* hsphy power off */
		msk = MASK_AON_APB_LVDSRF_PD_PD_L | MASK_AON_APB_LVDSRF_PS_PD_S;
		reg = msk;
		regmap_update_bits(phy->aon_apb, REG_AON_APB_MIPI_CSI_POWER_CTRL, msk, reg);

		/* disable usb cgm ref */
		msk = MASK_AON_APB_CGM_OTG_REF_EN | MASK_AON_APB_CGM_DPHY_REF_EN;
		regmap_update_bits(phy->aon_apb, REG_AON_APB_CGM_REG1, msk, 0);

		/*disable analog:0x64900004*/
		msk = MASK_AON_APB_AON_USB2_TOP_EB | MASK_AON_APB_OTG_PHY_EB;
		regmap_update_bits(phy->aon_apb, REG_AON_APB_APB_EB1, msk, 0);
	}

	/*ssphy vbus invalid */
	msk = MASK_AON_APB_SYS_VBUSVALID;
	regmap_update_bits(phy->aon_apb, REG_AON_APB_USB31DPCOMBPHY_CTRL, msk, 0);

	/* Reset USB31 PHY */
	regmap_read(phy->ipa_dispc1_glb_apb, DISPC1_GLB_APB_RST, &reg);
	reg |= BIT(4);
	regmap_write(phy->ipa_dispc1_glb_apb, DISPC1_GLB_APB_RST, reg);

	usleep_range(1, 10);
	/* ssphy power off @0x64900d14*/
	msk = MASK_AON_APB_PHY_TEST_POWERDOWN;
	reg = msk;
	regmap_update_bits(phy->aon_apb, REG_AON_APB_USB31DPCOMBPHY_CTRL, msk, reg);

	/*
	 * Due to chip design, some chips may turn on vddusb by default,
	 * we MUST avoid turning it off twice.
	 */

	if (phy->vdd && regulator_is_enabled(phy->vdd))
		regulator_disable(phy->vdd);

	if (phy->pmu_forced) {
		regmap_update_bits(phy->pmu_apb, REG_PMU_APB_USB31PLLV_REL_CFG,
				   MASK_PMU_APB_USB31PLLV_FRC_ON, 0);
		regmap_update_bits(phy->pmu_apb, REG_PMU_APB_SNPS_PHY_PWR_STABLE,
				   MASK_PMU_APB_REG_USB31_PHY_UPCS_PWR_STABLE |
				   MASK_PMU_APB_REG_USB31_PHY_PCS_PWR_STABLE, 0);
		phy->pmu_forced = false;
	}

	atomic_set(&phy->inited, 0);
	atomic_set(&phy->reset, 0);
}

/* dwc3's usb_phy user */
static int sprd_ssphy_init(struct usb_phy *x)
{
	struct sprd_ssphy *phy = container_of(x, struct sprd_ssphy, phy);
	int ret = 0;

	mutex_lock(&phy->pwr_lock);
	if (atomic_read(&phy->inited))
		dev_info(x->dev, "%s is already inited%s\n", __func__, phy->dp_user ? " (DP)" : "");
	else
		ret = sprd_ssphy_hw_init(phy);
	if (!ret)
		phy->usb_user = true;
	mutex_unlock(&phy->pwr_lock);

	return ret;
}

static void sprd_ssphy_shutdown(struct usb_phy *x)
{
	struct sprd_ssphy *phy = container_of(x, struct sprd_ssphy, phy);

	mutex_lock(&phy->pwr_lock);
	phy->usb_user = false;
	if (phy->dp_user)
		dev_dbg(x->dev, "shutdown deferred: DP holds the PHY\n");
	else
		sprd_ssphy_hw_shutdown(phy);
	mutex_unlock(&phy->pwr_lock);
}

/* --- DisplayPort: a second user of the combo PHY --- */

static int sprd_ssphy_dp_power_on(struct phy *gphy)
{
	struct sprd_ssphy *phy = phy_get_drvdata(gphy);
	int ret = 0;

	mutex_lock(&phy->pwr_lock);
	/* with USB up the PHY and the TCA (mux 3) are ready: nothing is reset under a live USB3 link */
	if (!atomic_read(&phy->inited))
		ret = sprd_ssphy_hw_init(phy);
	if (!ret)
		phy->dp_user = true;
	mutex_unlock(&phy->pwr_lock);

	return ret;
}

static int sprd_ssphy_dp_power_off(struct phy *gphy)
{
	struct sprd_ssphy *phy = phy_get_drvdata(gphy);

	mutex_lock(&phy->pwr_lock);
	phy->dp_user = false;
	if (!phy->usb_user)
		sprd_ssphy_hw_shutdown(phy);
	mutex_unlock(&phy->pwr_lock);

	return 0;
}

/* the PHY's CR registers through the usb31_dp glue's indirect window (address at 0x8, data at 0xc) */
#define USB31_DP_CR_CTRL		0x20
#define USB31_DP_CR_APB_EN		BIT(4)
#define USB31_DP_CR_CLK_EN		BIT(3)
#define USB31_DP_CR_ADDR		0x8
#define USB31_DP_CR_DATA		0xc
#define USB31_DP_CR_SEL			0x10000
#define CR_SUP_DIG_LVL_OVER_IN		0x21
#define CR_TX_VBOOST_LVL_DP		0xf0	/* Unisoc's shipped dptx_link_set_preemp_vswing */
#define CR_DP_TX_SWING			0x9002
#define CR_DP_TX_EQ			0x9003

/* [voltage swing][pre-emphasis]: CR 0x9002, CR 0x9003 (Unisoc dp_link.c); 0 = not a valid DP level pair */
static const u16 sprd_ssphy_dp_levels[4][4][2] = {
	{ { 0x570, 0x2000 }, { 0x5d0, 0x2300 }, { 0x620, 0x2580 }, { 0x6a0, 0x2980 } },
	{ { 0x620, 0x2000 }, { 0x6a0, 0x2400 }, { 0x700, 0x2700 }, { 0, 0 } },
	{ { 0x6e0, 0x2000 }, { 0x760, 0x2400 }, { 0, 0 }, { 0, 0 } },
	{ { 0x7e0, 0x2000 }, { 0, 0 }, { 0, 0 }, { 0, 0 } },
};

static void sprd_ssphy_cr_select(struct sprd_ssphy *phy, u32 addr)
{
	regmap_write(phy->ipa_usb31_dp, USB31_DP_CR_ADDR, USB31_DP_CR_SEL | addr);
}

static int sprd_ssphy_dp_configure(struct phy *gphy, union phy_configure_opts *opts)
{
	struct sprd_ssphy *phy = phy_get_drvdata(gphy);
	struct phy_configure_opts_dp *dp = &opts->dp;
	unsigned int lane, vs, pe;
	int ret = 0;
	u32 reg;

	/* rate and lanes go from the DPTX over its PHY interface: only what the TCA's mux 3 can carry */
	if ((dp->set_lanes || dp->set_voltages) && (dp->lanes < 1 || dp->lanes > gphy->attrs.bus_width))
		return -EINVAL;
	if (dp->set_rate && dp->link_rate != 1620 && dp->link_rate != 2700 && dp->link_rate != 5400)
		return -EINVAL;
	if (!dp->set_voltages)
		return 0;

	/* one setting for the PHY: the last lane's, as Unisoc's driver does */
	lane = dp->lanes - 1;
	vs = dp->voltage[lane];
	pe = dp->pre[lane];
	if (vs > 3 || pe > 3 || !sprd_ssphy_dp_levels[vs][pe][0])
		return -EINVAL;

	mutex_lock(&phy->pwr_lock);
	if (!atomic_read(&phy->inited)) {
		ret = -ENODEV;
		goto out;
	}
	regmap_update_bits(phy->ipa_usb31_dp, USB31_DP_CR_CTRL, USB31_DP_CR_APB_EN, USB31_DP_CR_APB_EN);
	regmap_update_bits(phy->ipa_usb31_dp, USB31_DP_CR_CTRL, USB31_DP_CR_CLK_EN, USB31_DP_CR_CLK_EN);

	sprd_ssphy_cr_select(phy, CR_SUP_DIG_LVL_OVER_IN);
	regmap_read(phy->ipa_usb31_dp, USB31_DP_CR_DATA, &reg);
	regmap_write(phy->ipa_usb31_dp, USB31_DP_CR_DATA, reg | CR_TX_VBOOST_LVL_DP);

	sprd_ssphy_cr_select(phy, CR_DP_TX_SWING);
	regmap_write(phy->ipa_usb31_dp, USB31_DP_CR_DATA, sprd_ssphy_dp_levels[vs][pe][0]);
	sprd_ssphy_cr_select(phy, CR_DP_TX_EQ);
	regmap_write(phy->ipa_usb31_dp, USB31_DP_CR_DATA, sprd_ssphy_dp_levels[vs][pe][1]);
out:
	mutex_unlock(&phy->pwr_lock);
	return ret;
}

static const struct phy_ops sprd_ssphy_dp_ops = {
	.power_on = sprd_ssphy_dp_power_on,
	.power_off = sprd_ssphy_dp_power_off,
	.configure = sprd_ssphy_dp_configure,
	.owner = THIS_MODULE,
};

static void sprd_ssphy_put_dp_phy(void *data)
{
	put_device(data);
}

struct phy *devm_sprd_ssphy_get_dp_phy(struct device *dev)
{
	struct device_node *np = of_parse_phandle(dev->of_node, "dp-usbphy", 0);
	struct platform_device *pdev;
	struct sprd_ssphy *phy;
	struct phy *dp_phy = NULL;
	int ret;

	if (!np)
		return ERR_PTR(-ENODEV);
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);

	/* bound, and done probing (drvdata is set before the DP PHY exists) */
	device_lock(&pdev->dev);
	phy = pdev->dev.driver ? platform_get_drvdata(pdev) : NULL;
	if (phy && phy->dp_phy) {
		dp_phy = phy->dp_phy;
		get_device(&dp_phy->dev);
	}
	device_unlock(&pdev->dev);
	put_device(&pdev->dev);
	if (!dp_phy)
		return ERR_PTR(-EPROBE_DEFER);

	ret = devm_add_action_or_reset(dev, sprd_ssphy_put_dp_phy, &dp_phy->dev);
	if (ret)
		return ERR_PTR(ret);

	return dp_phy;
}
EXPORT_SYMBOL_GPL(devm_sprd_ssphy_get_dp_phy);

int sprd_ssphy_set_orientation(struct phy *dp_phy, enum typec_orientation orientation)
{
	struct sprd_ssphy *phy = phy_get_drvdata(dp_phy);
	bool flip;
	int ret = 0;

	if (orientation == TYPEC_ORIENTATION_NONE)
		return 0;

	mutex_lock(&phy->pwr_lock);
	phy->orientation = orientation;
	/* a PHY that is down programs the TCA from @orientation in its next sprd_ssphy_hw_init() */
	flip = orientation == TYPEC_ORIENTATION_REVERSE;
	if (atomic_read(&phy->inited) && (!phy->tca_valid || phy->tca_flip != flip)) {
		ret = sprd_ssphy_tca_program(phy, flip);
		if (ret)
			dev_err(phy->phy.dev, "TCA orientation %s: no xa ack\n", flip ? "flipped" : "normal");
		else
			dev_info(phy->phy.dev, "TCA orientation %s\n", flip ? "flipped" : "normal");
	}
	mutex_unlock(&phy->pwr_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sprd_ssphy_set_orientation);

void sprd_ssphy_set_dp_ops(struct phy *dp_phy, const struct sprd_ssphy_dp_ops *ops, void *data)
{
	struct sprd_ssphy *phy = phy_get_drvdata(dp_phy);

	mutex_lock(&phy->pwr_lock);
	phy->dp_ops = ops;
	phy->dp_data = data;
	mutex_unlock(&phy->pwr_lock);
}
EXPORT_SYMBOL_GPL(sprd_ssphy_set_dp_ops);

static int sprd_ssphy_notify_connect(struct usb_phy *x,
				enum usb_device_speed speed)
{
	struct sprd_ssphy *phy = container_of(x, struct sprd_ssphy, phy);
	u32 msk = 0, reg = 0;

	if (!atomic_read(&phy->inited)) {
		dev_info(x->dev, "%s phy is not inited!\n", __func__);
		return 0;
	}

	if (phy->is_host)
		return 0;

	/*hsphy vbus valid */
	msk = MASK_AON_APB_OTG_VBUS_VALID_PHYREG;
	reg = msk;
	regmap_update_bits(phy->aon_apb, REG_AON_APB_OTG_PHY_TEST, msk, reg);
	/*ssphy vbus valid */
	msk = MASK_AON_APB_SYS_VBUSVALID;
	reg = msk;
	regmap_update_bits(phy->aon_apb, REG_AON_APB_USB31DPCOMBPHY_CTRL, msk, reg);

	msk = BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_VBUSVLDEXT;
	reg = msk;
	regmap_update_bits(phy->ana_g0l,
			REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_UTMI_CTL1,	msk, reg);
	dev_info(x->dev, "ssphy set vbus valid!\n");
	return 0;
}

static int sprd_ssphy_notify_disconnect(struct usb_phy *x,
				enum usb_device_speed speed)
{
	struct sprd_ssphy *phy = container_of(x, struct sprd_ssphy, phy);
	u32 msk = 0;

	if (!atomic_read(&phy->inited)) {
		dev_info(x->dev, "%s phy is not inited!\n", __func__);
		return 0;
	}

	if (phy->is_host)
		return 0;

	/*hsphy vbus invalid */
	msk = MASK_AON_APB_OTG_VBUS_VALID_PHYREG;
	regmap_update_bits(phy->aon_apb, REG_AON_APB_OTG_PHY_TEST, msk, 0);
	/*ssphy vbus invalid */
	msk = MASK_AON_APB_SYS_VBUSVALID;
	regmap_update_bits(phy->aon_apb, REG_AON_APB_USB31DPCOMBPHY_CTRL, msk, 0);

	msk = BIT_ANLG_PHY_G0L_ANALOG_USB20_USB20_VBUSVLDEXT;
	regmap_update_bits(phy->ana_g0l,
			REG_ANLG_PHY_G0L_ANALOG_USB20_USB20_UTMI_CTL1,	msk, 0);
	dev_info(x->dev, "ssphy set vbus invalid!\n");
	return 0;
}

#include "ums9620-bc12.h"

/* Battery boards detect before data-line handoff, without sleeping in extcon. */
static int sprd_ssphy_vbus_notify(struct notifier_block *nb, unsigned long event, void *data)
{
	struct usb_phy *x = container_of(nb, struct usb_phy, vbus_nb);
	struct sprd_ssphy *phy = container_of(x, struct sprd_ssphy, phy);

	if (phy->bc12_enabled) {
		unsigned long flags;

		spin_lock_irqsave(&phy->bc12_event_lock, flags);
		WRITE_ONCE(phy->vbus_present, !!event);
		WRITE_ONCE(x->chg_type, UNKNOWN_TYPE);
		atomic_inc(&phy->bc12_generation);
		if (READ_ONCE(phy->bc12_ready) && !READ_ONCE(phy->bc12_stopping))
			mod_delayed_work(system_power_efficient_wq, &phy->bc12_work, 0);
		spin_unlock_irqrestore(&phy->bc12_event_lock, flags);
		return NOTIFY_OK;
	}

	dev_info(x->dev, "vbus %s\n", event ? "on" : "off");
	if (event)
		sprd_ssphy_notify_connect(x, USB_SPEED_UNKNOWN);
	else
		sprd_ssphy_notify_disconnect(x, USB_SPEED_UNKNOWN);
	return NOTIFY_OK;
}

static struct regmap *sprd_ssphy_pmic_regmap(struct device *dev)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, "sprd,ump962x-syscon");
	struct platform_device *pdev;
	struct regmap *map = NULL;

	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (pdev && pdev->dev.parent)
		map = dev_get_regmap(pdev->dev.parent, NULL);
	if (pdev)
		put_device(&pdev->dev);
	return map;
}

/*
 * Where VBUS comes from. The F50's device tree gives the PHY the extcon-usb-gpio device; the U30 Air's gives it the
 * PMIC's Type-C block (sprd,sc27xx-typec), which has no driver here, so usb_add_phy_dev() waited for it for ever
 * and there was no USB at all (no network, no serial console). Both boards have the VBUS GPIO node: point the
 * property at it when it names the Type-C block. The property stays with the node, so it is never freed.
 */
static void sprd_ssphy_vbus_source(struct device *dev)
{
	struct device_node *np = dev->of_node, *ext, *gpio;
	struct property *prop;
	bool typec;

	ext = of_parse_phandle(np, "extcon", 0);
	if (!ext)
		return;
	typec = of_device_is_compatible(ext, "sprd,sc27xx-typec");
	of_node_put(ext);
	if (!typec)
		return;

	gpio = of_find_compatible_node(NULL, NULL, "linux,extcon-usb-gpio");
	if (!gpio || !of_device_is_available(gpio) || !gpio->phandle) {
		of_node_put(gpio);
		dev_warn(dev, "extcon is the Type-C block and there is no VBUS GPIO: USB will not come up\n");
		return;
	}
	prop = kzalloc(sizeof(*prop) + sizeof(__be32), GFP_KERNEL);
	if (prop) {
		prop->name = kstrdup("extcon", GFP_KERNEL);
		prop->length = sizeof(__be32);
		prop->value = prop + 1;
		*(__be32 *)prop->value = cpu_to_be32(gpio->phandle);
		if (prop->name && !of_update_property(np, prop))
			dev_info(dev, "VBUS from %pOF (no driver for the Type-C block)\n", gpio);
		else
			dev_warn(dev, "could not point extcon at %pOF\n", gpio);
	}
	of_node_put(gpio);
}

/*
 * dwc3 tells the PHY about a role change through otg_set_vbus() - host: the host eye pattern, an A-type ID and the
 * D+/D- pull-downs; device: back. Without an otg structure it never did, and a host-mode port kept the device
 * settings (a failed first descriptor read, "error -71", and slow transfers).
 */
static int sprd_ssphy_otg_set_vbus(struct usb_otg *otg, bool on)
{
	return sprd_ssphy_set_vbus(otg->usb_phy, on);
}

static int sprd_ssphy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct sprd_ssphy *phy;
	int ret;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	phy->pmic = sprd_ssphy_pmic_regmap(dev);
	if (!phy->pmic)
		return dev_err_probe(dev, -EPROBE_DEFER, "PMIC regmap not ready\n");
	phy->bc12_enabled = sprd_ssphy_has_battery();
	mutex_init(&phy->dpdm_lock);
	mutex_init(&phy->pwr_lock);
	spin_lock_init(&phy->bc12_event_lock);
	INIT_DELAYED_WORK(&phy->bc12_work, sprd_ssphy_bc12_work);
	atomic_set(&phy->bc12_generation, 1);

	phy->base = devm_platform_ioremap_resource_byname(pdev, "phy_glb_regs");
	if (IS_ERR(phy->base))
		return PTR_ERR(phy->base);

#define SYSCON(field, prop)							\
	do {									\
		phy->field = syscon_regmap_lookup_by_phandle(np, prop);		\
		if (IS_ERR(phy->field))						\
			return dev_err_probe(dev, PTR_ERR(phy->field), prop "\n");	\
	} while (0)
	SYSCON(aon_apb, "sprd,syscon-aon-apb");
	SYSCON(ana_g0l, "sprd,syscon-ana-g0l");
	SYSCON(ipa_apb, "sprd,syscon-ipa-apb");
	SYSCON(ipa_dispc1_glb_apb, "sprd,syscon-ipa-dispc1-glb-apb");
	SYSCON(ipa_usb31_dp, "sprd,syscon-ipa-usb31-dp");
	SYSCON(ipa_usb31_dptx, "sprd,syscon-ipa-usb31-dptx");
#undef SYSCON
	/* the PMU writes are an addition: without the syscon USB keeps working as before */
	phy->pmu_apb = syscon_regmap_lookup_by_phandle(np, "sprd,syscon-pmu-apb");
	if (IS_ERR(phy->pmu_apb)) {
		dev_warn(dev, "no PMU APB (%ld): USB31 PLL force-on and PHY power-stable left alone\n",
			 PTR_ERR(phy->pmu_apb));
		phy->pmu_apb = NULL;
	}

	if (of_property_read_u32(np, "sprd,vdd-voltage", &phy->vdd_vol))
		phy->vdd_vol = 3300000;
	phy->vdd = devm_regulator_get_optional(dev, "vdd");
	if (IS_ERR(phy->vdd)) {
		if (PTR_ERR(phy->vdd) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev_warn(dev, "no vdd supply\n");
		phy->vdd = NULL;
	} else if (regulator_set_voltage(phy->vdd, phy->vdd_vol, phy->vdd_vol)) {
		dev_warn(dev, "cannot set vdd to %u uV\n", phy->vdd_vol);
	}

	if (of_property_read_u32(np, "sprd,hsphy-device-eye-pattern", &phy->device_eye_pattern))
		phy->device_eye_pattern = DEFAULT_DEVICE_EYE_PATTERN;
	if (of_property_read_u32(np, "sprd,hsphy-host-eye-pattern", &phy->host_eye_pattern))
		phy->host_eye_pattern = DEFAULT_HOST_EYE_PATTERN;
	if (of_property_read_u32(np, "sprd,ssphy-ctl5-eye-pattern", &phy->ssphy_ctl5_eye_pattern))
		phy->ssphy_ctl5_eye_pattern = DEFAULT_SSPHY_CTL5_EYE_PATTERN;
	if (of_property_read_u32(np, "sprd,ssphy-ctl6-eye-pattern", &phy->ssphy_ctl6_eye_pattern))
		phy->ssphy_ctl6_eye_pattern = DEFAULT_SSPHY_CTL6_EYE_PATTERN;

	/* select the IPA_SYS USB controller */
	regmap_update_bits(phy->aon_apb, REG_AON_APB_AON_SOC_USB_CTRL,
			   MASK_AON_APB_USB20_CTRL_MUX_REG, MASK_AON_APB_USB20_CTRL_MUX_REG);

	platform_set_drvdata(pdev, phy);
	phy->dev = dev;
	phy->phy.dev = dev;
	phy->phy.label = "sprd-ssphy";
	phy->phy.init = sprd_ssphy_init;
	phy->phy.shutdown = sprd_ssphy_shutdown;
	phy->phy.set_vbus = sprd_ssphy_set_vbus;
	phy->phy.type = USB_PHY_TYPE_USB3;
	phy->phy.notify_connect = sprd_ssphy_notify_connect;
	phy->phy.notify_disconnect = sprd_ssphy_notify_disconnect;
	phy->phy.vbus_nb.notifier_call = sprd_ssphy_vbus_notify;
	phy->phy.otg = devm_kzalloc(dev, sizeof(*phy->phy.otg), GFP_KERNEL);
	if (!phy->phy.otg)
		return -ENOMEM;
	phy->phy.otg->usb_phy = &phy->phy;
	phy->phy.otg->set_vbus = sprd_ssphy_otg_set_vbus;

	sprd_ssphy_vbus_source(dev);
	if (phy->bc12_enabled) {
		phy->phy.charger_detect = sprd_ssphy_charger_detect;
		ret = sc27xx_dpdm_switch_to_phy(phy->pmic, false);
		if (!ret)
			ret = regmap_update_bits(phy->pmic, CHGR_DET_FGU_CTRL,
						 GENMASK(11, 8), 2 << 8);
		/*
		 * MU300: kanoqwq's version failed the probe here and below, which leaves the board without USB (no
		 * network, no serial console). USB comes first: D+/D- go to the PHY as on the other boards, and the
		 * charger keeps a USB port's 500 mA (or the Type-C Rp current).
		 */
		if (ret) {
			dev_err(dev, "BC1.2 detector setup failed (%d): D+/D- to USB without charger detection\n", ret);
			phy->bc12_enabled = false;
		}
	}
	ret = usb_add_phy_dev(&phy->phy);
	if (ret)
		return dev_err_probe(dev, ret, "cannot add phy\n");

	if (phy->bc12_enabled && IS_ERR_OR_NULL(phy->phy.edev)) {
		dev_err(dev, "no VBUS extcon: D+/D- to USB without charger detection\n");
		phy->bc12_enabled = false;
	}
	if (phy->bc12_enabled) {
		WRITE_ONCE(phy->bc12_ready, true);
		sprd_ssphy_vbus_notify(&phy->phy.vbus_nb,
			extcon_get_state(phy->phy.edev, EXTCON_USB) > 0, NULL);
	} else {
		/* Unchanged data-only path on F50/V50 and other battery-less boards. */
		sc27xx_dpdm_switch_to_phy(phy->pmic, true);
	}

	/* the DP side: its failure leaves USB as it is */
	phy->dp_phy = devm_phy_create(dev, NULL, &sprd_ssphy_dp_ops);
	if (IS_ERR(phy->dp_phy)) {
		dev_err(dev, "no DP PHY: %ld\n", PTR_ERR(phy->dp_phy));
		phy->dp_phy = NULL;
	} else {
		phy_set_drvdata(phy->dp_phy, phy);
		phy->dp_phy->attrs.bus_width = 2;	/* TCA mux 3: USB3 + 2-lane DP */
		phy->dp_phy->attrs.max_link_rate = 5400;
		phy->dp_phy->attrs.mode = PHY_MODE_DP;
	}
	dev_info(dev, "UMS9620 USB 3.1 PHY registered%s\n", phy->dp_phy ? " (with DP)" : "");
	return 0;
}

static void sprd_ssphy_bc12_stop(struct sprd_ssphy *phy)
{
	unsigned long flags;

	if (phy->bc12_enabled) {
		spin_lock_irqsave(&phy->bc12_event_lock, flags);
		WRITE_ONCE(phy->bc12_stopping, true);
		spin_unlock_irqrestore(&phy->bc12_event_lock, flags);
		devm_extcon_unregister_notifier(phy->dev, phy->phy.edev,
					       EXTCON_USB, &phy->phy.vbus_nb);
		cancel_delayed_work_sync(&phy->bc12_work);
		cancel_work_sync(&phy->phy.chg_work);
		regmap_update_bits(phy->pmic, UMP9620_BC12_CTRL2, BIT(0), 0);
		sc27xx_dpdm_switch_to_phy(phy->pmic, false);
	}
}

static void sprd_ssphy_shutdown_device(struct platform_device *pdev)
{
	sprd_ssphy_bc12_stop(platform_get_drvdata(pdev));
}

static void sprd_ssphy_remove(struct platform_device *pdev)
{
	struct sprd_ssphy *phy = platform_get_drvdata(pdev);

	sprd_ssphy_bc12_stop(phy);
	usb_remove_phy(&phy->phy);
}

static const struct of_device_id sprd_ssphy_match[] = {
	{ .compatible = "sprd,qogirn6pro-ssphy" },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_ssphy_match);

static struct platform_driver sprd_ssphy_driver = {
	.probe = sprd_ssphy_probe,
	.remove = sprd_ssphy_remove,
	.shutdown = sprd_ssphy_shutdown_device,
	.driver = { .name = "sprd-ssphy", .of_match_table = sprd_ssphy_match },
};
module_platform_driver(sprd_ssphy_driver);

MODULE_DESCRIPTION("Unisoc UMS9620 USB 3.1 PHY driver");
MODULE_LICENSE("GPL");
