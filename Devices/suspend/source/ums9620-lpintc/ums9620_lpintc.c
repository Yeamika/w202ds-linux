// SPDX-License-Identifier: GPL-2.0-only
/*
 * ums9620_lpintc: the interrupts Linux enabled as wake sources of the UMS9620's CPU cluster power-down.
 *
 * In cluster-pd (the deepest cpuidle state, PSCI) a cluster and its DSU are powered off and the GIC's redistributor
 * cannot wake them: what does is the always-on LP_AP_INTC, eight blocks of 32 lines, INTCn line b being GIC SPI
 * 32 * n + b. A line wakes the cluster only where its bit in that block's IRQ_EN is set. TF-A (sml) sets those once at
 * boot from a table of its own (sml_a.img + 0x114d8): INTC0 0xf5085c00, INTC1 0x2080b9fc, INTC7 0x40, and nothing in
 * INTC2-6 - where the USB controller (dwc3, SPI 142: INTC4 bit 14), the IPA (SPI 136-138) and the PCIe MSI of the
 * Wi-Fi chip are. An interrupt of those that came while CPU0's cluster was powered down waited until something else
 * woke it: pings over USB took up to ~120 ms, and the workaround was CPU0 without cluster-pd.
 *
 * This module adds every SPI Linux has enabled in the GIC distributor (GICD_ISENABLER1..8) to IRQ_EN, at load and
 * then every few seconds from deferrable work (it never wakes a CPU by itself), so drivers loaded later are taken up
 * too. It only adds: while running a bit TF-A set is never cleared, and a line Linux disables again keeps its bit (the GIC still
 * masks it; it can at most wake the cluster for nothing). An enabled interrupt has to be handled anyway, so waking for
 * it costs no work, only the wait. For system suspend the whole table is narrowed to suspend_wake_spi (the PMIC:
 * power key and RTC) and restored on resume. On a U30 Air (6.18.57) with CPU0's cluster-pd allowed, USB pings
 * from a computer took 9.5 ms on average and 42 ms at most instead of 15.7 and 69 (WFI only on every CPU: 7.1 and 40),
 * and the cluster still powered down.
 *
 * The blocks' registers: +0x08 IRQ_EN reads the enabled mask and sets the bits written as 1, +0x0c IRQ_DIS clears the
 * bits written as 1; both are single writes, so nothing is read, modified and written back and no lock with TF-A is
 * needed. A block whose clock (AON gate 0x64900008, ap-intcN-eb, kept on by CLK_IGNORE_UNUSED) is off is left alone.
 * Only the UMS9620 is accepted. docs/FINDINGS.md 218.
 */
#include <linux/bits.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/spinlock.h>
#include <linux/syscore_ops.h>
#include <linux/workqueue.h>

#define LPINTC_BLOCKS		8
#define LPINTC_EN		0x08	/* read: enabled mask; write: set */
#define LPINTC_DIS		0x0c	/* write: clear */
#define LPINTC_AON_GATE		0x64900008
#define LPINTC_GATE_BIT(n)	BIT(11 + (n))
#define GICD_CTLR		0x0000
#define GICD_CTLR_RWP		BIT(31)
#define GICD_ISENABLER(n)	(0x100 + 4 * (n))	/* n = 1 is SPI 0-31 */
#define GICD_ICENABLER(n)	(0x180 + 4 * (n))

static const phys_addr_t lpintc_base[LPINTC_BLOCKS] = {
	0x640c0000, 0x640d0000, 0x640e0000, 0x640f0000, 0x64100000, 0x64110000, 0x645b0000, 0x645c0000,
};

static unsigned int period = 5;
module_param(period, uint, 0444);
MODULE_PARM_DESC(period, "seconds between two looks at the GIC for newly enabled interrupts (deferrable)");

/*
 * System suspend wakes only on these SPIs. Default: SPI 230 (INTC7 bit 6), the
 * PMIC (sc27xx, spi4.0), which carries the power key and the RTC alarm. All
 * other lines of TF-A's table (modem/CP mailboxes, IPA, Wi-Fi PCIe, AP EIC and
 * GPIO) are cleared before suspend and the whole table is restored on resume.
 * The same SPIs are also disabled in the GIC distributor for the final step:
 * IRQF_NO_SUSPEND lines (the CP mailbox answers the AP's sleep notice) would
 * otherwise be pending at PSCI CPU_SUSPEND and return from it at once. Their
 * state stays latched in the source and is delivered after resume.
 */
static unsigned int suspend_wake_spi[LPINTC_BLOCKS] = { 230 };
static int suspend_wake_count = 1;
module_param_array(suspend_wake_spi, uint, &suspend_wake_count, 0444);
MODULE_PARM_DESC(suspend_wake_spi, "the only GIC SPIs allowed to wake from system suspend");

static void __iomem *gicd;
static void __iomem *intc[LPINTC_BLOCKS];
static u32 added[LPINTC_BLOCKS];		/* the bits this module set: the ones it takes back */
static u32 saved[LPINTC_BLOCKS];		/* IRQ_EN across system suspend */
static u32 saved_gicd[LPINTC_BLOCKS];		/* GICD_ISENABLER across system suspend */
static DEFINE_SPINLOCK(lpintc_lock);
static struct delayed_work lpintc_work;

static void lpintc_sync(void)
{
	unsigned long flags;
	u32 want, now;
	int n;

	spin_lock_irqsave(&lpintc_lock, flags);
	for (n = 0; n < LPINTC_BLOCKS; n++) {
		if (!intc[n])
			continue;
		want = readl_relaxed(gicd + GICD_ISENABLER(n + 1));
		now = readl_relaxed(intc[n] + LPINTC_EN);
		want &= ~now;
		if (!want)
			continue;
		writel_relaxed(want, intc[n] + LPINTC_EN);
		added[n] |= want;
		pr_info("ums9620_lpintc: INTC%d +0x%08x (SPI %d-%d), now 0x%08x\n", n, want, 32 * n, 32 * n + 31,
			readl_relaxed(intc[n] + LPINTC_EN));
	}
	spin_unlock_irqrestore(&lpintc_lock, flags);
}

static void lpintc_work_fn(struct work_struct *work)
{
	lpintc_sync();
	queue_delayed_work(system_power_efficient_wq, &lpintc_work, period * HZ);
}

/* the bits this module added, taken out at unload */
static void lpintc_apply(unsigned int reg)
{
	int n;

	for (n = 0; n < LPINTC_BLOCKS; n++)
		if (intc[n] && added[n])
			writel_relaxed(added[n], intc[n] + reg);
}

/* syscore: one CPU, interrupts off */
static int lpintc_suspend(void)
{
	u32 keep[LPINTC_BLOCKS] = { };
	int n;

	for (n = 0; n < suspend_wake_count; n++)
		if (suspend_wake_spi[n] < 32 * LPINTC_BLOCKS)
			keep[suspend_wake_spi[n] / 32] |= BIT(suspend_wake_spi[n] % 32);
	for (n = 0; n < LPINTC_BLOCKS; n++) {
		saved_gicd[n] = readl_relaxed(gicd + GICD_ISENABLER(n + 1));
		if (saved_gicd[n] & ~keep[n])
			writel_relaxed(saved_gicd[n] & ~keep[n], gicd + GICD_ICENABLER(n + 1));
		if (!intc[n])
			continue;
		saved[n] = readl_relaxed(intc[n] + LPINTC_EN);
		if (saved[n] & ~keep[n])
			writel_relaxed(saved[n] & ~keep[n], intc[n] + LPINTC_DIS);
		if (keep[n] & ~saved[n])
			writel_relaxed(keep[n] & ~saved[n], intc[n] + LPINTC_EN);
	}
	while (readl_relaxed(gicd + GICD_CTLR) & GICD_CTLR_RWP)
		cpu_relax();
	return 0;
}

static void lpintc_resume(void)
{
	int n;

	for (n = 0; n < LPINTC_BLOCKS; n++) {
		writel_relaxed(saved_gicd[n], gicd + GICD_ISENABLER(n + 1));
		if (!intc[n])
			continue;
		writel_relaxed(~saved[n], intc[n] + LPINTC_DIS);
		writel_relaxed(saved[n], intc[n] + LPINTC_EN);
	}
}

static struct syscore_ops lpintc_syscore_ops = {
	.suspend = lpintc_suspend,
	.resume = lpintc_resume,
};


static void lpintc_unmap(void)
{
	int n;

	for (n = 0; n < LPINTC_BLOCKS; n++)
		if (intc[n])
			iounmap(intc[n]);
	if (gicd)
		iounmap(gicd);
}

static int __init lpintc_init(void)
{
	struct device_node *np;
	void __iomem *gate;
	u32 eb;
	int n, used = 0;

	if (!of_machine_is_compatible("sprd,ums9620"))
		return -ENODEV;
	if (!period)
		period = 1;

	np = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
	if (!np)
		return -ENODEV;
	gicd = of_iomap(np, 0);
	of_node_put(np);
	if (!gicd)
		return -ENOMEM;

	gate = ioremap(LPINTC_AON_GATE, 4);
	if (!gate) {
		lpintc_unmap();
		return -ENOMEM;
	}
	eb = readl(gate);
	iounmap(gate);

	for (n = 0; n < LPINTC_BLOCKS; n++) {
		if (!(eb & LPINTC_GATE_BIT(n))) {
			pr_warn("ums9620_lpintc: INTC%d clock off (AON gate 0x%08x), left alone\n", n, eb);
			continue;
		}
		intc[n] = ioremap(lpintc_base[n], 0x10);
		if (!intc[n]) {
			/* a block that is on but cannot be reached would be left out of sync while the load says it worked
			 * (kanoqwq f0b5ad43): the load fails instead, and nothing is touched */
			pr_err("ums9620_lpintc: INTC%d cannot be mapped\n", n);
			lpintc_unmap();
			return -ENOMEM;
		}
		used++;
	}
	if (!used) {
		lpintc_unmap();
		return -ENODEV;
	}

	for (n = 0; n < LPINTC_BLOCKS; n++)
		if (intc[n])
			pr_info("ums9620_lpintc: INTC%d IRQ_EN 0x%08x as found\n", n, readl(intc[n] + LPINTC_EN));

	lpintc_sync();
	register_syscore_ops(&lpintc_syscore_ops);
	INIT_DEFERRABLE_WORK(&lpintc_work, lpintc_work_fn);
	queue_delayed_work(system_power_efficient_wq, &lpintc_work, period * HZ);
	return 0;
}

static void __exit lpintc_exit(void)
{
	unsigned long flags;

	cancel_delayed_work_sync(&lpintc_work);
	unregister_syscore_ops(&lpintc_syscore_ops);
	spin_lock_irqsave(&lpintc_lock, flags);
	lpintc_apply(LPINTC_DIS);
	spin_unlock_irqrestore(&lpintc_lock, flags);
	lpintc_unmap();
}

module_init(lpintc_init);
module_exit(lpintc_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UMS9620: the GIC's enabled interrupts as wake sources of the CPU cluster power-down (LP_AP_INTC)");
