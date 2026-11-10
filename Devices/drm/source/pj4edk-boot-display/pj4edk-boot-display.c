// SPDX-License-Identifier: GPL-2.0-only
/* Print inside the LK/UEFI PocketJS boot-output rectangle until the independent
 * display owner replaces it. printk only updates text; a worker renders the
 * final dirty glyphs without holding an IRQ-off lock. No USB/Wi-Fi dependency.
 */
#include <linux/console.h>
#include <linux/font.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/libfdt.h>
#include <linux/memblock.h>
#include <linux/module.h>
#include <linux/of_fdt.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <asm/cacheflush.h>
#include "pj4edk-boot-display-core.h"

#define PRESENT_RETRY_MS 1
static struct pj4edk_terminal terminal, frame;
static struct pj4edk_glyphs glyphs;
static struct pj4edk_shadow shadow;
static struct pj4edk_damage damage;
static DEFINE_RAW_SPINLOCK(pj4edk_lock);
static bool owned, workers_ready;
static void __iomem *inherited_dpu;
static bool present_pending, present_inflight, single_run;
static u32 present_done;
static unsigned long rendered_frames, rendered_cells, cache_ranges;
static unsigned long render_total_ns, render_max_ns, present_count;
static unsigned long queue_max_ns, present_busy;
static u64 queued_ns;
module_param(rendered_frames, ulong, 0444);
module_param(rendered_cells, ulong, 0444);
module_param(cache_ranges, ulong, 0444);
module_param(render_total_ns, ulong, 0444);
module_param(render_max_ns, ulong, 0444);
module_param(present_count, ulong, 0444);
module_param(queue_max_ns, ulong, 0444);
module_param(present_busy, ulong, 0444);
static void pj4edk_flush_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(pj4edk_work, pj4edk_flush_work);

/* The inherited panel uses single-run command mode on UEFI boots. Cleaning
 * framebuffer cache alone does not submit it. Use the existing scanout only,
 * without IRQ ownership, resets, clock changes or a synchronous busy wait. */
static void pj4edk_present(void)
{
	u32 ctrl;

	if (!inherited_dpu || !present_pending)
		return;
	if (present_inflight && !(readl(inherited_dpu + 0x25c) & present_done)) {
		present_busy++;
		return;
	}
	writel(present_done | BIT(16) | BIT(18) | BIT(2), inherited_dpu + 0x254);
	ctrl = readl(inherited_dpu + 0x08) & ~BIT(1);
	wmb();
	writel(ctrl | BIT(2) | BIT(4), inherited_dpu + 0x08);
	if (single_run || !(ctrl & BIT(0)))
		writel(readl(inherited_dpu + 0x08) | BIT(0), inherited_dpu + 0x08);
	present_inflight = true;
	present_pending = false;
	present_count++;
}

static void pj4edk_render(void)
{
	unsigned int col, x;
	u64 start = ktime_get_mono_fast_ns(), elapsed;

	pj4edk_draw_damage(&frame, &shadow, &damage);
	for (col = 0; col < frame.cols; col++) {
		unsigned int px, height;

		if (!damage.end[col])
			continue;
		px = 1200 - frame.top - damage.end[col] * PJ4EDK_CELL_H;
		height = (damage.end[col] - damage.first[col]) * PJ4EDK_CELL_H;
		for (x = frame.left + col * PJ4EDK_CELL_W;
		     x < frame.left + (col + 1) * PJ4EDK_CELL_W; x++) {
			unsigned long address = (unsigned long)(frame.pixels + x * 1200 + px);

			dcache_clean_poc(address, address + height * sizeof(u16));
			cache_ranges++;
		}
	}
	if (damage.cells) {
		present_pending = true;
		rendered_frames++;
		rendered_cells += damage.cells;
	}
	elapsed = ktime_get_mono_fast_ns() - start;
	render_total_ns += elapsed;
	render_max_ns = max_t(unsigned long, render_max_ns, elapsed);
}

static void pj4edk_flush_work(struct work_struct *work)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&pj4edk_lock, flags);
	if (!owned) {
		raw_spin_unlock_irqrestore(&pj4edk_lock, flags);
		return;
	}
	/* Coalesce only while hardware owns an in-flight frame. No fixed log
	 * batching delay, and no rewriting that scanout while it is pending.
	 * A delayed retry yields the CPU; it never spins in printk context.
	 */
	if (inherited_dpu && present_inflight &&
	    !(readl(inherited_dpu + 0x25c) & present_done)) {
		present_busy++;
		schedule_delayed_work(&pj4edk_work, msecs_to_jiffies(PRESENT_RETRY_MS));
		raw_spin_unlock_irqrestore(&pj4edk_lock, flags);
		return;
	}
	/* The bounded text snapshot is the only work under the IRQ-off lock.
	 * release waits for this worker before relinquishing display ownership.
	 */
	frame = terminal;
	terminal.dirty = 0;
	if (queued_ns) {
		queue_max_ns = max_t(unsigned long, queue_max_ns,
				    ktime_get_mono_fast_ns() - queued_ns);
		queued_ns = 0;
	}
	raw_spin_unlock_irqrestore(&pj4edk_lock, flags);
	if (frame.dirty)
		pj4edk_render();
	pj4edk_present();
	raw_spin_lock_irqsave(&pj4edk_lock, flags);
	if (owned && (terminal.dirty || (present_pending && inherited_dpu))) {
		if (!queued_ns)
			queued_ns = ktime_get_mono_fast_ns();
		schedule_delayed_work(&pj4edk_work, present_pending ?
			msecs_to_jiffies(PRESENT_RETRY_MS) : 0);
	}
	raw_spin_unlock_irqrestore(&pj4edk_lock, flags);
}

static void pj4edk_console_write(struct console *con, const char *s, unsigned int count)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&pj4edk_lock, flags);
	if (!owned) {
		raw_spin_unlock_irqrestore(&pj4edk_lock, flags);
		return;
	}
	while (count--)
		pj4edk_character(&terminal, *s++);
	/* Do not move an already pending deadline: a log storm must not starve
	 * rendering. No raster, cache maintenance or MMIO in printk context.
	 */
	if (terminal.dirty && workers_ready) {
		if (!queued_ns)
			queued_ns = ktime_get_mono_fast_ns();
		schedule_delayed_work(&pj4edk_work, 0);
	}
	raw_spin_unlock_irqrestore(&pj4edk_lock, flags);
}

/* A persistent secondary console: ordinary serial-console registration must
 * not discard it before KMS has a complete frame. It never becomes a tty.
 */
static struct console pj4edk_console = {
	.name = "pj4edklog", .write = pj4edk_console_write,
	.flags = CON_ENABLED | CON_PRINTBUFFER, .index = 0,
};

static bool pj4edk_boot_display_console_active(void)
{
	return READ_ONCE(owned);
}

static void pj4edk_boot_display_console_release(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&pj4edk_lock, flags);
	owned = false;
	raw_spin_unlock_irqrestore(&pj4edk_lock, flags);
	/* Process context only. Return only after framebuffer/MMIO writes stop.
	 * Retained clock references are released separately by aperture takeover.
	 */
	cancel_delayed_work_sync(&pj4edk_work);
}

static int __init pj4edk_console_init(void)
{
	const void *property;
	const __be32 *reg;
	struct memblock_region *r;
	u32 v[20];
	int chosen, logo, length;
	bool reserved = false;

	if (!initial_boot_params)
		return 0;
	chosen = fdt_path_offset(initial_boot_params, "/chosen");
	if (chosen < 0)
		return 0;
	property = fdt_getprop(initial_boot_params, chosen, PJ4EDK2_DISPLAY_PROPERTY, &length);
	if (!property || !pj4edk2_display_decode_be(property, length, v))
		return 0;
	logo = fdt_path_offset(initial_boot_params, "/reserved-memory/logobuffer@b0000000");
	if (logo < 0 || fdt_getprop(initial_boot_params, logo, "no-map", NULL) ||
	    fdt_getprop(initial_boot_params, logo, "reusable", NULL))
		return 0;
	reg = fdt_getprop(initial_boot_params, logo, "reg", &length);
	if (!reg || length != 16 || be32_to_cpu(reg[0]) ||
	    be32_to_cpu(reg[1]) != 0xb0000000 || be32_to_cpu(reg[2]) ||
	    be32_to_cpu(reg[3]) != 0x9e4000 ||
	    !memblock_is_region_memory(0xb0000000, 0x9e4000) ||
	    !memblock_is_map_memory(0xb0000000))
		return 0;
	/* memblock_is_region_reserved() tests intersection, not full coverage. */
	for_each_reserved_mem_region(r)
		if (r->base <= 0xb0000000 && r->base + r->size >= 0xb09e4000)
			reserved = true;
	if (!reserved || pj4edk_terminal_init(&terminal, v, phys_to_virt(0xb0000000), font_vga_8x16.data))
		return 0;
	pj4edk_prepare_font(&glyphs, terminal.font);
	terminal.glyphs = &glyphs;
	owned = true;
	register_console(&pj4edk_console);
	pr_info("pj4edk-boot-display: boot-output console %ux%u; native 12x24 font; retained scanout\n",
		terminal.cols, terminal.rows);
	return 0;
}
console_initcall(pj4edk_console_init);

static int __init pj4edk_workers_init(void)
{
	unsigned long flags;
	void __iomem *dpu = NULL;

	if (owned) {
		dpu = ioremap(0x31000000, 0x1000);
		if (dpu && (readl(dpu) != 0x600 || readl(dpu + 0x18) != 0x078004b0 ||
		    readl(dpu + 0x2c) != 1 || readl(dpu + 0x30) != 0xb0000000 ||
		    (readl(dpu + 0x40) & 0x007000f0) != 0x00200040 || readl(dpu + 0x4c) != 1200)) {
			iounmap(dpu);
			dpu = NULL;
		}
	}

	raw_spin_lock_irqsave(&pj4edk_lock, flags);
	inherited_dpu = dpu;
	if (dpu) {
		single_run = (readl(dpu + 0x0c) & (BIT(0) | BIT(1))) != 0;
		present_done = single_run ? BIT(0) : BIT(18);
	}
	workers_ready = true;
	raw_spin_unlock_irqrestore(&pj4edk_lock, flags);
	if (pj4edk_boot_display_console_active())
		schedule_delayed_work(&pj4edk_work, 0);
	return 0;
}
core_initcall(pj4edk_workers_init);

#include "pj4edk-boot-display-clocks.h"
