// SPDX-License-Identifier: GPL-2.0
/*
 * typec_switch + typec_mux providers on the "w202-usbc-mux" software node registered by
 * ump9620_tcpc (contract.md §1/§2). Phase 1 accepts DP pin assignment D only (2 lanes + USB3,
 * TCA mux 3 as already programmed by the ssphy); C/E (4 lanes) are refused.
 */

#include <linux/usb/typec_dp.h>
#include <linux/phy/phy-sprd-ums9620.h>
#include <linux/usb/typec_mux.h>

#include "w202_dp.h"

static int w202_switch_set(struct typec_switch_dev *sw,
			   enum typec_orientation orientation)
{
	struct w202_dp *dp = typec_switch_get_drvdata(sw);

	/* TCPM sets the orientation on attach, before the USB role and DP HPD: route USB3 and DP lanes first */
	w202_dp_set_orientation(dp, orientation);
	if (orientation != TYPEC_ORIENTATION_NONE)
		queue_work(system_wq, &dp->tca_work);
	return 0;
}

static int w202_mux_set(struct typec_mux_dev *mux, struct typec_mux_state *state)
{
	struct w202_dp *dp = typec_mux_get_drvdata(mux);
	struct typec_displayport_data *data;
	bool hpd, irq;
	int ret;

	/* TCPM typec_set_mode() (alt == NULL) or DP alt mode SAFE/USB: DP is gone */
	if (state->mode == TYPEC_STATE_SAFE || state->mode == TYPEC_STATE_USB)
		return w202_dp_hpd_event(dp, W202_HPD_UNPLUG);

	if (!state->alt || state->alt->svid != USB_TYPEC_DP_SID) {
		dev_dbg(dp->dev, "mux: ignoring mode %lu for svid 0x%04x\n", state->mode,
			state->alt ? state->alt->svid : 0);
		return 0;
	}

	if (state->mode != TYPEC_DP_STATE_D) {
		dev_warn(dp->dev,
			 "DP pin assignment %c refused: phase 1 supports pin D (2 lanes + USB3) only\n",
			 state->mode >= TYPEC_DP_STATE_A && state->mode <= TYPEC_DP_STATE_F ?
			 (char)('A' + state->mode - TYPEC_DP_STATE_A) : '?');
		w202_dp_hpd_event(dp, W202_HPD_UNPLUG);
		return -EOPNOTSUPP;
	}

	data = state->data;
	if (!data)
		return 0;
	hpd = data->status & DP_STATUS_HPD_STATE;
	irq = data->status & DP_STATUS_IRQ_HPD;
	dev_dbg(dp->dev, "mux: pin D status 0x%08x conf 0x%08x\n", data->status, data->conf);

	if (!hpd)
		return w202_dp_hpd_event(dp, W202_HPD_UNPLUG);
	ret = w202_dp_hpd_event(dp, W202_HPD_PLUG);
	if (ret || !irq)
		return ret;
	return w202_dp_hpd_event(dp, W202_HPD_IRQ);
}

int w202_typec_register(struct w202_dp *dp)
{
	struct typec_switch_desc sw_desc = {
		.fwnode = dp->mux_fwnode,
		.set = w202_switch_set,
		.name = "w202-dp-orientation",
		.drvdata = dp,
	};
	struct typec_mux_desc mux_desc = {
		.fwnode = dp->mux_fwnode,
		.set = w202_mux_set,
		.name = "w202-dp-mode",
		.drvdata = dp,
	};

	dp->sw = typec_switch_register(dp->dev, &sw_desc);
	if (IS_ERR(dp->sw))
		return dev_err_probe(dp->dev, PTR_ERR(dp->sw), "typec switch\n");

	dp->mux = typec_mux_register(dp->dev, &mux_desc);
	if (IS_ERR(dp->mux)) {
		typec_switch_unregister(dp->sw);
		dp->sw = NULL;
		return dev_err_probe(dp->dev, PTR_ERR(dp->mux), "typec mux\n");
	}
	return 0;
}

void w202_typec_unregister(struct w202_dp *dp)
{
	if (dp->mux)
		typec_mux_unregister(dp->mux);
	if (dp->sw)
		typec_switch_unregister(dp->sw);
	dp->mux = NULL;
	dp->sw = NULL;
}
