/** @file
  MesonPcieMuxLib for the Khadas VIM3: the shared-lane mux is an FUSB340
  switch driven by the onboard MCU, register 0x33 (0 = USB3, 1 = PCIe).

  The register is read once and cached - the mux is set from the firmware
  setup menu and only takes effect across a reset, so it cannot change
  mid-boot.  Per the MesonPcieMuxLib contract, every failure mode (I2C
  probe failure, untrusted MCU, read error, out-of-range value) answers
  FALSE so the boot falls back to the known-good USB3 path.

  The trust gate matters here even though this is only a read: an MCU that
  failed its identity probe produced that 0x33 value from unknown silicon,
  and steering the SoC's only SuperSpeed lane on its word could cost the
  user both USB3 *and* PCIe.  Vim3McuDxe applies the same policy to the DT
  it hands the OS, so firmware and OS always agree on the mode.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/MesonPcieMuxLib.h>
#include <Library/Vim3McuLib.h>

#include <Vim3Mcu.h>

STATIC BOOLEAN  mResolved;
STATIC BOOLEAN  mIsPcie;

BOOLEAN
EFIAPI
MesonPcieMuxIsPcie (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Mode;

  if (mResolved) {
    return mIsPcie;
  }

  mResolved = TRUE;
  mIsPcie   = FALSE;

  Status = Vim3McuInit ();
  if (EFI_ERROR (Status) || !Vim3McuWritesTrusted ()) {
    DEBUG ((DEBUG_INFO, "Vim3PcieMux: MCU not trusted (%r), assuming USB3\n", Status));
    return FALSE;
  }

  Status = Vim3McuReadByte (MCU_REG_USB_PCIE, &Mode);
  if (EFI_ERROR (Status) || (Mode > MCU_MUX_PCIE)) {
    DEBUG ((DEBUG_WARN, "Vim3PcieMux: mux read failed (%r, 0x%02x), assuming USB3\n", Status, Mode));
    return FALSE;
  }

  mIsPcie = (Mode == MCU_MUX_PCIE);
  DEBUG ((DEBUG_INFO, "Vim3PcieMux: lane mux is %a\n", mIsPcie ? "PCIe" : "USB3"));
  return mIsPcie;
}
