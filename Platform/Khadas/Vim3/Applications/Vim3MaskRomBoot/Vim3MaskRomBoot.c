/** @file
  Reboot the VIM3 into Amlogic MaskROM (USB recovery).

  Requests the Amlogic "update" reboot reason (3).  Unlike the
  Vim3UsbBootHttp test helper this application arms no BootNext and changes
  no variables: it is safe to expose as a firmware boot-menu entry.

  KNOWN LIMITATION (measured on the bench): on an eMMC-origin boot
  BL31 accepts reason 3 but the BootROM still resumes eMMC discovery, so
  this does not by itself reach MaskROM.  Physical Function x3 remains the
  only proven entry from a normal boot.  Investigate the MCU boot-mode
  register before presenting this entry as a complete replacement.

  Leaving MaskROM afterwards requires loading firmware over USB or a true
  power cycle; a warm reset stays in MaskROM (documented BootROM latch).

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>

#define ARM_SMC_ID_PSCI_SYSTEM_RESET  0x84000009U
#define MESON_UPDATE_REBOOT           3U

EFI_STATUS
EFIAPI
Vim3MaskRomBootEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  ARM_SMC_ARGS  Args;

  Print (L"VIM3: requesting Amlogic USB-recovery reboot (reason 3)\r\n");
  Print (L"VIM3: if the board boots normally instead, use Function x3\r\n");

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = ARM_SMC_ID_PSCI_SYSTEM_RESET;
  Args.Arg1 = MESON_UPDATE_REBOOT;
  ArmCallSmc (&Args);

  Print (L"VIM3: PSCI reset unexpectedly returned: 0x%lx\r\n", Args.Arg0);
  return EFI_DEVICE_ERROR;
}
