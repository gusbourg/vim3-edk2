/** @file
  Khadas VIM3 Meson G12B audio subsystem bring-up.

  Puts the audio hardware into the state the ACPI-mode kernel audio stack
  (clk axg-audio island + ASoC consumers, patches/linux/0014-0017) expects
  from firmware, mirroring the Ethernet obligation shape: fixed clock
  sources programmed, bus clock open, block reset pulsed, and the FIFO
  arbiter enabled.  Runs unconditionally in both description modes - the
  DT kernel's clock controller reprograms all of it at probe, so the DT
  path is unaffected.

  Every value below is replicated from the DT-mode register oracle
  (captured on hardware), not derived: "measure, don't recall".

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>

#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#include <MesonG12B.h>

//
// MPLL SDM/N2 words captured while the DT kernel held the assigned rates
// 270.9504 MHz (MPLL0, 44.1 kHz family), 393.216 MHz (MPLL1, S/PDIF
// family - programmed for parity, unused by the playback scope) and
// 294.912 MHz (MPLL2, 48 kHz family).  Bit 31 of CNTL1/3/5 is the
// per-MPLL enable and is deliberately SET here: the MPLLs belong to the
// main g12a clock controller, which has no ACPI port - the audio clock
// island (patch 0014) models them as fixed-rate stand-ins and can never
// enable them at runtime, so they must leave firmware running.  The DT
// kernel reprograms and gates them freely; a permanently-running MPLL
// costs microwatts and matches the playing-state oracle capture exactly.
//
#define AUDIO_MPLL_CNTL0_VALUE  0x00000000U
#define AUDIO_MPLL_CNTL1_VALUE  0xC070186AU  // MPLL0 270.9504 MHz, enabled
#define AUDIO_MPLL_CNTL2_VALUE  0x40000033U
#define AUDIO_MPLL_CNTL3_VALUE  0x40500586U  // MPLL1 393.216 MHz (unused scope, left disabled)
#define AUDIO_MPLL_CNTL4_VALUE  0x40000033U
#define AUDIO_MPLL_CNTL5_VALUE  0xC0603208U  // MPLL2 294.912 MHz, enabled
#define AUDIO_MPLL_CNTL6_VALUE  0x40000033U

#define HHI_GATE_AUDIO_PCLK     BIT0

//
// Arbiter enable (bit 31) plus the FRDDR_A request line (bit 4).  The
// ACPI kernel cannot manage this register itself: the arbiter reset is
// only reachable through the OF reset bindings, so the axg-fifo patch
// (0015) skips it under ACPI and relies on this firmware state.  The CLKA
// _RST method in SsdtAudio.asl rewrites the same value after pulsing
// RESET_AUDIO, keeping the invariant across a kernel-initiated reset.
//
#define AUDIO_ARB_ENABLE_FRDDR_A  0x80000010U

EFI_STATUS
EFIAPI
MesonAudioDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  //
  // Clock the audio bus before touching anything inside the block.
  //
  MmioOr32 (MESON_G12B_HHI_GCLK_MPEG1, HHI_GATE_AUDIO_PCLK);
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (10);

  //
  // Block-level reset, as the DT kernel's clock-controller probe would
  // issue.  The pulse register is self-clearing; the level bank already
  // holds the line deasserted out of the boot chain (DT mode never
  // touches it either).
  //
  MmioWrite32 (MESON_G12B_RESET_PULSE2, MESON_G12B_RESET_AUDIO);
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (20);

  //
  // Master PLL programming words.  These live in HHI, outside the
  // RESET_AUDIO domain; only the arbiter write below must follow the
  // pulse (the block reset clears the arbiter register).
  //
  MmioWrite32 (MESON_G12B_HHI_MPLL_CNTL0, AUDIO_MPLL_CNTL0_VALUE);
  MmioWrite32 (MESON_G12B_HHI_MPLL_CNTL1, AUDIO_MPLL_CNTL1_VALUE);
  MmioWrite32 (MESON_G12B_HHI_MPLL_CNTL2, AUDIO_MPLL_CNTL2_VALUE);
  MmioWrite32 (MESON_G12B_HHI_MPLL_CNTL3, AUDIO_MPLL_CNTL3_VALUE);
  MmioWrite32 (MESON_G12B_HHI_MPLL_CNTL4, AUDIO_MPLL_CNTL4_VALUE);
  MmioWrite32 (MESON_G12B_HHI_MPLL_CNTL5, AUDIO_MPLL_CNTL5_VALUE);
  MmioWrite32 (MESON_G12B_HHI_MPLL_CNTL6, AUDIO_MPLL_CNTL6_VALUE);

  MmioWrite32 (MESON_G12B_AUDIO_ARB_CTRL, AUDIO_ARB_ENABLE_FRDDR_A);
  ArmDataSynchronizationBarrier ();

  DEBUG ((
    DEBUG_INFO,
    "MesonAudio: mpll0=0x%08x mpll1=0x%08x mpll2=0x%08x gclk1=0x%08x arb=0x%08x\n",
    MmioRead32 (MESON_G12B_HHI_MPLL_CNTL1),
    MmioRead32 (MESON_G12B_HHI_MPLL_CNTL3),
    MmioRead32 (MESON_G12B_HHI_MPLL_CNTL5),
    MmioRead32 (MESON_G12B_HHI_GCLK_MPEG1),
    MmioRead32 (MESON_G12B_AUDIO_ARB_CTRL)
    ));

  return EFI_SUCCESS;
}
