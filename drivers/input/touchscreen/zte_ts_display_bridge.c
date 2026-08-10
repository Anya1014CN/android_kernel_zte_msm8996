/*
 * Compatibility shims no longer needed: TD4322 helpers live in mdss_dsi_panel.c
 * Keep a tiny object so existing Makefiles that reference this file still link.
 */
#include <linux/module.h>

MODULE_DESCRIPTION("Fujisan TS/display bridge placeholder");
MODULE_LICENSE("GPL");
