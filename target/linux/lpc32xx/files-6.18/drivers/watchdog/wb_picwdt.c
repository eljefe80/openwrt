// SPDX-License-Identifier: GPL-2.0-only
/*
 * WattBox WB-300VB-IP-5 external PIC watchdog driver.
 *
 * Feeds the vendor's external PIC watchdog by reproducing its GPIO
 * bit-bang protocol on P0.6 (DATA) / P0.7 (CLCK), reverse-engineered from
 * the stock kernel's built-in driver (see picwdt-waveform-RE.md in the
 * bring-up project) and already proven correct against real hardware as
 * a userspace tool (package/wb-picwdt) before being ported here.
 *
 * This has to run from a dedicated kthread started unconditionally in
 * probe(), not gated behind watchdog_ops start()/ping(): the PIC has no
 * register-based "kick" to prime and leave alone, it needs continuous,
 * well-formed protocol activity from very early boot onward, and -- the
 * whole reason this exists as a kernel driver instead of the userspace
 * daemon it replaces -- it must keep running through a sysupgrade's
 * procd handoff, which kills essentially all userspace processes before
 * the actual flash write. A kthread is not a process procd's cleanup
 * sweep can kill, so it survives that handoff with no special handling
 * needed on procd's side.
 *
 * Still registers as a real /dev/watchdog device so standard tooling
 * (procd's own watchdog status reporting, ubus, etc.) can see it -- the
 * safety property comes from the kthread itself, not from this
 * registration, but it keeps the device honest about what's there.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/watchdog.h>
#include <linux/platform_device.h>
#include <linux/of.h>

/*
 * Fixed physical address of the LPC32xx GPIO block (P0.6/P0.7 live in the
 * same bank the mainline gpio-lpc32xx driver also binds to). Deliberately
 * NOT claimed via a devicetree "reg" + devm_ioremap_resource() -- that
 * would conflict with gpio-lpc32xx's own exclusive claim on the same
 * range. ioremap() of the fixed address (matching what the userspace
 * /dev/mem-based version this replaces already did safely) avoids that
 * conflict; the two drivers only ever touch disjoint bits (6/7 here,
 * whatever gpio-lpc32xx's consumers use elsewhere) via the block's
 * separate set/clear registers, so there's no read-modify-write race.
 */
#define WB_PICWDT_GPIO_BASE	0x40028000UL
#define WB_PICWDT_GPIO_SIZE	0x1000

#define REG_INP_STATE	0x40
#define REG_OUTP_SET	0x44
#define REG_OUTP_CLR	0x48
#define REG_DIR_SET	0x50
#define REG_DIR_CLR	0x54

#define BIT_DATA	BIT(6)	/* P0.6 */
#define BIT_CLCK	BIT(7)	/* P0.7 */

/* 10ms/step matches the vendor's 100Hz protocol (50Hz CLCK strobe, one
 * DATA bit per CLCK edge) -- see picwdt-keepalive.c for the derivation. */
#define WB_PICWDT_STEP_US	10000

struct wb_picwdt {
	void __iomem *base;
	struct task_struct *thread;
	struct watchdog_device wdd;
	int phase;
	u32 nonce;
};

static inline void wb_reg_write(struct wb_picwdt *wp, u32 off, u32 val)
{
	writel(val, wp->base + off);
}

static inline u32 wb_reg_read(struct wb_picwdt *wp, u32 off)
{
	return readl(wp->base + off);
}

static void wb_data_out(struct wb_picwdt *wp, int v)
{
	wb_reg_write(wp, REG_DIR_SET, BIT_DATA);
	if (v)
		wb_reg_write(wp, REG_OUTP_SET, BIT_DATA);
	else
		wb_reg_write(wp, REG_OUTP_CLR, BIT_DATA);
}

static void wb_data_in(struct wb_picwdt *wp)
{
	wb_reg_write(wp, REG_DIR_CLR, BIT_DATA);
}

static int wb_data_read(struct wb_picwdt *wp)
{
	return !!(wb_reg_read(wp, REG_INP_STATE) & BIT_DATA);
}

static void wb_clk(struct wb_picwdt *wp, int v)
{
	if (v)
		wb_reg_write(wp, REG_OUTP_SET, BIT_CLCK);
	else
		wb_reg_write(wp, REG_OUTP_CLR, BIT_CLCK);
}

static void wb_step_tx(struct wb_picwdt *wp, int bit)
{
	wb_data_out(wp, bit);
	wb_clk(wp, wp->phase);
	wp->phase ^= 1;
	usleep_range(WB_PICWDT_STEP_US, WB_PICWDT_STEP_US + 500);
}

static int wb_step_rx(struct wb_picwdt *wp)
{
	int b;

	wb_clk(wp, wp->phase);
	wp->phase ^= 1;
	usleep_range(WB_PICWDT_STEP_US, WB_PICWDT_STEP_US + 500);
	b = wb_data_read(wp);
	return b;
}

/* 16-byte challenge frame: 4-byte counter nonce, expanded buf[i]=buf[i&3]+i
 * for the rest -- matches the vendor driver's own frame construction. */
static void wb_build_frame(u8 f[16], u32 nonce)
{
	int i;

	f[0] = nonce;
	f[1] = nonce >> 8;
	f[2] = nonce >> 16;
	f[3] = nonce >> 24;
	for (i = 4; i < 16; i++)
		f[i] = f[i & 3] + i;
}

static int wb_picwdt_thread(void *data)
{
	struct wb_picwdt *wp = data;

	while (!kthread_should_stop()) {
		u8 f[16];
		int i, b, reply;

		wb_build_frame(f, wp->nonce);

		/* 16-bit sync 0x7FFE, then 8-bit sync 0x70, MSB-first */
		for (b = 15; b >= 0; b--)
			wb_step_tx(wp, (0x7ffe >> b) & 1);
		for (b = 7; b >= 0; b--)
			wb_step_tx(wp, (0x70 >> b) & 1);

		/* 16 data bytes, each MSB-first */
		for (i = 0; i < 16; i++)
			for (b = 7; b >= 0; b--)
				wb_step_tx(wp, (f[i] >> b) & 1);

		/* release DATA, clock in 16 reply bits */
		wb_data_in(wp);
		reply = 0;
		for (i = 0; i < 16; i++)
			reply = (reply << 1) | wb_step_rx(wp);

		/* drive DATA low, idle one bit-time, then next frame */
		wb_data_out(wp, 0);
		wb_step_tx(wp, 0);

		wp->nonce = wp->nonce * 1664525u + 1013904223u;
	}
	return 0;
}

/*
 * The hardware can't be started/stopped or pinged on demand -- the
 * kthread already runs continuously from probe() onward, matching the
 * vendor's own always-on PIC integration. These exist only so this
 * shows up as a normal, well-behaved watchdog device to userspace.
 */
static int wb_picwdt_start(struct watchdog_device *wdd)
{
	return 0;
}

static int wb_picwdt_stop(struct watchdog_device *wdd)
{
	return 0;
}

static int wb_picwdt_ping(struct watchdog_device *wdd)
{
	return 0;
}

static const struct watchdog_ops wb_picwdt_ops = {
	.owner = THIS_MODULE,
	.start = wb_picwdt_start,
	.stop = wb_picwdt_stop,
	.ping = wb_picwdt_ping,
};

static const struct watchdog_info wb_picwdt_info = {
	.identity = "WattBox external PIC watchdog",
	.options = WDIOF_KEEPALIVEPING,
};

static int wb_picwdt_probe(struct platform_device *pdev)
{
	struct wb_picwdt *wp;
	int ret;

	wp = devm_kzalloc(&pdev->dev, sizeof(*wp), GFP_KERNEL);
	if (!wp)
		return -ENOMEM;

	wp->base = ioremap(WB_PICWDT_GPIO_BASE, WB_PICWDT_GPIO_SIZE);
	if (!wp->base)
		return -ENOMEM;

	wp->phase = 1;
	wp->nonce = 0x1a2b3c4d;

	/* both lines driven output-low, matching the vendor driver's probe */
	wb_reg_write(wp, REG_DIR_SET, BIT_CLCK | BIT_DATA);
	wb_reg_write(wp, REG_OUTP_CLR, BIT_CLCK | BIT_DATA);

	wp->wdd.info = &wb_picwdt_info;
	wp->wdd.ops = &wb_picwdt_ops;
	wp->wdd.timeout = 25;
	wp->wdd.min_timeout = 1;
	wp->wdd.max_timeout = 25;
	wp->wdd.parent = &pdev->dev;
	watchdog_set_nowayout(&wp->wdd, true);
	watchdog_set_drvdata(&wp->wdd, wp);

	ret = devm_watchdog_register_device(&pdev->dev, &wp->wdd);
	if (ret) {
		iounmap(wp->base);
		return ret;
	}

	wp->thread = kthread_run(wb_picwdt_thread, wp, "wb_picwdt");
	if (IS_ERR(wp->thread)) {
		ret = PTR_ERR(wp->thread);
		iounmap(wp->base);
		return ret;
	}

	platform_set_drvdata(pdev, wp);
	dev_info(&pdev->dev, "WattBox PIC watchdog feeder started\n");
	return 0;
}

static void wb_picwdt_remove(struct platform_device *pdev)
{
	/*
	 * Not expected to ever be called (board-only driver, never
	 * unbound). Deliberately does NOT stop the kthread: this hardware
	 * cannot be safely disarmed once running, so stopping the feed
	 * would just let the PIC reset the board on its own schedule
	 * instead of ours.
	 */
}

static const struct of_device_id wb_picwdt_of_match[] = {
	{ .compatible = "snapav,wb300vb-picwdt" },
	{ }
};
MODULE_DEVICE_TABLE(of, wb_picwdt_of_match);

static struct platform_driver wb_picwdt_driver = {
	.probe = wb_picwdt_probe,
	.remove = wb_picwdt_remove,
	.driver = {
		.name = "wb_picwdt",
		.of_match_table = wb_picwdt_of_match,
	},
};
module_platform_driver(wb_picwdt_driver);

MODULE_DESCRIPTION("WattBox WB-300VB-IP-5 external PIC watchdog driver");
MODULE_AUTHOR("Jeff Williams <jeff@wdwconsulting.net>");
MODULE_LICENSE("GPL");
