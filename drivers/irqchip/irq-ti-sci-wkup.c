// SPDX-License-Identifier: GPL-2.0
/*
 * Texas Instruments' K3 wakeup irqchip driver
 *
 * Copyright (C) 2022 Texas Instruments Incorporated - https://www.ti.com/
 */

#include <linux/arm-smccc.h>
#include <linux/cpu_pm.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#define K3_SIP_GET_WKUP_REASON	0xC2000003

/**
 * struct ti_sci_wkup_irq_domain - Structure representing a wakeup IRQ domain.
 */
struct ti_sci_wkup_irq_domain {
	struct irq_domain *irq_domain;
	struct notifier_block nb;
	struct device *dev;
	struct delayed_work wq_wkup;
};

static struct irq_chip ti_sci_wkup_irq_chip = {
	.name = "ti-sci-wkup",
	.flags = IRQCHIP_SKIP_SET_WAKE,
};

static int ti_sci_wkup_irq_domain_map(struct irq_domain *d, unsigned int irq,
				      irq_hw_number_t hw)
{
	struct ti_sci_wkup_irq_domain *intw = d->host_data;

	irq_set_chip_and_handler(irq, &ti_sci_wkup_irq_chip, handle_level_irq);
	irq_set_chip_data(irq, intw);
	irq_set_noprobe(irq);

	return 0;
}

static const struct irq_domain_ops ti_sci_wkup_irq_domain_ops = {
	.map = ti_sci_wkup_irq_domain_map,
	.xlate = irq_domain_xlate_onecell,
};

static int ti_sci_wkup_handle_wake_reason(struct ti_sci_wkup_irq_domain *intw)
{
	struct arm_smccc_res res;
	u32 wkup_source;
	int virq;

	arm_smccc_smc(K3_SIP_GET_WKUP_REASON, 0, 0, 0, 0, 0, 0, 0, &res);
	if (res.a0)
		return -EIO;

	wkup_source = (u32)res.a1;

	virq = irq_find_mapping(intw->irq_domain, wkup_source);
	if (virq) {
		generic_handle_irq(virq);
		dev_info(intw->dev, "DEBUG: Handled wake reason 0x%x virq %d\n",
			 wkup_source, virq);
		return 0;
	}

	dev_info(intw->dev, "DEBUG: Not handled wake reason 0x%x\n", wkup_source);
	return 0;
}

static void ti_sci_wkup_work(struct work_struct *work)
{
	struct ti_sci_wkup_irq_domain *intw =
		container_of(to_delayed_work(work),
			     struct ti_sci_wkup_irq_domain, wq_wkup);

	ti_sci_wkup_handle_wake_reason(intw);
}

static int ti_sci_wkup_notifier(struct notifier_block *nb,
				unsigned long cmd, void *v)
{
	struct ti_sci_wkup_irq_domain *intw =
		container_of(nb, struct ti_sci_wkup_irq_domain, nb);

	if (cmd == CPU_CLUSTER_PM_EXIT)
		ti_sci_wkup_handle_wake_reason(intw);
	return NOTIFY_OK;
}

static int ti_sci_wkup_irq_domain_probe(struct platform_device *pdev)
{
	struct ti_sci_wkup_irq_domain *intw;
	struct device *dev = &pdev->dev;

	intw = devm_kzalloc(dev, sizeof(*intw), GFP_KERNEL);
	if (!intw)
		return -ENOMEM;

	intw->dev = dev;
	platform_set_drvdata(pdev, intw);

	intw->irq_domain = irq_domain_add_tree(dev_of_node(dev),
					       &ti_sci_wkup_irq_domain_ops,
					       intw);
	if (!intw->irq_domain)
		return -ENOMEM;

	intw->nb.notifier_call = ti_sci_wkup_notifier;
	cpu_pm_register_notifier(&intw->nb);

	INIT_DELAYED_WORK(&intw->wq_wkup, ti_sci_wkup_work);
	dev_info(dev, "Wakeup interrupt domain created\n");

	return 0;
}

static void ti_sci_wkup_irq_domain_remove(struct platform_device *pdev)
{
	struct ti_sci_wkup_irq_domain *intw = platform_get_drvdata(pdev);

	cpu_pm_unregister_notifier(&intw->nb);
	irq_domain_remove(intw->irq_domain);
}

static const struct of_device_id ti_sci_wkup_irq_domain_of_match[] = {
	{ .compatible = "ti,sci-wkup", },
	{ },
};
MODULE_DEVICE_TABLE(of, ti_sci_wkup_irq_domain_of_match);

static struct platform_driver ti_sci_wkup_irq_domain_driver = {
	.probe = ti_sci_wkup_irq_domain_probe,
	.remove = ti_sci_wkup_irq_domain_remove,
	.driver = {
		.name = "ti-sci-wkup",
		.of_match_table = ti_sci_wkup_irq_domain_of_match,
	},
};
module_platform_driver(ti_sci_wkup_irq_domain_driver);
