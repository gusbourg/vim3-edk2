/** @file
  Watchdog-guarded inactive-SD controller diagnostic for Khadas VIM3.

  Each numbered stage is an independent candidate.  The tool is deliberately
  not included in the production firmware volume.  If a controller access
  hard-stalls the CPU, the Meson hardware watchdog resets the board and normal
  BootOrder resumes; the last serial line identifies the stalling operation.

  Linux sources are used only as a behavioral/register oracle.  The watchdog
  programming model is covered by the dual BSD-3-Clause option on
  drivers/watchdog/meson_gxbb_wdt.c.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/LoadedImage.h>

#include <MesonG12B.h>

#define BIT_N(n)                 (1U << (n))
#define GENMASK32(h, l)          \
  ((MAX_UINT32 << (l)) & (MAX_UINT32 >> (31U - (h))))

#define SD_EMMC_CLOCK            0x00
#define SD_EMMC_CFG              0x44
#define SD_EMMC_STATUS           0x48
#define SD_EMMC_IRQ_EN           0x4C

//
// SD_EMMC_CLOCK field facts (hardware facts; bit positions cross-checked
// against GPL oracles and bench measurement):
//   div      [5:0]   one-based divider, 0 = stopped at reset
//   src      [7:6]   0 = clkin0 (HHI mux), 1 = clkin1 (fclk_div2)
//   co_phase [9:8]   core clock phase, 2 = 180 degrees
//   always-on        BIT24 (v2 layout) / BIT28 (v3 layout, G12B)
//
// Stage 12 reproduces U-Boot's exact first MMC register write for a
// 400 kHz target: phase 180 | src 24 MHz | div 60.  Stage 13 reproduces
// Linux meson_mmc_clk_init: v3 always-on | max divider | phase 180.
// Stage 14 selects clkin1 (fclk_div2, always running) so the internal
// clock does not depend on any HHI mux state.
//
#define SD_CLK_UBOOT_400K        0x0000023CU
#define SD_CLK_LINUX_INIT        0x1000023FU
#define SD_CLK_CLKIN1_INIT       0x1000027FU

#define WDT_CTRL                 (MESON_G12B_WATCHDOG_BASE + 0x0)
#define WDT_TCNT                 (MESON_G12B_WATCHDOG_BASE + 0x8)
#define WDT_RESET                (MESON_G12B_WATCHDOG_BASE + 0xC)
#define WDT_CTRL_CLKDIV_EN       BIT25
#define WDT_CTRL_CLK_EN          BIT24
#define WDT_CTRL_SYSTEM_RESET    BIT21
#define WDT_CTRL_ENABLE          BIT18
#define WDT_CTRL_DIV_MASK        GENMASK32 (17, 0)

#define HHI_GATE_SD_EMMC_B       BIT25
#define HHI_CLK0_SD_EMMC_B       BIT23
#define RESET_SD_EMMC_B          BIT13

#define PERIPHS_MUX_C0_7         (MESON_G12B_PERIPHS_MUX_BASE + (0x9 * 4))
#define PERIPHS_GPIO_C_DIR       (MESON_G12B_PERIPHS_GPIO_BASE + (3 * 4))
#define PERIPHS_GPIO_C_IN        (MESON_G12B_PERIPHS_GPIO_BASE + (5 * 4))
#define PERIPHS_PULL_C           (MESON_G12B_PERIPHS_PULL_BASE + (1 * 4))
#define PERIPHS_PULLEN_C         (MESON_G12B_PERIPHS_PULLEN_BASE + (1 * 4))
#define GPIOC_6_BIT              BIT6

#define PROBE_WATCHDOG_MS        5000U
#define XTAL_HZ                  24000000U

STATIC
VOID
Rmw32 (
  IN UINTN   Address,
  IN UINT32  ClearMask,
  IN UINT32  SetMask
  )
{
  MmioWrite32 (Address, (MmioRead32 (Address) & ~ClearMask) | SetMask);
}

STATIC
VOID
WatchdogStop (
  VOID
  )
{
  MmioAnd32 (WDT_CTRL, ~WDT_CTRL_ENABLE);
  MmioWrite32 (WDT_RESET, 0);
  ArmDataSynchronizationBarrier ();
}

STATIC
VOID
WatchdogStart (
  IN UINT32  TimeoutMs
  )
{
  UINT32  Control;

  WatchdogStop ();
  Control = ((XTAL_HZ / 1000U) & WDT_CTRL_DIV_MASK) |
            WDT_CTRL_SYSTEM_RESET |
            WDT_CTRL_CLK_EN |
            WDT_CTRL_CLKDIV_EN;
  MmioWrite32 (WDT_CTRL, Control);
  MmioWrite32 (WDT_TCNT, MIN (TimeoutMs, 0xFFFFU));
  MmioWrite32 (WDT_RESET, 0);
  MmioWrite32 (WDT_CTRL, Control | WDT_CTRL_ENABLE);
  ArmDataSynchronizationBarrier ();
}

STATIC
VOID
ConfigureSdPins (
  VOID
  )
{
  Rmw32 (PERIPHS_MUX_C0_7, 0x0FFFFFFFU, 0x00111111U);
  Rmw32 (
    PERIPHS_PULLEN_C,
    GENMASK32 (6, 0),
    BIT0 | BIT1 | BIT2 | BIT3 | BIT5 | BIT6
    );
  Rmw32 (
    PERIPHS_PULL_C,
    GENMASK32 (6, 0),
    BIT0 | BIT1 | BIT2 | BIT3 | BIT5 | BIT6
    );
  MmioOr32 (PERIPHS_GPIO_C_DIR, GPIOC_6_BIT);
  ArmDataSynchronizationBarrier ();
}

STATIC
VOID
DisableSdDomain (
  VOID
  )
{
  MmioAnd32 (MESON_G12B_HHI_SD_EMMC_CLK_CNTL, ~HHI_CLK0_SD_EMMC_B);
  MmioAnd32 (MESON_G12B_HHI_GCLK_MPEG0, ~HHI_GATE_SD_EMMC_B);
  MmioAnd32 (MESON_G12B_RESET_LEVEL1, ~RESET_SD_EMMC_B);
  ArmDataSynchronizationBarrier ();
}

STATIC
VOID
EnableSdDomain (
  IN BOOLEAN  GateBeforeReset
  )
{
  if (GateBeforeReset) {
    MmioOr32 (MESON_G12B_HHI_GCLK_MPEG0, HHI_GATE_SD_EMMC_B);
    MmioOr32 (MESON_G12B_RESET_LEVEL1, RESET_SD_EMMC_B);
  } else {
    MmioOr32 (MESON_G12B_RESET_LEVEL1, RESET_SD_EMMC_B);
    MmioOr32 (MESON_G12B_HHI_GCLK_MPEG0, HHI_GATE_SD_EMMC_B);
  }

  MmioWrite32 (
    MESON_G12B_HHI_SD_EMMC_CLK_CNTL,
    HHI_CLK0_SD_EMMC_B
    );
  ArmDataSynchronizationBarrier ();
}

STATIC
VOID
PrintSafeSnapshot (
  VOID
  )
{
  Print (
    L"gate=%08x clk_b=%08x reset1=%08x mux_c=%08x pullen_c=%08x "
    L"pull_c=%08x gpio_dir=%08x gpio_in=%08x\n",
    MmioRead32 (MESON_G12B_HHI_GCLK_MPEG0),
    MmioRead32 (MESON_G12B_HHI_SD_EMMC_CLK_CNTL),
    MmioRead32 (MESON_G12B_RESET_LEVEL1),
    MmioRead32 (PERIPHS_MUX_C0_7),
    MmioRead32 (PERIPHS_PULLEN_C),
    MmioRead32 (PERIPHS_PULL_C),
    MmioRead32 (PERIPHS_GPIO_C_DIR),
    MmioRead32 (PERIPHS_GPIO_C_IN)
    );
}

STATIC
EFI_STATUS
ParseStage (
  IN  EFI_HANDLE  ImageHandle,
  OUT UINTN       *Stage
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  CHAR16                     *Options;
  UINTN                      Count;
  UINTN                      Index;
  UINTN                      Token;
  UINTN                      Value;
  EFI_STATUS                 Status;

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status) || (LoadedImage->LoadOptions == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Options = LoadedImage->LoadOptions;
  Count   = LoadedImage->LoadOptionsSize / sizeof (CHAR16);
  Index   = 0;
  while (Index < Count) {
    while ((Index < Count) &&
           ((Options[Index] == L' ') || (Options[Index] == L'\t')))
    {
      Index++;
    }

    if ((Index >= Count) || (Options[Index] == L'\0')) {
      break;
    }

    Token = Index;
    Value = 0;
    while ((Index < Count) && (Options[Index] >= L'0') &&
           (Options[Index] <= L'9'))
    {
      Value = (Value * 10U) + (Options[Index] - L'0');
      Index++;
    }

    if ((Index > Token) &&
        ((Index >= Count) || (Options[Index] == L'\0') ||
         (Options[Index] == L' ') || (Options[Index] == L'\t')))
    {
      *Stage = Value;
      return (*Stage <= 14U) ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
    }

    while ((Index < Count) && (Options[Index] != L'\0') &&
           (Options[Index] != L' ') && (Options[Index] != L'\t'))
    {
      Index++;
    }
  }

  return EFI_INVALID_PARAMETER;
}

STATIC
VOID
PrintUsage (
  VOID
  )
{
  Print (L"Usage: Vim3SdProbe.efi <0..14>\n");
  Print (L"  0  watchdog self-test (expected reset)\n");
  Print (L"  1  current-state first CLOCK read\n");
  Print (L"  2  current-state first CFG read\n");
  Print (L"  3  current-state first STATUS clear write\n");
  Print (L"  4  disable; gate -> reset -> clock; CLOCK read\n");
  Print (L"  5  disable; reset -> gate -> clock; CLOCK read\n");
  Print (L"  6  pins; disable; gate -> reset -> clock; CLOCK read\n");
  Print (L"  7  disable; gate -> reset -> clock; pins; CLOCK read\n");
  Print (L"  8  current-state CLOCK write then read\n");
  Print (L"  9  current-state IRQ disable then read\n");
  Print (L" 10  current-state STATUS clear then CFG read\n");
  Print (L" 11  full minimal register initialization\n");
  Print (L" 12  CLOCK write-first (U-Boot 400kHz value) then CFG/STATUS\n");
  Print (L" 13  CLOCK write-first (Linux init value) then CFG/STATUS\n");
  Print (L" 14  CLOCK write-first (clkin1 source) then CFG/STATUS\n");
}

STATIC
UINT32
ClockWriteFirstProbe (
  IN UINT32  ClockValue
  )
{
  UINT32  Value;

  MmioWrite32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CLOCK, ClockValue);
  ArmDataSynchronizationBarrier ();
  Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CLOCK);
  Print (L"Vim3SdProbe: CLOCK wrote=%08x read=%08x; ACCESS CFG read\n",
    ClockValue, Value);
  Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CFG);
  Print (L"Vim3SdProbe: CFG value=%08x; ACCESS STATUS read\n", Value);
  Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_STATUS);
  Print (L"Vim3SdProbe: STATUS value=%08x\n", Value);
  return Value;
}

EFI_STATUS
EFIAPI
Vim3SdProbeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINTN       Stage;
  UINT32      Value;

  Status = ParseStage (ImageHandle, &Stage);
  if (EFI_ERROR (Status)) {
    PrintUsage ();
    return EFI_INVALID_PARAMETER;
  }

  Print (L"Vim3SdProbe: stage %u, BootROM source=%u\n", Stage,
    MmioRead32 (MESON_G12B_AO_SEC_GP_CFG0) & 0xFU);
  PrintSafeSnapshot ();

  if (Stage == 0) {
    Print (L"Vim3SdProbe: arming 2-second watchdog; RESET is success\n");
    WatchdogStart (2000);
    CpuDeadLoop ();
  }

  WatchdogStart (PROBE_WATCHDOG_MS);
  switch (Stage) {
    case 1:
      Print (L"Vim3SdProbe: ACCESS CLOCK read\n");
      Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CLOCK);
      break;
    case 2:
      Print (L"Vim3SdProbe: ACCESS CFG read\n");
      Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CFG);
      break;
    case 3:
      Print (L"Vim3SdProbe: ACCESS STATUS clear write\n");
      MmioWrite32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_STATUS, 0xFFFFU);
      Value = 0;
      break;
    case 4:
      Print (L"Vim3SdProbe: disable; gate-reset-clock; ACCESS CLOCK read\n");
      DisableSdDomain ();
      EnableSdDomain (TRUE);
      Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CLOCK);
      Print (L"Vim3SdProbe: CLOCK value=%08x; ACCESS CFG read\n", Value);
      Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CFG);
      break;
    case 5:
      Print (L"Vim3SdProbe: disable; reset-gate-clock; ACCESS CLOCK read\n");
      DisableSdDomain ();
      EnableSdDomain (FALSE);
      Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CLOCK);
      Print (L"Vim3SdProbe: CLOCK value=%08x; ACCESS CFG read\n", Value);
      Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CFG);
      break;
    case 6:
      Print (L"Vim3SdProbe: pins; gate-reset-clock; ACCESS CLOCK read\n");
      ConfigureSdPins ();
      DisableSdDomain ();
      EnableSdDomain (TRUE);
      Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CLOCK);
      Print (L"Vim3SdProbe: CLOCK value=%08x; ACCESS CFG read\n", Value);
      Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CFG);
      break;
    case 7:
      Print (L"Vim3SdProbe: gate-reset-clock; pins; ACCESS CLOCK read\n");
      DisableSdDomain ();
      EnableSdDomain (TRUE);
      ConfigureSdPins ();
      Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CLOCK);
      Print (L"Vim3SdProbe: CLOCK value=%08x; ACCESS CFG read\n", Value);
      Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CFG);
      break;
    case 8:
      Print (L"Vim3SdProbe: ACCESS CLOCK write/read\n");
      MmioWrite32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CLOCK, 0x0000023CU);
      Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CLOCK);
      break;
    case 9:
      Print (L"Vim3SdProbe: ACCESS IRQ disable/read\n");
      MmioWrite32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_IRQ_EN, 0);
      Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_IRQ_EN);
      break;
    case 10:
      Print (L"Vim3SdProbe: ACCESS STATUS clear then CFG read\n");
      MmioWrite32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_STATUS, 0xFFFFU);
      Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CFG);
      break;
    case 11:
      Print (L"Vim3SdProbe: ACCESS full minimal initialization\n");
      MmioWrite32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_STATUS, 0xFFFFU);
      MmioWrite32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_IRQ_EN, 0);
      MmioWrite32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CLOCK, 0x0000023CU);
      Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CFG);
      MmioWrite32 (
        MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CFG,
        (Value & ~0x0000FFF3U) | 0x00004880U | BIT23
        );
      Value = MmioRead32 (MESON_G12B_SD_EMMC_B_BASE + SD_EMMC_CFG);
      break;
    case 12:
      Print (L"Vim3SdProbe: write-first U-Boot value; ACCESS CLOCK write\n");
      Value = ClockWriteFirstProbe (SD_CLK_UBOOT_400K);
      break;
    case 13:
      Print (L"Vim3SdProbe: write-first Linux value; ACCESS CLOCK write\n");
      Value = ClockWriteFirstProbe (SD_CLK_LINUX_INIT);
      break;
    case 14:
      Print (L"Vim3SdProbe: write-first clkin1 value; ACCESS CLOCK write\n");
      Value = ClockWriteFirstProbe (SD_CLK_CLKIN1_INIT);
      break;
    default:
      WatchdogStop ();
      return EFI_INVALID_PARAMETER;
  }

  WatchdogStop ();
  Print (L"Vim3SdProbe: stage %u PASS value=%08x; warm reset to BootOrder\n",
    Stage, Value);
  gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);
  CpuDeadLoop ();
  return EFI_DEVICE_ERROR;
}
