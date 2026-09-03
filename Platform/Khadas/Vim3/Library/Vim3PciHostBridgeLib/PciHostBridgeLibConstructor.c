/** @file
  Amlogic G12B PCIe bring-up, run from the PciHostBridgeLib constructor.

  The constructor pattern (borrowed from Armada7k8k) makes ordering trivial:
  this code executes when PciHostBridgeDxe is loaded, immediately before its
  entry point asks PciHostBridgeGetRootBridges() for the result - no APRIORI
  entry, no protocol dance.

  Behaviour contract:
   - Board mux = USB3 (or unknowable): return immediately, ZERO hardware
     touched.  The boot is bit-for-bit today's boot.
   - Board mux = PCIe: full bring-up.  If the link trains, GetRootBridges()
     reports one root bridge and NVMe enumeration proceeds.  If it does not
     (empty M.2 slot - the common case, and the only case testable on the
     bench), log the LTSSM state and report ZERO bridges, leaving the boot
     to continue from USB/eMMC.

  A311D erratum (Khadas U-Boot patch "fix usb fail when pci link fails to
  go up"): after a failed link-up the PCIe clocks must be LEFT ENABLED -
  gating them again breaks USB.  So the failure path deliberately powers
  nothing down.

  Init sequence and every register value are from U-Boot
  drivers/pci/pcie_dw_meson.c with two deliberate deviations, both taken
  from Linux pci-meson.c: the MPS/MRRS programming (U-Boot's has a known
  wrong-offset bug) and nothing else.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MesonPcieMuxLib.h>
#include <Library/TimerLib.h>

#include <IndustryStandard/Pci.h>
#include <MesonPcie.h>

//
// Read by PciHostBridgeLib.c (GetRootBridges) and MesonPciSegmentLib's
// link guard.
//
BOOLEAN  mMesonPcieLinkUp = FALSE;

STATIC
VOID
PcieRmw32 (
  IN UINT64  Address,
  IN UINT32  Clear,
  IN UINT32  Set
  )
{
  MmioWrite32 (Address, (MmioRead32 (Address) & ~Clear) | Set);
}

/**
  The G12A PCIe PLL: 24 MHz xtal * 150 / dividers = exactly 100 MHz.

  Same "strict register sequence" as Linux g12a_pcie_pll_init_regs and as
  MesonUsbDxe's MesonInitPciePll() (which runs only in USB3-mux mode, so
  there is no double-init).  Retried like Linux's meson_clk_pcie_pll_enable.
**/
STATIC
BOOLEAN
PciePllInit (
  VOID
  )
{
  UINTN  Retry;
  UINTN  Poll;

  for (Retry = 0; Retry < 10; Retry++) {
    MmioWrite32 (MESON_G12B_HHI_PCIE_PLL_CNTL0, 0x20090496);
    MmioWrite32 (MESON_G12B_HHI_PCIE_PLL_CNTL0, 0x30090496);
    MmioWrite32 (MESON_G12B_HHI_PCIE_PLL_CNTL1, 0x00000000);
    MmioWrite32 (MESON_G12B_HHI_PCIE_PLL_CNTL2, 0x00001100);
    MmioWrite32 (MESON_G12B_HHI_PCIE_PLL_CNTL3, 0x10058E00);
    MmioWrite32 (MESON_G12B_HHI_PCIE_PLL_CNTL4, 0x000100C0);
    MmioWrite32 (MESON_G12B_HHI_PCIE_PLL_CNTL5, 0x68000048);
    MmioWrite32 (MESON_G12B_HHI_PCIE_PLL_CNTL5, 0x68000068);
    MicroSecondDelay (20);
    MmioWrite32 (MESON_G12B_HHI_PCIE_PLL_CNTL4, 0x008100C0);
    MicroSecondDelay (10);
    MmioWrite32 (MESON_G12B_HHI_PCIE_PLL_CNTL0, 0x34090496);
    MmioWrite32 (MESON_G12B_HHI_PCIE_PLL_CNTL0, 0x14090496);
    MicroSecondDelay (10);
    MmioWrite32 (MESON_G12B_HHI_PCIE_PLL_CNTL2, 0x00001000);

    for (Poll = 0; Poll < 5000; Poll++) {
      if ((MmioRead32 (MESON_G12B_HHI_PCIE_PLL_CNTL0) & MESON_G12B_HHI_PCIE_PLL_LOCK) != 0) {
        return TRUE;
      }

      MicroSecondDelay (20);
    }

    DEBUG ((DEBUG_WARN, "PCIe: PLL lock retry %u\n", (UINT32)(Retry + 1)));
  }

  return FALSE;
}

/**
  Find a PCI capability in the root port's own (DBI) config space.
**/
STATIC
UINT32
DbiFindCapability (
  IN UINT8  CapId
  )
{
  UINT32  Pointer;
  UINT32  Header;
  UINTN   Guard;

  Pointer = MmioRead8 (MESON_PCIE_DBI_BASE + PCI_CAPBILITY_POINTER_OFFSET);
  for (Guard = 0; (Pointer >= 0x40) && (Guard < 48); Guard++) {
    Header = MmioRead32 (MESON_PCIE_DBI_BASE + Pointer);
    if ((Header & 0xFF) == CapId) {
      return Pointer;
    }

    Pointer = (Header >> 8) & 0xFF;
  }

  return 0;
}

/**
  Program one outbound iATU region (unrolled layout) and confirm the enable
  latched.  Barriers around the sequence per the Armada7k8k reference - the
  region registers must be complete before CTRL2.ENABLE is observed.
**/
VOID
MesonPcieAtuOutbound (
  IN UINTN   Index,
  IN UINT32  Type,
  IN UINT64  CpuBase,
  IN UINT64  BusBase,
  IN UINT32  Size
  )
{
  UINT64  Region;
  UINTN   Poll;

  Region = MESON_PCIE_ATU_REGION (Index);

  ArmDataMemoryBarrier ();
  MmioWrite32 (Region + ATU_LWR_BASE, (UINT32)CpuBase);
  MmioWrite32 (Region + ATU_UPPER_BASE, (UINT32)(CpuBase >> 32));
  MmioWrite32 (Region + ATU_LIMIT, (UINT32)(CpuBase + Size - 1));
  MmioWrite32 (Region + ATU_LWR_TARGET, (UINT32)BusBase);
  MmioWrite32 (Region + ATU_UPPER_TARGET, (UINT32)(BusBase >> 32));
  MmioWrite32 (Region + ATU_REGION_CTRL1, Type);
  MmioWrite32 (Region + ATU_REGION_CTRL2, ATU_CTRL2_ENABLE);
  ArmDataMemoryBarrier ();

  for (Poll = 0; Poll < 5; Poll++) {
    if ((MmioRead32 (Region + ATU_REGION_CTRL2) & ATU_CTRL2_ENABLE) != 0) {
      return;
    }

    MicroSecondDelay (10000);
  }

  DEBUG ((DEBUG_ERROR, "PCIe: ATU region %u enable did not latch\n", (UINT32)Index));
}

EFI_STATUS
EFIAPI
Vim3PciHostBridgeLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  Cap;
  UINT32  Status12;
  UINTN   Poll;
  UINT32  Ltssm;

  if (!MesonPcieMuxIsPcie ()) {
    //
    // USB3 mode: not a single register is touched on this path.
    //
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_INFO, "PCIe: lane mux selects M.2 - bringing up the port\n"));

  //
  // Clock gates: CLKID_PCIE_COMB (pclk, bit 24) + CLKID_PCIE_PHY (general,
  // bit 27).  Never gated back off - see the A311D erratum in the header.
  //
  PcieRmw32 (MESON_G12B_HHI_GCLK_MPEG1, 0, PCIE_GCLK_MPEG1_MASK);

  //
  // Controller resets: assert port+apb (write 0 - level_low_reset), hold
  // 500 us, release both.  APB is shared with other IP; the assert window
  // is deliberately this one pulse and nothing longer.
  //
  PcieRmw32 (MESON_G12B_RESET_LEVEL0, PCIE_RESET_CTRL_A | PCIE_RESET_APB, 0);
  MicroSecondDelay (PCIE_RESET_DELAY_US);
  PcieRmw32 (MESON_G12B_RESET_LEVEL0, 0, PCIE_RESET_CTRL_A | PCIE_RESET_APB);
  MicroSecondDelay (PCIE_RESET_DELAY_US);

  //
  // 100 MHz reference PLL.  In USB3 mode MesonUsbDxe runs the identical
  // sequence; in PCIe mode that code is skipped, so it runs here.
  //
  if (!PciePllInit ()) {
    DEBUG ((DEBUG_ERROR, "PCIe: reference PLL never locked - no link possible; leaving clocks on\n"));
    return EFI_SUCCESS;
  }

  //
  // Combo PHY, PCIe personality.  Do NOT touch PHY_R0[6:5] (the USB3
  // steering field): leaving it at its reset value of 0 IS the PCIe
  // selection.  Then the whole of the Linux/U-Boot PCIe PHY driver:
  // power state 0x1C, pulse the PHY reset, settle.
  //
  PcieRmw32 (MESON_G12B_USB3_PHY_BASE + 0x0, PHY_R0_PCIE_POWER_MASK, PHY_R0_PCIE_POWER_ON);
  PcieRmw32 (MESON_G12B_RESET_LEVEL0, PCIE_RESET_PHY, 0);
  MicroSecondDelay (PCIE_PERST_SETTLE_US);
  PcieRmw32 (MESON_G12B_RESET_LEVEL0, 0, PCIE_RESET_PHY);
  MicroSecondDelay (PCIE_PERST_SETTLE_US);

  //
  // PERST# (GPIOA_8, active low at the pad).  Mux the pad to GPIO, make it
  // an output, drive LOW (device held in reset) for a generous 1 s - the
  // spec floor is 100 ms; U-Boot chose 1 s after finding devices that need
  // 600 ms - then HIGH to release.  Every transition is logged because a
  // polarity mistake here is invisible on a bench with an empty slot.
  //
  PcieRmw32 (GPIOA_MUX_REG, GPIOA_8_MUX_MASK, 0);
  PcieRmw32 (GPIOA_DIR_REG, GPIOA_8_BIT, 0);
  PcieRmw32 (GPIOA_OUT_REG, GPIOA_8_BIT, 0);
  DEBUG ((DEBUG_INFO, "PCIe: PERST# asserted (GPIOA_8 low)\n"));
  MicroSecondDelay (PCIE_PERST_ASSERT_US);
  PcieRmw32 (GPIOA_OUT_REG, 0, GPIOA_8_BIT);
  DEBUG ((DEBUG_INFO, "PCIe: PERST# released (GPIOA_8 high)\n"));

  //
  // Port logic, under the DBI read-only-field unlock: single lane, no fast
  // link mode, DLL link enable; lane width 1 + speed-change initiate.
  //
  MmioWrite32 (MESON_PCIE_DBI_BASE + PCIE_DBI_RO_WR_EN, 1);

  PcieRmw32 (
    MESON_PCIE_DBI_BASE + PCIE_PORT_LINK_CONTROL,
    PORT_LINK_FAST_LINK_MODE | PORT_LINK_MODE_MASK,
    PORT_LINK_DLL_LINK_EN | PORT_LINK_MODE_1_LANE
    );
  PcieRmw32 (
    MESON_PCIE_DBI_BASE + PCIE_LINK_WIDTH_SPEED_CTL,
    PORT_LOGIC_WIDTH_MASK,
    PORT_LOGIC_WIDTH_1_LANE | PORT_LOGIC_SPEED_CHANGE
    );

  //
  // MPS/MRRS 256 - Linux pci-meson.c's version of this code, not U-Boot's
  // (whose final DEVCTL write lands on the wrong offset).
  //
  Cap = DbiFindCapability (PCI_EXPRESS_CAPABILITY_ID);
  if (Cap != 0) {
    PcieRmw32 (
      MESON_PCIE_DBI_BASE + Cap + 8,      // PCI Express Device Control
      (0x7U << 5) | (0x7U << 12),         // MaxPayload [7:5], MaxReadReq [14:12]
      (0x1U << 5) | (0x1U << 12)          // 256 bytes each
      );
  }

  MmioWrite32 (MESON_PCIE_DBI_BASE + PCIE_DBI_RO_WR_EN, 0);

  //
  // Outbound MEM window, identity-mapped.  The CFG region (index 1) is
  // owned by MesonPciSegmentLib and programmed on first config access.
  //
  MesonPcieAtuOutbound (
    MESON_PCIE_ATU_IDX_MEM,
    ATU_TYPE_MEM,
    MESON_PCIE_MEM_BASE,
    MESON_PCIE_MEM_BASE,
    (UINT32)MESON_PCIE_MEM_SIZE
    );

  //
  // Light the link: LTSSM enable in the Amlogic wrapper, then poll SMLH +
  // RDLH (+ log the LTSSM state) on U-Boot's 40 ms budget.
  //
  PcieRmw32 (MESON_PCIE_CFG_BLOCK + PCIE_CFG0, 0, PCIE_CFG0_APP_LTSSM_EN);

  for (Poll = 0; Poll < PCIE_LINK_POLL_COUNT; Poll++) {
    Status12 = MmioRead32 (MESON_PCIE_CFG_BLOCK + PCIE_CFG_STATUS12);
    if (((Status12 & STATUS12_SMLH_LINK_UP) != 0) &&
        ((Status12 & STATUS12_RDLH_LINK_UP) != 0))
    {
      mMesonPcieLinkUp = TRUE;
      DEBUG ((
        DEBUG_INFO,
        "PCIe: link up after %u us (STATUS12=0x%08x LTSSM=0x%02x)\n",
        (UINT32)(Poll * PCIE_LINK_POLL_US),
        Status12,
        (Status12 >> STATUS12_LTSSM_SHIFT) & STATUS12_LTSSM_MASK
        ));
      return EFI_SUCCESS;
    }

    MicroSecondDelay (PCIE_LINK_POLL_US);
  }

  Ltssm = (MmioRead32 (MESON_PCIE_CFG_BLOCK + PCIE_CFG_STATUS12) >> STATUS12_LTSSM_SHIFT) &
          STATUS12_LTSSM_MASK;
  DEBUG ((
    DEBUG_INFO,
    "PCIe: no link (LTSSM=0x%02x) - empty slot or training failure; "
    "leaving PCIe clocks enabled (A311D USB erratum)\n",
    Ltssm
    ));

  //
  // Deliberately no teardown - see the file header.
  //
  return EFI_SUCCESS;
}
