/** @file
  Khadas VIM3 Meson G12B DesignWare Ethernet platform glue.

  This driver owns only the Meson clock, reset, pinmux, and PRG_ETHERNET
  setup.  It publishes the standard non-discoverable-device protocol for the
  BSD-licensed DesignWare SNP driver.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>

#include <Guid/NonDiscoverableDevice.h>

#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/Vim3MesonSmLib.h>

#include <Protocol/NonDiscoverableDevice.h>

#include <MesonG12B.h>

#define MESON_G12B_ETH_BASE       0xFF3F0000ULL
#define MESON_G12B_ETH_SIZE       0x00010000ULL
#define MESON_G12B_PRG_ETH0       0xFF634540ULL

#define HHI_GATE_ETH_PHY          BIT4
#define HHI_GATE_ETH_CORE         BIT3
#define HHI_MEM_PD_ETH            (BIT2 | BIT3)
#define RESET_ETHERNET            BIT11

#define PERIPHS_MUX_Z0_7          (MESON_G12B_PERIPHS_MUX_BASE + 0x18)
#define PERIPHS_MUX_Z8_15         (MESON_G12B_PERIPHS_MUX_BASE + 0x1C)
#define PERIPHS_DS_Z              (MESON_G12B_PERIPHS_DS_BASE + 0x14)

#define PRG_ETH0_RGMII_MODE       BIT0
#define PRG_ETH0_TX_DELAY_2NS     BIT5
#define PRG_ETH0_M250_DIV4        (4U << 7)
#define PRG_ETH0_RGMII_TX_CLK_EN  BIT10
#define PRG_ETH0_TX_PHY_REF_EN    BIT12

//
// Captured from the board's published DT and confirmed in Linux.
//
#define VIM3_MAC_ADDRESS          0xC8631470B575ULL

STATIC
VOID
ConfigureEthernetHardware (
  VOID
  )
{
  UINT32  PrgEth0;

  //
  // GPIOZ[0:13] are the external RGMII/MDIO signals, all function 1.
  //
  MmioWrite32 (PERIPHS_MUX_Z0_7, 0x11111111U);
  MmioAndThenOr32 (PERIPHS_MUX_Z8_15, 0xFF000000U, 0x00111111U);
  MmioAndThenOr32 (PERIPHS_DS_Z, 0xF0000000U, 0x0FFFFFFFU);

  //
  // The G12A Ethernet power domain is memory-only: zero in HHI_MEM_PD_REG0
  // powers up the MAC SRAM.  BL2 often leaves it on, but firmware must not
  // depend on that source-specific state.
  //
  MmioAnd32 (MESON_G12B_HHI_MEM_PD_REG0, ~HHI_MEM_PD_ETH);
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (20);

  MmioOr32 (MESON_G12B_HHI_GCLK_MPEG0, HHI_GATE_ETH_PHY);
  MmioOr32 (MESON_G12B_HHI_GCLK_MPEG1, HHI_GATE_ETH_CORE);
  MmioOr32 (MESON_G12B_RESET_LEVEL1, RESET_ETHERNET);
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (1000);

  //
  // External RGMII, fclk_div2 / 4 / 2 = 125 MHz, 2 ns TX delay from the
  // board DT, and the line-rate-aware RGMII transmit clock enabled.
  //
  PrgEth0 = PRG_ETH0_RGMII_MODE |
            PRG_ETH0_TX_DELAY_2NS |
            PRG_ETH0_M250_DIV4 |
            PRG_ETH0_RGMII_TX_CLK_EN |
            PRG_ETH0_TX_PHY_REF_EN;
  MmioWrite32 (MESON_G12B_PRG_ETH0, PrgEth0);
  MmioWrite32 (MESON_G12B_PRG_ETH0 + 4, 0);
  ArmDataSynchronizationBarrier ();

  DEBUG ((
    DEBUG_INFO,
    "MesonEth: RGMII configured prg0=0x%08x gate0=0x%08x gate1=0x%08x reset=0x%08x\n",
    MmioRead32 (MESON_G12B_PRG_ETH0),
    MmioRead32 (MESON_G12B_HHI_GCLK_MPEG0),
    MmioRead32 (MESON_G12B_HHI_GCLK_MPEG1),
    MmioRead32 (MESON_G12B_RESET_LEVEL1)
    ));
}

STATIC
EFI_STATUS
RegisterEthernetDevice (
  VOID
  )
{
  NON_DISCOVERABLE_DEVICE            *Device;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Desc;
  EFI_ACPI_END_TAG_DESCRIPTOR        *End;
  EFI_HANDLE                         Handle;
  EFI_STATUS                         Status;
  UINTN                              AllocationSize;
  UINT64                             MacValue;
  UINT8                              Mac[6];

  AllocationSize = sizeof (*Device) +
                   2 * sizeof (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR) +
                   sizeof (EFI_ACPI_END_TAG_DESCRIPTOR);
  Device = AllocateZeroPool (AllocationSize);
  if (Device == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Device->Type       = &gDwEmacNetNonDiscoverableDeviceGuid;
  Device->DmaType    = NonDiscoverableDeviceDmaTypeNonCoherent;
  Device->Initialize = NULL;
  Device->Resources  = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)(Device + 1);

  Desc                       = &Device->Resources[0];
  Desc->Desc                 = ACPI_ADDRESS_SPACE_DESCRIPTOR;
  Desc->Len                  = sizeof (*Desc) - 3;
  Desc->ResType              = ACPI_ADDRESS_SPACE_TYPE_MEM;
  Desc->AddrSpaceGranularity = 32;
  Desc->AddrRangeMin         = MESON_G12B_ETH_BASE;
  Desc->AddrRangeMax         = MESON_G12B_ETH_BASE + MESON_G12B_ETH_SIZE - 1;
  Desc->AddrLen              = MESON_G12B_ETH_SIZE;

  //
  // Prefer the board's real MAC from the SoC eFUSE (via the secure monitor);
  // the MCU does not carry it and the hardcoded constant is only correct on
  // this one unit.  Fall back to the built-in default if the eFUSE MAC is
  // blank or invalid.
  //
  MacValue = VIM3_MAC_ADDRESS;
  if (!EFI_ERROR (MesonSmGetMac (Mac))) {
    MacValue = ((UINT64)Mac[0] << 40) | ((UINT64)Mac[1] << 32) |
               ((UINT64)Mac[2] << 24) | ((UINT64)Mac[3] << 16) |
               ((UINT64)Mac[4] << 8)  | (UINT64)Mac[5];
    DEBUG ((
      DEBUG_INFO,
      "MesonEth: eFUSE MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
      Mac[0], Mac[1], Mac[2], Mac[3], Mac[4], Mac[5]
      ));
  } else {
    DEBUG ((DEBUG_WARN, "MesonEth: eFUSE MAC unavailable; using built-in default\n"));
  }

  //
  // The existing DesignWare SNP binding encodes the six-byte station address
  // in its second resource descriptor.
  //
  Desc                       = &Device->Resources[1];
  Desc->Desc                 = ACPI_ADDRESS_SPACE_DESCRIPTOR;
  Desc->Len                  = sizeof (*Desc) - 3;
  Desc->ResType              = ACPI_ADDRESS_SPACE_TYPE_MEM;
  Desc->AddrSpaceGranularity = 64;
  Desc->AddrRangeMin         = MacValue;
  Desc->AddrRangeMax         = MacValue;
  Desc->AddrLen              = 1;

  End           = (EFI_ACPI_END_TAG_DESCRIPTOR *)&Device->Resources[2];
  End->Desc     = ACPI_END_TAG_DESCRIPTOR;
  End->Checksum = 0;

  Handle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  Device
                  );
  if (EFI_ERROR (Status)) {
    FreePool (Device);
  }

  return Status;
}

EFI_STATUS
EFIAPI
MesonEthDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  ConfigureEthernetHardware ();
  return RegisterEthernetDevice ();
}
