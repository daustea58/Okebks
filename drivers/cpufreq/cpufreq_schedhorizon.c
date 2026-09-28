// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/cpufreq/cpufreq_schedhorizon.c
 *
 * CPUFreq governor "schedhorizon" — tuned for gaming workloads.
 *
 * Referensi desain: cpufreq_governor.c (common ondemand/conservative
 * infrastructure) + gaya interactive governor (ramp cepat, decay halus).
 * Ditulis ulang sebagai implementasi independen untuk POCO X3 NFC (surya),
 * kernel 4.14.x non-GKI.
 *
 * Karakteristik:
 *  - sampling_rate pendek saat "game_mode" aktif (baca dari
 *    drivers/misc/game_mode.c melalui game_mode_is_active()).
 *  - naik ke freq maksimum kandidat lebih cepat ("fast ramp-up") saat
 *    load melewati up_threshold.
 *  - turun bertahap (decay) untuk menghindari freq flapping yang bikin
 *    microstutter saat load turun sesaat (misal loading screen singkat).
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/slab.h>
#include <linux/tick.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>

#include "cpufreq_governor.h"

/* Tuning default; bisa diubah lewat sysfs di
 * /sys/devices/system/cpu/cpufreq/schedhorizon/
 */
#define SH_DEF_SAMPLING_RATE_US		20000	/* 20 ms normal */
#define SH_DEF_SAMPLING_RATE_GAME_US		10000	/* 10 ms saat game_mode */
#define SH_DEF_UP_THRESHOLD			80
#define SH_DEF_DOWN_DIFFERENTIAL		10
#define SH_DEF_FAST_RAMP_THRESHOLD		95
#define SH_DEF_DECAY_STEPS			3	/* langkah turun bertahap */
#define SH_DEF_MIN_SAMPLE_MS			10

extern bool game_mode_is_active(void);

struct sh_policy_dbs_info {
	struct policy_dbs_info policy_dbs;
	unsigned int requested_freq;
	unsigned int decay_step;
	unsigned int rate_mult;
};

static inline struct sh_policy_dbs_info *to_dbs_info(struct policy_dbs_info *p)
{
	return container_of(p, struct sh_policy_dbs_info, policy_dbs);
}

struct sh_dbs_tuners {
	unsigned int up_threshold;
	unsigned int down_differential;
	unsigned int fast_ramp_threshold;
	unsigned int decay_steps;
	unsigned int sampling_rate_game_us;
};

static unsigned int sh_effective_sampling_rate(struct dbs_data *dbs_data)
{
	struct sh_dbs_tuners *tuners = dbs_data->tuners;

	if (game_mode_is_active())
		return tuners->sampling_rate_game_us;

	return dbs_data->sampling_rate;
}

static unsigned int sh_dbs_update(struct cpufreq_policy *policy)
{
	struct policy_dbs_info *policy_dbs = policy->governor_data;
	struct sh_policy_dbs_info *sh_dbs = to_dbs_info(policy_dbs);
	struct dbs_data *dbs_data = policy_dbs->dbs_data;
	struct sh_dbs_tuners *tuners = dbs_data->tuners;
	unsigned int load = dbs_update(policy);
	unsigned int target_freq;

	/*
	 * Fast ramp: kalau load sangat tinggi (mis. mulai loading berat di
	 * game), langsung lompat ke freq maksimum yang diizinkan policy,
	 * jangan nunggu step demi step.
	 */
	if (load >= tuners->fast_ramp_threshold) {
		sh_dbs->requested_freq = policy->max;
		sh_dbs->decay_step = 0;
		goto out;
	}

	if (load >= tuners->up_threshold) {
		if (sh_dbs->requested_freq < policy->max) {
			unsigned int inc = (policy->max - policy->min) / 4;

			sh_dbs->requested_freq += inc ? inc : 1;
			if (sh_dbs->requested_freq > policy->max)
				sh_dbs->requested_freq = policy->max;
		}
		sh_dbs->decay_step = 0;
	} else if (load < (tuners->up_threshold - tuners->down_differential)) {
		/*
		 * Decay bertahap: hanya turunkan freq setiap
		 * `decay_steps` sample supaya tidak flapping saat load
		 * naik-turun cepat (khas pola input game).
		 */
		sh_dbs->decay_step++;
		if (sh_dbs->decay_step >= tuners->decay_steps) {
			unsigned int dec = (policy->max - policy->min) / 8;

			sh_dbs->decay_step = 0;
			if (sh_dbs->requested_freq > policy->min + dec)
				sh_dbs->requested_freq -= dec ? dec : 1;
			else
				sh_dbs->requested_freq = policy->min;
		}
	}

out:
	target_freq = sh_dbs->requested_freq;
	if (target_freq > policy->max)
		target_freq = policy->max;
	if (target_freq < policy->min)
		target_freq = policy->min;

	__cpufreq_driver_target(policy, target_freq, CPUFREQ_RELATION_C);

	return sh_effective_sampling_rate(dbs_data);
}

/* ------------------------- sysfs tunables ------------------------- */

static ssize_t up_threshold_store(struct gov_attr_set *attr_set,
				   const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct sh_dbs_tuners *tuners = dbs_data->tuners;
	unsigned int input;
	int ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 ||
	    input <= tuners->down_differential)
		return -EINVAL;

	tuners->up_threshold = input;
	return count;
}

static ssize_t down_differential_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct sh_dbs_tuners *tuners = dbs_data->tuners;
	unsigned int input;
	int ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input >= tuners->up_threshold)
		return -EINVAL;

	tuners->down_differential = input;
	return count;
}

static ssize_t fast_ramp_threshold_store(struct gov_attr_set *attr_set,
					  const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct sh_dbs_tuners *tuners = dbs_data->tuners;
	unsigned int input;
	int ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 || input < tuners->up_threshold)
		return -EINVAL;

	tuners->fast_ramp_threshold = input;
	return count;
}

static ssize_t decay_steps_store(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct sh_dbs_tuners *tuners = dbs_data->tuners;
	unsigned int input;
	int ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input < 1 || input > 20)
		return -EINVAL;

	tuners->decay_steps = input;
	return count;
}

static ssize_t sampling_rate_game_store(struct gov_attr_set *attr_set,
					 const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct sh_dbs_tuners *tuners = dbs_data->tuners;
	unsigned int input;
	int ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input < 1000)
		return -EINVAL;

	tuners->sampling_rate_game_us = input;
	return count;
}

gov_show_one_common(sampling_rate);
gov_show_one(schedhorizon, up_threshold);
gov_show_one(schedhorizon, down_differential);
gov_show_one(schedhorizon, fast_ramp_threshold);
gov_show_one(schedhorizon, decay_steps);
gov_show_one(schedhorizon, sampling_rate_game_us);

gov_attr_rw(sampling_rate);
gov_attr_rw(up_threshold);
gov_attr_rw(down_differential);
gov_attr_rw(fast_ramp_threshold);
gov_attr_rw(decay_steps);
gov_attr_rw(sampling_rate_game_us);

static struct attribute *schedhorizon_attrs[] = {
	&sampling_rate.attr,
	&up_threshold.attr,
	&down_differential.attr,
	&fast_ramp_threshold.attr,
	&decay_steps.attr,
	&sampling_rate_game_us.attr,
	NULL
};
ATTRIBUTE_GROUPS(schedhorizon);

/* ------------------------- governor glue ------------------------- */

static struct policy_dbs_info *sh_alloc(void)
{
	struct sh_policy_dbs_info *sh_dbs;

	sh_dbs = kzalloc(sizeof(*sh_dbs), GFP_KERNEL);
	return sh_dbs ? &sh_dbs->policy_dbs : NULL;
}

static void sh_free(struct policy_dbs_info *policy_dbs)
{
	kfree(to_dbs_info(policy_dbs));
}

static int sh_init(struct dbs_data *dbs_data)
{
	struct sh_dbs_tuners *tuners;

	tuners = kzalloc(sizeof(*tuners), GFP_KERNEL);
	if (!tuners)
		return -ENOMEM;

	tuners->up_threshold = SH_DEF_UP_THRESHOLD;
	tuners->down_differential = SH_DEF_DOWN_DIFFERENTIAL;
	tuners->fast_ramp_threshold = SH_DEF_FAST_RAMP_THRESHOLD;
	tuners->decay_steps = SH_DEF_DECAY_STEPS;
	tuners->sampling_rate_game_us = SH_DEF_SAMPLING_RATE_GAME_US;

	dbs_data->tuners = tuners;
	dbs_data->sampling_rate = SH_DEF_SAMPLING_RATE_US;
	dbs_data->min_sampling_rate = SH_DEF_MIN_SAMPLE_MS * USEC_PER_MSEC;
	dbs_data->ignore_nice_load = 0;
	dbs_data->io_is_busy = 1;	/* penting untuk load yang datang dari I/O berat, mis. asset streaming */

	return 0;
}

static void sh_exit(struct dbs_data *dbs_data)
{
	kfree(dbs_data->tuners);
}

static void sh_start(struct cpufreq_policy *policy)
{
	struct policy_dbs_info *policy_dbs = policy->governor_data;
	struct sh_policy_dbs_info *sh_dbs = to_dbs_info(policy_dbs);

	sh_dbs->requested_freq = policy->cur;
	sh_dbs->decay_step = 0;
}

static struct dbs_governor sh_governor = {
	.gov = CPUFREQ_DBS_GOVERNOR_INITIALIZER("schedhorizon"),
	.kobj_type = { .default_groups = schedhorizon_groups },
	.gov_dbs_update = sh_dbs_update,
	.alloc = sh_alloc,
	.free = sh_free,
	.init = sh_init,
	.exit = sh_exit,
	.start = sh_start,
};

#define CPU_FREQ_GOV_SCHEDHORIZON	(&sh_governor.gov)

static int __init cpufreq_gov_dbs_init(void)
{
	return cpufreq_register_governor(CPU_FREQ_GOV_SCHEDHORIZON);
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(CPU_FREQ_GOV_SCHEDHORIZON);
}

MODULE_AUTHOR("Asep");
MODULE_DESCRIPTION("'cpufreq_schedhorizon' - gaming-tuned cpufreq governor untuk surya (POCO X3 NFC)");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDHORIZON
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return CPU_FREQ_GOV_SCHEDHORIZON;
}

fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);
