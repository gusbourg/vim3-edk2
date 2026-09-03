/** @file
  Amlogic Meson G12B USB2 host and XHCI registration driver.

  This implements the board power, clock, reset, USB2/USB3 PHY, and G12A glue
  sequences used by the upstream Linux and U-Boot drivers.  Firmware brings up
  SuperSpeed as well as USB2, so a device on a USB3 receptacle is a usable boot
  device; that requires the PCIe PLL (the USB3 PHY's reference clock) as well
  as the glue and the PHY itself - see MesonInitPciePll().  The USB3-vs-M.2
  lane assignment is owned by the MCU mux (Vim3McuDxe register 0x33).

  The USB-C OTG port (usb2-phy1 / glue port 1) is left in the DeviceTree
  dr_mode = "peripheral" end state (device mode, U2D_ACT routed to the DWC2):
  a DT kernel reprograms the glue identically at probe, and an ACPI kernel -
  which never touches the glue - inherits the correct state instead of a
  phantom host port.  The firmware itself never uses phy1; both USB-A
  receptacles sit behind the onboard hub on phy0.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MesonPcieMuxLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/NonDiscoverableDevice.h>

#include <MesonG12B.h>

#define BIT_N(n)         (1U << (n))
#define FIELD32(v, s)    ((UINT32)(v) << (s))

//
// BL2 records the BootROM source in AO_SEC_GP_CFG0.  A RAM-loaded MaskROM
// recovery boot reaches Linux through Ethernet HTTP Boot; enumerating a
// marginal USB mass-storage device first can wedge XhciDxe before BootNext is
// processed.  In that recovery-only case, initialize the board power, clocks,
// resets, PHYs, and glue, but do not register XHCI with the UEFI driver stack.
// A pure-ACPI OS can then bind the standards-based XHCI device without needing
// a Meson-specific glue/PHY driver.
//
#define BOOT_DEVICE_MASK  0xFU
#define BOOT_DEVICE_USB   5U

//
// Clock and reset controls.  A set reset-level bit means deasserted.
//
#define HHI_GATE_USB              BIT25
#define HHI_GATE_USB1_DDR_BRIDGE  BIT8
#define RESET_USB                 BIT2
#define RESET_USB_PHY20           BIT16
#define RESET_USB_PHY21           BIT17

//
// VIM3 fixed regulators:
//   VCC_5V  - GPIOH_8, active-high open drain (release the line to enable)
//   USB_PWR - GPIOA_6, active high
//
#define GPIO_H_DIRECTION  (MESON_G12B_PERIPHS_GPIO_BASE + (9 * 4))
#define GPIO_H_OUTPUT     (MESON_G12B_PERIPHS_GPIO_BASE + (10 * 4))
#define GPIO_A_DIRECTION  (MESON_G12B_PERIPHS_GPIO_BASE + (16 * 4))
#define GPIO_A_OUTPUT     (MESON_G12B_PERIPHS_GPIO_BASE + (17 * 4))
#define MUX_GPIOH_8       (MESON_G12B_PERIPHS_MUX_BASE + (12 * 4))
#define MUX_GPIOA_6       (MESON_G12B_PERIPHS_MUX_BASE + (13 * 4))
#define MUX_GPIOH_8_MASK  (0xFU << 0)
#define MUX_GPIOA_6_MASK  (0xFU << 24)

//
// G12A USB2 PHY control registers.
//
#define PHY_R3             0x0C
#define PHY_R4             0x10
#define PHY_R13            0x34
#define PHY_R14            0x38
#define PHY_R16            0x40
#define PHY_R17            0x44
#define PHY_R18            0x48
#define PHY_R20            0x50
#define PHY_R21            0x54

#define PHY_R16_VALUE      (FIELD32 (20, 0) | FIELD32 (1, 10) | \
                            BIT22 | BIT24 | BIT27 | BIT28)
#define PHY_R17_VALUE      (FIELD32 (7, 17) | FIELD32 (7, 20) | \
                            FIELD32 (2, 24) | FIELD32 (9, 28))
#define PHY_R18_VALUE      (FIELD32 (1, 0) | FIELD32 (9, 2) | \
                            FIELD32 (0x27, 6) | FIELD32 (1, 14) | \
                            FIELD32 (7, 16) | FIELD32 (3, 19) | \
                            FIELD32 (1, 22) | FIELD32 (3, 26) | \
                            FIELD32 (1, 29) | BIT31)
#define PHY_R20_VALUE      (FIELD32 (4, 1) | BIT4 | FIELD32 (15, 9) | \
                            BIT13 | FIELD32 (3, 14))
#define PHY_R4_VALUE       (FIELD32 (0x0F, 0) | FIELD32 (0x0F, 8) | \
                            FIELD32 (0x0F, 16) | BIT27)
#define PHY_R3_VALUE       (FIELD32 (1, 2) | FIELD32 (3, 4))
#define PHY_R13_VALUE      (BIT15 | FIELD32 (7, 16))

//
// G12A USB glue registers.
//
#define U2P_R0(port)                 ((port) * 0x20)
#define U2P_R0_HOST_DEVICE           BIT0
#define U2P_R0_POWER_ON_RESET        BIT3
#define U2P_R0_ID_PULLUP             BIT4
#define U2P_R0_DRV_VBUS              BIT5
#define USB_R0                       0x80
#define USB_R0_U2D_SS_SCALEDOWN_MODE_MASK  (0x3U << 29)
#define USB_R0_U2D_ACT               BIT31
#define USB_R1                       0x84
#define USB_R1_U3_PORT_DISABLE       BIT16
#define USB_R1_PORT_POWER_CTRL       BIT17
#define USB_R1_FLADJ_MASK            (0x3FU << 19)
#define USB_R1_TX_SWING_FULL_MASK    (0x7FU << 25)
//
// SuperSpeed (USB3) glue.  Offsets are the Amlogic USB_Rn block at
// GLUE_BASE + 0x80, i.e. Linux's USB_R2/USB_R3 (0x08/0x0c) + 0x80.
//
#define USB_R2                       0x88
#define USB_R2_TX_DEEMPH_3P5DB_MASK  (0x3FU << 20)
#define USB_R2_TX_DEEMPH_6DB_MASK    (0x3FU << 26)
#define USB_R3                       0x8C
#define USB_R3_P30_SSC_ENABLE        BIT0
#define USB_R3_P30_SSC_RANGE_MASK    (0x7U << 1)
#define USB_R3_P30_REF_SSP_EN        BIT13
#define USB_R4                       0x90
#define USB_R4_P21_SLEEP_M0          BIT1
#define USB_R4_P21_ONLY              BIT4
#define USB_R5                       0x94
#define USB_R5_ID_DIG_EN_0           BIT4
#define USB_R5_ID_DIG_EN_1           BIT5
#define USB_R5_ID_DIG_TH_MASK        (0xFFU << 8)

#define USB_VBUS_STABILIZE_US         100000

//
// DWC3 controller global registers (Synopsys core), within the XHCI MMIO
// window at XHCI_BASE + DWC3_GLOBALS_REGS_START (0xC100).  EDK2's generic
// XhciDxe only resets the XHCI layer (HCRST); these core registers retain
// state across a warm reboot - notably Linux leaves GUSB2PHYCFG.SUSPHY set,
// which lets the high-speed PHY power down and breaks device detection on the
// next firmware boot until a full power cycle.  Mirror U-Boot's proven
// dwc3_core_soft_reset(): hold the core in reset, assert then deassert BOTH
// GUSB2PHYCFG/GUSB3PIPECTL PHYSOFTRST so the DWC3<->PHY interface re-syncs,
// release the core, clear the PHY suspend bits, and select host mode (the core
// soft reset re-defaults PRTCAPDIR).
//
#define DWC3_GCTL                    (MESON_G12B_XHCI_BASE + 0xC110)
#define DWC3_GCTL_CORESOFTRESET      BIT11
#define DWC3_GCTL_PRTCAPDIR_MASK     (0x3U << 12)
#define DWC3_GCTL_PRTCAPDIR_HOST     (0x1U << 12)
#define DWC3_GUSB2PHYCFG0            (MESON_G12B_XHCI_BASE + 0xC200)
#define DWC3_GUSB2PHYCFG_SUSPHY      BIT6
#define DWC3_GUSB2PHYCFG_PHYSOFTRST  BIT31
#define DWC3_GUSB3PIPECTL0           (MESON_G12B_XHCI_BASE + 0xC2C0)
#define DWC3_GUSB3PIPECTL_SUSPHY     BIT17
#define DWC3_GUSB3PIPECTL_PHYSOFTRST BIT31

//
// Frame-length adjustment.  The Amlogic G12 DT sets
// snps,quirk-frame-length-adjustment = 0x20; without it the micro-frame (SOF)
// length is left at the reset default, which mis-schedules periodic and split
// transactions behind a hub (works direct, flaky behind a hub).  Linux applies
// it in dwc3_frame_length_adjustment().
//
#define DWC3_GFLADJ                  (MESON_G12B_XHCI_BASE + 0xC630)
#define DWC3_GFLADJ_30MHZ_MASK       0x3FU
#define DWC3_GFLADJ_30MHZ_SDBND_SEL  BIT7
#define VIM3_DWC3_FLADJ              0x20U

//
// Park mode.  The board DT sets snps,parkmode-disable-ss-quirk, the third and
// last of the three dwc3 quirks it requests (the other two are handled above:
// dis_u2_susphy and quirk-frame-length-adjustment).  Park mode lets the
// controller's bus arbiter stay parked on a single endpoint; on this
// integration that loses transactions once several devices are active, which
// is why a lone device works while a populated hub tree is flaky.  Linux
// applies it unconditionally from the DT property in dwc3_core_init()
// (drivers/usb/dwc3/core.c, DWC3_GUCTL1_PARKMODE_DISABLE_SS).
//
#define DWC3_GUCTL1                     (MESON_G12B_XHCI_BASE + 0xC11C)
#define DWC3_GUCTL1_PARKMODE_DISABLE_SS BIT17

//
// USB3/PCIe combo PHY (amlogic,g12a-usb3-pcie-phy).  Releasing the SuperSpeed
// port in the controller glue is not enough on its own: this PHY still has to
// be switched from PCIe to USB3 and configured, or the port is enabled with an
// unusable PHY behind it.  Linux does this in phy-meson-g12a-usb3-pcie.c; under
// DeviceTree that hides a firmware omission, but an ACPI OS binds only the
// generic xHCI driver and never touches this block, so the firmware must leave
// it correct.
//
// PHY_R4/PHY_R5 are a serial control-register ("CR") port into the PHY's own
// register file, driven by a capture/strobe handshake against CR_ACK.
//
#define RESET_USB3_PCIE_PHY        BIT14           // RESET_LEVEL0, DT reset id 14
#define U3P_R0                     0x00
#define U3P_R0_PCIE_USB3_SWITCH    (0x3U << 5)
#define U3P_R1                     0x04
#define U3P_R1_LOS_LEVEL_MASK      (0x1FU << 16)
#define U3P_R1_LOS_BIAS_MASK       (0x7U << 21)
#define U3P_R2                     0x08
#define U3P_R2_TX_VBOOST_LVL_MASK  (0x7U << 18)
#define U3P_R4                     0x10
#define U3P_R4_CR_WRITE            BIT0
#define U3P_R4_CR_READ             BIT1
#define U3P_R4_CR_DATA_IN_MASK     (0xFFFFU << 2)
#define U3P_R4_CR_CAP_DATA         BIT18
#define U3P_R4_CR_CAP_ADDR         BIT19
#define U3P_R5                     0x14
#define U3P_R5_CR_DATA_OUT_MASK    0xFFFFU
#define U3P_R5_CR_ACK              BIT16
//
// CR handshake budget: Linux polls every 5 us up to 1 ms.
//
#define U3P_CR_POLL_US             5
#define U3P_CR_POLL_COUNT          200

//
// Settle delay after DWC3/PHY host-mode bring-up, before XhciDxe starts.  The
// first enumeration is otherwise timing-sensitive at RELEASE speed and the
// whole USB tree intermittently fails to enumerate (all-or-nothing).
//
#define MESON_USB_HOST_SETTLE_US    (500 * 1000)

STATIC
VOID
Rmw32 (
  IN UINTN   Address,
  IN UINT32  ClearMask,
  IN UINT32  SetMask
  )
{
  MmioWrite32 (Address, (MmioRead32 (Address) & ~ClearMask) | SetMask);
}

STATIC
VOID
PulseReset (
  IN UINTN   ResetLevel,
  IN UINT32  Mask,
  IN UINTN   HoldMicroseconds
  )
{
  MmioAnd32 (ResetLevel, ~Mask);
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (HoldMicroseconds);
  MmioOr32 (ResetLevel, Mask);
  ArmDataSynchronizationBarrier ();
}

STATIC
VOID
MesonEnableUsbPower (
  VOID
  )
{
  //
  // Make both regulator control pins GPIOs. GPIOH_8 lives in the second
  // pinmux word for bank H; GPIOA_6 lives in the first word for bank A.
  //
  Rmw32 (MUX_GPIOH_8, MUX_GPIOH_8_MASK, 0);
  Rmw32 (MUX_GPIOA_6, MUX_GPIOA_6_MASK, 0);

  //
  // Open-drain high is represented by releasing GPIOH_8 (input).  Program
  // its output latch high as well so a later output transition stays safe.
  //
  MmioOr32 (GPIO_H_OUTPUT, BIT8);
  MmioOr32 (GPIO_H_DIRECTION, BIT8);

  //
  // Drive USB_PWR high.  Program the latch before changing direction to
  // avoid a low pulse on the 5 V rail.
  //
  MmioOr32 (GPIO_A_OUTPUT, BIT6);
  MmioAnd32 (GPIO_A_DIRECTION, ~BIT6);
  ArmDataSynchronizationBarrier ();

  //
  // The VIM3 host ports sit behind externally powered USB circuitry.  A
  // one-millisecond delay is not sufficient after a cold power-on: the XHCI
  // root hub can observe a connection before the hub or mass-storage device
  // is ready to complete SET_ADDRESS, producing an intermittent transaction
  // error.  USB 2.0 permits up to 100 ms for power to stabilize.
  //
  MicroSecondDelay (USB_VBUS_STABILIZE_US);
}

STATIC
VOID
MesonInitUsb2Phy (
  IN UINTN   Base,
  IN UINT32  ResetMask
  )
{
  PulseReset (MESON_G12B_RESET_LEVEL1, ResetMask, 1);
  MicroSecondDelay (1000);

  MmioAnd32 (Base + PHY_R21, ~BIT2);
  MmioWrite32 (Base + PHY_R16, PHY_R16_VALUE | BIT29);
  MmioWrite32 (Base + PHY_R17, PHY_R17_VALUE);
  MmioWrite32 (Base + PHY_R18, PHY_R18_VALUE);
  MicroSecondDelay (100);
  MmioWrite32 (Base + PHY_R16, PHY_R16_VALUE);

  MmioWrite32 (Base + PHY_R20, PHY_R20_VALUE);
  MmioWrite32 (Base + PHY_R4, PHY_R4_VALUE);
  MmioWrite32 (Base + PHY_R3, PHY_R3_VALUE);
  MmioWrite32 (Base + PHY_R14, 0);
  MmioWrite32 (Base + PHY_R13, PHY_R13_VALUE);
  ArmDataSynchronizationBarrier ();
}

/**
  Wait for the USB3 PHY control-register handshake line to reach a state.

  @param  Asserted  TRUE to wait for CR_ACK set, FALSE for CR_ACK clear.

  @retval TRUE      The handshake completed.
  @retval FALSE     Timed out; the PHY is not responding.
**/
STATIC
BOOLEAN
U3PhyWaitAck (
  IN BOOLEAN  Asserted
  )
{
  UINTN    Index;
  BOOLEAN  IsSet;

  for (Index = 0; Index < U3P_CR_POLL_COUNT; Index++) {
    IsSet = (MmioRead32 (MESON_G12B_USB3_PHY_BASE + U3P_R5) & U3P_R5_CR_ACK) != 0;
    if (IsSet == Asserted) {
      return TRUE;
    }

    MicroSecondDelay (U3P_CR_POLL_US);
  }

  DEBUG ((DEBUG_ERROR, "MesonUSB: USB3 PHY CR handshake timeout (want ack=%d)\n", Asserted));
  return FALSE;
}

/**
  Latch an address onto the USB3 PHY control-register bus.
**/
STATIC
BOOLEAN
U3PhyCrSetAddress (
  IN UINT16  Address
  )
{
  UINTN   Reg;
  UINT32  Value;

  Reg   = MESON_G12B_USB3_PHY_BASE + U3P_R4;
  Value = ((UINT32)Address << 2) & U3P_R4_CR_DATA_IN_MASK;

  //
  // The data lines are driven twice before the capture strobe, as the PHY
  // samples them on the strobe edge (mirrors the Linux driver exactly).
  //
  MmioWrite32 (Reg, Value);
  MmioWrite32 (Reg, Value);
  MmioWrite32 (Reg, Value | U3P_R4_CR_CAP_ADDR);
  if (!U3PhyWaitAck (TRUE)) {
    return FALSE;
  }

  MmioWrite32 (Reg, Value);
  return U3PhyWaitAck (FALSE);
}

/**
  Read one 16-bit USB3 PHY control register.
**/
STATIC
BOOLEAN
U3PhyCrRead (
  IN  UINT16  Address,
  OUT UINT16  *Data
  )
{
  UINTN  Reg;

  if (!U3PhyCrSetAddress (Address)) {
    return FALSE;
  }

  Reg = MESON_G12B_USB3_PHY_BASE + U3P_R4;
  MmioWrite32 (Reg, 0);
  MmioWrite32 (Reg, U3P_R4_CR_READ);
  if (!U3PhyWaitAck (TRUE)) {
    return FALSE;
  }

  *Data = (UINT16)(MmioRead32 (MESON_G12B_USB3_PHY_BASE + U3P_R5) & U3P_R5_CR_DATA_OUT_MASK);

  MmioWrite32 (Reg, 0);
  return U3PhyWaitAck (FALSE);
}

/**
  Write one 16-bit USB3 PHY control register.
**/
STATIC
BOOLEAN
U3PhyCrWrite (
  IN UINT16  Address,
  IN UINT16  Data
  )
{
  UINTN   Reg;
  UINT32  Value;

  if (!U3PhyCrSetAddress (Address)) {
    return FALSE;
  }

  Reg   = MESON_G12B_USB3_PHY_BASE + U3P_R4;
  Value = ((UINT32)Data << 2) & U3P_R4_CR_DATA_IN_MASK;

  MmioWrite32 (Reg, Value);
  MmioWrite32 (Reg, Value);
  MmioWrite32 (Reg, Value | U3P_R4_CR_CAP_DATA);
  if (!U3PhyWaitAck (TRUE)) {
    return FALSE;
  }

  MmioWrite32 (Reg, Value);
  if (!U3PhyWaitAck (FALSE)) {
    return FALSE;
  }

  MmioWrite32 (Reg, Value);
  MmioWrite32 (Reg, Value | U3P_R4_CR_WRITE);
  if (!U3PhyWaitAck (TRUE)) {
    return FALSE;
  }

  MmioWrite32 (Reg, Value);
  return U3PhyWaitAck (FALSE);
}

/**
  Read-modify-write one USB3 PHY control register.
**/
STATIC
BOOLEAN
U3PhyCrUpdate (
  IN UINT16  Address,
  IN UINT16  ClearMask,
  IN UINT16  SetMask
  )
{
  UINT16  Data;

  if (!U3PhyCrRead (Address, &Data)) {
    return FALSE;
  }

  Data = (UINT16)((Data & ~ClearMask) | SetMask);
  return U3PhyCrWrite (Address, Data);
}

/**
  Bring up the PCIe PLL, which is the USB3/PCIe PHY's reference clock.

  The PHY's DT node names it explicitly:

      clocks = <&clkc CLKID_PCIE_PLL>;
      clock-names = "ref_clk";
      assigned-clock-rates = <100000000>;

  so under DeviceTree the clock framework turns it on before the PHY driver
  runs, and firmware never noticed it was missing.  With no reference clock the
  SuperSpeed PHY cannot generate a PIPE clock: the port stays in RxDetect and
  never asserts CCS, so *no SuperSpeed device is visible to firmware at all* -
  a device on a USB3 receptacle simply does not exist as far as the boot menu
  is concerned, while Linux sees it fine because it programs this PLL itself.

  Measured on hardware (VIM3 Pro, USB3 hub + PNY stick), reading xHCI PORTSC
  for the SuperSpeed root port from the UEFI Shell:

      PLL disabled (CNTL0 = 0x20000000):  PORTSC3 = 0x000002A0
                                          CCS=0, PLS=5 (RxDetect)
      PLL locked   (CNTL0 = 0xD4090496):  PORTSC3 = 0x00001203
                                          CCS=1, PED=1, PLS=0 (U0), Speed=4 (SS)

  The register sequence is Linux's g12a_pcie_pll_init_regs (drivers/clk/meson/
  g12a.c) replayed verbatim; the values encode m=150, n=1, od=9 off the 24 MHz
  xtal, i.e. 24*150/1 = 3600 MHz DCO, /2 = 1800, /9 = 200, /2 = 100 MHz.
  Bit 31 of CNTL0 is the hardware lock indication.
**/
STATIC
VOID
MesonInitPciePll (
  VOID
  )
{
  UINTN  Retry;
  UINTN  Poll;

  //
  // Retried like Linux's meson_clk_pcie_pll_enable and like the PCIe-mode
  // copy of this sequence in Vim3PciHostBridgeLib: the full register
  // sequence is replayed on every attempt.  Do not fail the whole USB
  // bring-up if it never locks: USB2 is independent of this PLL and must
  // still work - but a single unretried attempt leaving SuperSpeed
  // released behind a clockless PHY is the condition previously observed
  // to destabilise USB2 as well.
  //
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
    ArmDataSynchronizationBarrier ();

    for (Poll = 0; Poll < 5000; Poll++) {
      if ((MmioRead32 (MESON_G12B_HHI_PCIE_PLL_CNTL0) &
           MESON_G12B_HHI_PCIE_PLL_LOCK) != 0)
      {
        DEBUG ((DEBUG_INFO, "MesonUSB: PCIe PLL locked (SuperSpeed ref clock up)\n"));
        return;
      }

      MicroSecondDelay (20);
    }

    DEBUG ((DEBUG_WARN, "MesonUSB: PCIe PLL lock retry %u\n", (UINT32)(Retry + 1)));
  }

  DEBUG ((DEBUG_ERROR, "MesonUSB: PCIe PLL did not lock; SuperSpeed unavailable\n"));
}

/**
  Bring up the USB3 (SuperSpeed) PHY - phy_g12a_usb3_init() in Linux's
  phy-meson-g12a-usb3-pcie.c.  The magic CR-bus addresses are the PHY vendor's
  errata workarounds; the comments name what each one does.
**/
STATIC
VOID
MesonInitUsb3Phy (
  VOID
  )
{
  UINT16  Data;

  //
  // The PHY's reference clock must be running before it is reset and
  // configured, or nothing downstream of it can train.
  //
  MesonInitPciePll ();

  PulseReset (MESON_G12B_RESET_LEVEL0, RESET_USB3_PCIE_PHY, 1);
  MicroSecondDelay (100);

  //
  // Steer the shared PHY to USB3 rather than PCIe.  Without this the
  // SuperSpeed port has no PHY behind it at all.
  //
  Rmw32 (
    MESON_G12B_USB3_PHY_BASE + U3P_R0,
    U3P_R0_PCIE_USB3_SWITCH,
    U3P_R0_PCIE_USB3_SWITCH
    );

  //
  // SSPHY suspend erratum: without LANE0.TX_ALT_BLOCK.EN_ALT_BUS the port
  // enumerates at high speed instead of SuperSpeed.
  //
  if (!U3PhyCrUpdate (0x102D, 0, BIT7)) {
    return;
  }

  if (!U3PhyCrUpdate (0x1010, 0xFF0, 20 & 0xFF0)) {
    return;
  }

  //
  // LANE0.RX_OVRD_IN_HI: force RX equalisation to 3 and take the override.
  //
  if (!U3PhyCrRead (0x1006, &Data)) {
    return;
  }

  Data &= (UINT16) ~BIT6;
  Data |= BIT7;
  Data &= (UINT16) ~(0x7 << 8);
  Data |= (0x3 << 8);
  Data |= BIT11;
  if (!U3PhyCrWrite (0x1006, Data)) {
    return;
  }

  //
  // LANE0.TX_OVRD_DRV_LO: pre-emphasis 22, amplitude 127, enable.
  //
  if (!U3PhyCrRead (0x1002, &Data)) {
    return;
  }

  Data &= (UINT16) ~0x3F80;
  Data |= (0x16 << 7);
  Data &= (UINT16) ~0x7F;
  Data |= (0x7F | BIT14);
  if (!U3PhyCrWrite (0x1002, Data)) {
    return;
  }

  //
  // MPLL_LOOP_CTL.PROP_CNTRL = 8.
  //
  if (!U3PhyCrUpdate (0x30, 0xF << 4, 8 << 4)) {
    return;
  }

  Rmw32 (
    MESON_G12B_USB3_PHY_BASE + U3P_R2,
    U3P_R2_TX_VBOOST_LVL_MASK,
    FIELD32 (0x4, 18)
    );
  Rmw32 (
    MESON_G12B_USB3_PHY_BASE + U3P_R1,
    U3P_R1_LOS_BIAS_MASK | U3P_R1_LOS_LEVEL_MASK,
    FIELD32 (4, 21) | FIELD32 (9, 16)
    );

  ArmDataSynchronizationBarrier ();
  DEBUG ((DEBUG_INFO, "MesonUSB: USB3 PHY initialised\n"));
}

STATIC
VOID
MesonInitUsbGlue (
  VOID
  )
{
  UINTN   Base;
  UINT32  Port;
  UINT32  Value;

  Base = MESON_G12B_USB_GLUE_BASE;
  for (Port = 0; Port < 2; Port++) {
    //
    // Port 0 drives the onboard hub (both USB-A receptacles): host mode.
    // Port 1 is the USB-C OTG port: the board DT says dr_mode =
    // "peripheral", so mirror dwc3-meson-g12a's PHY_MODE_USB_DEVICE state
    // (HOST_DEVICE clear).  ID_PULLUP and DRV_VBUS stay SET - Linux sets
    // both on the OTG phy unconditionally in either mode, and the VIM3 DT
    // has no vbus-supply, so this matches the DT end state exactly.  A DT
    // kernel reprograms all of this identically at glue probe (no-op); an
    // ACPI kernel inherits it, which is the point.
    //
    Value = U2P_R0_POWER_ON_RESET;
    if (Port == 1) {
      Value |= U2P_R0_ID_PULLUP | U2P_R0_DRV_VBUS;
    } else {
      Value |= U2P_R0_HOST_DEVICE;
    }

    Rmw32 (
      Base + U2P_R0 (Port),
      U2P_R0_POWER_ON_RESET | U2P_R0_HOST_DEVICE |
      U2P_R0_ID_PULLUP | U2P_R0_DRV_VBUS,
      Value
      );
    Rmw32 (
      Base + U2P_R0 (Port),
      U2P_R0_POWER_ON_RESET,
      0
      );
  }

  //
  // Program the frame-length adjustment, and explicitly RELEASE the SuperSpeed
  // port.  Earlier revisions of this driver set USB_R1_U3_PORT_DISABLE and
  // USB_R4_P21_ONLY, which disable the USB3 port and force the glue into
  // USB2-only mode: any device reached over SuperSpeed - directly or through a
  // USB3 hub - was then invisible to the firmware, while Linux saw it because
  // dwc3-meson-g12a re-initialises the glue and never sets either bit (they are
  // defined in drivers/usb/dwc3/dwc3-meson-g12a.c and never written).  Both are
  // cleared rather than merely left alone, so a warm reboot inherits a released
  // port instead of whatever the previous boot left behind.
  //
  Rmw32 (
    Base + USB_R1,
    USB_R1_FLADJ_MASK | USB_R1_U3_PORT_DISABLE,
    FIELD32 (0x20, 19)
    );
  Rmw32 (
    Base + USB_R5,
    USB_R5_ID_DIG_EN_0 | USB_R5_ID_DIG_EN_1 | USB_R5_ID_DIG_TH_MASK,
    USB_R5_ID_DIG_EN_0 | USB_R5_ID_DIG_EN_1 |
    FIELD32 (0xFF, 8)
    );
  //
  // Device mode for the OTG port, per dwc3_meson_g12a_usb_otg_apply_mode
  // (PHY_MODE_USB_DEVICE): route phy1's UTMI to the DWC2 device controller
  // (U2D_ACT), no scaledown, and let the host controller sleep port 21.
  // P21_ONLY stays cleared so the SuperSpeed path and the phy0 host ports
  // remain fully enabled - Linux runs its xHCI against exactly this glue
  // state on this silicon.  G12B never sets the USB_R1 port-isolation
  // bits (only GXL/GXM drvdata do), so neither do we.
  //
  Rmw32 (
    Base + USB_R0,
    USB_R0_U2D_ACT | USB_R0_U2D_SS_SCALEDOWN_MODE_MASK,
    USB_R0_U2D_ACT
    );
  Rmw32 (
    Base + USB_R4,
    USB_R4_P21_SLEEP_M0 | USB_R4_P21_ONLY,
    USB_R4_P21_SLEEP_M0
    );

  //
  // dwc3_meson_g12a_usb3_init(): bring up the SuperSpeed PIPE interface -
  // reference clock, TX de-emphasis and swing.  Without this the U3 port is
  // released but never properly configured.
  //
  // NOTE: spread-spectrum stays OFF.  Linux's source reads as if it enables
  // SSC, but regmap_update_bits() masks the value by the mask before writing
  // (tmp = orig & ~mask; tmp |= val & mask), and USB_R3_P30_SSC_ENABLE (BIT0)
  // is not in that mask - so the hardware never sees it.  Setting it here is a
  // real behavioural divergence from Linux: spreading the SuperSpeed PIPE
  // reference clock made usb1-port1 fail connect-debounce under an ACPI OS,
  // which re-uses this state instead of re-initialising the glue itself.
  //
  Rmw32 (
    Base + USB_R3,
    USB_R3_P30_SSC_ENABLE | USB_R3_P30_SSC_RANGE_MASK | USB_R3_P30_REF_SSP_EN,
    FIELD32 (2, 1) | USB_R3_P30_REF_SSP_EN
    );
  MicroSecondDelay (2);

  Rmw32 (Base + USB_R2, USB_R2_TX_DEEMPH_3P5DB_MASK, FIELD32 (0x15, 20));
  Rmw32 (Base + USB_R2, USB_R2_TX_DEEMPH_6DB_MASK, FIELD32 (0x20, 26));
  MicroSecondDelay (2);

  Rmw32 (Base + USB_R1, USB_R1_PORT_POWER_CTRL, USB_R1_PORT_POWER_CTRL);
  Rmw32 (Base + USB_R1, USB_R1_TX_SWING_FULL_MASK, FIELD32 (127, 25));

  ArmDataSynchronizationBarrier ();
}

STATIC
EFI_STATUS
EFIAPI
MesonUsbInitialize (
  IN NON_DISCOVERABLE_DEVICE  *This
  )
{
  UINT32  Capability;

  //
  // No mUsbInitialized short-circuit here: NonDiscoverablePciDeviceDxe calls
  // this once per Start (guarded by its own Dev->Enabled), so normal boot still
  // initialises once - but a deliberate DisconnectController/ConnectController
  // of the XHCI handle (used to retry a failed enumeration) allocates a fresh
  // Dev and re-enters here, which MUST re-run the full DWC3/PHY bring-up for the
  // retry to start from a clean controller.
  //

  DEBUG ((DEBUG_INFO, "MesonUSB: enabling VIM3 USB power rails\n"));
  MesonEnableUsbPower ();

  DEBUG ((DEBUG_INFO, "MesonUSB: enabling controller clocks\n"));
  MmioOr32 (MESON_G12B_HHI_GCLK_MPEG1, HHI_GATE_USB);
  MmioOr32 (
    MESON_G12B_HHI_GCLK_MPEG2,
    HHI_GATE_USB1_DDR_BRIDGE
    );

  DEBUG ((DEBUG_INFO, "MesonUSB: resetting controller and USB2 PHYs\n"));
  PulseReset (MESON_G12B_RESET_LEVEL1, RESET_USB, 1);
  MesonInitUsbGlue ();
  MesonInitUsb2Phy (MESON_G12B_USB2_PHY0_BASE, RESET_USB_PHY20);
  MesonInitUsb2Phy (MESON_G12B_USB2_PHY1_BASE, RESET_USB_PHY21);
  //
  // The glue above releases the SuperSpeed port; this configures the PHY that
  // sits behind it.  Ordering matches dwc3-meson-g12a: glue first, PHYs after.
  //
  // Unless the board mux routes the shared lane to PCIe: then the combo PHY
  // belongs to the PCIe host bridge, which has ALREADY trained its link by
  // the time this driver runs at BDS connect.  MesonInitUsb3Phy() would
  // reset that PHY block and re-steer PHY_R0[6:5] to USB3, killing the
  // trained link - the exact override that used to defeat the mux setting.
  // USB2 is a different PHY and keeps working either way.
  //
  if (MesonPcieMuxIsPcie ()) {
    DEBUG ((DEBUG_INFO, "MesonUSB: lane mux is PCIe - leaving the combo PHY alone (USB2 only)\n"));
  } else {
    MesonInitUsb3Phy ();
  }

  //
  // Full DWC3 core soft reset, mirroring U-Boot's dwc3_core_soft_reset() (the
  // proven-on-this-board bring-up we replace).  A bare GCTL.CORESOFTRESET pulse
  // is NOT enough after a warm reboot: the DWC3<->UTMI/PIPE interface inherits
  // stale state from the previous OS and high-speed device detection then fails
  // in firmware (BIOS/GRUB) until a cold or reset-button cycle.  Hold the core
  // in reset, toggle BOTH PHY soft-resets so the interface re-syncs to the
  // freshly-initialized Meson USB2 PHYs (PLLs already up above), then release
  // the core.  100 ms settle each, per the DWC3 programming guide.
  //
  DEBUG ((DEBUG_INFO, "MesonUSB: DWC3 core + PHY soft reset\n"));
  MmioOr32 (DWC3_GCTL, DWC3_GCTL_CORESOFTRESET);
  MmioOr32 (DWC3_GUSB3PIPECTL0, DWC3_GUSB3PIPECTL_PHYSOFTRST);
  MmioOr32 (DWC3_GUSB2PHYCFG0, DWC3_GUSB2PHYCFG_PHYSOFTRST);
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (100000);

  MmioAnd32 (DWC3_GUSB3PIPECTL0, ~DWC3_GUSB3PIPECTL_PHYSOFTRST);
  MmioAnd32 (DWC3_GUSB2PHYCFG0, ~DWC3_GUSB2PHYCFG_PHYSOFTRST);
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (100000);

  MmioAnd32 (DWC3_GCTL, ~DWC3_GCTL_CORESOFTRESET);
  ArmDataSynchronizationBarrier ();

  //
  // dwc3_phy_setup(): the Amlogic DWC3 node sets snps,dis_u2/u3_susphy_quirk,
  // so keep both PHY-suspend bits cleared (Linux leaves GUSB2PHYCFG.SUSPHY set
  // at shutdown), then select host mode (PRTCAPDIR re-defaults on core reset).
  //
  MmioAnd32 (DWC3_GUSB2PHYCFG0, ~DWC3_GUSB2PHYCFG_SUSPHY);
  MmioAnd32 (DWC3_GUSB3PIPECTL0, ~DWC3_GUSB3PIPECTL_SUSPHY);

  //
  // dwc3_frame_length_adjustment(): the Amlogic G12 node sets
  // snps,quirk-frame-length-adjustment = 0x20.  Without it the micro-frame
  // (SOF) length keeps its reset default, which mis-schedules periodic and
  // split transactions once a hub's Transaction Translator is in the path
  // (works with a device wired direct, flaky behind a hub).  Program the
  // 6-bit FLADJ_30MHZ field and select the sideband source, as Linux does.
  //
  Rmw32 (
    DWC3_GFLADJ,
    DWC3_GFLADJ_30MHZ_MASK,
    DWC3_GFLADJ_30MHZ_SDBND_SEL | VIM3_DWC3_FLADJ
    );

  //
  // The board node also sets snps,parkmode-disable-ss-quirk.  Park mode keeps
  // the bus arbiter on one endpoint and drops transactions once the tree is
  // busy, so a single device enumerates while a populated hub tree does not.
  // Must be programmed after the core soft reset, which restores the default.
  //
  MmioOr32 (DWC3_GUCTL1, DWC3_GUCTL1_PARKMODE_DISABLE_SS);

  Rmw32 (DWC3_GCTL, DWC3_GCTL_PRTCAPDIR_MASK, DWC3_GCTL_PRTCAPDIR_HOST);
  ArmDataSynchronizationBarrier ();

  Capability = MmioRead32 (MESON_G12B_XHCI_BASE);
  DEBUG ((
    DEBUG_INFO,
    "MesonUSB: XHCI CAPLENGTH=0x%02x HCIVERSION=0x%04x\n",
    Capability & 0xFF,
    Capability >> 16
    ));

  if (((Capability & 0xFF) == 0) ||
      ((Capability & 0xFF) == 0xFF) ||
      ((Capability >> 16) == 0) ||
      ((Capability >> 16) == 0xFFFF))
  {
    DEBUG ((DEBUG_ERROR, "MesonUSB: invalid XHCI capability register\n"));
    return EFI_DEVICE_ERROR;
  }

  //
  // Let the DWC3 core, the USB2/USB3 PHYs and the host-mode port logic fully
  // settle before XhciDxe resets the controller and enumerates.  Without this,
  // the very first device enumeration intermittently fails under RELEASE timing
  // and the whole USB tree comes up empty (verbose DEBUG output masks it by
  // spacing the boot out); the failure is all-or-nothing, so the fix belongs
  // here, once, before the host controller driver starts.
  //
  MicroSecondDelay (MESON_USB_HOST_SETTLE_US);

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MesonUsbDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      BootDevice;

  BootDevice = MmioRead32 (MESON_G12B_AO_SEC_GP_CFG0) & BOOT_DEVICE_MASK;
  if (BootDevice == BOOT_DEVICE_USB) {
    DEBUG ((
      DEBUG_INFO,
      "MesonUSB: priming hardware for MaskROM/USB-origin OS boot\n"
      ));
    Status = MesonUsbInitialize (NULL);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "MesonUSB: recovery hardware initialization failed: %r\n",
        Status
        ));
      return Status;
    }

    DEBUG ((
      DEBUG_INFO,
      "MesonUSB: UEFI XHCI registration deferred on recovery boot\n"
      ));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_INFO, "MesonUSB: registering non-discoverable XHCI\n"));
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeXhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             MesonUsbInitialize,
             NULL,
             1,
             (UINTN)MESON_G12B_XHCI_BASE,
             (UINTN)MESON_G12B_XHCI_SIZE
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MesonUSB: XHCI registration failed: %r\n", Status));
  }

  return Status;
}
