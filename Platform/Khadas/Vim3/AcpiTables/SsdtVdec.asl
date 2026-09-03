/** @file
  Video decoder (DOS) for ACPI mode - conditionally installed SSDT.

  Installed by MesonDisplayDxe under the same gate as SsdtDisplay and
  SsdtAudio: ACPI mode AND the VPU/HDMI bring-up succeeded.  The decoder
  depends on the canvas provider (\_SB.CNVS, declared in SsdtDisplay),
  so it shares the display gate; decode on a headless boot is a
  non-goal on this platform.

  The kernel side is the patched staging meson-vdec (patch 0021): the
  driver self-manages the VDEC power islands through the AO sysctrl
  window and the DOS-internal MEM_PD registers (there is no VDEC power
  domain on G12 - verified on silicon), loads codec firmware via plain
  request_firmware + IMEM DMA, and models its five clocks as a local
  island over the HHI window with only the fixed fclk_div parents -
  so, unlike DT, a rate request can never escape to a PLL (the DT CCF
  was observed on this bench parking vdec_1 on HIFI_PLL).

  _CRS index order is driver ABI (ACPI resources are unnamed; the
  DT reg-names become index positions here):
    VDEC [0] MEM 0xFF620000 64 KiB  DOS register block ("dos")
         [1] MEM 0xFFD0E180 0xE4    esparser ("esparser")
         [2] MEM 0xFFD01008 4       EE reset pulse bank 1
                                    (RESET_PARSER = line 40 -> bit 8;
                                    self-clearing, same controller as
                                    the audio bank-2 precedent)
         [3] MEM 0xFF800000 0x100   AO sysctrl window (private regmap
                                    replacing the DT syscon phandle;
                                    the driver RMWs SLEEP0 0xE8 /
                                    ISO0 0xEC)
         [4] MEM 0xFF63C000 4 KiB   HHI window (clk island: VDEC
                                    0x1E0/0x1E4 mux/div/gate + the
                                    dos gates in GCLK_MPEG0/1; mapped
                                    without claiming, meson-drm
                                    precedent)
    IRQ 76 edge  vdec     (GIC SPI 44)
    IRQ 64 edge  esparser (GIC SPI 32)

  _CCA Zero: the DOS masters through the ordinary non-coherent AXI
  ports (the DT marks nothing here dma-coherent).

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "AcpiTables.h"

DefinitionBlock ("SsdtVdec.aml", "SSDT", 2, "KHADAS", "VIM3VDEC", 1)
{
  // The canvas provider lives in SsdtDisplay, which is always installed
  // before this table (same installer, same gate, earlier call).
  External (\_SB.CNVS, DeviceObj)

  Scope (\_SB)
  {
    Device (VDEC)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 0x50)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)      // presence is decided by the installer, not AML
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF620000, 0x00010000)  // [0] dos
        Memory32Fixed (ReadWrite, 0xFFD0E180, 0x000000E4)  // [1] esparser
        Memory32Fixed (ReadWrite, 0xFFD01008, 0x00000004)  // [2] parser reset pulse
        Memory32Fixed (ReadWrite, 0xFF800000, 0x00000100)  // [3] AO sysctrl
        Memory32Fixed (ReadWrite, 0xFF63C000, 0x00001000)  // [4] HHI
        Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 76 }
        Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 64 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,g12a-vdec" },
          // Device reference replacing the DT amlogic,canvas phandle;
          // resolved by the patched meson_canvas_get via
          // fwnode_find_reference (SsdtDisplay CNVS pattern).
          Package () { "amlogic,canvas", \_SB.CNVS },
        }
      })
    }
  }
}
