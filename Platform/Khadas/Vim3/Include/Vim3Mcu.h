/** @file
  Khadas VIM3 onboard MCU register map (AO-I2C, slave 0x18).

  Single source of the MCU register numbers, shared by Vim3McuLib and its
  consumers.  The published VIM3 MCU register map is Khadas' vim3-mcu-reg-en.pdf.

  Note on this board's production MCU firmware: the read-only identity/inventory
  registers (DEVICE_NO 0x14, MAC 0x06, USID 0x0c) return zero - the MAC and a
  unique board serial come from the Amlogic SoC eFUSE / secure monitor, not the
  MCU.  The MCU is authoritative for power/wake/LED/fan/mux configuration and
  the last-shutdown status.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef VIM3_MCU_H_
#define VIM3_MCU_H_

#define MCU_I2C_ADDRESS  0x18U

//
// Read-only identity / inventory (zero on this board's production MCU).
//
#define MCU_REG_MAC        0x06U   // 6 bytes
#define MCU_REG_USID       0x0CU   // 6 bytes
#define MCU_REG_VERSION    0x12U   // 2 bytes
#define MCU_REG_DEVICE_NO  0x14U   // 2 bytes; VIM3 == 0x0003 (reads 0000 here)

//
// Boot source + power-on / wake source enables.
//
#define MCU_REG_BOOT_MODE     0x20U   // BootROM probe order.  Writable, but
                                      // ONLY from the SPI-NOR install path
                                      // after a verified NOR image - see
                                      // Vim3McuLib's whitelist note.

#define VIM3_MCU_BOOT_MODE_SPI_FIRST  0U
#define VIM3_MCU_BOOT_MODE_EMMC       1U

#define MCU_REG_BOOT_EN_WOL   0x21U
#define MCU_REG_BOOT_EN_RTC   0x22U
#define MCU_REG_BOOT_EN_EXP   0x23U
#define MCU_REG_BOOT_EN_IR    0x24U
#define MCU_REG_BOOT_EN_DCIN  0x25U
#define MCU_REG_BOOT_EN_KEY   0x26U

//
// Behaviour / indicators / IR.
//
#define MCU_REG_KEY_MODE      0x27U
#define MCU_REG_LED_MODE_ON   0x28U
#define MCU_REG_LED_MODE_OFF  0x29U
#define MCU_REG_MAC_SWITCH    0x2DU   // 0 = MAC from OTP/eFUSE, 1 = from MCU
#define MCU_REG_MCU_SLEEP     0x2EU
#define MCU_REG_IR_CODE1      0x2FU   // 4 bytes
#define MCU_REG_USB_PCIE      0x33U   // 0 = USB3, 1 = PCIe
#define MCU_REG_IR_CODE2      0x34U   // 4 bytes

//
// Command / status.  Write registers >= 0x80 are power-off/password/command
// and are refused by Vim3McuLib's write whitelist.
//
#define MCU_REG_SHUTDOWN_STATUS  0x86U   // 0 = last power-off normal, else abort
#define MCU_REG_FAN_CONTROL      0x88U   // 0 = off, 1..3 = fan level

#define MCU_DEVICE_VIM3  0x03U
#define MCU_MUX_USB3     0U
#define MCU_MUX_PCIE     1U

#endif // VIM3_MCU_H_
