// SPDX-License-Identifier: GPL-2.0
/*
 * IPC IRQ chip driver for TI K3 AM62L SoC.
 *
 * Copyright (C) 2026 Texas Instruments Incorporated - https://www.ti.com/
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/types.h>

/* IPC register offsets */
#define REG_SET		0x100
#define REG_CLR		0x180

/*
 * IPC_SET / IPC_CLR register layout
 *
 *  [31: 8]  command information
 *  [ 7: 4]  command
 *  [ 3: 1]  reserved
 *  [ 0: 0]  set IPC register bit
 */
#define CMD_MASK	GENMASK(7, 4)
#define CMD_LPM_INFO	0x1

/*
 * IPC_SET register layout for LPM_INFO command
 *
 *  [31:24]  wakeup pin index (WKUP_PIN_UNKNOWN if no specific pin)
 *  [23:12]  wakeup source index (12-bit non-linear firmware encoding)
 *  [11: 8]  low power mode index
 *  [ 7: 4]  command
 *  [ 3: 1]  reserved
 *  [ 0: 0]  set IPC register bit
 */
#define LPM_INFO_WKUP_PIN_MASK	GENMASK(31, 24)
#define LPM_INFO_WKUP_SRC_MASK	GENMASK(23, 12)
#define LPM_INFO_MODE_MASK	GENMASK(11, 8)
#define LPM_INFO_WKUP_PIN_UNKNOWN	0xFF

/**
 * struct ti_ipc_irq_data - K3 ipc irqchip data
 * @irq_domain:	IRQ domain mapping to virqs
 * @dev:	Platform device
 * @base:	MMIO base of IPC registers
 * @parent_irq:	IRQ that triggers child IRQs
 */
struct ti_ipc_irq_data {
	struct irq_domain *irq_domain;
	struct device *dev;
	void __iomem *base;
	int parent_irq;
};

static struct irq_chip ti_ipc_irq_chip = {
	.name	= "k3-ti-ipc",
	.flags	= IRQCHIP_SKIP_SET_WAKE,
};

static int ti_ipc_domain_map(struct irq_domain *d, unsigned int irq,
				 irq_hw_number_t hw)
{
	irq_set_chip_and_handler(irq, &ti_ipc_irq_chip, handle_simple_irq);
	irq_set_chip_data(irq, d->host_data);
	irq_set_noprobe(irq);
	return 0;
}

static const struct irq_domain_ops ti_ipc_domain_ops = {
	.map	= ti_ipc_domain_map,
	.xlate	= irq_domain_xlate_onecell,
};

/**
 * ti_ipc_decode_lpm_info_wkup_src - Expand firmware-packed wakeup source bits
 * @raw: 12-bit source field extracted from IPC_SET[23:12]
 *
 * The firmware packs wakeup sources into IPC_SET[23:12] with non-contiguous
 * bit positions. This function expands this to the sparse decoded
 * values that matches the bit positions from the WKUP_CTRL_MMR_CFG5_WKUP0_SRC
 * register which can be found in the AM62L TRM.
 *
 * Firmware bit -> decoded bit:
 *   [3:0]  -> [3:0]
 *   [6:4]  -> [7:5]   (shift +1)
 *   [8:7]  -> [10:9]  (shift +2)
 *   [11:9] -> [18:16] (shift +7)
 *
 * Returns the decoded wakeup source bitmask used as the hwirq base.
 */
static inline u32 ti_ipc_decode_lpm_info_wkup_src(u32 raw)
{
	return  (raw & 0x00F)        |
		((raw & 0x070) << 1) |
		((raw & 0x180) << 2) |
		((raw & 0xE00) << 7);
}

/**
 * ti_ipc_action_lpm_info - Execute actions based on the LPM info command
 * @data: K3 ipc irqchip data
 * @ipc_val: value received from the IPC register
 *
 * This function decodes the IPC register value based off of the LPM info
 * command. The register will hold the value of the wakeup pin, wakeup source,
 * and last low power mode entered. The function will print the wakeup
 * information, then it will dispatch a virq to the wakeup source consumer if
 * it has a wake irq registered with the interrupt controller. The hwirq for the
 * consumer is encoded as the following: (wkup_src << 8 | wkup_pin).
 */
static void ti_ipc_action_lpm_info(struct ti_ipc_irq_data *data, u32 ipc_val)
{
	irq_hw_number_t hwirq;
	unsigned int virq;
	u32 wkup_src_mask;
	u32 lpm_mask;
	u32 wkup_pin;

	lpm_mask = FIELD_GET(LPM_INFO_MODE_MASK, ipc_val);
	wkup_src_mask = ti_ipc_decode_lpm_info_wkup_src(FIELD_GET(LPM_INFO_WKUP_SRC_MASK, ipc_val));
	wkup_pin = FIELD_GET(LPM_INFO_WKUP_PIN_MASK, ipc_val);

	dev_info_ratelimited(data->dev, "wakeup source:0x%x, pin:0x%x, mode:0x%x\n",
		wkup_src_mask, wkup_pin, lpm_mask);

	if (wkup_pin == LPM_INFO_WKUP_PIN_UNKNOWN)
		wkup_pin = 0;

	hwirq = (wkup_src_mask << 8) | wkup_pin;

	/* don't care if no mapping is found since not all wakeup sources have an interrupt */
	virq = irq_find_mapping(data->irq_domain, hwirq);
	if (virq)
		generic_handle_domain_irq(data->irq_domain, hwirq);
	else
		dev_dbg(data->dev, "No mapping for hwirq %lu\n", hwirq);
}

static void ti_ipc_chained_handle_irq(struct irq_desc *desc)
{
	struct ti_ipc_irq_data *data = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u32 ipc_val;
	u32 cmd;

	chained_irq_enter(chip, desc);

	ipc_val = readl(data->base + REG_SET);
	cmd = FIELD_GET(CMD_MASK, ipc_val);

	if (cmd == CMD_LPM_INFO)
		ti_ipc_action_lpm_info(data, ipc_val);

	writel(ipc_val, data->base + REG_CLR);

	chained_irq_exit(chip, desc);
}

static int ti_ipc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ti_ipc_irq_data *data;
	int parent_irq;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;

	data->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(data->base))
		return PTR_ERR(data->base);

	parent_irq = platform_get_irq(pdev, 0);
	if (parent_irq < 0)
		return parent_irq;

	data->parent_irq = parent_irq;
	data->irq_domain = irq_domain_create_tree(dev_fwnode(dev),
						  &ti_ipc_domain_ops, data);
	if (!data->irq_domain)
		return -ENOMEM;

	irq_set_chained_handler_and_data(data->parent_irq,
					 ti_ipc_chained_handle_irq, data);

	platform_set_drvdata(pdev, data);

	return 0;
}

static void ti_ipc_remove(struct platform_device *pdev)
{
	struct ti_ipc_irq_data *data = platform_get_drvdata(pdev);

	irq_set_chained_handler_and_data(data->parent_irq, NULL, NULL);
	irq_domain_remove(data->irq_domain);
}

static const struct of_device_id ti_ipc_of_match[] = {
	{ .compatible = "ti,k3-ipc-irq" },
	{ }
};
MODULE_DEVICE_TABLE(of, ti_ipc_of_match);

static struct platform_driver ti_ipc_driver = {
	.probe	= ti_ipc_probe,
	.remove	= ti_ipc_remove,
	.driver	= {
		.name		= "ti-k3-ipc",
		.of_match_table	= ti_ipc_of_match,
	},
};
module_platform_driver(ti_ipc_driver);

MODULE_DESCRIPTION("Texas Instruments K3 IPC irqchip driver");
MODULE_AUTHOR("Kendall Willis <k-willis@ti.com>");
MODULE_LICENSE("GPL");
