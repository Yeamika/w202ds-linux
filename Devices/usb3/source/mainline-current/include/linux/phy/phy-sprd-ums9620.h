/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The DisplayPort side of the Unisoc UMS9620 USB 3.1/DP combo PHY (drivers/usb/phy/phy-sprd-ums9620-ssphy.c).
 */
#ifndef __PHY_SPRD_UMS9620_H
#define __PHY_SPRD_UMS9620_H

#include <linux/err.h>
#include <linux/usb/typec.h>

struct device;
struct phy;

struct sprd_ssphy_dp_ops {
	bool (*tca_ack)(void *data);
	void (*tca_request)(void *data);
};

#if IS_ENABLED(CONFIG_USB_SPRD_UMS9620_SSPHY)
/*
 * The combo PHY's DP generic PHY for the DPTX device @dev, whose node names the PHY with "dp-usbphy" (phandle 0).
 * -ENODEV without that property, -EPROBE_DEFER until the PHY driver is bound. The device reference is dropped
 * when @dev's devres is released.
 */
struct phy *devm_sprd_ssphy_get_dp_phy(struct device *dev);

/*
 * The plug orientation from the Type-C port (TCPM). The TCA routes USB3 and the two DP lanes by it: switched here
 * when the PHY is up and the orientation changed, else used by the next PHY init instead of the AON BOOT_MODE
 * latch. NONE (detach) keeps the last orientation. The switch is a TCA transaction that includes the DPTX's
 * handshake (sprd_ssphy_set_dp_ops()), so the caller holds the DPTX powered; it sleeps, ~0.1 s, and up to the
 * TCA's ~2.5 s timeout when it fails (-ETIMEDOUT, routing unchanged).
 */
int sprd_ssphy_set_orientation(struct phy *dp_phy, enum typec_orientation orientation);

/*
 * The DPTX side of the TCA, registered by the DP driver (NULL unregisters). Both run in process context under the
 * PHY's lock and must not call back into the PHY.
 * @tca_ack: services one step of the TCA's handshake with the DPTX; true while it is not back to idle (DP lanes
 *	quiesced and DISABLE_ACK raised, or the ack still to be dropped). Called before each TCPC request, while the
 *	PHY waits for the TCA and until it returns false after.
 * @tca_request: a PHY init (resume) left the TCA unrouted, as the DPTX is down there and cannot ack: switch it with
 *	the DPTX powered, by sprd_ssphy_set_orientation() for the current orientation, from another context.
 */

void sprd_ssphy_set_dp_ops(struct phy *dp_phy, const struct sprd_ssphy_dp_ops *ops, void *data);

#else
static inline struct phy *devm_sprd_ssphy_get_dp_phy(struct device *dev)
{
	return ERR_PTR(-ENODEV);
}

static inline int sprd_ssphy_set_orientation(struct phy *dp_phy, enum typec_orientation orientation)
{
	return 0;
}

static inline void sprd_ssphy_set_dp_ops(struct phy *dp_phy, const struct sprd_ssphy_dp_ops *ops, void *data)
{
}
#endif

#endif /* __PHY_SPRD_UMS9620_H */
