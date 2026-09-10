// SPDX-License-Identifier: GPL-2.0-only

#include <linux/init.h>
#include <linux/module.h>

static int __init qcom_app_example_init(void)
{
    pr_info("QCOM application framework example module loaded\n");
    return 0;
}

static void __exit qcom_app_example_exit(void)
{
    pr_info("QCOM application framework example module unloaded\n");
}

module_init(qcom_app_example_init);
module_exit(qcom_app_example_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QCOM application framework build validation");
