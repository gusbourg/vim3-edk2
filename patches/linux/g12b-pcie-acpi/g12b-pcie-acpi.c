// SPDX-License-Identifier: GPL-2.0
/*
 * Amlogic G12B (A311D) PCIe host driver for ACPI-mode boots.
 *
 * The DWC root complex on this SoC is not ECAM, so the standard ACPI PCI
 * path (MCFG, or an MCFG quirk in pci_mcfg.c - which is built-in only)
 * cannot describe it to a stock distro kernel.  Instead the firmware
 * installs an SSDT with a PRP0001 platform device - only when the board's
 * USB3/M.2 mux actually selects PCIe - and this module registers the PCI
 * host itself.
 *
 * The firmware has already done the bring-up by the time Linux runs: PHY,
 * clocks (left ON even on link-fail - gating them kills USB on this SoC),
 * link training, bus numbers, BARs, and both outbound iATU regions.  This
 * driver only provides config-space accessors and the host bridge
 * registration; it deliberately mirrors the firmware's own accessor rules
 * (MesonPciSegmentLib in the vim3-edk2 tree):
 *
 *  - root port config space is the DWC DBI block, accessed directly;
 *    bus 0 has exactly one function
 *  - everything downstream goes through a 1 MiB CPU window whose bus-side
 *    target is outbound iATU region 1 - an index/data pair, retargeted in
 *    map_bus (the PCI core's pci_lock serializes config accesses, the same
 *    contract dw_pcie_other_conf_map_bus relies on)
 *  - the root port answers CFG0 for EVERY device number on the secondary
 *    bus; only device 0 is real, the rest must be masked or enumeration
 *    finds 32 copies of the endpoint
 *  - nothing downstream is reachable without a trained link
 *  - the hardware ignores writes to the root port's class-code register
 *    and reports 0x000000: fabricate PCI-PCI bridge on reads, exactly as
 *    mainline pci-meson.c does for DT boots
 *
 * _CRS resource order is fixed ABI with the firmware SSDT (SsdtPcie.asl):
 *   [0] DBI 0xFC000000 4 MiB (iATU unroll at +0x300000)
 *   [1] downstream config window 0xFC500000 1 MiB
 *   [2] Amlogic glue/status block 0xFF648000
 *   [3] bus MEM aperture 0xFC700000 25 MiB (bus == CPU, 1:1)
 *   IRQ[0] = all INTx of all devices (they funnel to one SPI in hardware)
 *   IRQ[1] = DWC built-in MSI demux (unused here; INTx only for now)
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

/* iATU, unrolled mode: flat register file at DBI + 0x300000, 512 B stride */
#define ATU_REGION_OFFSET(n)	(0x300000 + ((n) << 9))
#define ATU_REGION_CFG		1	/* region 0 carries the MEM aperture */
#define ATU_REGION_CTRL1	0x00
#define ATU_REGION_CTRL2	0x04
#define ATU_LWR_BASE		0x08
#define ATU_UPPER_BASE		0x0c
#define ATU_LIMIT		0x10
#define ATU_LWR_TARGET		0x14
#define ATU_UPPER_TARGET	0x18
#define ATU_TYPE_CFG0		0x4
#define ATU_TYPE_CFG1		0x5
#define ATU_CTRL2_ENABLE	BIT(31)

/* glue/status block (_CRS[2]) */
#define PCIE_CFG_STATUS12	0x30
#define STATUS12_SMLH_LINK_UP	BIT(6)
#define STATUS12_RDLH_LINK_UP	BIT(16)
#define STATUS12_LTSSM		GENMASK(14, 10)

/*
 * Clock gates in the always-present HHI block.  Not part of _CRS (it
 * belongs to the whole SoC, not this device); probed once through a
 * throwaway mapping purely as a guard against firmware/module version
 * skew - if the gates were never opened, the first DBI read would stall
 * the fabric, so refuse to probe instead.
 */
#define HHI_GCLK_MPEG1		0xff63c144
#define PCIE_GCLK_COMB		BIT(24)
#define PCIE_GCLK_PHY		BIT(27)
#define PCIE_GCLK_MASK		(PCIE_GCLK_COMB | PCIE_GCLK_PHY)

#define PCI_VENDOR_ID_SYNOPSYS_RC 0x16c3

struct g12b_pcie {
	void __iomem	*dbi;
	void __iomem	*cfg;
	void __iomem	*glue;
	phys_addr_t	cfg_bus_addr;	/* _CRS[1] start, for the iATU base */
	resource_size_t	cfg_size;
	u32		last_atu_target; /* 0 = none yet */
	int		intx_irq;
	struct resource	busn_res;
};

static bool g12b_pcie_link_up(struct g12b_pcie *pcie)
{
	u32 status12 = readl(pcie->glue + PCIE_CFG_STATUS12);

	return (status12 & STATUS12_SMLH_LINK_UP) &&
	       (status12 & STATUS12_RDLH_LINK_UP);
}

/* Point outbound iATU region 1 at one downstream function's config space. */
static void g12b_pcie_retarget_cfg(struct g12b_pcie *pcie, u32 target,
				   u32 type)
{
	void __iomem *region = pcie->dbi + ATU_REGION_OFFSET(ATU_REGION_CFG);
	int poll;

	writel(lower_32_bits(pcie->cfg_bus_addr), region + ATU_LWR_BASE);
	writel(0, region + ATU_UPPER_BASE);
	writel(lower_32_bits(pcie->cfg_bus_addr + pcie->cfg_size - 1),
	       region + ATU_LIMIT);
	writel(target, region + ATU_LWR_TARGET);
	writel(0, region + ATU_UPPER_TARGET);
	writel(type, region + ATU_REGION_CTRL1);
	writel(ATU_CTRL2_ENABLE, region + ATU_REGION_CTRL2);

	/*
	 * Runs under the PCI core's raw pci_lock (map_bus context): busy-wait
	 * only.  The enable bit latches on the first read in practice.
	 */
	for (poll = 0; poll < 100; poll++) {
		if (readl(region + ATU_REGION_CTRL2) & ATU_CTRL2_ENABLE)
			return;
		udelay(10);
	}
}

static void __iomem *g12b_pcie_map_bus(struct pci_bus *bus,
				       unsigned int devfn, int where)
{
	struct g12b_pcie *pcie = bus->sysdata;
	u32 busnr = bus->number;
	u32 target;

	if (busnr == 0) {
		/* The root port itself, straight at the DBI.  One function. */
		if (devfn != 0)
			return NULL;
		return pcie->dbi + where;
	}

	if (!g12b_pcie_link_up(pcie))
		return NULL;

	/* Secondary bus: only device 0 is real (root-port CFG0 alias). */
	if (busnr == 1 && PCI_SLOT(devfn) > 0)
		return NULL;

	target = (busnr << 24) | (PCI_SLOT(devfn) << 19) |
		 (PCI_FUNC(devfn) << 16);
	if (target != pcie->last_atu_target) {
		g12b_pcie_retarget_cfg(pcie, target,
				       busnr == 1 ? ATU_TYPE_CFG0
						  : ATU_TYPE_CFG1);
		pcie->last_atu_target = target;
	}

	return pcie->cfg + where;
}

static int g12b_pcie_rd_conf(struct pci_bus *bus, unsigned int devfn,
			     int where, int size, u32 *val)
{
	int ret = pci_generic_config_read(bus, devfn, where, size, val);

	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;

	/*
	 * The root port's class-code register is read-only garbage
	 * (0x000000); fabricate a PCI-PCI bridge so the core recognises
	 * the port.  Same quirk as mainline pci-meson.c.
	 */
	if (bus->number == 0 && devfn == 0 &&
	    (where & ~3) == PCI_CLASS_REVISION) {
		if (size <= 2)
			*val = (*val & ((1 << (size * 8)) - 1))
			       << (8 * (where & 3));
		*val &= ~GENMASK(31, 8);
		*val |= PCI_CLASS_BRIDGE_PCI_NORMAL << 8;
		if (size <= 2)
			*val = (*val >> (8 * (where & 3)))
			       & ((1 << (size * 8)) - 1);
	}

	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops g12b_pcie_ops = {
	.map_bus	= g12b_pcie_map_bus,
	.read		= g12b_pcie_rd_conf,
	.write		= pci_generic_config_write,
};

/* Every INTx of every device funnels to one SPI in this SoC's hardware. */
static int g12b_pcie_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	struct g12b_pcie *pcie = dev->bus->sysdata;

	return pcie->intx_irq;
}

static int g12b_pcie_clocks_on(struct device *dev)
{
	void __iomem *gclk = ioremap(HHI_GCLK_MPEG1, 4);
	u32 val;

	if (!gclk)
		return -ENOMEM;
	val = readl(gclk);
	iounmap(gclk);

	if ((val & PCIE_GCLK_MASK) != PCIE_GCLK_MASK) {
		dev_err(dev,
			"PCIe clock gates closed (GCLK_MPEG1=%08x); firmware skew?\n",
			val);
		return -ENODEV;
	}
	return 0;
}

static int g12b_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *bridge;
	struct g12b_pcie *pcie;
	struct resource *res;
	u32 status12, ids;
	int ret;

	/* Guard first: a DBI access with gated clocks stalls the fabric. */
	ret = g12b_pcie_clocks_on(dev);
	if (ret)
		return ret;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	if (!bridge)
		return -ENOMEM;
	pcie = pci_host_bridge_priv(bridge);

	pcie->dbi = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pcie->dbi))
		return PTR_ERR(pcie->dbi);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res)
		return -EINVAL;
	pcie->cfg_bus_addr = res->start;
	pcie->cfg_size = resource_size(res);
	pcie->cfg = devm_ioremap_resource(dev, res);
	if (IS_ERR(pcie->cfg))
		return PTR_ERR(pcie->cfg);

	pcie->glue = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(pcie->glue))
		return PTR_ERR(pcie->glue);

	/*
	 * _CRS[3] is the bus MEM aperture, not registers: it becomes the host
	 * bridge window.  ACPI already inserted it into the iomem tree as the
	 * platform device's resource, so use it directly - re-requesting it
	 * collides with itself (-EBUSY), and BARs will nest under it.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 3);
	if (!res)
		return -EINVAL;

	pcie->intx_irq = platform_get_irq(pdev, 0);
	if (pcie->intx_irq < 0)
		return pcie->intx_irq;

	ids = readl(pcie->dbi + PCI_VENDOR_ID);
	if ((u16)ids != PCI_VENDOR_ID_SYNOPSYS_RC) {
		dev_err(dev, "root port IDs %08x, expected vendor %04x\n",
			ids, PCI_VENDOR_ID_SYNOPSYS_RC);
		return -ENODEV;
	}

	status12 = readl(pcie->glue + PCIE_CFG_STATUS12);
	dev_info(dev, "root port %08x, link %s (LTSSM %#lx)\n", ids,
		 g12b_pcie_link_up(pcie) ? "up" : "down",
		 FIELD_GET(STATUS12_LTSSM, status12));

	pcie->busn_res = (struct resource) {
		.name	= "g12b-pcie busn",
		.start	= 0,
		.end	= 0xff,
		.flags	= IORESOURCE_BUS,
	};
	pci_add_resource(&bridge->windows, &pcie->busn_res);
	pci_add_resource_offset(&bridge->windows, res, 0);

	bridge->sysdata = pcie;
	bridge->ops = &g12b_pcie_ops;
	bridge->map_irq = g12b_pcie_map_irq;
	bridge->swizzle_irq = pci_common_swizzle;
	/*
	 * Explicit domain: on ACPI-booted kernels the generic domain lookup
	 * (acpi_pci_bus_find_domain_nr) dereferences sysdata as an ECAM
	 * pci_config_window, which ours is not - it oopses.  Setting
	 * domain_nr skips that path entirely; this is the only host.
	 */
	bridge->domain_nr = 0;
	platform_set_drvdata(pdev, bridge);

	return pci_host_probe(bridge);
}

static void g12b_pcie_remove(struct platform_device *pdev)
{
	struct pci_host_bridge *bridge = platform_get_drvdata(pdev);

	pci_lock_rescan_remove();
	pci_stop_root_bus(bridge->bus);
	pci_remove_root_bus(bridge->bus);
	pci_unlock_rescan_remove();
}

static const struct of_device_id g12b_pcie_match[] = {
	{ .compatible = "amlogic,g12b-pcie-acpi" },
	{ }
};
MODULE_DEVICE_TABLE(of, g12b_pcie_match);

static struct platform_driver g12b_pcie_platform_driver = {
	.driver = {
		.name			= "g12b-pcie-acpi",
		.of_match_table		= g12b_pcie_match,
		.suppress_bind_attrs	= true,
	},
	.probe	= g12b_pcie_probe,
	.remove	= g12b_pcie_remove,
};
module_platform_driver(g12b_pcie_platform_driver);

MODULE_DESCRIPTION("Amlogic G12B PCIe host for ACPI-mode boots");
MODULE_LICENSE("GPL");
