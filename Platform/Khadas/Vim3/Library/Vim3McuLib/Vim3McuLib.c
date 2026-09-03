/** @file
  Khadas VIM3 onboard MCU access library.  See Vim3McuLib.h.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/Vim3AoI2cLib.h>
#include <Library/Vim3McuLib.h>

#include <Vim3Mcu.h>

STATIC BOOLEAN  mProbed;
STATIC BOOLEAN  mTrusted;

//
// The MCU does not auto-increment its register pointer, so every byte is a
// discrete register-index-then-read transaction.
//
EFI_STATUS
EFIAPI
Vim3McuReadByte (
  IN  UINT8  Register,
  OUT UINT8  *Value
  )
{
  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return Vim3AoI2cRead (MCU_I2C_ADDRESS, Register, Value, 1);
}

EFI_STATUS
EFIAPI
Vim3McuReadBlock (
  IN  UINT8  Register,
  OUT UINT8  *Buffer,
  IN  UINTN  Length
  )
{
  UINTN       Index;
  EFI_STATUS  Status;

  if ((Buffer == NULL) || (Length == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < Length; Index++) {
    Status = Vim3AoI2cRead (
               MCU_I2C_ADDRESS,
               (UINT8)(Register + Index),
               &Buffer[Index],
               1
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

//
// Only the configuration registers are writable through this library.  The
// power-off (0x80), password (0x00/0x40/0x81-0x83) and factory (0x16)
// registers are deliberately unreachable.
//
// BOOT_MODE (0x20) is the one exception, and it is deliberate: the SPI-NOR
// install has to point the BootROM at the copy it just wrote, or the NOR
// image sits there dormant while the board keeps booting eMMC.  The FMP
// writes it only after a NOR image has been read back and verified, and only
// when the operator opted in through the setup menu.  It is not a soft
// brick: a bad NOR falls through to the eMMC rescue by itself, and
// "i2cset -f -y 0 0x18 0x20 1" plus a power cycle undoes it.
//
STATIC
BOOLEAN
McuRegisterWritable (
  IN UINT8  Register
  )
{
  switch (Register) {
    case MCU_REG_BOOT_EN_WOL:
    case MCU_REG_BOOT_EN_RTC:
    case MCU_REG_BOOT_EN_EXP:
    case MCU_REG_BOOT_EN_IR:
    case MCU_REG_BOOT_EN_DCIN:
    case MCU_REG_BOOT_EN_KEY:
    case MCU_REG_KEY_MODE:
    case MCU_REG_LED_MODE_ON:
    case MCU_REG_LED_MODE_OFF:
    case MCU_REG_MCU_SLEEP:
    case MCU_REG_USB_PCIE:
    case MCU_REG_FAN_CONTROL:
    case MCU_REG_BOOT_MODE:
      return TRUE;
    default:
      break;
  }

  //
  // IR power-key code bytes, written one register at a time.
  //
  if ((Register >= MCU_REG_IR_CODE1) && (Register <= MCU_REG_IR_CODE1 + 3)) {
    return TRUE;
  }

  if ((Register >= MCU_REG_IR_CODE2) && (Register <= MCU_REG_IR_CODE2 + 3)) {
    return TRUE;
  }

  return FALSE;
}

EFI_STATUS
EFIAPI
Vim3McuWriteByte (
  IN UINT8  Register,
  IN UINT8  Value
  )
{
  //
  // Probe lazily if the caller has not.  mProbed/mTrusted are module-local:
  // this is a statically linked library, so every consumer gets its own copy
  // of that state.  A driver that never calls Vim3McuInit() itself would
  // otherwise have EVERY write refused with EFI_ACCESS_DENIED even though
  // another driver had already probed the same MCU successfully - which is
  // exactly what happened to Vim3FmpDxe's boot-mode flip, on hardware, after
  // a NOR image had already been written and verified.
  //
  // This does not weaken the guard: Vim3McuInit() is idempotent and still
  // refuses trust if the identity probe fails.
  //
  if (!mProbed) {
    Vim3McuInit ();
  }

  if (!mTrusted) {
    return EFI_ACCESS_DENIED;
  }

  if (!McuRegisterWritable (Register)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // No read-back here: the fan command (0x88) is write-only, and callers that
  // want verification (settings, mux) read the register back themselves.
  //
  return Vim3AoI2cWrite (MCU_I2C_ADDRESS, Register, &Value, sizeof (Value));
}

BOOLEAN
EFIAPI
Vim3McuWritesTrusted (
  VOID
  )
{
  return mTrusted;
}

EFI_STATUS
EFIAPI
Vim3McuInit (
  VOID
  )
{
  UINT8  Device[2];
  UINT8  Rtc;

  if (mProbed) {
    return mTrusted ? EFI_SUCCESS : EFI_DEVICE_ERROR;
  }

  mProbed  = TRUE;
  mTrusted = FALSE;
  Vim3AoI2cInitialize ();

  //
  // Identity probe.  DEVICE_NO is a two-byte register: this board's
  // production MCU returns 0x00 at 0x14 (high byte) and 0x03 = VIM3 at 0x15
  // (low byte) - the low byte is the real identity, read byte-at-a-time
  // because the MCU register pointer does not auto-increment.  The probe
  // previously used BOOT_MODE == 1 as an anti-stale discriminator because it
  // only read the zero high byte; that tied MCU trust to the eMMC-first
  // BootROM setting and would have refused all MCU writes on a SPI-primary
  // boot (BOOT_MODE = 0), where trust matters just as much.  BOOT_EN_RTC is
  // only range-checked since it is user configurable.
  //
  if (EFI_ERROR (Vim3McuReadBlock (MCU_REG_DEVICE_NO, Device, sizeof (Device))) ||
      EFI_ERROR (Vim3McuReadByte (MCU_REG_BOOT_EN_RTC, &Rtc)))
  {
    DEBUG ((DEBUG_ERROR, "Vim3McuLib: identity read failed; MCU writes refused\n"));
    return EFI_DEVICE_ERROR;
  }

  if ((Device[0] != 0) ||
      (Device[1] != MCU_DEVICE_VIM3) ||
      (Rtc > 2))
  {
    DEBUG ((
      DEBUG_ERROR,
      "Vim3McuLib: identity mismatch dev=%02x%02x rtc=%02x; MCU writes refused\n",
      Device[0],
      Device[1],
      Rtc
      ));
    return EFI_DEVICE_ERROR;
  }

  mTrusted = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Vim3McuGetVersion (
  OUT UINT16  *Version
  )
{
  UINT8       Bytes[2];
  EFI_STATUS  Status;

  if (Version == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = Vim3McuReadBlock (MCU_REG_VERSION, Bytes, sizeof (Bytes));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Version = (UINT16)((Bytes[0] << 8) | Bytes[1]);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Vim3McuGetShutdownStatus (
  OUT UINT8  *Status
  )
{
  if (Status == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return Vim3McuReadByte (MCU_REG_SHUTDOWN_STATUS, Status);
}
