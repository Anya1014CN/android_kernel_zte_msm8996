/*
 * Minimal display-side bridges for Fujisan dual Synaptics touch.
 *
 * On the stock/LOS 3.18 tree these live in mdss_dsi_panel.c and coordinate
 * TD4322 shared LCD/touch power. Keep the independent regulator/GPIO path as
 * the safe default until the full 4.4 MDSS dual-panel power path is ported.
 */
#include <linux/module.h>

MODULE_DESCRIPTION("Fujisan TS/display bridge compatibility object");
MODULE_LICENSE("GPL");
