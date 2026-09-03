/** @file
  Amlogic G12B (A311D) PCIe controller - register truth.

  Single-lane DesignWare core, Gen2-capable, sharing its PHY and 100 MHz
  reference PLL with the USB3 port.  Every constant here is taken from a
  working implementation - U-Boot drivers/pci/pcie_dw_meson.c (v2026.10),
  Linux drivers/pci/controller/dwc/pci-meson.c and
  drivers/phy/amlogic/phy-meson-g12a-usb3-pcie.c (v7.0.12) - not from
  datasheets.  Where U-Boot and Linux disagree, the choice is noted.

  The PCIe PLL registers live in MesonG12B.h (shared with MesonUsbDxe,
  which programs the same PLL as the USB3 PHY reference).

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef MESON_PCIE_H_
#define MESON_PCIE_H_

#include <MesonG12B.h>

//
// ---- Address map --------------------------------------------------------
//
// The controller owns CPU window 0xFC000000..0xFDFFFFFF.  The DT carves it
// as dbi 0xFC000000/4M, config 0xFC400000/2M, io 0xFC600000/1M, mem
// 0xFC700000/25M.  U-Boot ignores the DT config window and derives
// io.start - io.size = 0xFC500000/1M instead; that placement is the one
// proven on this hardware, so it is the one used here.  IO is not mapped
// at all - NVMe has no use for port IO.
//
#define MESON_PCIE_DBI_BASE       0xFC000000ULL
#define MESON_PCIE_DBI_SIZE       SIZE_4MB
#define MESON_PCIE_CFG_BASE       0xFC500000ULL
#define MESON_PCIE_CFG_SIZE       SIZE_1MB
#define MESON_PCIE_MEM_BASE       0xFC700000ULL
#define MESON_PCIE_MEM_SIZE       0x01900000ULL   // 25 MiB, bus == CPU (1:1)

//
// iATU, unrolled mode: a flat register file at DBI + 0x300000, one 512-byte
// stripe per region (outbound region N at (N << 9)).  This is the layout
// U-Boot programs on this SoC.
//
#define MESON_PCIE_ATU_BASE       (MESON_PCIE_DBI_BASE + 0x300000ULL)
#define MESON_PCIE_ATU_REGION(n)  (MESON_PCIE_ATU_BASE + ((UINT64)(n) << 9))
#define ATU_REGION_CTRL1          0x00U
#define ATU_REGION_CTRL2          0x04U
#define ATU_LWR_BASE              0x08U
#define ATU_UPPER_BASE            0x0CU
#define ATU_LIMIT                 0x10U
#define ATU_LWR_TARGET            0x14U
#define ATU_UPPER_TARGET          0x18U

#define ATU_TYPE_MEM              0x0U
#define ATU_TYPE_IO               0x2U
#define ATU_TYPE_CFG0             0x4U
#define ATU_TYPE_CFG1             0x5U
#define ATU_CTRL2_ENABLE          BIT31

#define MESON_PCIE_ATU_IDX_MEM    0
#define MESON_PCIE_ATU_IDX_CFG    1

//
// ---- Amlogic wrapper ("cfg") block at 0xFF648000 ------------------------
//
#define MESON_PCIE_CFG_BLOCK      0xFF648000ULL
#define PCIE_CFG0                 0x00U
#define PCIE_CFG0_APP_LTSSM_EN    BIT7
#define PCIE_CFG_STATUS12         0x30U
#define STATUS12_SMLH_LINK_UP     BIT6
#define STATUS12_RDLH_LINK_UP     BIT16
#define STATUS12_LTSSM_SHIFT      10
#define STATUS12_LTSSM_MASK       0x1FU
#define STATUS12_LTSSM_UP         0x11U            // L0

//
// ---- DWC port-logic registers (offsets from DBI) ------------------------
//
#define PCIE_PORT_LINK_CONTROL    0x710U
#define PORT_LINK_FAST_LINK_MODE  BIT7
#define PORT_LINK_DLL_LINK_EN     BIT5
#define PORT_LINK_MODE_MASK       (0x3FU << 16)
#define PORT_LINK_MODE_1_LANE     (0x1U << 16)

#define PCIE_LINK_WIDTH_SPEED_CTL 0x80CU
#define PORT_LOGIC_SPEED_CHANGE   BIT17
#define PORT_LOGIC_WIDTH_MASK     (0x1FU << 8)
#define PORT_LOGIC_WIDTH_1_LANE   (0x1U << 8)

#define PCIE_DBI_RO_WR_EN         0x8BCU           // bit0 unlocks RO config fields

//
// ---- Clock gates (HHI_GCLK_MPEG1 @ 0xFF63C144, see MesonG12B.h) ---------
//
// DT: CLKID_PCIE_COMB ("pclk") = bit 24, CLKID_PCIE_PHY ("general") = bit 27.
//
#define PCIE_GCLK_MPEG1_COMB      BIT24
#define PCIE_GCLK_MPEG1_PHY       BIT27
#define PCIE_GCLK_MPEG1_MASK      (PCIE_GCLK_MPEG1_COMB | PCIE_GCLK_MPEG1_PHY)

//
// ---- Resets -------------------------------------------------------------
//
// Level register RESET_LEVEL0 (0xFFD01080): write 0 = assert, 1 = release
// (level_low_reset).  RESET_PCIE_APB is SHARED with other IP - assert it
// only for the documented 500 us bring-up pulse, never park it low.
//
#define PCIE_RESET_CTRL_A         BIT12            // "port"
#define PCIE_RESET_PHY            BIT14            // combo PHY block
#define PCIE_RESET_APB            BIT15            // shared!
#define PCIE_RESET_DELAY_US       500

//
// ---- Combo USB3/PCIe PHY @ 0xFF646000 (base in MesonG12B.h) -------------
//
// PCIe mode needs no CR-bus tuning at all: leave PHY_R0[6:5] (the USB3
// steering field MesonUsbDxe sets for USB3 mode) at 0, write the power
// state, pulse RESET_PCIE_PHY.  That is the entire Linux/U-Boot PCIe PHY
// driver.
//
#define PHY_R0_PCIE_POWER_MASK    0x1FU            // [4:0]
#define PHY_R0_PCIE_POWER_ON      0x1CU
#define PHY_R0_PCIE_POWER_OFF     0x1DU

//
// ---- PERST# on GPIOA_8 --------------------------------------------------
//
// Active low at the pad.  U-Boot's proven sequence: mux the pad to GPIO,
// output, drive LOW (reset asserted) for a full second - the spec floor is
// 100 ms, U-Boot chose 1 s for slow-spinup devices - then HIGH (released).
//
#define GPIOA_MUX_REG             0xFF6346F8ULL    // PERIPHS_PIN_MUX_E
#define GPIOA_8_MUX_MASK          0x0000000FU      // [3:0] = 0 selects GPIO
#define GPIOA_DIR_REG             0xFF634480ULL    // 0 = output
#define GPIOA_OUT_REG             0xFF634484ULL
#define GPIOA_8_BIT               BIT8
#define PCIE_PERST_ASSERT_US      1000000
#define PCIE_PERST_SETTLE_US      500

//
// ---- Link training budget ----------------------------------------------
//
#define PCIE_LINK_POLL_US         10
#define PCIE_LINK_POLL_COUNT      4000             // 40 ms, U-Boot's budget

#endif // MESON_PCIE_H_
