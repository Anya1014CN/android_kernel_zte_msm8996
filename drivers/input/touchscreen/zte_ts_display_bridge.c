/*
 * Minimal display-side bridges for Fujisan dual Synaptics touch.
 *
 * On stock/LOS 3.18 these live in mdss_dsi_panel.c and coordinate TD4322
 * shared LCD/touch power. Until the full Fujisan MDSS dual-panel helpers
 * are ported to 4.4, provide safe defaults so the touch drivers link and
 * use their regulator / GPIO power paths.
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
	 * Full implementation needs mdss_dsi_panel_reset_for_ts() and
	 * mdss_dsi_panel_5v_power(). No-op is correct while zte_ts_is_td4322()
	 * returns 0; secondary touch then uses its own regulator path.
	 */
	pr_debug("%s: enable=%d (bridge stub)\n", __func__, enable);
	if (force_td4322)
		msleep(200);
}
EXPORT_SYMBOL(zte_lcd_power_ctrl_func);
