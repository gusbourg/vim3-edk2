/** @file
  PCIe root complex for ACPI mode - conditionally installed SSDT.

  This table is NOT part of the always-published set: Vim3PlatformDxe
  installs it only when the OS hardware description is ACPI AND the
  USB3/M.2 mux positively selects PCIe (MesonPcieMuxLib).  In every other
  configuration the device does not exist anywhere in the namespace,
  keeping the DSDT allow-list promise: describe only hardware the OS can
  actually use.

  The device is a PRP0001 platform device, not a PNP0A08 host bridge: the
  DWC root complex is not ECAM, and the standard ACPI PCI path (MCFG +
  built-in ECAM quirk) cannot be delivered as a loadable module to stock
  distro kernels.  The out-of-tree g12b-pcie-acpi module binds the
  compatible below and registers the PCI host itself, mirroring the
  firmware's own MesonPciSegmentLib config-access rules.

  _CRS index order is the module's ABI (it indexes, like g12b-cpufreq):
    [0] MEM  0xFC000000 4 MiB   DWC DBI + iATU unroll (root port config)
    [1] MEM  0xFC500000 1 MiB   downstream config window (iATU region 1)
    [2] MEM  0xFF648000 8 KiB   Amlogic glue (clock/link status, STATUS12)
    [3] MEM  0xFC700000 25 MiB  bus MEM aperture (identity-mapped BARs)
    [4] IRQ  255 (GIC SPI 223)  all INTx of all devices (hardware funnel)
    [5] IRQ  253 (GIC SPI 221)  DWC built-in MSI demux (future use)

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "AcpiTables.h"

DefinitionBlock ("SsdtPcie.aml", "SSDT", 2, "KHADAS", "VIM3PCIE", 1)
{
  Scope (\_SB)
  {
    Device (PCIE)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 0x20)
      Name (_CCA, Zero)      // DWC master port is not coherent on G12B
      Name (_STA, 0x0F)      // presence is decided by the installer, not AML

      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFC000000, 0x00400000)  // DBI
        Memory32Fixed (ReadWrite, 0xFC500000, 0x00100000)  // cfg window
        Memory32Fixed (ReadWrite, 0xFF648000, 0x00002000)  // glue/status
        Memory32Fixed (ReadWrite, 0xFC700000, 0x01900000)  // MEM aperture
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 255 }
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 253 }
      })

      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,g12b-pcie-acpi" },
          Package () { "interrupt-names", Package () { "intx", "msi" } },
        }
      })
    }
  }
}
