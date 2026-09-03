/** @file
  Exercise the vendor Amlogic USB-upgrade request on a Khadas VIM3.

  Hardware testing established an important limitation: FORCE_USB_BOOT plus
  reboot reason 8 changes the BL31 reboot reason, but does not redirect an
  eMMC-origin boot into MaskROM.  It only appears to return to USB when the
  running firmware was itself loaded from MaskROM and the BootROM source is
  already latched as USB.  This application remains useful as a diagnostic,
  but must not be presented as an autonomous eMMC-to-MaskROM transition.

  This is deliberately a standalone, build-only diagnostic.  It is not
  embedded in the production firmware volume and does not modify storage.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#define AML_SMC_SET_USB_BOOT_FUNC      0x82000043U
#define AML_FORCE_USB_BOOT             2U
#define ARM_SMC_ID_PSCI_SYSTEM_RESET   0x84000009U
#define MESON_USB_BURNING_REBOOT       8U

EFI_STATUS
EFIAPI
Vim3UsbBootEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  ARM_SMC_ARGS  Args;

  Print (L"VIM3: testing Amlogic FORCE_USB_BOOT (diagnostic only)\r\n");
  Print (L"VIM3: eMMC-origin boots are known to remain on eMMC\r\n");
  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = AML_SMC_SET_USB_BOOT_FUNC;
  Args.Arg1 = AML_FORCE_USB_BOOT;
  ArmCallSmc (&Args);
  Print (L"VIM3: FORCE_USB_BOOT returned 0x%lx\r\n", Args.Arg0);

  //
  // Give the console enough time to drain before BL31 resets the platform.
  //
  gBS->Stall (250000);

  Print (L"VIM3: requesting an Amlogic USB-burning reboot (reason 8)\r\n");
  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = ARM_SMC_ID_PSCI_SYSTEM_RESET;
  Args.Arg1 = MESON_USB_BURNING_REBOOT;
  ArmCallSmc (&Args);

  //
  // A successful PSCI reset never returns.
  //
  Print (L"VIM3: PSCI reset unexpectedly returned: 0x%lx\r\n", Args.Arg0);
  return EFI_DEVICE_ERROR;
}
