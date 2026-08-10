/*
 * Minimal display-side bridges for Fujisan dual Synaptics touch.
 *
 * On the stock/LOS 3.18 tree these live in mdss_dsi_panel.c and coordinate
 * TD4322 shared LCD/touch power. Keep the independent regulator/GPIO path as
 * the safe default until the full 4.4 MDSS dual-panel power path is ported.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>

static bool force_td4322;
module_param_named(force_td4322, force_td4322, bool, 0644);
MODULE_PARM_DESC(force_td4322, "Force TD4322 shared power path (debug)");

char zte_ts_is_td4322(void)
{
	return force_td4322 ? 1 : 0;
}
EXPORT_SYMBOL(zte_ts_is_td4322);

void zte_lcd_power_ctrl_func(int enable)
{
	/*
	 * No-op is correct while zte_ts_is_td4322() returns 0: the secondary
	 * touch controller uses its own regulator and GPIO power sequence.
	 */
	pr_debug("%s: enable=%d (bridge stub)\n", __func__, enable);
	if (force_td4322)
		msleep(200);
}
EXPORT_SYMBOL(zte_lcd_power_ctrl_func);
