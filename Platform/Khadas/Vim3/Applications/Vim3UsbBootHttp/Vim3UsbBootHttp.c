/** @file
  Enter the proven VIM3 USB-upgrade path and re-arm the HTTP BootNext.

  This is a build-only hardware-test helper. It is not embedded in the
  production firmware volume and does not alter BootOrder.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/GlobalVariable.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#define ARM_SMC_ID_PSCI_SYSTEM_RESET  0x84000009U
#define MESON_UPDATE_REBOOT           3U
#define PHASE6_HTTP_BOOT_OPTION        0x000AU

EFI_STATUS
EFIAPI
Vim3UsbBootHttpEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  ARM_SMC_ARGS  Args;
  EFI_STATUS    Status;
  UINT16        BootNext;

  BootNext = PHASE6_HTTP_BOOT_OPTION;
  Status   = gRT->SetVariable (
                    L"BootNext",
                    &gEfiGlobalVariableGuid,
                    EFI_VARIABLE_NON_VOLATILE |
                    EFI_VARIABLE_BOOTSERVICE_ACCESS |
                    EFI_VARIABLE_RUNTIME_ACCESS,
                    sizeof (BootNext),
                    &BootNext
                    );
  Print (
    L"VIM3: HTTP BootNext %04x: %r\r\n",
    BootNext,
    Status
    );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Print (L"VIM3: requesting proven Amlogic USB upgrade reboot (reason 3)\r\n");
  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = ARM_SMC_ID_PSCI_SYSTEM_RESET;
  Args.Arg1 = MESON_UPDATE_REBOOT;
  ArmCallSmc (&Args);

  Print (L"VIM3: PSCI reset unexpectedly returned: 0x%lx\r\n", Args.Arg0);
  return EFI_DEVICE_ERROR;
}
