/** @file
  Khadas VIM3 setup variable definitions.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef VIM3_PLATFORM_CONFIG_H_
#define VIM3_PLATFORM_CONFIG_H_

#define VIM3_PLATFORM_FORMSET_GUID \
  { 0xc6025aae, 0xfa97, 0x46e7, { 0xb4, 0x16, 0xb5, 0x0b, 0xee, 0x7e, 0x04, 0xec } }

#define VIM3_FAN_POLICY_VARIABLE_NAME  L"FanPolicy"

#define VIM3_FAN_DISABLED  0
#define VIM3_FAN_LEVEL_1   1
#define VIM3_FAN_LEVEL_2   2
#define VIM3_FAN_LEVEL_3   3

#define VIM3_HW_DESCRIPTION_VARIABLE_NAME  L"HwDescription"

#define VIM3_HW_DESCRIPTION_DEVICE_TREE  0
#define VIM3_HW_DESCRIPTION_ACPI         1

//
// USB3 <-> M.2 PCIe mux.  The MCU (register 0x33) is the persistent source of
// truth; this variable only mirrors it so the setup browser can present a
// oneof that shows the current mode.  The driver syncs it from the MCU at boot
// and writes the MCU on change.  Values match the MCU encoding.
//
#define VIM3_USB3_PCIE_VARIABLE_NAME  L"Usb3PcieMode"

#define VIM3_USB3_PCIE_USB3   0
#define VIM3_USB3_PCIE_M2     1

#define VIM3_QUESTION_USB_PCIE_MUX  0x2003

//
// SPI-NOR firmware install opt-in.
//
// Capsule updates rewrite the NOR primary only when the running firmware came
// from it; on an eMMC boot the NOR belongs to someone else (oowow, on every
// beta board) so it is deliberately left alone.  Setting this variable is the
// operator saying "the NOR is mine now": the next capsule update writes it as
// well as the eMMC rescue, then flips the MCU to boot SPI-first.
//
// One-shot by construction - the FMP deletes it after a verified write, so a
// later capsule cannot silently reflash the NOR.  A failed write keeps it, so
// a retry needs no second visit to the setup menu.
//
#define VIM3_SPI_NOR_INSTALL_VARIABLE_NAME  L"SpiNorInstall"

// 0 = off.  1 = piggyback on the next capsule update (the payload is the
// capsule).  2 = do it NOW from the eMMC boot partition on the next boot, no
// capsule involved - BDS asks Vim3FmpDxe to copy the firmware the board is
// already running and then resets.
#define VIM3_SPI_NOR_INSTALL_DISABLED  0
#define VIM3_SPI_NOR_INSTALL_ENABLED   1
#define VIM3_SPI_NOR_INSTALL_NOW       2

//
// MCU wake / power-on sources, LED behaviour, key mode, IR power-key codes.
// Each field mirrors a persistent single-byte MCU register (0x21-0x2e) except
// the two 4-byte IR codes (0x2f / 0x34).  The MCU is the source of truth: the
// driver mirrors it into this variable at boot and writes the MCU back on an
// interactive change.  Values match the MCU encoding.
//
#define VIM3_MCU_SETTINGS_VARIABLE_NAME  L"McuSettings"

// Wake / power-on source enables (WOL and EXP also accept 2 = reset).
#define VIM3_WAKE_DISABLED  0
#define VIM3_WAKE_ENABLED   1
#define VIM3_WAKE_RESET     2

// Power-key press mode (register 0x27).
#define VIM3_KEY_SHORT_PRESS  0
#define VIM3_KEY_LONG_PRESS   1

// LED behaviour (registers 0x28 on / 0x29 off).
#define VIM3_LED_OFF        0
#define VIM3_LED_ON         1
#define VIM3_LED_BREATH     2
#define VIM3_LED_HEARTBEAT  3

// MCU sleep after power-off (register 0x2e).
#define VIM3_MCU_SLEEP_DISABLED  0
#define VIM3_MCU_SLEEP_ENABLED   1

#define VIM3_QUESTION_WAKE_WOL   0x2100
#define VIM3_QUESTION_WAKE_RTC   0x2101
#define VIM3_QUESTION_WAKE_EXP   0x2102
#define VIM3_QUESTION_WAKE_IR    0x2103
#define VIM3_QUESTION_WAKE_DCIN  0x2104
#define VIM3_QUESTION_WAKE_KEY   0x2105
#define VIM3_QUESTION_KEY_MODE   0x2106
#define VIM3_QUESTION_LED_ON     0x2107
#define VIM3_QUESTION_LED_OFF    0x2108
#define VIM3_QUESTION_MCU_SLEEP  0x2109
#define VIM3_QUESTION_IR_CODE1   0x210A
#define VIM3_QUESTION_IR_CODE2   0x210B

#pragma pack (1)
typedef struct {
  UINT8    Level;
} VIM3_FAN_POLICY_VARSTORE_DATA;

typedef struct {
  UINT8    Mode;
} VIM3_HW_DESCRIPTION_VARSTORE_DATA;

typedef struct {
  UINT8    Mode;
} VIM3_USB3_PCIE_VARSTORE_DATA;

typedef struct {
  UINT8    Enable;
} VIM3_SPI_NOR_INSTALL_VARSTORE_DATA;

typedef struct {
  UINT8     WolMode;    // 0x21 BOOT_EN_WOL
  UINT8     RtcWake;    // 0x22 BOOT_EN_RTC
  UINT8     ExpWake;    // 0x23 BOOT_EN_EXP
  UINT8     IrWake;     // 0x24 BOOT_EN_IR
  UINT8     DcinWake;   // 0x25 BOOT_EN_DCIN
  UINT8     KeyWake;    // 0x26 BOOT_EN_KEY
  UINT8     KeyMode;    // 0x27 KEY_MODE
  UINT8     LedOn;      // 0x28 LED_MODE_ON
  UINT8     LedOff;     // 0x29 LED_MODE_OFF
  UINT8     McuSleep;   // 0x2e MCU_SLEEP_MODE
  UINT32    IrCode1;    // 0x2f IR_CODE1 (4 bytes)
  UINT32    IrCode2;    // 0x34 IR_CODE2 (4 bytes)
} VIM3_MCU_SETTINGS_VARSTORE_DATA;
#pragma pack ()

#endif
