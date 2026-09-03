/** @file
  Khadas VIM3 PrePi platform initialization and FDT normalization.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>
#include <Guid/FdtHob.h>
#include <Guid/VariableFlashInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>

#include <MesonG12B.h>

STATIC
EFI_STATUS
SetRegRange (
  IN VOID         *Fdt,
  IN CONST CHAR8  *Path,
  IN UINT64       Base,
  IN UINT64       Size
  )
{
  INT32   Node;
  UINT64  Reg[2];

  Node = FdtPathOffset (Fdt, Path);
  if (Node < 0) {
    DEBUG ((DEBUG_ERROR, "VIM3: missing FDT node %a\n", Path));
    return EFI_NOT_FOUND;
  }

  Reg[0] = SwapBytes64 (Base);
  Reg[1] = SwapBytes64 (Size);
  if (FdtSetProp (Fdt, Node, "reg", Reg, sizeof (Reg)) != 0) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
NormalizeDeviceTree (
  IN VOID  *Fdt
  )
{
  INT32       Chosen;
  EFI_STATUS  Status;

  Status = SetRegRange (
             Fdt,
             "/memory",
             MESON_G12B_DRAM_LOW_BASE,
             MESON_G12B_DRAM_TOP
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SetRegRange (
             Fdt,
             "/reserved-memory/secmon@5000000",
             MESON_G12B_SECURE_BASE,
             0x00300000
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SetRegRange (
             Fdt,
             "/reserved-memory/secmon@5300000",
             MESON_G12B_SECURE_BASE + 0x00300000,
             0x02000000
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen >= 0) {
    FdtDelProp (Fdt, Chosen, "bootargs");
    FdtDelProp (Fdt, Chosen, "linux,initrd-start");
    FdtDelProp (Fdt, Chosen, "linux,initrd-end");
    FdtDelProp (Fdt, Chosen, "kaslr-seed");
    FdtDelProp (Fdt, Chosen, "u-boot,version");
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
PlatformPeim (
  VOID
  )
{
  VOID        *Source;
  VOID        *Destination;
  UINTN       Pages;
  UINTN       Size;
  UINT64      *FdtHobData;
  EFI_STATUS  Status;
  VOID                 *VariableShadow;
  VARIABLE_FLASH_INFO  *VariableFlashInfo;

  Source = (VOID *)(UINTN)PcdGet64 (PcdDeviceTreeInitialBaseAddress);
  if ((Source == NULL) || (FdtCheckHeader (Source) != 0)) {
    DEBUG ((DEBUG_ERROR, "VIM3: invalid embedded FDT at %p\n", Source));
    return EFI_VOLUME_CORRUPTED;
  }

  Size  = FdtTotalSize (Source) + PcdGet32 (PcdDeviceTreeAllocationPadding);
  Pages = EFI_SIZE_TO_PAGES (Size);
  Destination = AllocatePages (Pages);
  if (Destination == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (FdtOpenInto (Source, Destination, EFI_PAGES_TO_SIZE (Pages)) != 0) {
    return EFI_VOLUME_CORRUPTED;
  }

  Status = NormalizeDeviceTree (Destination);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "VIM3: FDT normalization failed: %r\n", Status));
    return Status;
  }

  FdtHobData = BuildGuidHob (&gFdtHobGuid, sizeof (*FdtHobData));
  if (FdtHobData == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *FdtHobData = (UINTN)Destination;

  //
  // Meson SPIFC has no directly addressable flash aperture.  Reserve a
  // contiguous runtime shadow for the NOR range containing the variable FV
  // and both FTW areas, then describe its subranges to the stock EDK2
  // variable stack.  MesonSpifcFvbDxe fills the shadow before publishing FVB.
  //
  VariableShadow = AllocateRuntimePages (
                     EFI_SIZE_TO_PAGES (VIM3_NOR_RUNTIME_SIZE)
                     );
  if (VariableShadow == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  SetMem (VariableShadow, VIM3_NOR_RUNTIME_SIZE, 0xFF);
  VariableFlashInfo = BuildGuidHob (
                        &gVariableFlashInfoHobGuid,
                        sizeof (*VariableFlashInfo)
                        );
  if (VariableFlashInfo == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (VariableFlashInfo, sizeof (*VariableFlashInfo));
  VariableFlashInfo->Version = VARIABLE_FLASH_INFO_HOB_VERSION;
  VariableFlashInfo->NvVariableBaseAddress =
    (EFI_PHYSICAL_ADDRESS)(UINTN)VariableShadow;
  VariableFlashInfo->NvVariableLength = VIM3_NOR_VARIABLE_SIZE;
  VariableFlashInfo->FtwWorkingBaseAddress =
    (EFI_PHYSICAL_ADDRESS)(UINTN)VariableShadow +
    (VIM3_NOR_FTW_WORKING_BASE - VIM3_NOR_RUNTIME_BASE);
  VariableFlashInfo->FtwWorkingLength = VIM3_NOR_FTW_WORKING_SIZE;
  VariableFlashInfo->FtwSpareBaseAddress =
    (EFI_PHYSICAL_ADDRESS)(UINTN)VariableShadow +
    (VIM3_NOR_FTW_SPARE_BASE - VIM3_NOR_RUNTIME_BASE);
  VariableFlashInfo->FtwSpareLength = VIM3_NOR_FTW_SPARE_SIZE;

  BuildFvHob (PcdGet64 (PcdFvBaseAddress), PcdGet32 (PcdFvSize));
  return EFI_SUCCESS;
}
