/** @file
  MMU memory map for Meson G12B.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>

#include <MesonG12B.h>

#define DESCRIPTOR_COUNT  5

VOID
ArmVirtGetMemoryMap (
  OUT ARM_MEMORY_REGION_DESCRIPTOR  **VirtualMemoryMap
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR  *Map;

  ASSERT (VirtualMemoryMap != NULL);
  Map = AllocateZeroPool (sizeof (*Map) * DESCRIPTOR_COUNT);
  ASSERT (Map != NULL);
  if (Map == NULL) {
    return;
  }

  Map[0].PhysicalBase = MESON_G12B_DRAM_LOW_BASE;
  Map[0].VirtualBase  = MESON_G12B_DRAM_LOW_BASE;
  Map[0].Length       = MESON_G12B_DRAM_LOW_SIZE;
  Map[0].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;

  Map[1].PhysicalBase = MESON_G12B_SECURE_BASE;
  Map[1].VirtualBase  = MESON_G12B_SECURE_BASE;
  Map[1].Length       = MESON_G12B_SECURE_SIZE;
  Map[1].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  Map[2].PhysicalBase = MESON_G12B_DRAM_HIGH_BASE;
  Map[2].VirtualBase  = MESON_G12B_DRAM_HIGH_BASE;
  Map[2].Length       = MESON_G12B_DRAM_HIGH_SIZE;
  Map[2].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;

  Map[3].PhysicalBase = MESON_G12B_DRAM_TOP;
  Map[3].VirtualBase  = MESON_G12B_DRAM_TOP;
  Map[3].Length       = SIZE_4GB - MESON_G12B_DRAM_TOP;
  Map[3].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  *VirtualMemoryMap = Map;
}
