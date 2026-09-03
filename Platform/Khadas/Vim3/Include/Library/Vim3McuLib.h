/** @file
  Khadas VIM3 onboard MCU access library.

  Centralises MCU register access over Vim3AoI2cLib: the no-auto-increment
  block read, the write whitelist (only 0x21-0x2e config, 0x2f-0x37 IR, 0x33
  mux, 0x88 fan - never 0x80 power-off or the password/command registers), and
  the identity/trust probe that gates writes.  Register numbers live in
  <Vim3Mcu.h>.

  All access is boot-time / DXE (single-threaded dispatch, no interrupt-driven
  I2C); do not call at runtime (the RTC library owns the bus after
  ExitBootServices).

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef VIM3_MCU_LIB_H_
#define VIM3_MCU_LIB_H_

#include <Uefi/UefiBaseType.h>

/**
  Initialise the AO-I2C transport and run the one-time MCU identity probe.
  Idempotent.

  @retval EFI_SUCCESS  The MCU responded and its signature is trusted.
  @retval other        The probe failed; writes will be refused.
**/
EFI_STATUS
EFIAPI
Vim3McuInit (
  VOID
  );

/**
  @retval TRUE   The MCU passed the identity probe; writes are permitted.
  @retval FALSE  The probe failed or has not run; writes are refused.
**/
BOOLEAN
EFIAPI
Vim3McuWritesTrusted (
  VOID
  );

/**
  Read a single MCU register.
**/
EFI_STATUS
EFIAPI
Vim3McuReadByte (
  IN  UINT8  Register,
  OUT UINT8  *Value
  );

/**
  Read Length consecutive registers with one single-byte transaction each
  (the MCU does not auto-increment its register pointer).
**/
EFI_STATUS
EFIAPI
Vim3McuReadBlock (
  IN  UINT8  Register,
  OUT UINT8  *Buffer,
  IN  UINTN  Length
  );

/**
  Write a single MCU register, then read it back to confirm.  Refuses any
  register outside the configuration whitelist and refuses all writes unless
  the identity probe passed.

  @retval EFI_ACCESS_DENIED    The MCU is not trusted.
  @retval EFI_INVALID_PARAMETER  Register is not in the whitelist.
**/
EFI_STATUS
EFIAPI
Vim3McuWriteByte (
  IN UINT8  Register,
  IN UINT8  Value
  );

/**
  Read the 2-byte MCU firmware version (register 0x12).
**/
EFI_STATUS
EFIAPI
Vim3McuGetVersion (
  OUT UINT16  *Version
  );

/**
  Read the last-power-off status (register 0x86): 0 = normal, else aborted.
**/
EFI_STATUS
EFIAPI
Vim3McuGetShutdownStatus (
  OUT UINT8  *Status
  );

#endif // VIM3_MCU_LIB_H_
