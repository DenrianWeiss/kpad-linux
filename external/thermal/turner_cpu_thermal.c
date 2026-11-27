// SPDX-License-Identifier: GPL-2.0-only
/* MT6991 firmware publishes a signed temperature in millidegrees per CPU.
 * The offsets match get_cpu_temp() in Turner's thermal_interface.ko.
 */
#include <linux/cpu.h>
#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

#define TURNER_CORES 8
#define TURNER_POLICIES 3

struct turner_thermal {
	void __iomem *tcm;
	struct thermal_cooling_device *cooling[TURNER_POLICIES];
	struct thermal_zone_device *zone;
};

static int turner_get_temp(struct thermal_zone_device *zone, int *temp)
{
	struct turner_thermal *sensor = thermal_zone_device_priv(zone);
	int cpu, hottest = INT_MIN;

	for (cpu = 0; cpu < TURNER_CORES; cpu++) {
		int value = (s32)readl(sensor->tcm + cpu * sizeof(u32));

		/* Exclude uninitialized words and the vendor's -274000 sentinel. */
		if (value && value >= -40000 && value <= 150000)
			hottest = max(hottest, value);
	}
	if (hottest == INT_MIN)
		return -ENODATA;
	*temp = hottest;
	return 0;
}

static bool turner_bind(struct thermal_zone_device *zone,
			const struct thermal_trip *trip,
			struct thermal_cooling_device *cdev,
			struct cooling_spec *spec)
{
	struct turner_thermal *sensor = thermal_zone_device_priv(zone);
	int i;

	if (trip->type != THERMAL_TRIP_PASSIVE)
		return false;
	for (i = 0; i < ARRAY_SIZE(sensor->cooling); i++) {
		if (cdev != sensor->cooling[i])
			continue;
		spec->upper = cdev->max_state;
		spec->lower = trip->temperature == 80000 ? spec->upper : 0;
		return true;
	}
	return false;
}

static const struct thermal_zone_device_ops turner_tz_ops = {
	.get_temp = turner_get_temp,
	.should_bind = turner_bind,
};

/* Conservative policy for the RAM desktop while validating idle states. */
static const struct thermal_trip trips[] = {
	{ .type = THERMAL_TRIP_PASSIVE, .temperature = 65000, .hysteresis = 5000 },
	{ .type = THERMAL_TRIP_PASSIVE, .temperature = 80000, .hysteresis = 5000 },
	{ .type = THERMAL_TRIP_CRITICAL, .temperature = 90000 },
};
static const struct thermal_zone_params params = { .governor_name = "step_wise" };

static void release_cooling(void *data)
{
	cpufreq_cooling_unregister(data);
}

static void release_zone(void *data)
{
	thermal_zone_device_unregister(data);
}

static int turner_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct turner_thermal *sensor;
	struct resource *res;
	int cpu, count = 0, ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "therm_cputcm");
	if (!res || resource_size(res) < TURNER_CORES * sizeof(u32))
		return -EINVAL;
	sensor->tcm = devm_ioremap_resource(dev, res);
	if (IS_ERR(sensor->tcm))
		return PTR_ERR(sensor->tcm);

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
		struct thermal_cooling_device *cooling;

		if (!policy)
			return -EPROBE_DEFER;
		if (cpu != cpumask_first(policy->related_cpus)) {
			cpufreq_cpu_put(policy);
			continue;
		}
		if (count >= ARRAY_SIZE(sensor->cooling)) {
			cpufreq_cpu_put(policy);
			return -EINVAL;
		}
		cooling = cpufreq_cooling_register(policy);
		cpufreq_cpu_put(policy);
		if (IS_ERR(cooling))
			return PTR_ERR(cooling);
		sensor->cooling[count++] = cooling;
		ret = devm_add_action_or_reset(dev, release_cooling, cooling);
		if (ret)
			return ret;
	}
	if (count != ARRAY_SIZE(sensor->cooling))
		return -ENODEV;
	sensor->zone = thermal_zone_device_register_with_trips("turner-cpu-max",
			trips, ARRAY_SIZE(trips), sensor, &turner_tz_ops, &params,
			250, 1000);
	if (IS_ERR(sensor->zone))
		return PTR_ERR(sensor->zone);
	ret = devm_add_action_or_reset(dev, release_zone, sensor->zone);
	if (ret)
		return ret;
	return thermal_zone_device_enable(sensor->zone);
}

static const struct of_device_id turner_thermal_ids[] = {
	{ .compatible = "mediatek,therm_intf" }, {}
};
MODULE_DEVICE_TABLE(of, turner_thermal_ids);

static struct platform_driver turner_thermal_driver = {
	.probe = turner_probe,
	.driver = { .name = "turner-cpu-thermal", .of_match_table = turner_thermal_ids },
};
module_platform_driver(turner_thermal_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Turner CPU firmware temperature and cluster cooling");
