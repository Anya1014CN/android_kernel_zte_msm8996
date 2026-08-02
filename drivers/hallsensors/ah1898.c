/*
 * Stock-compat hall_status sysfs for ZTE Axon M (fujisan).
 * Official dual-LCD stack reads:
 *   /sys/module/ah1898/parameters/hall_status
 * Values match mxm1120: 1=A (folded), 2=B (opening), 3=C (fully unfolded).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

extern int m1120_export_hall_status_get(void);

static int hall_status_get(char *buffer, const struct kernel_param *kp)
{
	int v = m1120_export_hall_status_get();
	return sprintf(buffer, "%d\n", v);
}

static int hall_status_set(const char *val, const struct kernel_param *kp)
{
	/* read-only mirror of mxm1120 */
	return 0;
}

static const struct kernel_param_ops hall_status_ops = {
	.get = hall_status_get,
	.set = hall_status_set,
};

static int hall_status;
module_param_cb(hall_status, &hall_status_ops, &hall_status, 0444);
MODULE_PARM_DESC(hall_status, "1=A folded, 2=B opening, 3=C fully unfolded");

static int __init ah1898_init(void)
{
	pr_info("ah1898: stock-compat hall_status mirror ready\n");
	return 0;
}
static void __exit ah1898_exit(void) {}
module_init(ah1898_init);
module_exit(ah1898_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Fujisan ah1898 hall_status compat");
