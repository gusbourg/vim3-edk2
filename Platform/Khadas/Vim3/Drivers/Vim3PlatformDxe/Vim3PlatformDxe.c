/** @file
  Khadas VIM3 DXE platform policy.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/PlatformHasDeviceTree.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MesonPcieMuxLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/AcpiTable.h>

#include <Vim3PlatformConfig.h>

STATIC VOID  *mAcpiTableNotifyRegistration;

/**
  Install the PCIe SSDT once EFI_ACPI_TABLE_PROTOCOL appears.

  The SSDT lives in its own FFS file (gVim3SsdtPcieFileGuid) precisely so
  AcpiPlatformDxe does not publish it with the static set; this callback is
  the only installer, and it only ever runs when the OS hardware
  description is ACPI and the USB3/M.2 mux positively selects PCIe.
**/
STATIC
VOID
EFIAPI
Vim3InstallPcieSsdt (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS               Status;
  EFI_ACPI_TABLE_PROTOCOL  *AcpiTable;
  VOID                     *Table;
  UINTN                    TableSize;
  UINTN                    TableKey;

  Status = gBS->LocateProtocol (
                  &gEfiAcpiTableProtocolGuid,
                  mAcpiTableNotifyRegistration,
                  (VOID **)&AcpiTable
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  gBS->CloseEvent (Event);

  Table  = NULL;
  Status = GetSectionFromAnyFv (
             &gVim3SsdtPcieFileGuid,
             EFI_SECTION_RAW,
             0,
             &Table,
             &TableSize
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "VIM3: PCIe SSDT section not found: %r\n", Status));
    return;
  }

  Status = AcpiTable->InstallAcpiTable (AcpiTable, Table, TableSize, &TableKey);
  FreePool (Table);
  DEBUG ((DEBUG_INFO, "VIM3: PCIe SSDT install: %r\n", Status));
}

EFI_STATUS
EFIAPI
Vim3PlatformDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                         Status;
  VIM3_HW_DESCRIPTION_VARSTORE_DATA  HwDescription;
  UINTN                              Size;

  //
  // The setup-menu HwDescription selector chooses which hardware description
  // the OS receives.  ACPI tables are always published; the Device Tree
  // configuration table is published only in Device Tree mode, because an OS
  // that sees a DT prefers it.  Any read failure or invalid value falls back
  // to Device Tree, the fully-supported default.
  //
  Size   = sizeof (HwDescription);
  Status = gRT->GetVariable (
                  VIM3_HW_DESCRIPTION_VARIABLE_NAME,
                  &gVim3PlatformFormSetGuid,
                  NULL,
                  &Size,
                  &HwDescription
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (HwDescription)) ||
      (HwDescription.Mode > VIM3_HW_DESCRIPTION_ACPI))
  {
    HwDescription.Mode = VIM3_HW_DESCRIPTION_DEVICE_TREE;
  }

  if (HwDescription.Mode == VIM3_HW_DESCRIPTION_ACPI) {
    DEBUG ((DEBUG_INFO, "VIM3: ACPI hardware description selected; DT withheld\n"));

    //
    // The PCIe root complex is described by a separate SSDT installed only
    // when the shared-lane mux selects PCIe; with the lane on USB3 the
    // device must not exist anywhere in the namespace (allow-list rule).
    // A protocol notify keeps this driver's dispatch timing unchanged:
    // AcpiTableDxe need not have run yet, and the DT path is untouched.
    //
    if (MesonPcieMuxIsPcie ()) {
      EfiCreateProtocolNotifyEvent (
        &gEfiAcpiTableProtocolGuid,
        TPL_CALLBACK,
        Vim3InstallPcieSsdt,
        NULL,
        &mAcpiTableNotifyRegistration
        );
    } else {
      DEBUG ((DEBUG_INFO, "VIM3: mux is USB3; PCIe SSDT withheld\n"));
    }

    return EFI_SUCCESS;
  }

  //
  // Device Tree mode: FdtClientDxe waits for this marker before publishing
  // the normalized DTB in the UEFI system configuration table.
  //
  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gEdkiiPlatformHasDeviceTreeGuid,
                  EFI_NATIVE_INTERFACE,
                  NULL
                  );
  DEBUG ((DEBUG_INFO, "VIM3: Device Tree platform policy: %r\n", Status));
  return Status;
}
