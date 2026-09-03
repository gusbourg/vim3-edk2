/** @file
  Amlogic Meson G12B SD/eMMC host controller driver.

  The controller programming model follows the upstream meson-gx-mmc driver.
  The controller accepts up to 511 blocks in one command.  Advertising that
  capability is also required by EmbeddedPkg's generic MmcDxe: its fallback
  loop does not correctly advance RemainingBlock when IsMultiBlock is false.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/MmcHost.h>

#include <MesonG12B.h>

#define MESON_MMC_SIGNATURE  SIGNATURE_32 ('M', 'M', 'C', 'G')
#define BIT_N(n)              (1U << (n))
#define GENMASK32(h, l)       \
  ((MAX_UINT32 << (l)) & (MAX_UINT32 >> (31U - (h))))

#define SD_EMMC_CLOCK        0x00
#define   CLK_MAX_DIV        63U
#define   CLK_SRC_24M        (0U << 6)
#define   CLK_SRC_DIV2       (1U << 6)
#define   CLK_CO_PHASE_180   (2U << 8)
#define   CLK_TX_PHASE_000   (0U << 10)

#define SD_EMMC_CFG          0x44
#define   CFG_BUS_WIDTH_MASK GENMASK32 (1, 0)
#define   CFG_BUS_WIDTH_1    0U
#define   CFG_BUS_WIDTH_4    1U
#define   CFG_BUS_WIDTH_8    2U
#define   CFG_BL_LEN_MASK    GENMASK32 (7, 4)
#define   CFG_BL_LEN_SHIFT   4
#define   CFG_RESP_TIMEOUT_256 (8U << 8)
#define   CFG_RESP_TIMEOUT_MASK GENMASK32 (11, 8)
#define   CFG_RC_CC_16       (4U << 12)
#define   CFG_RC_CC_MASK     GENMASK32 (15, 12)
#define   CFG_SDCLK_ALWAYS_ON BIT18
#define   CFG_AUTO_CLK       BIT23

#define SD_EMMC_STATUS       0x48
#define   STATUS_CLEAR_MASK  GENMASK32 (15, 0)
#define   STATUS_ERROR_MASK  GENMASK32 (12, 0)
#define   STATUS_RESP_TIMEOUT BIT11
#define   STATUS_END_OF_CHAIN BIT13
//
// SD_EMMC_STATUS[23:16] mirrors the live DAT[7:0] line levels (same layout
// Linux's meson-gx-mmc uses).  DAT0 low means the card is busy programming.
//
#define   STATUS_DAT0_IDLE   BIT16

#define SD_EMMC_IRQ_EN       0x4C
#define SD_EMMC_CMD_CFG      0x50
#define   CMD_CFG_LENGTH_MASK GENMASK32 (8, 0)
#define   CMD_CFG_MAX_BLOCKS  511U
#define   CMD_CFG_BLOCK_MODE BIT9
#define   CMD_CFG_R1B        BIT10
#define   CMD_CFG_END_OF_CHAIN BIT11
#define   CMD_CFG_TIMEOUT_4S (12U << 12)
#define   CMD_CFG_NO_RESP    BIT16
#define   CMD_CFG_DATA_IO    BIT18
#define   CMD_CFG_DATA_WR    BIT19
#define   CMD_CFG_RESP_NOCRC BIT20
#define   CMD_CFG_RESP_128   BIT21
#define   CMD_CFG_CMD_INDEX_SHIFT 24
#define   CMD_CFG_OWNER      BIT31
#define SD_EMMC_CMD_ARG      0x54
#define SD_EMMC_CMD_DAT      0x58
#define SD_EMMC_CMD_RSP      0x5C
#define SD_EMMC_CMD_RSP1     0x60
#define SD_EMMC_CMD_RSP2     0x64
#define SD_EMMC_CMD_RSP3     0x68

#define SD_EMMC_CLKSRC_24M   24000000U
#define SD_EMMC_CLKSRC_DIV2  1000000000U
#define IDENTIFICATION_CLOCK 400000U
#define MAX_TRANSFER_CLOCK   50000000U
#define COMMAND_TIMEOUT_US   100000U

//
// SD_EMMC_A is the SDIO controller (onboard Wi-Fi).  Bit positions derived from
// the Linux G12A clock driver rather than inferred:
//   MESON_GATE(g12a_emmc_a, HHI_GCLK_MPEG0, 24)   - peripheral gate
//   g12a_sd_emmc_a_clk0 .offset = HHI_SD_EMMC_CLK_CNTL, .bit_idx = 7
//   RESET_SD_EMMC_A = 44 -> RESET_LEVEL1 bit (44 % 32) = 12
// The B/C values below agree with the same table, which is the cross-check.
//
// Note A and B share HHI_SD_EMMC_CLK_CNTL: A's enable is bit 7 and B's is
// bit 23, so writing B alone (as this driver used to) actively CLEARED A's
// clock.  An ungated peripheral stalls the AXI bus and hard-hangs the SoC if
// anything touches its registers, which is why the ACPI SDIO device had to
// stay disabled until this landed.
//
//
// SDIO power sequence for the onboard BCM4359 Wi-Fi.  DeviceTree expresses this
// as mmc-pwrseq-simple with reset-gpios = GPIOX_6 (active low) and an ext_clock
// of 32.768 kHz produced by a pwm-clock on PWM_E.  ACPI can express neither, so
// firmware must leave the chip released and clocked before handing off.
//
// All register locations derived from the Linux drivers, then cross-checked
// against values this file/MesonUsbDxe already use and are known good:
//
//   pinctrl-meson-g12a.c  BANK_PMX("X", GPIOX_0, GPIOX_19, 0x3, 0)
//     mux reg = 3 + (index / 8), field = (index % 8) * 4
//     GPIOX_6  -> reg 3, bits 27:24   GPIOX_16 -> reg 5, bits 3:0
//     Cross-check: the same formula gives GPIOH_8 -> reg 12 bit 0 and
//     GPIOA_6 -> reg 13 bit 24, which is exactly what MesonUsbDxe uses.
//
//   BANK_DS("X", ...) gives DIR = reg 6, OUT = reg 7 (indices into
//     PERIPHS_GPIO_BASE).  Cross-check: bank A gives DIR 16 / OUT 17, matching
//     MesonUsbDxe's GPIO_A_DIRECTION / GPIO_A_OUTPUT.
//     Direction bit SET = input, CLEARED = output (as used for GPIOA_6).
//
//   pwm_e is GPIOX_16 with mux function 1 (pwm_e_pins / GROUP(pwm_e, 1)).
//   pwm_ef is cbus 0xffd00000 + 0x19000.  pwm-meson.c: REG_PWM_A = 0x0,
//   REG_MISC_AB = 0x8, A_EN = BIT0, A_CLK_SEL shift 4, A_CLK_DIV shift 8,
//   A_CLK_EN = BIT15, and parent_names[0] = "xtal" so CLK_SEL 0 = 24 MHz.
//
//   Timing follows meson_pwm_calc(): cnt = fin_freq * period / 1e9.
//   24 MHz * 30518 ns / 1e9 = 732 ticks; 50% duty -> hi = lo = 366.
//   Actual 24e6 / 732 = 32787 Hz, 0.06% above 32768 - well inside tolerance
//   for the Wi-Fi sleep clock.
//
#define MESON_G12B_PWM_EF_BASE  0xFFD19000ULL
#define PWM_REG_A               0x00
#define PWM_REG_MISC_AB         0x08
#define PWM_MISC_A_EN           BIT0
#define PWM_MISC_A_CLK_SEL_MASK (0x3U << 4)
#define PWM_MISC_A_CLK_DIV_MASK (0x7FU << 8)
#define PWM_MISC_A_CLK_EN       BIT15

//
// The SDIO bus itself is GPIOX_0..5 (sdio_d0..d3, sdio_clk, sdio_cmd), all mux
// function 1, and they share pinmux register 3 with GPIOX_6 - so one write
// covers the bus and the Wi-Fi reset pin.  Firmware never muxed these, which
// left SD_EMMC_A driving pads that are not wired to the Wi-Fi chip at all: the
// controller then answered SD-style probes (CMD8/CMD41) while CMD5 - the only
// command a BCM4359 actually responds to - always timed out.
//
//   GPIOX_0 bits  3:0    GPIOX_3 bits 15:12
//   GPIOX_1 bits  7:4    GPIOX_4 bits 19:16
//   GPIOX_2 bits 11:8    GPIOX_5 bits 23:20    GPIOX_6 bits 27:24
//
#define MUX_GPIOX_0_7           (MESON_G12B_PERIPHS_MUX_BASE + (3 * 4))
#define MUX_GPIOX_0_6_MASK      (0x0FFFFFFFU)
#define MUX_GPIOX_SDIO_BUS      (0x00111111U)   // X0..X5 = func 1, X6 = GPIO
#define MUX_GPIOX_6             (MESON_G12B_PERIPHS_MUX_BASE + (3 * 4))
#define MUX_GPIOX_6_MASK        (0xFU << 24)
#define MUX_GPIOX_16            (MESON_G12B_PERIPHS_MUX_BASE + (5 * 4))
#define MUX_GPIOX_16_MASK       (0xFU << 0)
#define MUX_GPIOX_16_PWM_E      (0x1U << 0)
#define GPIO_X_DIRECTION        (MESON_G12B_PERIPHS_GPIO_BASE + (6 * 4))
#define GPIO_X_OUTPUT           (MESON_G12B_PERIPHS_GPIO_BASE + (7 * 4))
#define GPIO_X_WIFI_RESET       BIT6

#define WIFI_32K_PWM_VALUE      ((366U << 16) | 366U)

//
// Bluetooth half of the same combo module: uart_A on GPIOX_12..15
// (uart_a tx/rx/cts/rts, all mux function 1), sharing pinmux register 4
// (GPIOX_8..15 - the formula above):
//   GPIOX_12 bits 19:16   GPIOX_14 bits 27:24
//   GPIOX_13 bits 23:20   GPIOX_15 bits 31:28
// The EE-domain UART0 peripheral clock gate is HHI_GCLK_MPEG0 bit 13
// (g12a.c CLKID_UART0) - never opened by firmware before, exactly the
// hazard the DSDT's UARA note warned about.
//
#define MUX_GPIOX_8_15          (MESON_G12B_PERIPHS_MUX_BASE + (4 * 4))
#define MUX_GPIOX_12_15_MASK    (0xFFFF0000U)
#define MUX_GPIOX_UART_A        (0x11110000U)
#define HHI_GCLK_MPEG0_REG      (0xFF63C000ULL + 0x140)
#define HHI_GCLK_UART0          BIT13

#define HHI_GATE_SD_EMMC_A   BIT24
#define HHI_CLK0_SD_EMMC_A   BIT7
#define RESET_SD_EMMC_A      BIT12
#define HHI_GATE_SD_EMMC_B   BIT25
#define HHI_GATE_SD_EMMC_C   BIT26
#define HHI_CLK0_SD_EMMC_B   BIT23
#define HHI_CLK0_SD_EMMC_C   BIT7
#define RESET_SD_EMMC_B      BIT13
#define RESET_SD_EMMC_C      BIT14

#define BOOT_DEVICE_MASK      0xFU
#define BOOT_DEVICE_EMMC      1U
#define BOOT_DEVICE_SPI_NOR   3U
#define BOOT_DEVICE_SD        4U
#define BOOT_DEVICE_USB       5U

#define PERIPHS_MUX_BOOT0_7  (MESON_G12B_PERIPHS_MUX_BASE + (0x0 * 4))
#define PERIPHS_MUX_BOOT8_15 (MESON_G12B_PERIPHS_MUX_BASE + (0x1 * 4))
#define PERIPHS_MUX_C0_7     (MESON_G12B_PERIPHS_MUX_BASE + (0x9 * 4))
#define PERIPHS_GPIO_C_DIR   (MESON_G12B_PERIPHS_GPIO_BASE + (3 * 4))
#define PERIPHS_GPIO_C_IN    (MESON_G12B_PERIPHS_GPIO_BASE + (5 * 4))

//
// The GPIO window starts at the BOOT bank: direction (output-enable-low),
// output, then input, before the GPIOC triple used above.  BOOT_12 is the
// eMMC hardware reset line (published DT: emmc-pwrseq reset-gpios pin 0x25,
// active low).
//
#define PERIPHS_GPIO_BOOT_DIR  (MESON_G12B_PERIPHS_GPIO_BASE + (0 * 4))
#define PERIPHS_GPIO_BOOT_OUT  (MESON_G12B_PERIPHS_GPIO_BASE + (1 * 4))
#define EMMC_RESET_BIT         BIT12

#define PERIPHS_PULL_BOOT    (MESON_G12B_PERIPHS_PULL_BASE + (0 * 4))
#define PERIPHS_PULLEN_BOOT  (MESON_G12B_PERIPHS_PULLEN_BASE + (0 * 4))
#define PERIPHS_DS_BOOT      (MESON_G12B_PERIPHS_DS_BASE + (0 * 4))
#define PERIPHS_PULL_C       (MESON_G12B_PERIPHS_PULL_BASE + (1 * 4))
#define PERIPHS_PULLEN_C     (MESON_G12B_PERIPHS_PULLEN_BASE + (1 * 4))
#define PERIPHS_DS_C         (MESON_G12B_PERIPHS_DS_BASE + (1 * 4))

#define GPIOC_6_BIT          BIT6

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} MESON_MMC_DEVICE_PATH;

typedef struct {
  UINT32                 Signature;
  EFI_HANDLE             Handle;
  EFI_MMC_HOST_PROTOCOL  Protocol;
  UINTN                  Base;
  BOOLEAN                IsEmmc;
  BOOLEAN                AppCommand;
  BOOLEAN                AddressingKnown;
  BOOLEAN                SectorAddressed;
  BOOLEAN                PendingValid;
  MMC_CMD                PendingCmd;
  UINT32                 PendingArgument;
  UINT32                 Response[4];
} MESON_MMC_HOST;

#define MESON_MMC_FROM_PROTOCOL(a) \
  CR (a, MESON_MMC_HOST, Protocol, MESON_MMC_SIGNATURE)

STATIC EFI_GUID  mMesonSdDevicePathGuid =
  { 0xb072f2f9, 0x1405, 0x4a13, { 0xa2, 0x10, 0x3e, 0x5a, 0x4d, 0xd3, 0xb5, 0x20 } };
STATIC EFI_GUID  mMesonEmmcDevicePathGuid =
  { 0x9ec8ef97, 0x39d8, 0x48a3, { 0x84, 0xaf, 0xa1, 0x94, 0xd9, 0x92, 0xc6, 0x37 } };

STATIC MESON_MMC_HOST  mSdHost;
STATIC MESON_MMC_HOST  mEmmcHost;

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

/**
  Release the onboard Wi-Fi from reset and give it its 32.768 kHz clock.

  This is mmc-pwrseq-simple done in firmware.  Without it the BCM4359 sits in
  reset with no sleep clock, cannot answer CMD5, and the SDIO controller reports
  "no support for card's volts" - which looks like a voltage problem but is not.

  Left running for the OS: under ACPI nothing can re-create this, and under
  DeviceTree Linux drives the same pins itself, so leaving the chip released and
  clocked is safe in both cases.
**/
STATIC
VOID
MesonSdioPowerSequence (
  VOID
  )
{
  //
  // Clock first: mmc-pwrseq-simple enables ext_clock before deasserting reset,
  // and the chip samples it coming out of reset.
  //
  Rmw32 (MUX_GPIOX_16, MUX_GPIOX_16_MASK, MUX_GPIOX_16_PWM_E);
  MmioWrite32 (MESON_G12B_PWM_EF_BASE + PWM_REG_A, WIFI_32K_PWM_VALUE);
  Rmw32 (
    MESON_G12B_PWM_EF_BASE + PWM_REG_MISC_AB,
    PWM_MISC_A_CLK_SEL_MASK | PWM_MISC_A_CLK_DIV_MASK,
    PWM_MISC_A_CLK_EN | PWM_MISC_A_EN
    );
  ArmDataSynchronizationBarrier ();

  //
  // Now pulse reset.  GPIOX_6 is active low, and on this SoC a cleared
  // direction bit means output.
  //
  //
  // Mux the SDIO bus to the controller and GPIOX_6 to GPIO in one go; they
  // live in the same register.
  //
  Rmw32 (MUX_GPIOX_0_7, MUX_GPIOX_0_6_MASK, MUX_GPIOX_SDIO_BUS);
  MmioAnd32 (GPIO_X_OUTPUT, ~GPIO_X_WIFI_RESET);
  MmioAnd32 (GPIO_X_DIRECTION, ~GPIO_X_WIFI_RESET);
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (10000);

  MmioOr32 (GPIO_X_OUTPUT, GPIO_X_WIFI_RESET);
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (10000);

  //
  // Bluetooth companion setup: mux uart_A onto GPIOX_12..15 and open the
  // EE UART0 peripheral clock gate.  The ACPI UARA/BTH0 devices state
  // "clock-frequency" as a firmware fact - this is that fact.  BT_EN
  // (GPIOX_17) is left to the OS: hci_bcm owns the chip's power sequence
  // (validated: it needs a real REG_ON rising edge under its own timing).
  // Harmless under DeviceTree - pinctrl and the clock controller re-apply
  // the same configuration at probe.
  //
  Rmw32 (MUX_GPIOX_8_15, MUX_GPIOX_12_15_MASK, MUX_GPIOX_UART_A);
  MmioOr32 (HHI_GCLK_MPEG0_REG, HHI_GCLK_UART0);
  ArmDataSynchronizationBarrier ();

  DEBUG ((DEBUG_INFO, "MesonMMC: SDIO Wi-Fi released, 32.768 kHz clock running, BT uart_A muxed+clocked\n"));
}

STATIC
VOID
SetDriveStrength4mA (
  IN UINTN   Address,
  IN UINT32  Pins
  )
{
  UINT32  Mask;
  UINT32  Pin;

  Mask = 0;
  for (Pin = 0; Pin < 16; Pin++) {
    if ((Pins & BIT_N (Pin)) != 0) {
      Mask |= 3U << (Pin * 2);
    }
  }

  Rmw32 (Address, Mask, Mask);
}

STATIC
VOID
MesonInitBoardIo (
  VOID
  )
{
  UINT32  BootPins;
  UINT32  SdPins;

  //
  // The boot source leaves one of these shared clock-control registers in a
  // source-dependent state.  Mainline U-Boot's G12A clock driver deliberately
  // clears both at probe time to avoid a controller access deadlock.  Do the
  // same before selecting the 24 MHz crystal and enabling B/C clock outputs.
  //
  DEBUG ((DEBUG_INFO, "MesonMMC: normalizing controller clocks\n"));
  MmioWrite32 (MESON_G12B_HHI_SD_EMMC_CLK_CNTL, 0);
  MmioWrite32 (MESON_G12B_HHI_NAND_CLK_CNTL, 0);
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (1000);

  //
  // Match the ordering used by U-Boot's bulk clock enable: open the
  // controller bus gates before starting their functional clock outputs.
  // Starting CLK0 while the bus gate is still closed can leave the G12B
  // clock domain inaccessible after a warm reset.
  //
  MmioOr32 (
    MESON_G12B_HHI_GCLK_MPEG0,
    HHI_GATE_SD_EMMC_A | HHI_GATE_SD_EMMC_B | HHI_GATE_SD_EMMC_C
    );
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (1000);

  //
  // A and B both live in this register; write them together.  Divider and
  // selector fields stay zero for both, i.e. the 24 MHz crystal divided by one.
  //
  MmioWrite32 (
    MESON_G12B_HHI_SD_EMMC_CLK_CNTL,
    HHI_CLK0_SD_EMMC_A | HHI_CLK0_SD_EMMC_B
    );
  MmioWrite32 (
    MESON_G12B_HHI_NAND_CLK_CNTL,
    HHI_CLK0_SD_EMMC_C
    );
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (1000);

  //
  // Keep both controllers out of reset.  Pulsing RESET_LEVEL after their clock
  // outputs are enabled makes the first controller register access stall on
  // the SD boot path.
  //
  DEBUG ((DEBUG_INFO, "MesonMMC: releasing controller resets\n"));
  MmioOr32 (
    MESON_G12B_RESET_LEVEL1,
    RESET_SD_EMMC_A | RESET_SD_EMMC_B | RESET_SD_EMMC_C
    );
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (1000);

  MesonSdioPowerSequence ();

  DEBUG ((
    DEBUG_INFO,
    "MesonMMC: clock state gate=0x%08x b=0x%08x c=0x%08x reset=0x%08x\n",
    MmioRead32 (MESON_G12B_HHI_GCLK_MPEG0),
    MmioRead32 (MESON_G12B_HHI_SD_EMMC_CLK_CNTL),
    MmioRead32 (MESON_G12B_HHI_NAND_CLK_CNTL),
    MmioRead32 (MESON_G12B_RESET_LEVEL1)
    ));

  //
  // SD: GPIOC[0:5] function 1, GPIOC_6 card detect as GPIO.
  //
  DEBUG ((DEBUG_INFO, "MesonMMC: configuring SD pins\n"));
  Rmw32 (PERIPHS_MUX_C0_7, 0x0FFFFFFFU, 0x00111111U);
  SdPins = BIT0 | BIT1 | BIT2 | BIT3 | BIT5 | BIT6;
  Rmw32 (PERIPHS_PULLEN_C, GENMASK32 (6, 0), SdPins);
  Rmw32 (PERIPHS_PULL_C, GENMASK32 (6, 0), SdPins);
  SetDriveStrength4mA (PERIPHS_DS_C, GENMASK32 (5, 0));
  MmioOr32 (PERIPHS_GPIO_C_DIR, GPIOC_6_BIT);

  //
  // eMMC: BOOT[0:3], BOOT_8, BOOT_10, and BOOT_13 function 1.
  //
  // BOOT[4:7] are shared with the on-board SPIFC.  This platform assigns
  // those pins to the SPI NOR permanently, so never select their eMMC
  // function.
  //
  DEBUG ((DEBUG_INFO, "MesonMMC: configuring eMMC pins\n"));
  Rmw32 (PERIPHS_MUX_BOOT0_7, 0x0000FFFFU, 0x00001111U);
  Rmw32 (PERIPHS_MUX_BOOT8_15, 0x00F00F0FU, 0x00100101U);
  BootPins = GENMASK32 (3, 0) | BIT8 | BIT10 | BIT13;
  Rmw32 (
    PERIPHS_PULLEN_BOOT,
    GENMASK32 (8, 0) | BIT10 | BIT13,
    GENMASK32 (3, 0) | BIT10 | BIT13
    );
  Rmw32 (
    PERIPHS_PULL_BOOT,
    GENMASK32 (8, 0) | BIT10 | BIT13,
    GENMASK32 (3, 0) | BIT10
    );
  SetDriveStrength4mA (PERIPHS_DS_BOOT, BootPins);

  ArmDataSynchronizationBarrier ();
  DEBUG ((DEBUG_INFO, "MesonMMC: board I/O configured\n"));
}

//
// Pulse the eMMC hardware reset line, the sequence the OS gets from the
// published DT's mmc-pwrseq-emmc node.  BL2 performs this itself on any
// boot where it initializes storage; when the BootROM skips storage (USB
// recovery) nothing has reset the card, and it never answers CMD1 even
// though its controller registers are accessible.  Timings follow the
// pwrseq contract: assert briefly, then let the card settle before
// identification.
//
STATIC
VOID
MesonResetEmmcCard (
  VOID
  )
{
  DEBUG ((DEBUG_INFO, "MesonMMC: pulsing eMMC hardware reset\n"));
  MmioAnd32 (PERIPHS_GPIO_BOOT_DIR, ~(UINT32)EMMC_RESET_BIT);
  MmioAnd32 (PERIPHS_GPIO_BOOT_OUT, ~(UINT32)EMMC_RESET_BIT);
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (10);
  MmioOr32 (PERIPHS_GPIO_BOOT_OUT, EMMC_RESET_BIT);
  ArmDataSynchronizationBarrier ();
  MicroSecondDelay (200);
}

STATIC
VOID
MesonSetClock (
  IN MESON_MMC_HOST  *Host,
  IN UINT32          RequestedHz
  )
{
  UINT32  Divider;
  UINT32  Source;
  UINT32  SourceHz;

  if (RequestedHz == 0) {
    return;
  }

  RequestedHz = MIN (RequestedHz, MAX_TRANSFER_CLOCK);
  if (RequestedHz > 16000000U) {
    SourceHz = SD_EMMC_CLKSRC_DIV2;
    Source   = CLK_SRC_DIV2;
  } else {
    SourceHz = SD_EMMC_CLKSRC_24M;
    Source   = CLK_SRC_24M;
  }

  Divider = (SourceHz + RequestedHz - 1) / RequestedHz;
  Divider = MIN (MAX (Divider, 1U), CLK_MAX_DIV);
  MmioWrite32 (
    Host->Base + SD_EMMC_CLOCK,
    CLK_CO_PHASE_180 | CLK_TX_PHASE_000 | Source | Divider
    );
}

STATIC
EFI_STATUS
MesonSetBusWidth (
  IN MESON_MMC_HOST  *Host,
  IN UINT32          BusWidth
  )
{
  UINT32  Config;
  UINT32  Width;

  switch (BusWidth) {
    case 1:
      Width = CFG_BUS_WIDTH_1;
      break;
    case 4:
      Width = CFG_BUS_WIDTH_4;
      break;
    case 8:
      //
      // eMMC D4-D7 are owned by SPIFC on VIM3.  The generic eMMC layer will
      // fall back to four-bit operation when the host declines eight-bit.
      //
      if (Host->IsEmmc) {
        return EFI_UNSUPPORTED;
      }

      Width = CFG_BUS_WIDTH_8;
      break;
    default:
      return EFI_UNSUPPORTED;
  }

  Config  = MmioRead32 (Host->Base + SD_EMMC_CFG);
  Config &= ~CFG_BUS_WIDTH_MASK;
  Config |= Width;
  Config &= ~(CFG_RESP_TIMEOUT_MASK | CFG_RC_CC_MASK | CFG_SDCLK_ALWAYS_ON);
  Config |= CFG_RESP_TIMEOUT_256 | CFG_RC_CC_16 | CFG_AUTO_CLK;
  MmioWrite32 (Host->Base + SD_EMMC_CFG, Config);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MesonHardwareInit (
  IN MESON_MMC_HOST  *Host
  )
{
  DEBUG ((
    DEBUG_INFO,
    "MesonMMC: initializing %a registers at 0x%lx\n",
    Host->IsEmmc ? "eMMC" : "SD",
    Host->Base
    ));

  Host->AppCommand  = FALSE;
  Host->AddressingKnown = FALSE;
  Host->SectorAddressed = FALSE;
  Host->PendingValid = FALSE;
  ZeroMem (Host->Response, sizeof (Host->Response));

  //
  // The controller core register file (0x40 and up: START/CFG/STATUS/CMD) is
  // clocked by the controller-internal divided clock configured by
  // SD_EMMC_CLOCK.  At reset the divider is zero, that clock is stopped, and
  // the first core access hard-stalls the CPU; the BootROM only programs
  // SD_EMMC_CLOCK on controllers it probes for boot.  Writing SD_EMMC_CLOCK
  // (a register in the always-clocked 0x00-0x10 group) must therefore be the
  // FIRST controller access.  Both Linux and U-Boot order their init this
  // way; proven on this silicon by Vim3SdProbe stage 12
  // (inactive controller B on an eMMC-origin boot: CFG/STATUS readable
  // only after this write).
  //
  MesonSetClock (Host, IDENTIFICATION_CLOCK);
  ArmDataSynchronizationBarrier ();
  DEBUG ((DEBUG_INFO, "MesonMMC: %a identification clock set\n", Host->IsEmmc ? "eMMC" : "SD"));
  MmioWrite32 (Host->Base + SD_EMMC_STATUS, STATUS_CLEAR_MASK);
  DEBUG ((DEBUG_INFO, "MesonMMC: %a status cleared\n", Host->IsEmmc ? "eMMC" : "SD"));
  MmioWrite32 (Host->Base + SD_EMMC_IRQ_EN, 0);
  DEBUG ((DEBUG_INFO, "MesonMMC: %a IRQs disabled\n", Host->IsEmmc ? "eMMC" : "SD"));
  if (EFI_ERROR (MesonSetBusWidth (Host, 1))) {
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_INFO, "MesonMMC: %a hardware initialized\n", Host->IsEmmc ? "eMMC" : "SD"));
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
MesonCommandHasData (
  IN MESON_MMC_HOST  *Host,
  IN UINT32          Index,
  IN BOOLEAN         IsApplicationCommand
  )
{
  if ((Index == 17) || (Index == 18) || (Index == 24) || (Index == 25) ||
      (Index == 51))
  {
    return TRUE;
  }

  if (Host->IsEmmc) {
    return Index == 8;
  }

  return (Index == 6) && !IsApplicationCommand;
}

STATIC
EFI_STATUS
MesonExecuteCommand (
  IN MESON_MMC_HOST  *Host,
  IN MMC_CMD         Cmd,
  IN UINT32          Argument,
  IN VOID            *Buffer OPTIONAL,
  IN UINTN           Length,
  IN BOOLEAN         Write
  )
{
  UINT32  BlockLength;
  UINT32  BlockCount;
  UINT32  Command;
  UINT32  Config;
  UINT32  Index;
  UINT32  Status;
  UINTN   Timeout;

  Index   = MMC_GET_INDX (Cmd);
  Command = Index << CMD_CFG_CMD_INDEX_SHIFT;

  if ((Cmd & MMC_CMD_WAIT_RESPONSE) == 0) {
    Command |= CMD_CFG_NO_RESP;
  } else {
    if ((Cmd & MMC_CMD_LONG_RESPONSE) != 0) {
      Command |= CMD_CFG_RESP_128;
    }

    if ((Cmd & MMC_CMD_NO_CRC_RESPONSE) != 0) {
      Command |= CMD_CFG_RESP_NOCRC;
    }
  }

  if (Buffer != NULL) {
    if ((Length == 0) ||
        ((UINTN)Buffer > MAX_UINT32) ||
        ((Length - 1) > (MAX_UINT32 - (UINTN)Buffer)))
    {
      DEBUG ((
        DEBUG_ERROR,
        "MesonMMC@%lx: invalid DMA buffer %p length 0x%lx\n",
        Host->Base,
        Buffer,
        Length
        ));
      return EFI_BAD_BUFFER_SIZE;
    }

    if ((Index == 17) || (Index == 18) || (Index == 24) || (Index == 25)) {
      if (((Length % 512) != 0) ||
          ((Length / 512) > CMD_CFG_MAX_BLOCKS))
      {
        DEBUG ((
          DEBUG_ERROR,
          "MesonMMC@%lx: invalid block transfer length 0x%lx\n",
          Host->Base,
          Length
          ));
        return EFI_BAD_BUFFER_SIZE;
      }

      BlockLength = 9;
      BlockCount  = (UINT32)(Length / 512);
    } else {
      if ((Length > 512) || ((Length & (Length - 1)) != 0)) {
        DEBUG ((
          DEBUG_ERROR,
          "MesonMMC@%lx: invalid auxiliary transfer length 0x%lx\n",
          Host->Base,
          Length
          ));
        return EFI_BAD_BUFFER_SIZE;
      }

      BlockLength = LowBitSet32 ((UINT32)Length);
      BlockCount  = 1;
    }

    Config      = MmioRead32 (Host->Base + SD_EMMC_CFG);
    Config     &= ~CFG_BL_LEN_MASK;
    Config     |= BlockLength << CFG_BL_LEN_SHIFT;
    MmioWrite32 (Host->Base + SD_EMMC_CFG, Config);

    Command |= CMD_CFG_DATA_IO | CMD_CFG_BLOCK_MODE |
               (BlockCount & CMD_CFG_LENGTH_MASK);
    if (Write) {
      Command |= CMD_CFG_DATA_WR;
      WriteBackDataCacheRange (Buffer, Length);
    } else {
      WriteBackInvalidateDataCacheRange (Buffer, Length);
    }

    MmioWrite32 (Host->Base + SD_EMMC_CMD_DAT, (UINT32)(UINTN)Buffer);
  } else {
    MmioWrite32 (Host->Base + SD_EMMC_CMD_DAT, 0);
  }

  if ((Index == 7) || (Index == 12) ||
      (Host->IsEmmc && (Index == 6)))
  {
    Command |= CMD_CFG_R1B;
  }

  Command |= CMD_CFG_TIMEOUT_4S | CMD_CFG_OWNER | CMD_CFG_END_OF_CHAIN;

  MmioWrite32 (Host->Base + SD_EMMC_STATUS, STATUS_CLEAR_MASK);
  ArmDataSynchronizationBarrier ();
  MmioWrite32 (Host->Base + SD_EMMC_CMD_CFG, Command);
  MmioWrite32 (Host->Base + SD_EMMC_CMD_ARG, Argument);

  Status = 0;
  for (Timeout = 0; Timeout < COMMAND_TIMEOUT_US; Timeout++) {
    Status = MmioRead32 (Host->Base + SD_EMMC_STATUS);
    if ((Status & STATUS_END_OF_CHAIN) != 0) {
      break;
    }

    MicroSecondDelay (1);
  }

  Host->Response[0] = MmioRead32 (Host->Base + SD_EMMC_CMD_RSP);
  Host->Response[1] = MmioRead32 (Host->Base + SD_EMMC_CMD_RSP1);
  Host->Response[2] = MmioRead32 (Host->Base + SD_EMMC_CMD_RSP2);
  Host->Response[3] = MmioRead32 (Host->Base + SD_EMMC_CMD_RSP3);

  if ((Buffer != NULL) && !Write) {
    InvalidateDataCacheRange (Buffer, Length);
  }

  MmioWrite32 (Host->Base + SD_EMMC_STATUS, STATUS_CLEAR_MASK);

  if ((Status & STATUS_END_OF_CHAIN) == 0) {
    DEBUG ((DEBUG_ERROR, "MesonMMC@%lx: CMD%d software timeout\n", Host->Base, Index));
    return EFI_TIMEOUT;
  }

  if ((Status & STATUS_RESP_TIMEOUT) != 0) {
    return EFI_TIMEOUT;
  }

  if ((Status & STATUS_ERROR_MASK) != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "MesonMMC@%lx: CMD%d status 0x%08x\n",
      Host->Base,
      Index,
      Status
      ));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EFIAPI
MesonIsCardPresent (
  IN EFI_MMC_HOST_PROTOCOL  *This
  )
{
  MESON_MMC_HOST  *Host;

  Host = MESON_MMC_FROM_PROTOCOL (This);
  if (Host->IsEmmc) {
    return TRUE;
  }

  return (MmioRead32 (PERIPHS_GPIO_C_IN) & GPIOC_6_BIT) == 0;
}

STATIC
BOOLEAN
EFIAPI
MesonIsReadOnly (
  IN EFI_MMC_HOST_PROTOCOL  *This
  )
{
  return FALSE;
}

STATIC
EFI_STATUS
EFIAPI
MesonBuildDevicePath (
  IN  EFI_MMC_HOST_PROTOCOL     *This,
  OUT EFI_DEVICE_PATH_PROTOCOL  **DevicePath
  )
{
  MESON_MMC_HOST       *Host;
  VENDOR_DEVICE_PATH   *Vendor;

  if (DevicePath == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Host   = MESON_MMC_FROM_PROTOCOL (This);
  Vendor = AllocateZeroPool (sizeof (*Vendor));
  if (Vendor == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Vendor->Header.Type    = HARDWARE_DEVICE_PATH;
  Vendor->Header.SubType = HW_VENDOR_DP;
  SetDevicePathNodeLength (&Vendor->Header, sizeof (*Vendor));
  CopyGuid (
    &Vendor->Guid,
    Host->IsEmmc ? &mMesonEmmcDevicePathGuid : &mMesonSdDevicePathGuid
    );
  *DevicePath = &Vendor->Header;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MesonNotifyState (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN MMC_STATE              State
  )
{
  MESON_MMC_HOST  *Host;

  Host = MESON_MMC_FROM_PROTOCOL (This);
  if (State == MmcHwInitializationState) {
    return MesonHardwareInit (Host);
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MesonSendCommand (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN MMC_CMD                Cmd,
  IN UINT32                 Argument
  )
{
  MESON_MMC_HOST  *Host;
  BOOLEAN         IsApplicationCommand;
  EFI_STATUS      Status;
  UINT32          Index;

  Host                 = MESON_MMC_FROM_PROTOCOL (This);
  Index                = MMC_GET_INDX (Cmd);
  IsApplicationCommand = Host->AppCommand;
  Host->AppCommand     = FALSE;

  if (MesonCommandHasData (Host, Index, IsApplicationCommand)) {
    Host->PendingCmd      = Cmd;
    Host->PendingArgument = Argument;
    Host->PendingValid    = TRUE;
    return EFI_SUCCESS;
  }

  Host->PendingValid = FALSE;
  Status = MesonExecuteCommand (Host, Cmd, Argument, NULL, 0, FALSE);
  if (!EFI_ERROR (Status) &&
      ((Host->IsEmmc && (Index == 1)) ||
       (!Host->IsEmmc && IsApplicationCommand && (Index == 41))))
  {
    //
    // OCR bit 30 selects sector rather than byte command arguments.
    //
    Host->AddressingKnown  = TRUE;
    Host->SectorAddressed  = (Host->Response[0] & BIT30) != 0;
  }

  if (!EFI_ERROR (Status) && (Index == 55)) {
    Host->AppCommand = TRUE;
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
MesonReceiveResponse (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN MMC_RESPONSE_TYPE      Type,
  IN UINT32                 *Buffer
  )
{
  MESON_MMC_HOST  *Host;

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Host = MESON_MMC_FROM_PROTOCOL (This);
  if (Type == MMC_RESPONSE_TYPE_R2) {
    Buffer[0] = Host->Response[3];
    Buffer[1] = Host->Response[2];
    Buffer[2] = Host->Response[1];
    Buffer[3] = Host->Response[0];
  } else {
    Buffer[0] = Host->Response[0];
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MesonTransferData (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN EFI_LBA                Lba,
  IN UINTN                  Length,
  IN UINT32                 *Buffer,
  IN BOOLEAN                Write
  )
{
  MESON_MMC_HOST  *Host;
  BOOLEAN         SectorAddressed;
  EFI_LBA         CurrentLba;
  MMC_CMD         TransferCmd;
  UINT32          Argument;
  UINTN           ChunkBlocks;
  UINTN           ChunkLength;
  UINTN           Remaining;
  EFI_STATUS      Status;

  Host = MESON_MMC_FROM_PROTOCOL (This);
  if (!Host->PendingValid || (Buffer == NULL)) {
    return EFI_NOT_READY;
  }

  TransferCmd = Host->PendingCmd;
  Argument    = Host->PendingArgument;
  CurrentLba  = Lba;
  Remaining   = Length;

  //
  // EmbeddedPkg's generic MmcDxe can submit as many as 65535 blocks in one
  // host request, while this controller's internal descriptor has a 9-bit
  // length field and accepts at most 511.  Split the request here.  Each
  // intermediate CMD18/CMD25 is terminated before starting the next one; the
  // generic driver terminates the final multi-block command in its normal
  // completion path.
  //
  SectorAddressed = Host->AddressingKnown ?
                    Host->SectorAddressed :
                    (Host->PendingArgument == (UINT32)CurrentLba);
  Status = EFI_SUCCESS;
  while (Remaining > 0) {
    ChunkLength = MIN (Remaining, CMD_CFG_MAX_BLOCKS * 512U);
    ChunkBlocks = ChunkLength / 512U;

    Status = MesonExecuteCommand (
               Host,
               TransferCmd,
               Argument,
               Buffer,
               ChunkLength,
               Write
               );
    if (EFI_ERROR (Status)) {
      break;
    }

    Remaining -= ChunkLength;
    if (Remaining == 0) {
      break;
    }

    Status = MesonExecuteCommand (Host, MMC_CMD12, 0, NULL, 0, FALSE);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "MesonMMC@%lx: failed to stop chunk at LBA 0x%Lx: %r\n",
        Host->Base,
        CurrentLba,
        Status
        ));
      break;
    }

    //
    // A write chunk is not finished when CMD12 completes: the card then
    // PROGRAMS what it buffered, holding DAT0 low.  Launching the next
    // CMD25 data phase into a busy card corrupts it - hardware-observed as
    // an interleaving of written, stale, zero and garbage sectors.  Reads
    // have no programming phase, which is why only writes needed this and
    // why the bug hid until the first large firmware-side write (the
    // capsule update path).  Wait for DAT0 to go idle before continuing.
    //
    if (Write) {
      UINTN  BusyRetry;

      for (BusyRetry = 0; BusyRetry < 1000000; BusyRetry++) {
        if ((MmioRead32 (Host->Base + SD_EMMC_STATUS) & STATUS_DAT0_IDLE) != 0) {
          break;
        }

        gBS->Stall (1);
      }

      if (BusyRetry == 1000000) {
        DEBUG ((
          DEBUG_ERROR,
          "MesonMMC@%lx: card stuck busy after write chunk at LBA 0x%Lx\n",
          Host->Base,
          CurrentLba
          ));
        Status = EFI_TIMEOUT;
        break;
      }
    }

    Buffer     = (UINT32 *)((UINT8 *)Buffer + ChunkLength);
    CurrentLba += ChunkBlocks;
    Argument   += SectorAddressed ? (UINT32)ChunkBlocks :
                                    (UINT32)ChunkLength;
  }

  Host->PendingValid = FALSE;
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
MesonReadBlockData (
  IN  EFI_MMC_HOST_PROTOCOL  *This,
  IN  EFI_LBA                Lba,
  IN  UINTN                  Length,
  OUT UINT32                 *Buffer
  )
{
  return MesonTransferData (This, Lba, Length, Buffer, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
MesonWriteBlockData (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN EFI_LBA                Lba,
  IN UINTN                  Length,
  IN UINT32                 *Buffer
  )
{
  return MesonTransferData (This, Lba, Length, Buffer, TRUE);
}

STATIC
EFI_STATUS
EFIAPI
MesonSetIos (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN UINT32                 BusClockFreq,
  IN UINT32                 BusWidth,
  IN UINT32                 TimingMode
  )
{
  MESON_MMC_HOST  *Host;
  EFI_STATUS      Status;

  Host = MESON_MMC_FROM_PROTOCOL (This);

  //
  // The first port supports legacy and single-data-rate high-speed modes.
  // Decline DDR/HS200/HS400 so the generic eMMC driver falls back cleanly.
  //
  if (Host->IsEmmc &&
      (TimingMode != EMMCBACKWARD) &&
      (TimingMode != EMMCHS26) &&
      (TimingMode != EMMCHS52))
  {
    return EFI_UNSUPPORTED;
  }

  Status = MesonSetBusWidth (Host, BusWidth);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MesonSetClock (Host, BusClockFreq);
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EFIAPI
MesonIsMultiBlock (
  IN EFI_MMC_HOST_PROTOCOL  *This
  )
{
  return TRUE;
}

STATIC
VOID
MesonInitHost (
  OUT MESON_MMC_HOST  *Host,
  IN  UINTN           Base,
  IN  BOOLEAN         IsEmmc
  )
{
  ZeroMem (Host, sizeof (*Host));
  Host->Signature                = MESON_MMC_SIGNATURE;
  Host->Base                     = Base;
  Host->IsEmmc                   = IsEmmc;
  Host->Protocol.Revision        = MMC_HOST_PROTOCOL_REVISION;
  Host->Protocol.IsCardPresent   = MesonIsCardPresent;
  Host->Protocol.IsReadOnly      = MesonIsReadOnly;
  Host->Protocol.BuildDevicePath = MesonBuildDevicePath;
  Host->Protocol.NotifyState     = MesonNotifyState;
  Host->Protocol.SendCommand     = MesonSendCommand;
  Host->Protocol.ReceiveResponse = MesonReceiveResponse;
  Host->Protocol.ReadBlockData   = MesonReadBlockData;
  Host->Protocol.WriteBlockData  = MesonWriteBlockData;
  Host->Protocol.SetIos          = MesonSetIos;
  Host->Protocol.IsMultiBlock    = MesonIsMultiBlock;
}

STATIC
EFI_STATUS
MesonInstallHost (
  IN OUT MESON_MMC_HOST  *Host
  )
{
  EFI_STATUS  Status;

  Status = MesonHardwareInit (Host);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Host->Handle,
                  &gEmbeddedMmcHostProtocolGuid,
                  &Host->Protocol,
                  NULL
                  );
  if (!EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "MesonMMC: installed %a host at 0x%lx\n",
      Host->IsEmmc ? "eMMC" : "SD",
      Host->Base
      ));
  }

  return Status;
}

EFI_STATUS
EFIAPI
MesonSdMmcDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      BootDevice;

  DEBUG ((DEBUG_INFO, "MesonMMC: entry\n"));
  MesonInitBoardIo ();
  MesonInitHost (&mSdHost, MESON_G12B_SD_EMMC_B_BASE, FALSE);
  MesonInitHost (&mEmmcHost, MESON_G12B_SD_EMMC_C_BASE, TRUE);

  //
  // BL2 records the BootROM source in AO_SEC_GP_CFG0.  It selects only the
  // installation order below; safety no longer depends on it.
  //
  BootDevice = MmioRead32 (MESON_G12B_AO_SEC_GP_CFG0) & BOOT_DEVICE_MASK;
  DEBUG ((DEBUG_INFO, "MesonMMC: BootROM device=%u\n", BootDevice));

  //
  // MesonHardwareInit's write-first SD_EMMC_CLOCK ordering makes a
  // BootROM-unprobed controller safe to initialize (the historical
  // hard-stall was the internal core clock left stopped at reset; see
  // MesonHardwareInit).  All boot sources therefore initialize both
  // controllers; the boot controller still goes first so the medium the
  // platform booted from enumerates first.
  //
  if ((BootDevice == BOOT_DEVICE_EMMC) || (BootDevice == BOOT_DEVICE_USB)) {
    if (BootDevice == BOOT_DEVICE_USB) {
      //
      // Only the recovery path needs this: on an eMMC-origin boot BL2 has
      // already reset and initialized the card, and that path is proven.
      //
      MesonResetEmmcCard ();
    }

    Status = MesonInstallHost (&mEmmcHost);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MesonMMC: failed to install eMMC host: %r\n", Status));
      return Status;
    }

    MicroSecondDelay (1000);
    Status = MesonInstallHost (&mSdHost);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MesonMMC: failed to install SD host: %r\n", Status));
    }
  } else {
    if ((BootDevice != BOOT_DEVICE_SD) && (BootDevice != BOOT_DEVICE_SPI_NOR)) {
      DEBUG ((DEBUG_WARN, "MesonMMC: unknown boot device; using SD-first order\n"));
    }

    //
    // On these origins nobody has released the eMMC's hardware reset:
    // BL2 does it only when it boots from the card, and the USB branch
    // above covers recovery.  BOOT_12/nRST powers up low, and a card
    // held in reset times out on every command - measured on the first
    // SPI-NOR-origin boot as a dead bus (CMD13 unanswered at every RCA,
    // no GPT, capsule updates failing at the boot0 stage) while Linux,
    // which pulses the reset itself, saw a healthy device.
    //
    MesonResetEmmcCard ();

    Status = MesonInstallHost (&mSdHost);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MesonMMC: failed to install SD host: %r\n", Status));
      return Status;
    }

    MicroSecondDelay (1000);
    Status = MesonInstallHost (&mEmmcHost);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MesonMMC: failed to install eMMC host: %r\n", Status));
    }
  }

  return Status;
}
