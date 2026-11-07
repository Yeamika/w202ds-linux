// SPDX-License-Identifier: GPL-2.0
/*
 * W200DS DPU1 (Unisoc dpu-lite-r3p0 @ 0x31880000): one CRTC + one primary plane.
 *
 * Register map and sequences: vendor dpu_lite_r3p0.c (dpu_init :409-437, dpu_layer :684-764,
 * dpu_flip :766-812, dpu_dpi_init :827-876, dpu_isr :249-330, run/stop) and qogirn6pro/global_dpu1.c
 * (clock selection :145-267). Scanout is physical (GEM-DMA/CMA); the DPU1-internal MMU is
 * switched off (MMU_EN 0x804 = 0, as mainline sprd_dpu.c does at init).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of_irq.h>
#include <linux/sizes.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include "w202_dp.h"

/* global_dpu1.c: dpi source 297 MHz = pixelpll path, pixelpll = dpi * (297M / dpi) * 9 */
#define DPI_SRC_HZ		297000000UL
#define PIXELPLL_POST_DIV	9
#define DPI_DIV_MAX		16	/* ipa-dpi divider: 4 bits */
#define PIXELPLL_MIN_HZ		800000000UL	/* fvco 1.6 GHz with the /2 post divider */
#define PIXELPLL_IDLE_HZ	2673000000UL	/* global_dpu1.c dpu_clk_disable() */
/* phase 1 link: HBR2 x 2 lanes, 8b/10b, RGB 8 bpc */
#define LINK_PAYLOAD_KBPS	(540000ULL * 10 * 2 * 8 / 10)

/* vendor dpu_context_init() QoS defaults (no sprd,qos node on the W200DS) */
#define DPU_CFG1_VALUE		((0xa << 12) | (0xa << 8) | (0xd << 4) | 0xc | \
				 BIT(22) | BIT(23) | BIT(24))

static const u32 w202_dpu1_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static inline void dpu_write(struct w202_dp *dp, u32 reg, u32 val)
{
	writel_relaxed(val, dp->dpu + reg);
}

static inline u32 dpu_read(struct w202_dp *dp, u32 reg)
{
	return readl_relaxed(dp->dpu + reg);
}

static int w202_dpu1_calc_clk(unsigned long pclk, unsigned long *pll_hz,
			      unsigned long *dpi_hz)
{
	unsigned long div;

	if (!pclk || pclk > DPI_SRC_HZ)
		return -ERANGE;
	div = min_t(unsigned long, DPI_SRC_HZ / pclk, DPI_DIV_MAX);
	*pll_hz = pclk * div * PIXELPLL_POST_DIV;
	*dpi_hz = pclk * PIXELPLL_POST_DIV;
	return 0;
}

enum drm_mode_status w202_dpu1_mode_valid(struct w202_dp *dp,
					  const struct drm_display_mode *mode)
{
	unsigned long pclk = mode->clock * 1000UL, pll, dpi;
	long rounded;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		return MODE_NO_INTERLACE;
	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		return MODE_NO_DBLESCAN;
	if (mode->hdisplay > 4096 || mode->vdisplay > 4096)
		return MODE_BAD;
	/* DPI_H/V_TIMING: sync [7:0], back porch [19:8], front porch [31:20] */
	if (mode->hsync_end - mode->hsync_start > 0xff ||
	    mode->htotal - mode->hsync_end > 0xfff ||
	    mode->hsync_start - mode->hdisplay > 0xfff)
		return MODE_HBLANK_WIDE;
	if (mode->vsync_end - mode->vsync_start > 0xff ||
	    mode->vtotal - mode->vsync_end > 0xfff ||
	    mode->vsync_start - mode->vdisplay > 0xfff)
		return MODE_VBLANK_WIDE;
	/* RGB888 over HBR2 x 2 */
	if ((u64)mode->clock * 24 > LINK_PAYLOAD_KBPS)
		return MODE_CLOCK_HIGH;
	/* no bypass/YUV420 path: dpi = pclk, from the 297 MHz (pixelpll) source */
	if (w202_dpu1_calc_clk(pclk, &pll, &dpi))
		return MODE_CLOCK_HIGH;
	if (pll < PIXELPLL_MIN_HZ)
		return MODE_CLOCK_LOW;
	rounded = clk_round_rate(dp->pixelpll, pll);
	if (rounded <= 0 || abs_diff((unsigned long)rounded, pll) > pll / 200)
		return MODE_CLOCK_RANGE;
	return MODE_OK;
}

static enum drm_mode_status w202_crtc_mode_valid(struct drm_crtc *crtc,
						 const struct drm_display_mode *mode)
{
	return w202_dpu1_mode_valid(drm_to_w202(crtc->dev), mode);
}

static void w202_dpu1_send_event(struct w202_dp *dp,
				 struct drm_pending_vblank_event *event)
{
	unsigned long flags;

	spin_lock_irqsave(&dp->drm.event_lock, flags);
	drm_crtc_send_vblank_event(&dp->crtc, event);
	spin_unlock_irqrestore(&dp->drm.event_lock, flags);
}

static irqreturn_t w202_dpu1_irq(int irq, void *data)
{
	struct w202_dp *dp = data;
	struct drm_pending_vblank_event *event = NULL;
	u32 sts;

	spin_lock(&dp->dpu_lock);
	if (!dp->dpu_enabled) {
		spin_unlock(&dp->dpu_lock);
		return IRQ_NONE;
	}
	sts = dpu_read(dp, DPU_INT_STS);
	dpu_write(dp, DPU_INT_CLR, sts);
	if (sts & DPU_INT_ERR) {
		/* underflow: masked until the next flip, as dpu_isr() does */
		dp->dpu_int_en &= ~DPU_INT_ERR;
		dpu_write(dp, DPU_INT_EN, dp->dpu_int_en);
	}
	if ((sts & DPU_INT_UPDATE_DONE) && dp->event) {
		event = dp->event;
		dp->event = NULL;
	}
	spin_unlock(&dp->dpu_lock);

	if (!sts)
		return IRQ_NONE;
	if (sts & DPU_INT_ERR)
		drm_err_ratelimited(&dp->drm, "DPU1 underflow/error, INT_STS 0x%x\n", sts);
	if (sts & DPU_INT_VSYNC)
		drm_crtc_handle_vblank(&dp->crtc);
	if (event)
		w202_dpu1_send_event(dp, event);
	if (sts & DPU_INT_DONE)
		complete(&dp->dpu_stop_done);
	return IRQ_HANDLED;
}

static int w202_dpu1_clk_on(struct w202_dp *dp, unsigned long pclk)
{
	unsigned long pll, dpi;
	int ret;

	ret = w202_dpu1_calc_clk(pclk, &pll, &dpi);
	if (ret)
		return ret;

	ret = clk_prepare_enable(dp->dispc1_eb);
	if (ret)
		return ret;
	/* calc_dpu_core_clk(): last entry = 512 MHz */
	ret = clk_set_parent(dp->dpu_core, dp->src_512m);
	if (ret)
		drm_warn(&dp->drm, "dpu_core parent 512M: %d\n", ret);
	ret = clk_set_parent(dp->dpu_dpi, dp->pixelpll);
	if (ret)
		drm_warn(&dp->drm, "dpu_dpi parent pixelpll: %d\n", ret);
	ret = clk_set_rate(dp->pixelpll, pll);
	if (ret)
		drm_err(&dp->drm, "pixelpll %lu Hz: %d\n", pll, ret);
	ret = clk_set_rate(dp->dpu_dpi, dpi);
	if (ret)
		drm_err(&dp->drm, "dpu_dpi %lu Hz: %d\n", dpi, ret);
	drm_dbg_kms(&dp->drm, "pclk %lu: pixelpll %lu (got %lu) dpi %lu (got %lu)\n",
		    pclk, pll, clk_get_rate(dp->pixelpll), dpi, clk_get_rate(dp->dpu_dpi));

	ret = clk_prepare_enable(dp->dpu_core);
	if (ret)
		goto err_eb;
	ret = clk_prepare_enable(dp->pixelpll);
	if (ret)
		goto err_core;
	ret = clk_prepare_enable(dp->dpu_dpi);
	if (ret)
		goto err_pll;
	return 0;

err_pll:
	clk_disable_unprepare(dp->pixelpll);
err_core:
	clk_disable_unprepare(dp->dpu_core);
err_eb:
	clk_disable_unprepare(dp->dispc1_eb);
	return ret;
}

static void w202_dpu1_clk_off(struct w202_dp *dp)
{
	/* global_dpu1.c dpu_clk_disable(): gate, then restore the idle parents/rates */
	clk_disable_unprepare(dp->dpu_dpi);
	clk_disable_unprepare(dp->pixelpll);
	clk_disable_unprepare(dp->dpu_core);
	clk_set_parent(dp->dpu_dpi, dp->pixelpll);
	clk_set_parent(dp->dpu_core, dp->src_192m);
	if (clk_set_rate(dp->pixelpll, PIXELPLL_IDLE_HZ))
		drm_warn(&dp->drm, "pixelpll idle rate restore failed\n");
	if (clk_set_rate(dp->dpu_dpi, PIXELPLL_IDLE_HZ))
		drm_warn(&dp->drm, "dpu_dpi idle rate restore failed\n");
	clk_disable_unprepare(dp->dispc1_eb);
}

static void w202_crtc_atomic_enable(struct drm_crtc *crtc,
				    struct drm_atomic_state *state)
{
	struct w202_dp *dp = drm_to_w202(crtc->dev);
	const struct drm_display_mode *m = &crtc->state->adjusted_mode;
	u32 dpi_ctrl, val;
	int i, ret;

	ret = w202_dp_power_get(dp, W202_PWR_CRTC);
	if (ret) {
		drm_err(&dp->drm, "PD_DPU_DP power-up failed: %d\n", ret);
		return;
	}
	dp->pclk_hz = m->clock * 1000UL;
	ret = w202_dpu1_clk_on(dp, dp->pclk_hz);
	if (ret) {
		drm_err(&dp->drm, "DPU1 clocks: %d\n", ret);
		w202_dp_power_put(dp, W202_PWR_CRTC);
		return;
	}

	/* dpu_init() */
	dpu_write(dp, DPU_BG_COLOR, 0);
	dpu_write(dp, DPU_PANEL_SIZE, (m->vdisplay << 16) | m->hdisplay);
	dpu_write(dp, DPU_CFG0, 0);
	dpu_write(dp, DPU_CFG1, DPU_CFG1_VALUE);
	for (i = 0; i < DPU_LAY_COUNT; i++)
		dpu_write(dp, DPU_LAY_CTRL + i * DPU_LAY_STRIDE, 0);
	dpu_write(dp, DPU_INT_CLR, 0xffff);
	dpu_write(dp, DPU_SDP_CFG, 0);
	dpu_write(dp, DPU_CURSOR_EN, 0);
	dpu_write(dp, DPU_MMU_EN, 0);

	/* dpu_dpi_init(); global_dp.c dp_detect() forces DPI_CTRL = 3 ("dpi polarity workaround") */
	if (w202_sync_pol_vendor) {
		dpi_ctrl = DPU_DPI_HSYNC_POL | DPU_DPI_VSYNC_POL;
	} else {
		dpi_ctrl = 0;
		if (!(m->flags & DRM_MODE_FLAG_NHSYNC))
			dpi_ctrl |= DPU_DPI_HSYNC_POL;
		if (!(m->flags & DRM_MODE_FLAG_NVSYNC))
			dpi_ctrl |= DPU_DPI_VSYNC_POL;
	}
	dpu_write(dp, DPU_DPI_CTRL, dpi_ctrl);
	val = (m->hsync_end - m->hsync_start) |
	      (m->htotal - m->hsync_end) << 8 |
	      (m->hsync_start - m->hdisplay) << 20;
	dpu_write(dp, DPU_DPI_H_TIMING, val);
	val = (m->vsync_end - m->vsync_start) |
	      (m->vtotal - m->vsync_end) << 8 |
	      (m->vsync_start - m->vdisplay) << 20;
	dpu_write(dp, DPU_DPI_V_TIMING, val);
	if (m->vsync_end - m->vsync_start + m->vtotal - m->vsync_end < 32)
		drm_warn(&dp->drm, "vsync + vbp < 32: DPU1 underflow risk\n");
	dpu_write(dp, DPU_MODE, dpu_read(dp, DPU_MODE) & ~DPU_MODE_BYPASS);

	spin_lock_irq(&dp->dpu_lock);
	dp->dpu_int_en |= DPU_INT_DONE | DPU_INT_ERR | DPU_INT_UPDATE_DONE;
	dpu_write(dp, DPU_INT_EN, dp->dpu_int_en);
	dp->dpu_enabled = true;
	dp->dpu_running = false;
	spin_unlock_irq(&dp->dpu_lock);
	enable_irq(dp->dpu_irq);

	drm_crtc_vblank_on(crtc);
}

void w202_dpu1_run(struct w202_dp *dp)
{
	spin_lock_irq(&dp->dpu_lock);
	if (dp->dpu_enabled && !dp->dpu_running) {
		dpu_write(dp, DPU_CTRL, dpu_read(dp, DPU_CTRL) | DPU_CTRL_RUN);
		dp->dpu_running = true;
	}
	spin_unlock_irq(&dp->dpu_lock);
}

void w202_dpu1_stop(struct w202_dp *dp)
{
	struct drm_pending_vblank_event *event;
	bool was_running;

	spin_lock_irq(&dp->dpu_lock);
	was_running = dp->dpu_enabled && dp->dpu_running;
	if (was_running) {
		reinit_completion(&dp->dpu_stop_done);
		dpu_write(dp, DPU_CTRL, dpu_read(dp, DPU_CTRL) | DPU_CTRL_STOP);
	}
	spin_unlock_irq(&dp->dpu_lock);
	if (!was_running)
		return;

	if (!wait_for_completion_timeout(&dp->dpu_stop_done, msecs_to_jiffies(500)))
		drm_err(&dp->drm, "DPU1 stop: no DONE interrupt in 500 ms\n");

	spin_lock_irq(&dp->dpu_lock);
	dp->dpu_running = false;
	event = dp->event;
	dp->event = NULL;
	spin_unlock_irq(&dp->dpu_lock);
	if (event)
		w202_dpu1_send_event(dp, event);
}

static void w202_crtc_atomic_disable(struct drm_crtc *crtc,
				     struct drm_atomic_state *state)
{
	struct w202_dp *dp = drm_to_w202(crtc->dev);
	bool was_enabled;

	drm_crtc_vblank_off(crtc);
	w202_dpu1_stop(dp);

	spin_lock_irq(&dp->dpu_lock);
	was_enabled = dp->dpu_enabled;
	if (was_enabled) {
		/* dpu_fini() */
		dpu_write(dp, DPU_INT_EN, 0);
		dpu_write(dp, DPU_INT_CLR, 0xff);
	}
	dp->dpu_enabled = false;
	dp->dpu_int_en &= DPU_INT_VSYNC;
	spin_unlock_irq(&dp->dpu_lock);

	if (was_enabled) {
		disable_irq(dp->dpu_irq);
		w202_dpu1_clk_off(dp);
		w202_dp_power_put(dp, W202_PWR_CRTC);
	}

	if (crtc->state->event && !crtc->state->active) {
		w202_dpu1_send_event(dp, crtc->state->event);
		crtc->state->event = NULL;
	}
}

static void w202_crtc_atomic_flush(struct drm_crtc *crtc,
				   struct drm_atomic_state *state)
{
	struct w202_dp *dp = drm_to_w202(crtc->dev);
	struct drm_pending_vblank_event *event = crtc->state->event;
	bool armed = false;

	crtc->state->event = NULL;

	spin_lock_irq(&dp->dpu_lock);
	if (dp->dpu_enabled && dp->dpu_running) {
		/* dpu_flip(): latch shadow registers, completion = UPDATE_DONE */
		dpu_write(dp, DPU_CTRL, dpu_read(dp, DPU_CTRL) | DPU_CTRL_REG_UPDATE);
		dp->dpu_int_en |= DPU_INT_ERR;
		dpu_write(dp, DPU_INT_EN, dp->dpu_int_en);
		if (event) {
			WARN_ON(dp->event);
			dp->event = event;
			armed = true;
		}
	}
	spin_unlock_irq(&dp->dpu_lock);

	/* stopped DPU: registers take effect at RUN (vendor writes them directly) */
	if (event && !armed)
		w202_dpu1_send_event(dp, event);
}

static int w202_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct w202_dp *dp = drm_to_w202(crtc->dev);
	unsigned long flags;

	spin_lock_irqsave(&dp->dpu_lock, flags);
	dp->dpu_int_en |= DPU_INT_VSYNC;
	if (dp->dpu_enabled)
		dpu_write(dp, DPU_INT_EN, dp->dpu_int_en);
	spin_unlock_irqrestore(&dp->dpu_lock, flags);
	return 0;
}

static void w202_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct w202_dp *dp = drm_to_w202(crtc->dev);
	unsigned long flags;

	spin_lock_irqsave(&dp->dpu_lock, flags);
	dp->dpu_int_en &= ~DPU_INT_VSYNC;
	if (dp->dpu_enabled)
		dpu_write(dp, DPU_INT_EN, dp->dpu_int_en);
	spin_unlock_irqrestore(&dp->dpu_lock, flags);
}

static const struct drm_crtc_helper_funcs w202_crtc_helper_funcs = {
	.mode_valid = w202_crtc_mode_valid,
	.atomic_enable = w202_crtc_atomic_enable,
	.atomic_disable = w202_crtc_atomic_disable,
	.atomic_flush = w202_crtc_atomic_flush,
};

static const struct drm_crtc_funcs w202_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = w202_crtc_enable_vblank,
	.disable_vblank = w202_crtc_disable_vblank,
};

static int w202_plane_atomic_check(struct drm_plane *plane,
				   struct drm_atomic_state *state)
{
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *cs = NULL;
	struct drm_gem_dma_object *obj;
	struct drm_framebuffer *fb = ps->fb;
	u64 start, end;
	int ret;

	if (ps->crtc)
		cs = drm_atomic_get_new_crtc_state(state, ps->crtc);
	ret = drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING, false, true);
	if (ret || !ps->visible)
		return ret;

	/* LAY_BASE_ADDR is 32 bit and must be 16-byte aligned (dpu_layer()) */
	obj = drm_fb_dma_get_gem_obj(fb, 0);
	start = obj->dma_addr + fb->offsets[0];
	end = start + (u64)fb->pitches[0] * fb->height;
	if (end > SZ_4G || (start & 0xf) || (fb->pitches[0] & 3)) {
		drm_dbg_atomic(plane->dev, "buffer 0x%llx-0x%llx pitch %u not scannable\n",
			       start, end, fb->pitches[0]);
		return -EINVAL;
	}
	return 0;
}

static void w202_plane_atomic_update(struct drm_plane *plane,
				     struct drm_atomic_state *state)
{
	struct w202_dp *dp = drm_to_w202(plane->dev);
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = ps->fb;
	struct drm_gem_dma_object *obj;
	u32 x, y, w, h;

	if (!dp->dpu_enabled)
		return;
	if (!ps->visible) {
		dpu_write(dp, DPU_LAY_CTRL, 0);
		return;
	}

	obj = drm_fb_dma_get_gem_obj(fb, 0);
	x = ps->src.x1 >> 16;
	y = ps->src.y1 >> 16;
	w = drm_rect_width(&ps->src) >> 16;
	h = drm_rect_height(&ps->src) >> 16;

	/* dpu_layer(), layer 0 */
	dpu_write(dp, DPU_LAY_BASE_ADDR, lower_32_bits(obj->dma_addr + fb->offsets[0]));
	dpu_write(dp, DPU_LAY_POS, 0);
	dpu_write(dp, DPU_LAY_SIZE, w | (h << 16));
	dpu_write(dp, DPU_LAY_CROP_START, (y << 16) | x);
	dpu_write(dp, DPU_LAY_ALPHA, 0xff);
	dpu_write(dp, DPU_LAY_PITCH, fb->pitches[0] / fb->format->cpp[0]);
	/*
	 * XRGB8888/ARGB8888 are both FORMAT_ARGB8888 with B0B1B2B3 endian and no R/B switch
	 * (dpu_img_ctrl()). Layer-alpha 0xff over the black background: correct for XRGB and for
	 * premultiplied ARGB, the DRM default when no blend property is exposed.
	 */
	dpu_write(dp, DPU_LAY_CTRL, DPU_LAY_EN | DPU_LAY_FORMAT_ARGB8888 | DPU_LAY_LAYER_ALPHA);
}

static void w202_plane_atomic_disable(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
	struct w202_dp *dp = drm_to_w202(plane->dev);

	if (dp->dpu_enabled)
		dpu_write(dp, DPU_LAY_CTRL, 0);
}

static const struct drm_plane_helper_funcs w202_plane_helper_funcs = {
	.atomic_check = w202_plane_atomic_check,
	.atomic_update = w202_plane_atomic_update,
	.atomic_disable = w202_plane_atomic_disable,
};

static const struct drm_plane_funcs w202_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static struct clk *w202_dpu1_clk(struct w202_dp *dp, const char *name)
{
	struct clk *clk = devm_get_clk_from_child(dp->dev, dp->dpu_np, name);

	if (IS_ERR(clk))
		dev_err_probe(dp->dev, PTR_ERR(clk), "dpu1 clock %s\n", name);
	return clk;
}

int w202_dpu1_init(struct w202_dp *dp)
{
	struct device *dev = dp->dev;
	int ret;

	dp->dispc1_eb = w202_dpu1_clk(dp, "clk_ipa_apb_dispc1_eb");
	if (IS_ERR(dp->dispc1_eb))
		return PTR_ERR(dp->dispc1_eb);
	dp->dpu_core = w202_dpu1_clk(dp, "clk_dpu_core");
	if (IS_ERR(dp->dpu_core))
		return PTR_ERR(dp->dpu_core);
	dp->dpu_dpi = w202_dpu1_clk(dp, "clk_dpu_dpi");
	if (IS_ERR(dp->dpu_dpi))
		return PTR_ERR(dp->dpu_dpi);
	dp->pixelpll = w202_dpu1_clk(dp, "clk_pixelpll");
	if (IS_ERR(dp->pixelpll))
		return PTR_ERR(dp->pixelpll);
	dp->src_192m = w202_dpu1_clk(dp, "clk_src_192m");
	if (IS_ERR(dp->src_192m))
		return PTR_ERR(dp->src_192m);
	dp->src_512m = w202_dpu1_clk(dp, "clk_src_512m");
	if (IS_ERR(dp->src_512m))
		return PTR_ERR(dp->src_512m);

	spin_lock_init(&dp->dpu_lock);
	init_completion(&dp->dpu_stop_done);

	dp->dpu_irq = irq_of_parse_and_map(dp->dpu_np, 0);
	if (!dp->dpu_irq)
		return dev_err_probe(dev, -EINVAL, "dpu1 interrupt\n");
	/* enabled only while the DPU1 is clocked and PD_DPU_DP is up */
	ret = devm_request_irq(dev, dp->dpu_irq, w202_dpu1_irq, IRQF_NO_AUTOEN,
			       "w202-dpu1", dp);
	if (ret)
		return dev_err_probe(dev, ret, "dpu1 irq %d\n", dp->dpu_irq);

	ret = drm_universal_plane_init(&dp->drm, &dp->plane, 0, &w202_plane_funcs,
				       w202_dpu1_formats, ARRAY_SIZE(w202_dpu1_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&dp->plane, &w202_plane_helper_funcs);

	ret = drm_crtc_init_with_planes(&dp->drm, &dp->crtc, &dp->plane, NULL,
					&w202_crtc_funcs, "dpu1");
	if (ret)
		return ret;
	drm_crtc_helper_add(&dp->crtc, &w202_crtc_helper_funcs);
	return 0;
}
