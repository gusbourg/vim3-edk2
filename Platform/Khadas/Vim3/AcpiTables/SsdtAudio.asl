/** @file
  HDMI audio playback pipeline for ACPI mode - conditionally installed SSDT.

  Installed by MesonDisplayDxe under the same gate as SsdtDisplay: ACPI
  mode AND the VPU/HDMI bring-up succeeded.  The playback path terminates
  in TOHDMITX feeding \_SB.HDMI, so audio devices without a powered
  display pipeline would violate the allow-list promise.

  Scope is HDMI playback only: FRDDR_A (DMA) -> TDMOUT_A (serializer,
  clocked by TDM interface A) -> TOHDMITX (mux) -> dw-hdmi I2S input.
  TODDR/TDMIN/S/PDIF/PDM/toacodec are deliberately not described, and the
  audio arbiter has no device: the kernel cannot reach its reset under
  ACPI (patch 0015 skips it) - MesonAudioDxe pre-enables the FRDDR_A
  request line and the CLKA _RST below restores it after a block reset.

  First use of Method(_RST) in this firmware: the Linux reset core
  evaluates AML _RST when a device has an ACPI companion, which satisfies
  the two device_reset() call sites (axg-audio clkc probe, tohdmitx
  probe) with zero kernel delta.

  _CRS index order is driver ABI (single MEM each, FRDA adds its IRQ):
    CLKA [0] MEM 0xFF642000 0xB4    audio clock controller window
    FRDA [0] MEM 0xFF6421C0 0x2C    FRDDR_A FIFO
         [1] IRQ 184 edge           FRDDR_A (GIC SPI 152)
    TDMA [0] MEM 0xFF642500 0x40    TDMOUT_A
    TIFA (none - virtual clock-only DAI)
    THTX [0] MEM 0xFF642744 0x4     TOHDMITX mux
    CARD (none - machine driver)

  All _CCA Zero: the audio FIFOs master through the ordinary non-coherent
  AXI ports (the DT marks none of them dma-coherent).

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "AcpiTables.h"

DefinitionBlock ("SsdtAudio.aml", "SSDT", 2, "KHADAS", "VIM3AUDI", 1)
{
  Scope (\_SB)
  {
    // audio clock controller @ 0xff642000 (clk axg-audio ACPI island,
    // patch 0014).  Declared first: every other audio device consumes
    // its clocks via clkdev lookups the island registers at probe.
    Device (CLKA)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 0x40)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)      // presence is decided by the installer, not AML
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF642000, 0x000000B4)
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,g12a-audio-clkc" },
          // Master-clock input rates, stated as firmware facts: MPLL0 and
          // MPLL2 are programmed and enabled by MesonAudioDxe (the main
          // clock controller has no ACPI description).  The clkc island
          // (patch 0014) models each stated input as a fixed-rate clock.
          Package () { "amlogic,mst-in0-rate", 270950400 },
          Package () { "amlogic,mst-in2-rate", 294912000 },
        }
      })

      //
      // EE reset controller pulse bank 2 (self-clearing); RESET_AUDIO is
      // line 65 = bit 1.  The arbiter register is cleared by that block
      // reset, so the firmware-provided FRDDR_A grant (see MesonAudioDxe)
      // is rewritten afterwards - device_reset() in the clkc probe must
      // not be able to strand the FIFO without bus arbitration.
      //
      OperationRegion (RSTP, SystemMemory, 0xFFD0100C, 0x4)
      Field (RSTP, DWordAcc, NoLock, Preserve)
      {
        RPU2, 32
      }

      OperationRegion (AARB, SystemMemory, 0xFF642280, 0x4)
      Field (AARB, DWordAcc, NoLock, Preserve)
      {
        ARBC, 32
      }

      Method (_RST, 0, Serialized)
      {
        RPU2 = 0x00000002
        Stall (20)
        ARBC = 0x80000010
      }
    }

    // frddr_a @ 0xff6421c0 - memory-to-tofifo DMA (ASoC axg-fifo,
    // patch 0015).
    Device (FRDA)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 0x41)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF6421C0, 0x0000002C)
        Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 184 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,g12a-frddr" },
          Package () { "amlogic,fifo-depth", 512 },
        }
      })
    }

    // tdmout_a @ 0xff642500 - TDM serializer formatter (ASoC
    // axg-tdm-formatter, patch 0016).
    Device (TDMA)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 0x42)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF642500, 0x00000040)
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,g12a-tdmout" },
        }
      })
    }

    // TDM interface A - virtual clock-only DAI in front of TDMOUT_A
    // (ASoC axg-tdm-interface, patch 0017).  No registers of its own.
    Device (TIFA)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 0x43)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,axg-tdm-iface" },
        }
      })
    }

    // tohdmitx @ 0xff642744 - audio-to-HDMI-TX mux (in-tree driver needs
    // no patch: match data unused, no clocks, device_reset satisfied by
    // the _RST below).
    Device (THTX)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 0x44)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF642744, 0x00000004)
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,g12a-tohdmitx" },
        }
      })

      //
      // Auxiliary reset inside the audio clock controller:
      // AUDIO_SW_RESET (0xFF642024), TOHDMITX is line 24.  Level-type:
      // set asserts, clear releases.
      //
      OperationRegion (ASWR, SystemMemory, 0xFF642024, 0x4)
      Field (ASWR, DWordAcc, NoLock, Preserve)
      {
        SWRS, 32
      }

      Method (_RST, 0, Serialized)
      {
        SWRS = (SWRS | 0x01000000)
        Stall (20)
        SWRS = (SWRS & 0xFEFFFFFF)
      }
    }

    // Machine driver anchor for the out-of-tree ACPI card
    // (patches/linux/vim3-hdmi-snd/).  No resources: the driver resolves
    // every component by compatible scan at probe.
    Device (CARD)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 0x45)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "khadas,vim3-acpi-hdmi-sound" },
        }
      })
    }
  }
}
