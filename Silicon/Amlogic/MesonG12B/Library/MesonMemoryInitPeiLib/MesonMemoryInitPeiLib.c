/** @file
  PrePi memory HOB and MMU initialization for Meson G12B.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>
#include <Library/ArmMmuLib.h>
#include <Library/ArmVirtMemInfoLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>

#include <MesonG12B.h>

VOID
BuildMemoryTypeInformationHob (
  VOID
  );

EFI_STATUS
EFIAPI
MemoryPeim (
  IN EFI_PHYSICAL_ADDRESS  UefiMemoryBase,
  IN UINT64                UefiMemorySize
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR  *MemoryTable;
  VOID                          *TranslationTableBase;
  UINTN                         TranslationTableSize;
  RETURN_STATUS                 Status;
  EFI_RESOURCE_ATTRIBUTE_TYPE   Attributes;

  Attributes = EFI_RESOURCE_ATTRIBUTE_PRESENT |
               EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
               EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |
               EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |
               EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE |
               EFI_RESOURCE_ATTRIBUTE_TESTED;

  BuildResourceDescriptorHob (
    EFI_RESOURCE_SYSTEM_MEMORY,
    Attributes,
    MESON_G12B_DRAM_LOW_BASE,
    MESON_G12B_DRAM_LOW_SIZE
    );
  BuildResourceDescriptorHob (
    EFI_RESOURCE_MEMORY_RESERVED,
    EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED,
    MESON_G12B_SECURE_BASE,
    MESON_G12B_SECURE_SIZE
    );
  BuildResourceDescriptorHob (
    EFI_RESOURCE_SYSTEM_MEMORY,
    Attributes,
    MESON_G12B_DRAM_HIGH_BASE,
    MESON_G12B_DRAM_HIGH_SIZE
    );

  ArmVirtGetMemoryMap (&MemoryTable);
  Status = ArmConfigureMmu (
             MemoryTable,
             &TranslationTableBase,
             &TranslationTableSize
             );
  ASSERT_RETURN_ERROR (Status);

  if (FeaturePcdGet (PcdPrePiProduceMemoryTypeInformationHob)) {
    BuildMemoryTypeInformationHob ();
  }

  return RETURN_ERROR (Status) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
