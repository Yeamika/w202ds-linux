/* SPDX-License-Identifier: GPL-2.0 */
/*
 * W200DS (UMS9620) USB-C DisplayPort: DPU1 + DW DPTX glue.
 */
#ifndef __W202_DP_H__
#define __W202_DP_H__

#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/usb/typec.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <drm/drm_bridge.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/bridge/dw_dp.h>
#include <drm/drm_plane.h>

struct clk;
struct clk_bulk_data;
struct device_node;
struct drm_bridge;
struct gpio_desc;
struct i2c_client;
struct phy;
struct regmap;
struct typec_mux_dev;
struct typec_switch_dev;

/* ---- PMU APB (sprd,syscon-force-shutdown, 0x64910000) ---- */
#define PMU_PD_DPU_DP_CFG_0		0x03e4
#define PMU_PD_DPU_DP_FORCE_SHUTDOWN	BIT(25)
#define PMU_PWR_STATUS_DBG_22		0x0548
#define PMU_PD_DPU_DP_STATE		GENMASK(20, 16)
#define PMU_PD_IPA_STATE		GENMASK(12, 8)

/* ---- AON APB (0x64900000) ---- */
#define AON_BOOT_MODE			0x002c
#define AON_BOOT_MODE_CC_ORIENT		BIT(10)

/* ---- ipa dispc1 glb APB (0x31800000) ---- */
#define DISPC1_HPD_UPDATE		0x0050
#define DISPC1_HPD_IRQ			BIT(0)
#define DISPC1_HPD_STATE		BIT(1)
#define DISPC1_HPD_UPDATE_STROBE	BIT(2)
#define DISPC1_DPTX_CONFIG_EN		BIT(3)
#define DISPC1_DPTX_CTRL		0x00b0

/* ---- usb31 dp phy syscon (0x31810000) ---- */
#define USB31DP_AUX_PHY_CFG		0x005c
#define USB31DP_AUX_PHY_PWDNB		BIT(10)

/* ---- DPTX (0x31890000), registers the glue touches directly ---- */
#define DPTX_VERSION_NUMBER		0x0000
#define DPTX_VERSION_TYPE		0x0004
#define DPTX_ID				0x0008
#define DPTX_CONFIG1			0x0100
#define DPTX_VSAMPLE_CTRL		0x0300
#define DPTX_VIDEO_STREAM_ENABLE	BIT(5)
#define DPTX_VINPUT_POLARITY_CTRL	0x030c
#define DPTX_VSYNC_IN_POLARITY		BIT(0)
#define DPTX_HSYNC_IN_POLARITY		BIT(1)
#define DPTX_TYPE_C_CTRL		0x0c08
#define DPTX_TYPEC_DISABLE_ACK		BIT(0)
#define DPTX_TYPEC_DISABLE_STATUS	BIT(1)
#define DPTX_TYPEC_CTRL_ACK		0x5	/* shipped dw_dptx.c: DISABLE_ACK | b2 */
#define DPTX_TYPEC_CTRL_RELEASE		0x4
#define DPTX_PHYIF_CTRL			0x0a00
#define DPTX_PHYIF_BUSY			GENMASK(15, 12)
#define DPTX_PHYIF_LANE_PWRDOWN		GENMASK(20, 17)
#define DPTX_ISTS			0x0d00
#define DPTX_IEN			0x0d04
#define DPTX_IEN_TYPE_C			BIT(7)
#define DPTX_HPDSTS			0x0d08
#define DPTX_MAP_SIZE			0x1000

/* ---- DPU1 (dpu-lite-r3p0, 0x31880000) ---- */
#define DPU_VERSION			0x0000
#define DPU_CTRL			0x0004
#define DPU_CTRL_RUN			BIT(0)
#define DPU_CTRL_STOP			BIT(1)
#define DPU_CTRL_REG_UPDATE		BIT(2)
#define DPU_CFG0			0x0008
#define DPU_CFG1			0x000c
#define DPU_MODE			0x0010
#define DPU_MODE_BYPASS			BIT(0)
#define DPU_PANEL_SIZE			0x0020
#define DPU_BG_COLOR			0x002c
#define DPU_LAY_BASE_ADDR		0x0030
#define DPU_LAY_CTRL			0x0040
#define DPU_LAY_SIZE			0x0044
#define DPU_LAY_PITCH			0x0048
#define DPU_LAY_POS			0x004c
#define DPU_LAY_ALPHA			0x0050
#define DPU_LAY_CROP_START		0x005c
#define DPU_LAY_STRIDE			0x0030
#define DPU_LAY_COUNT			8
#define DPU_LAY_EN			BIT(0)
#define DPU_LAY_LAYER_ALPHA		(0x1 << 2)
#define DPU_LAY_FORMAT_ARGB8888		(0x3 << 4)
#define DPU_INT_EN			0x01e0
#define DPU_INT_CLR			0x01e4
#define DPU_INT_STS			0x01e8
#define DPU_INT_DONE			BIT(0)
#define DPU_INT_ERR			BIT(2)
#define DPU_INT_UPDATE_DONE		BIT(4)
#define DPU_INT_VSYNC			BIT(5)
#define DPU_DPI_CTRL			0x01f0
#define DPU_DPI_HSYNC_POL		BIT(0)
#define DPU_DPI_VSYNC_POL		BIT(1)
#define DPU_DPI_H_TIMING		0x01f4
#define DPU_DPI_V_TIMING		0x01f8
#define DPU_CURSOR_EN			0x0400
#define DPU_SDP_CFG			0x0420
#define DPU_MMU_EN			0x0804
#define DPU_MAP_SIZE			0x3000

/* power users (bit numbers in w202_dp.pwr_users) */
enum w202_pwr_user {
	W202_PWR_PROBE,
	W202_PWR_HPD,
	W202_PWR_CRTC,
	W202_PWR_BRIDGE,
	W202_PWR_FORCE,
	W202_PWR_TCA,
};

enum w202_hpd_event {
	W202_HPD_UNPLUG,
	W202_HPD_PLUG,
	W202_HPD_IRQ,
};

struct w202_redriver {
	struct i2c_client *client;
	const char *of_path;
	bool present;		/* ACKed since the current power-up */
	bool id_logged;
};

struct w202_dp {
	struct drm_device drm;
	struct device *dev;
	struct device_node *dpu_np;

	struct regmap *pmu;
	struct regmap *aon;
	struct regmap *dispc1;
	struct regmap *usb31dp;
	void __iomem *dptx;
	void __iomem *dpu;
	int dptx_irq;
	int dpu_irq;

	struct clk_bulk_data *dp_clks;
	int num_dp_clks;
	struct clk *dispc1_eb;
	struct clk *dpu_core;
	struct clk *dpu_dpi;
	struct clk *pixelpll;
	struct clk *src_192m;
	struct clk *src_512m;

	struct phy *phy;
	struct gpio_desc *gpio_en2;

	struct dw_dp *dwdp;
	struct dw_dp_plat_data plat;
	struct drm_bridge *dw_bridge;
	struct drm_bridge *guard;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector *connector;
	struct fwnode_handle *conn_fwnode;
	struct fwnode_handle *mux_fwnode;

	/* PD_DPU_DP power state */
	struct mutex pwr_lock;
	unsigned long pwr_users;
	bool powered;
	bool phy_on;
	bool dptx_irq_on;
	spinlock_t hw_lock;
	int hw_users;
	bool hw_closing;
	wait_queue_head_t hw_wq;

	/* USB-C state, protected by pwr_lock */
	bool hpd;
	enum typec_orientation orientation;
	int applied_dir;
	struct typec_switch_dev *sw;
	struct typec_mux_dev *mux;
	struct delayed_work hpd_notify_work;
	struct delayed_work restore_work;
	struct work_struct tca_work;	/* TCA switch for @orientation */

	struct w202_redriver pi3dpx;
	struct w202_redriver ptn;

	/* DPU1 state */
	spinlock_t dpu_lock;
	bool dpu_enabled;
	bool dpu_running;
	u32 dpu_int_en;
	struct completion dpu_stop_done;
	struct drm_pending_vblank_event *event;
	unsigned long pclk_hz;
};

static inline struct w202_dp *drm_to_w202(struct drm_device *drm)
{
	return container_of(drm, struct w202_dp, drm);
}

/* module parameters shared between files */
extern int w202_sync_pol_vendor;
extern bool w202_forward_irq_hpd;
extern bool w202_pi3dpx_reg08;
extern int w202_redriver_dir;

/* w202_dp_main.c */
int w202_dp_power_get(struct w202_dp *dp, enum w202_pwr_user user);
void w202_dp_power_put(struct w202_dp *dp, enum w202_pwr_user user);
int w202_dp_hw_get(void *priv);
void w202_dp_hw_put(void *priv);
int w202_dp_hpd_event(struct w202_dp *dp, enum w202_hpd_event ev);
void w202_dp_set_orientation(struct w202_dp *dp, enum typec_orientation o);

/* w202_dp_dpu1.c */
int w202_dpu1_init(struct w202_dp *dp);
void w202_dpu1_run(struct w202_dp *dp);
void w202_dpu1_stop(struct w202_dp *dp);
enum drm_mode_status w202_dpu1_mode_valid(struct w202_dp *dp,
					  const struct drm_display_mode *mode);

/* w202_dp_typec.c */
int w202_typec_register(struct w202_dp *dp);
void w202_typec_unregister(struct w202_dp *dp);

/* w202_dp_redriver.c */
void w202_redriver_init(struct w202_dp *dp);
void w202_redriver_release(struct w202_dp *dp);
void w202_redriver_dp_on(struct w202_dp *dp, int dir);
void w202_redriver_dp_off(struct w202_dp *dp);
bool w202_redriver_pi3dpx_present(struct w202_dp *dp);
ssize_t w202_redriver_show(struct w202_dp *dp, char *buf, ssize_t len);

#endif /* __W202_DP_H__ */
