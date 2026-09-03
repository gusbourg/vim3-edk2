/** @file
  Board-level USB3/PCIe lane mux query.

  The G12B SoC routes its single SuperSpeed-capable lane either to the USB3
  port or to an M.2 slot; which one is a BOARD decision (on the VIM3 an
  FUSB340 switch driven by the onboard MCU).  Silicon code (MesonUsbDxe,
  the PCIe host bridge) must agree with the board's choice, so it asks
  through this library class rather than linking board code directly.

  Contract: TRUE only when the board positively selects PCIe.  Any doubt -
  no mux hardware, untrusted controller, failed read - returns FALSE, and
  the boot behaves exactly like a USB3-mux boot, which is the known-good
  default path.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef MESON_PCIE_MUX_LIB_H_
#define MESON_PCIE_MUX_LIB_H_

/**
  Whether the board's shared-lane mux selects PCIe.

  @retval TRUE   The lane is routed to PCIe/M.2; USB3 has no PHY behind it.
  @retval FALSE  The lane is routed to USB3 (or the answer is unknowable).
**/
BOOLEAN
EFIAPI
MesonPcieMuxIsPcie (
  VOID
  );

#endif // MESON_PCIE_MUX_LIB_H_
