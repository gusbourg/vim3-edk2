/** @file
  Display pipeline for ACPI mode - conditionally installed SSDT.

  This table is NOT part of the always-published set: MesonDisplayDxe
  installs it only when the OS hardware description is ACPI AND its own
  VPU/HDMI power-and-clock bring-up completed (an HDMI sink was present and
  MesonDisplayHardwareInit succeeded).  On a headless boot the driver exits
  before powering the VPU, so these devices must not exist anywhere in the
  namespace - describing an unpowered VPU is the SDA0 AXI-hang hazard.
  Consequence, by design: an ACPI boot started headless cannot hot-plug a
  monitor later (DT mode can, because the kernel powers the VPU itself).

  The kernel side is the patched in-tree meson-drm stack (patches/linux/
  0008..0012): PRP0001 matching, resources by INDEX (ACPI _CRS entries are
  unnamed - the DT reg-names "vpu"/"hhi" become index order here), canvas
  looked up through the _DSD "amlogic,canvas" device reference, clocks and
  resets skipped (firmware left every needed gate open; mode-set clocking
  is raw HHI writes inside meson-drm itself).

  _CRS index order is driver ABI:
    VPU0 [0] MEM 0xFF900000 1 MiB   VPU register block ("vpu")
         [1] MEM 0xFF63C000 4 KiB   HHI window ("hhi" - SHARED region, see
                                    the HHI ownership table;
                                    meson-drm maps it without claiming)
         [2] IRQ 35 edge            VENC vsync (GIC SPI 3)
    HDMI [0] MEM 0xFF600000 64 KiB  DW-HDMI DWC + Amlogic TOP (+0x8000)
         [1] IRQ 89 edge            dw-hdmi TOP/controller (GIC SPI 57)
    CNVS [0] MEM 0xFF638048 0x14    canvas provider (inside DMC sysctrl)

  All three are _CCA Zero: the display path masters through the ordinary
  non-coherent AXI ports (the DT marks none of them dma-coherent - the
  Mali/GPU0 ACE-lite exception does not apply here).

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "AcpiTables.h"

DefinitionBlock ("SsdtDisplay.aml", "SSDT", 2, "KHADAS", "VIM3DISP", 1)
{
  Scope (\_SB)
  {
    // canvas @ 0xff638048 - dimension/stride LUT the VPU scans out through.
    // Declared first: VPU0's _DSD references it, and the kernel canvas
    // provider must exist for meson-drm bind to complete (EPROBE_DEFER
    // handles ordering either way).
    Device (CNVS)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 0x30)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)      // presence is decided by the installer, not AML
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF638048, 0x00000014)
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,canvas" },
        }
      })
    }

    // vpu @ 0xff900000 - OSD/VIU/VENC compositor and encoder (meson-drm).
    Device (VPU0)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 0x31)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF900000, 0x00100000)  // [0] "vpu"
        Memory32Fixed (ReadWrite, 0xFF63C000, 0x00001000)  // [1] "hhi"
        Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 35 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,meson-g12a-vpu" },
          // Device reference replacing the DT amlogic,canvas phandle;
          // resolved by the patched meson_canvas_get via fwnode_find_reference.
          Package () { "amlogic,canvas", \_SB.CNVS },
        }
      })
    }

    // hdmi tx @ 0xff600000 - Synopsys DWC HDMI TX + Amlogic TOP glue.
    Device (HDMI)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 0x32)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF600000, 0x00010000)
        Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 89 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,meson-g12a-dw-hdmi" },
        }
      })
    }

    // cecb_AO @ 0xff800280 (SPI 203 edge) - HDMI CEC, CEC-B unit.
    //
    // Lives HERE (not the DSDT) because "hdmi-phandle" references
    // \_SB.HDMI above, and CEC is meaningless without the display sink
    // this SSDT is gated on.  Patch 0019 pairs with this device:
    // device_get_match_data, a fixed-rate oscin stand-in (the DT clock
    // is a READ-ONLY xtal gate Linux never toggles - "oscin-frequency"
    // states the rate), and the "hdmi-phandle" _DSD device reference
    // resolved to the dw-hdmi platform device.  The 0009 revision makes
    // the HDMI encoder register the cec-notifier against the SAME
    // device under ACPI (pointer identity is the notifier contract),
    // pushing the physical address from EDID on hotplug.
    //
    // The CEC-B clock dual-divider lives at offsets 0x00/0x04 of this
    // window - no extra clock resources needed.  The pad (GPIOH_3,
    // function 5) is muxed by MesonDisplayHw.c - without that the bus
    // is electrically dead.  _CRS abuts AO-secure (ends 0xFF80027F)
    // exactly; no overlap with any described AO window.
    Device (CECB)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 21)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF800280, 0x0000001C)
        Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 235 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,meson-g12a-ao-cec" },
          Package () { "hdmi-phandle", \_SB.HDMI },
          Package () { "oscin-frequency", 24000000 },
        }
      })
    }
  }
}
