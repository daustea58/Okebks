// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/misc/game_mode.c
 *
 * Flag global "game_mode" yang dibaca oleh:
 *   - governor cpufreq schedhorizon (drivers/cpufreq/cpufreq_schedhorizon.c)
 *   - I/O scheduler zen (block/zen-iosched.c)
 *
 * Diaktifkan dari userspace lewat sysfs:
 *   echo 1 > /sys/kernel/game_mode/enable
 *   cat  /sys/kernel/game_mode/enable
 *
 * Idealnya di-toggle otomatis oleh app manager / Magisk module berdasarkan
 * daftar paket game yang sedang foreground.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/atomic.h>

static atomic_t game_mode_enabled = ATOMIC_INIT(0);
static struct kobject *game_mode_kobj;

bool game_mode_is_active(void)
{
	return atomic_read(&game_mode_enabled) != 0;
}
EXPORT_SYMBOL(game_mode_is_active);

static ssize_t enable_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sprintf(buf, "%d\n", atomic_read(&game_mode_enabled));
}

static ssize_t enable_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	unsigned int val;
	int ret = kstrtouint(buf, 10, &val);

	if (ret)
		return ret;

	atomic_set(&game_mode_enabled, val ? 1 : 0);
	return count;
}

static struct kobj_attribute enable_attr = __ATTR_RW(enable);

static struct attribute *game_mode_attrs[] = {
	&enable_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(game_mode);

static int __init game_mode_init(void)
{
	int ret;

	game_mode_kobj = kobject_create_and_add("game_mode", kernel_kobj);
	if (!game_mode_kobj)
		return -ENOMEM;

	ret = sysfs_create_groups(game_mode_kobj, game_mode_groups);
	if (ret) {
		kobject_put(game_mode_kobj);
		return ret;
	}

	pr_info("game_mode: sysfs node siap di /sys/kernel/game_mode/enable\n");
	return 0;
}

static void __exit game_mode_exit(void)
{
	kobject_put(game_mode_kobj);
}

/* core_initcall supaya siap sebelum cpufreq governor & elevator init */
core_initcall(game_mode_init);
module_exit(game_mode_exit);

MODULE_AUTHOR("Asep");
MODULE_DESCRIPTION("Sysfs flag global game_mode untuk surya (POCO X3 NFC)");
MODULE_LICENSE("GPL");
