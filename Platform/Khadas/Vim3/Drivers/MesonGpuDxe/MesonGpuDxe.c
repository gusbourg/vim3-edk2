/** @file
  Khadas VIM3 Meson G12B Mali G52 (Bifrost) power-on.

  Firmware-side half of the ACPI GPU contract: enable the Mali clock and
  release the DVALIN resets so the DSDT GPU0 device (PRP0001 ->
  "amlogic,meson-g12a-mali") is backed by live hardware.  Under ACPI there is
  no clock provider and no devfreq, so the rate chosen here is the rate the
  GPU runs at forever; the DSDT states it via the "clock-frequency" property,
  which MUST be kept in step with this driver (SDC0-clkin convention).

  Runs unconditionally in DT mode too: the DT kernel's clock framework simply
  takes ownership of HHI_MALI_CLK_CNTL afterward (glitch-free mux), and reset
  deassertion is idempotent with what the DT kernel does itself.  Keeping the
  GPU powered in both modes also protects the acpi=on-while-DT-mode corner
  from the ungated-MMIO AXI hang class documented in Dsdt.asl.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>

#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#include <MesonG12B.h>

#define MALI_GPU_BASE            0xFFE40000ULL
#define MALI_GPU_ID              (MALI_GPU_BASE + 0x0000)

//
// HHI_MALI_CLK_CNTL fields (see MesonG12B.h for the layout source).
//
#define MALI_CLK_TOP_MUX         BIT31
#define MALI_CLK0_GATE           BIT8
#define MALI_CLK0_SEL_MASK       (7U << 9)
#define MALI_CLK0_SEL_FDIV2P5    (3U << 9)
#define MALI_CLK0_DIV_MASK       0x7FU

#define FIX_PLL_FDIV2P5_GATE     BIT25

//
// DVALIN reset lines, kernel dt-bindings ids 20 and 78.  The level registers
// hold 32 ids each and are active-low: a SET bit means "out of reset".
//
#define RESET_LVL0_DVALIN_CAPB3  BIT20     // id 20
#define RESET_LVL2_DVALIN        BIT14     // id 78 - 64

//
// Bring-up watchdog, same recipe as MesonDisplayDxe: if the first GPU
// register access stalls the AXI bus, the board reboots instead of wedging.
//
#define WDT_CTRL   0x00
#define WDT_TCNT   0x08
#define WDT_RESET  0x0C

STATIC
VOID
WatchdogStart (
  VOID
  )
{
  UINT32  Control;

  MmioAnd32 (MESON_G12B_WATCHDOG_BASE + WDT_CTRL, ~BIT18);
  MmioWrite32 (MESON_G12B_WATCHDOG_BASE + WDT_RESET, 0);
  Control = 24000U | BIT25 | BIT24 | BIT21;
  MmioWrite32 (MESON_G12B_WATCHDOG_BASE + WDT_CTRL, Control);
  MmioWrite32 (MESON_G12B_WATCHDOG_BASE + WDT_TCNT, 5000);
  MmioWrite32 (MESON_G12B_WATCHDOG_BASE + WDT_RESET, 0);
  MmioWrite32 (MESON_G12B_WATCHDOG_BASE + WDT_CTRL, Control | BIT18);
  ArmDataSynchronizationBarrier ();
}

STATIC
VOID
WatchdogStop (
  VOID
  )
{
  MmioAnd32 (MESON_G12B_WATCHDOG_BASE + WDT_CTRL, ~BIT18);
  MmioWrite32 (MESON_G12B_WATCHDOG_BASE + WDT_RESET, 0);
  ArmDataSynchronizationBarrier ();
}

STATIC
VOID
ConfigureMaliHardware (
  VOID
  )
{
  UINT32  GpuId;

  //
  // The 800 MHz source (fclk_div2p5) must be running before the mux selects
  // it.  fixed_pll itself is always on (it feeds the fabric).
  //
  MmioOr32 (MESON_G12B_HHI_FIX_PLL_CNTL1, FIX_PLL_FDIV2P5_GATE);

  //
  // Park the glitch-free top mux on mali_0, then program mali_0 =
  // fclk_div2p5 / 1 = 800 MHz with the gate still closed.  800 MHz is the DT
  // kernel's own top OPP at the board's fixed 0.8 V GPU rail (no amlogic DT
  // describes a mali-supply).  Thermal fallback if soak testing demands it:
  // select 4 (fclk_div3) = 666 MHz, and change the DSDT "clock-frequency"
  // in the same commit.
  //
  MmioAnd32 (MESON_G12B_HHI_MALI_CLK_CNTL, ~MALI_CLK_TOP_MUX);
  MmioAndThenOr32 (
    MESON_G12B_HHI_MALI_CLK_CNTL,
    ~(MALI_CLK0_SEL_MASK | MALI_CLK0_DIV_MASK | MALI_CLK0_GATE),
    MALI_CLK0_SEL_FDIV2P5
    );
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (10);

  MmioOr32 (MESON_G12B_HHI_MALI_CLK_CNTL, MALI_CLK0_GATE);
  ArmDataSynchronizationBarrier ();

  //
  // Release DVALIN out of reset (set = deasserted in the level registers).
  //
  MmioOr32 (MESON_G12B_RESET_LEVEL0, RESET_LVL0_DVALIN_CAPB3);
  MmioOr32 (MESON_G12B_RESET_LEVEL2, RESET_LVL2_DVALIN);
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (100);

  //
  // Smoke test: the first read of GPU_ID proves the clock and resets took.
  // Expect 0x7212xxxx (Mali G52 r1).  Run it under the bring-up watchdog so
  // a wrong sequence reboots into the unmodified boot path instead of
  // hanging the AXI bus with SysRq dead.
  //
  WatchdogStart ();
  GpuId = MmioRead32 (MALI_GPU_ID);
  WatchdogStop ();

  DEBUG ((
    DEBUG_INFO,
    "MesonGpu: MALI_CLK=0x%08x LVL0=0x%08x LVL2=0x%08x GPU_ID=0x%08x\n",
    MmioRead32 (MESON_G12B_HHI_MALI_CLK_CNTL),
    MmioRead32 (MESON_G12B_RESET_LEVEL0),
    MmioRead32 (MESON_G12B_RESET_LEVEL2),
    GpuId
    ));
}

EFI_STATUS
EFIAPI
MesonGpuDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  ConfigureMaliHardware ();
  return EFI_SUCCESS;
}
