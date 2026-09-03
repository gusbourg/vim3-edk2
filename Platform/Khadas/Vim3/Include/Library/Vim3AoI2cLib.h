/** @file
  Transport primitives for the Khadas VIM3 always-on I2C bus.

  The AO I2C controller at 0xFF805000 carries at least three devices the
  firmware cares about: the board MCU at 0x18, the GPIO expander at 0x20,
  and the HYM8563 RTC at 0x51.  Both the MCU driver and the RTC library
  drive that bus, so the transport lives here instead of being duplicated:
  a private copy in each consumer would leave the shared I2C_SLAVE_ADDR
  register holding whichever address ran last, and the next transfer would
  silently address the wrong chip.  Every transaction here programs the
  slave address it is about to use.

  SECURITY NOTE.  The MCU register map places destructive commands next to
  benign ones (0x80 powers the board off, one register away from the fan
  control at 0x88).  This library is deliberately transport-only and has no
  opinion about registers; the obligation to keep MCU writes structurally
  fixed-register, with no caller-supplied register or length, stays with
  Vim3McuDxe at its call sites.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef VIM3_AO_I2C_LIB_H_
#define VIM3_AO_I2C_LIB_H_

#include <Uefi/UefiBaseType.h>

///
/// Longest payload a single transaction can carry.  The controller exposes
/// two 32-bit data windows, so eight bytes move per transfer; a register
/// write spends one of them on the register index.
///
#define VIM3_AO_I2C_MAX_READ   8
#define VIM3_AO_I2C_MAX_WRITE  7

/**
  Prepare the AO I2C bus for use.

  Ungates the controller clock and claims GPIOAO_2/3 for I2C.  Idempotent:
  every consumer calls it before its first transfer, in whatever order the
  DXE dispatcher happens to start them.
**/
VOID
EFIAPI
Vim3AoI2cInitialize (
  VOID
  );

/**
  Read consecutive registers from a device on the AO I2C bus.

  Issues the usual register-index write followed by a repeated-START read.

  @param[in]  SlaveAddress  7-bit device address.
  @param[in]  Register      First register index to read.
  @param[out] Buffer        Receives Length bytes.
  @param[in]  Length        1 to VIM3_AO_I2C_MAX_READ bytes.

  @retval EFI_SUCCESS            Buffer holds the register contents.
  @retval EFI_INVALID_PARAMETER  Buffer is NULL or Length is out of range.
  @retval EFI_DEVICE_ERROR       The device did not acknowledge.
  @retval EFI_TIMEOUT            The controller never went idle.
**/
EFI_STATUS
EFIAPI
Vim3AoI2cRead (
  IN  UINT8  SlaveAddress,
  IN  UINT8  Register,
  OUT UINT8  *Buffer,
  IN  UINTN  Length
  );

/**
  Write consecutive registers to a device on the AO I2C bus.

  @param[in] SlaveAddress  7-bit device address.
  @param[in] Register      First register index to write.
  @param[in] Buffer        Length bytes to send.
  @param[in] Length        1 to VIM3_AO_I2C_MAX_WRITE bytes.

  @retval EFI_SUCCESS            The device acknowledged the transfer.
  @retval EFI_INVALID_PARAMETER  Buffer is NULL or Length is out of range.
  @retval EFI_DEVICE_ERROR       The device did not acknowledge.
  @retval EFI_TIMEOUT            The controller never went idle.
**/
EFI_STATUS
EFIAPI
Vim3AoI2cWrite (
  IN UINT8        SlaveAddress,
  IN UINT8        Register,
  IN CONST UINT8  *Buffer,
  IN UINTN        Length
  );

#endif // VIM3_AO_I2C_LIB_H_
