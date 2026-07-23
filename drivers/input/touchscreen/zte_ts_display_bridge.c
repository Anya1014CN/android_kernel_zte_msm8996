/*
 * Fujisan touch/display bridge helpers.
 *
 * Stock/LOS 3.18 implement these in mdss_dsi_panel.c. Until full dual-panel
 * MDSS helpers are ported, keep a self-contained bridge that:
 *  - detects TD4322 from kernel cmdline / built-in panel string
 *  - provides lcd power hooks used by the secondary touch stack
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <asm/setup.h>

static bool force_td4322;
module_param_named(force_td4322, force_td4322, bool, 0644);
MODULE_PARM_DESC(force_td4322, "Force TD4322 shared power path");

static bool td4322_detected;
static bool td4322_checked;

static bool fujisan_panel_is_td4322(void)
{
	if (force_td4322)
		return true;
	if (td4322_checked)
		return td4322_detected;

	td4322_checked = true;
	if (strstr(boot_command_line, "td4322") ||
	    strstr(boot_command_line, "TD4322")) {
		td4322_detected = true;
		pr_info("fujisan: TD4322 panel string found in cmdline\n");
	} else {
		td4322_detected = false;
		pr_info("fujisan: no TD4322 panel string in cmdline\n");
	}
	return td4322_detected;
}

char zte_ts_is_td4322(void)
{
	return fujisan_panel_is_td4322() ? 1 : 0;
}
EXPORT_SYMBOL(zte_ts_is_td4322);

void zte_lcd_power_ctrl_func(int enable)
{
	/*
	 * Full sequence needs mdss_dsi_panel_reset_for_ts() + 5V rails.
	 * Delay keeps secondary-touch probe timing closer to stock while
	 * panel rails come up via the normal MDSS unblank path.
	 */
	pr_info("%s: enable=%d td4322=%d\n", __func__, enable,
		zte_ts_is_td4322());
	if (enable)
		msleep(200);
	else
		msleep(50);
}
EXPORT_SYMBOL(zte_lcd_power_ctrl_func);
