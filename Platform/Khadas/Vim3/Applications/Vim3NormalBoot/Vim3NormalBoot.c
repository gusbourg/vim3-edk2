/** @file
  Test clearing the VIM3 vendor forced-USB flag before a cold-reason reset.

  Amlogic's vendor BL31 exposes SET_USB_BOOT_FUNC as SMC 0x82000043.
  Subcommand 1 clears the forced-USB flag.  A subsequent PSCI SYSTEM_RESET
  with reboot reason 0 requests the vendor cold-reboot path.

  Hardware testing showed that this does not exit a USB-origin MaskROM
  session: BL31 accepts both calls, but BootROM still reports USB:0 after the
  reset.  A true power-on reset is required to return to eMMC source discovery.

  This is deliberately a standalone, build-only recovery diagnostic.  It is
  not embedded in the production firmware volume and does not modify storage.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#define AML_SMC_SET_USB_BOOT_FUNC  0x82000043U
#define AML_CLEAR_USB_BOOT         1U
#define ARM_SMC_ID_PSCI_SYSTEM_RESET  0x84000009U
#define MESON_COLD_REBOOT             0U

EFI_STATUS
EFIAPI
Vim3NormalBootEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  ARM_SMC_ARGS  Args;

  Print (L"VIM3: clearing the Amlogic forced-USB boot latch\r\n");
  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = AML_SMC_SET_USB_BOOT_FUNC;
  Args.Arg1 = AML_CLEAR_USB_BOOT;
  ArmCallSmc (&Args);
  Print (L"VIM3: CLEAR_USB_BOOT returned 0x%lx\r\n", Args.Arg0);

  //
  // Give the console enough time to drain before BL31 resets the platform.
  //
  gBS->Stall (250000);

  Print (L"VIM3: requesting an Amlogic cold reboot (reason 0)\r\n");
  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = ARM_SMC_ID_PSCI_SYSTEM_RESET;
  Args.Arg1 = MESON_COLD_REBOOT;
  ArmCallSmc (&Args);

  //
  // A successful PSCI reset never returns.
  //
  Print (L"VIM3: PSCI reset unexpectedly returned: 0x%lx\r\n", Args.Arg0);
  return EFI_DEVICE_ERROR;
}
