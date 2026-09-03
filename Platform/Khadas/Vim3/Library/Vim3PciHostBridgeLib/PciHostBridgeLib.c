/** @file
  PciHostBridgeLib for the Khadas VIM3: one DesignWare root port, reported
  only when the board mux selects PCIe AND the link actually trained.
  Zero bridges otherwise - PciHostBridgeDxe exits EFI_UNSUPPORTED cleanly
  and the boot proceeds from USB/eMMC.

  Aperture truth (see MesonPcie.h): MEM 0xFC700000 + 25 MiB identity-
  mapped, no IO window (NVMe needs none), no prefetchable or above-4G
  windows, 256-byte config space only (the DWC DBI view used for the root
  port is not a 4 KiB ECAM).

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PciHostBridgeLib.h>

#include <IndustryStandard/Pci.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PciHostBridgeResourceAllocation.h>
#include <Protocol/PciRootBridgeIo.h>

#include <MesonPcie.h>

extern BOOLEAN  mMesonPcieLinkUp;

#pragma pack(1)
typedef struct {
  ACPI_HID_DEVICE_PATH        AcpiDevicePath;
  EFI_DEVICE_PATH_PROTOCOL    EndDevicePath;
} EFI_PCI_ROOT_BRIDGE_DEVICE_PATH;
#pragma pack()

STATIC CONST EFI_PCI_ROOT_BRIDGE_DEVICE_PATH  mRootBridgeDevicePath = {
  {
    {
      ACPI_DEVICE_PATH,
      ACPI_DP,
      {
        (UINT8)(sizeof (ACPI_HID_DEVICE_PATH)),
        (UINT8)(sizeof (ACPI_HID_DEVICE_PATH) >> 8)
      }
    },
    EISA_PNP_ID (0x0A08),  // PCI Express root bridge
    0
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      END_DEVICE_PATH_LENGTH,
      0
    }
  }
};

STATIC PCI_ROOT_BRIDGE  mRootBridge = {
  0,                                       // Segment
  0,                                       // Supports (no legacy attributes)
  0,                                       // Attributes
  FALSE,                                   // DmaAbove4G - 32-bit DMA only;
                                           //   pairs with NonCoherentDmaLib
  TRUE,                                    // NoExtendedConfigSpace (256-byte)
  FALSE,                                   // ResourceAssigned
  EFI_PCI_HOST_BRIDGE_COMBINE_MEM_PMEM,    // AllocationAttributes
  { 0,           PCI_MAX_BUS  },           // Bus
  { MAX_UINT64,  0,          0 },          // Io: none (Base > Limit)
  {                                        // Mem: the identity 25 MiB window
    MESON_PCIE_MEM_BASE,
    MESON_PCIE_MEM_BASE + MESON_PCIE_MEM_SIZE - 1,
    0
  },
  { MAX_UINT64,  0,          0 },          // MemAbove4G: none
  { MAX_UINT64,  0,          0 },          // PMem: none
  { MAX_UINT64,  0,          0 },          // PMemAbove4G: none
  (EFI_DEVICE_PATH_PROTOCOL *)&mRootBridgeDevicePath
};

PCI_ROOT_BRIDGE *
EFIAPI
PciHostBridgeGetRootBridges (
  OUT UINTN  *Count
  )
{
  if (!mMesonPcieLinkUp) {
    *Count = 0;
    return NULL;
  }

  *Count = 1;
  return &mRootBridge;
}

VOID
EFIAPI
PciHostBridgeFreeRootBridges (
  IN PCI_ROOT_BRIDGE  *Bridges,
  IN UINTN            Count
  )
{
  //
  // Static allocation - nothing to free.
  //
}

VOID
EFIAPI
PciHostBridgeResourceConflict (
  IN EFI_HANDLE  HostBridgeHandle,
  IN VOID        *Configuration
  )
{
  DEBUG ((DEBUG_ERROR, "PCIe: resource conflict reported by PciHostBridgeDxe\n"));
}
