/** @file
  Transport primitives for the Khadas VIM3 always-on I2C bus.

  Extracted verbatim in behavior from Vim3McuDxe, which proved this
  sequence on hardware; see Vim3AoI2cLib.h for why it is shared.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/Vim3AoI2cLib.h>

#define VIM3_AO_I2C_BASE    0xFF805000ULL
#define VIM3_AO_PINMUX_REG  0xFF800014ULL
#define VIM3_HHI_GCLK_MPEG0 0xFF63C140ULL

#define I2C_CTRL          (VIM3_AO_I2C_BASE + 0x00)
#define I2C_SLAVE_ADDR    (VIM3_AO_I2C_BASE + 0x04)
#define I2C_TOKEN_LIST0   (VIM3_AO_I2C_BASE + 0x08)
#define I2C_TOKEN_LIST1   (VIM3_AO_I2C_BASE + 0x0C)
#define I2C_TOKEN_WDATA0  (VIM3_AO_I2C_BASE + 0x10)
#define I2C_TOKEN_WDATA1  (VIM3_AO_I2C_BASE + 0x14)
#define I2C_TOKEN_RDATA0  (VIM3_AO_I2C_BASE + 0x18)
#define I2C_TOKEN_RDATA1  (VIM3_AO_I2C_BASE + 0x1C)

#define I2C_CTRL_START   BIT0
#define I2C_CTRL_STATUS  BIT2
#define I2C_CTRL_ERROR   BIT3

///
/// SCL divider, in quarter-bit units of the controller clock (CLK81): ten
/// bits at [21:12] plus a two-bit extension at [29:28], giving
/// SCL = CLK81 / (4 * divider).  Same encoding U-Boot's meson_i2c uses.
///
#define I2C_CTRL_CLKDIV_SHIFT     12
#define I2C_CTRL_CLKDIV_MASK      (0x3FFU << I2C_CTRL_CLKDIV_SHIFT)
#define I2C_CTRL_CLKDIVEXT_SHIFT  28
#define I2C_CTRL_CLKDIVEXT_MASK   (0x3U << I2C_CTRL_CLKDIVEXT_SHIFT)

#define VIM3_CLK81_HZ       166666667U
#define VIM3_AO_I2C_SCL_HZ  100000U

#define I2C_TOKEN_START       1U
#define I2C_TOKEN_ADDR_WRITE  2U
#define I2C_TOKEN_ADDR_READ   3U
#define I2C_TOKEN_DATA        4U
#define I2C_TOKEN_DATA_LAST   5U
#define I2C_TOKEN_STOP        6U

///
/// Sixteen four-bit token slots span the two list registers.
///
#define TOKENS_PER_LIST  8

/**
  Place a token in a 16-slot token list split across two registers.
**/
STATIC
VOID
TokenSet (
  IN OUT UINT32  *List0,
  IN OUT UINT32  *List1,
  IN     UINTN   Index,
  IN     UINT32  Token
  )
{
  ASSERT (Index < (2 * TOKENS_PER_LIST));

  if (Index < TOKENS_PER_LIST) {
    *List0 |= Token << (Index * 4);
  } else {
    *List1 |= Token << ((Index - TOKENS_PER_LIST) * 4);
  }
}

STATIC
EFI_STATUS
I2cWait (
  VOID
  )
{
  UINTN   Timeout;
  UINT32  Control;

  for (Timeout = 0; Timeout < 100000; Timeout++) {
    Control = MmioRead32 (I2C_CTRL);
    if ((Control & I2C_CTRL_STATUS) == 0) {
      MmioAnd32 (I2C_CTRL, ~I2C_CTRL_START);
      if ((Control & I2C_CTRL_ERROR) != 0) {
        return EFI_DEVICE_ERROR;
      }

      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  MmioAnd32 (I2C_CTRL, ~I2C_CTRL_START);
  return EFI_TIMEOUT;
}

/**
  Run one token list against the currently selected slave.
**/
STATIC
EFI_STATUS
I2cRun (
  IN  UINT8   SlaveAddress,
  IN  UINT32  Tokens0,
  IN  UINT32  Tokens1,
  IN  UINT32  WriteData0,
  IN  UINT32  WriteData1
  )
{
  EFI_STATUS  Status;
  UINT32      Control;

  //
  // A START left set by an interrupted predecessor prevents the controller
  // from recognizing the next 0->1 launch edge.  Clear it before replacing
  // the token and data windows.
  //
  MmioAnd32 (I2C_CTRL, ~I2C_CTRL_START);

  //
  // The slave address register is shared by every consumer of this bus, so
  // it is programmed per transaction rather than once at initialization.
  //
  MmioWrite32 (I2C_SLAVE_ADDR, (UINT32)SlaveAddress << 1);
  MmioWrite32 (I2C_TOKEN_LIST0, Tokens0);
  MmioWrite32 (I2C_TOKEN_LIST1, Tokens1);
  MmioWrite32 (I2C_TOKEN_WDATA0, WriteData0);
  MmioWrite32 (I2C_TOKEN_WDATA1, WriteData1);
  MmioOr32 (I2C_CTRL, I2C_CTRL_START);

  //
  // STATUS is not asserted synchronously with START.  Without this delay an
  // immediate read can see the old idle state, clear START, and report a
  // false success without putting anything on the wire.
  //
  MicroSecondDelay (5);
  Status  = I2cWait ();
  Control = MmioRead32 (I2C_CTRL);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Vim3AoI2c: transfer to 0x%02x failed: %r ctrl=0x%08x tokens=0x%08x/0x%08x\n",
      SlaveAddress,
      Status,
      Control,
      Tokens0,
      Tokens1
      ));
  }

  return Status;
}

VOID
EFIAPI
Vim3AoI2cInitialize (
  VOID
  )
{
  UINT32  Control;
  UINT32  Divider;

  //
  // AO I2C uses GPIOAO_2/3, function 1.  The two four-bit fields are in the
  // first AO mux register; UART AO on GPIOAO_0/1 is left untouched.
  //
  MmioOr32 (VIM3_HHI_GCLK_MPEG0, BIT9);
  MmioAndThenOr32 (VIM3_AO_PINMUX_REG, ~0x0000FF00U, 0x00001100U);

  //
  // Program a deterministic 100 kHz standard-mode divider instead of
  // inheriting whatever the boot chain left in the register.  The inherited
  // value was proven on one bench board only, and a beta board turned up
  // with the MCU never ACKing in firmware while Linux (which programs its
  // own divider) talked to it fine - board- or boot-path-dependent leftover
  // state is the prime suspect.  CLK81 is fixed at 166.6 MHz by BL2 on
  // every boot (it is in the BL2 banner), so the derivation is sound, and
  // 100 kHz matches what Linux uses on this bus on every VIM3 in the field.
  //
  Divider = (VIM3_CLK81_HZ + (4U * VIM3_AO_I2C_SCL_HZ) - 1U) /
            (4U * VIM3_AO_I2C_SCL_HZ);

  Control  = MmioRead32 (I2C_CTRL);
  Control &= ~(I2C_CTRL_START | I2C_CTRL_CLKDIV_MASK | I2C_CTRL_CLKDIVEXT_MASK);
  Control |= (Divider & 0x3FFU) << I2C_CTRL_CLKDIV_SHIFT;
  Control |= ((Divider >> 10U) & 0x3U) << I2C_CTRL_CLKDIVEXT_SHIFT;
  MmioWrite32 (I2C_CTRL, Control);
  DEBUG ((
    DEBUG_INFO,
    "Vim3AoI2c: ctrl=0x%08x pinmux=0x%08x gate=0x%08x\n",
    MmioRead32 (I2C_CTRL),
    MmioRead32 (VIM3_AO_PINMUX_REG),
    MmioRead32 (VIM3_HHI_GCLK_MPEG0)
    ));
}

EFI_STATUS
EFIAPI
Vim3AoI2cRead (
  IN  UINT8  SlaveAddress,
  IN  UINT8  Register,
  OUT UINT8  *Buffer,
  IN  UINTN  Length
  )
{
  EFI_STATUS  Status;
  UINT32      Tokens0;
  UINT32      Tokens1;
  UINT32      Data[2];
  UINTN       Index;

  if ((Buffer == NULL) || (Length == 0) || (Length > VIM3_AO_I2C_MAX_READ)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Point the device at the register, without a STOP: the read below opens
  // with a repeated START.
  //
  Tokens0 = 0;
  Tokens1 = 0;
  TokenSet (&Tokens0, &Tokens1, 0, I2C_TOKEN_START);
  TokenSet (&Tokens0, &Tokens1, 1, I2C_TOKEN_ADDR_WRITE);
  TokenSet (&Tokens0, &Tokens1, 2, I2C_TOKEN_DATA);

  Status = I2cRun (SlaveAddress, Tokens0, Tokens1, Register, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Tokens0 = 0;
  Tokens1 = 0;
  TokenSet (&Tokens0, &Tokens1, 0, I2C_TOKEN_START);
  TokenSet (&Tokens0, &Tokens1, 1, I2C_TOKEN_ADDR_READ);
  for (Index = 0; Index < Length; Index++) {
    TokenSet (
      &Tokens0,
      &Tokens1,
      2 + Index,
      (Index == (Length - 1)) ? I2C_TOKEN_DATA_LAST : I2C_TOKEN_DATA
      );
  }

  TokenSet (&Tokens0, &Tokens1, 2 + Length, I2C_TOKEN_STOP);

  Status = I2cRun (SlaveAddress, Tokens0, Tokens1, 0, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Data[0] = MmioRead32 (I2C_TOKEN_RDATA0);
  Data[1] = MmioRead32 (I2C_TOKEN_RDATA1);
  for (Index = 0; Index < Length; Index++) {
    Buffer[Index] = (UINT8)(Data[Index / 4] >> ((Index % 4) * 8));
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Vim3AoI2cWrite (
  IN UINT8        SlaveAddress,
  IN UINT8        Register,
  IN CONST UINT8  *Buffer,
  IN UINTN        Length
  )
{
  UINT32  Tokens0;
  UINT32  Tokens1;
  UINT32  Data[2];
  UINTN   Index;

  if ((Buffer == NULL) || (Length == 0) || (Length > VIM3_AO_I2C_MAX_WRITE)) {
    return EFI_INVALID_PARAMETER;
  }

  Tokens0 = 0;
  Tokens1 = 0;
  Data[0] = Register;
  Data[1] = 0;

  TokenSet (&Tokens0, &Tokens1, 0, I2C_TOKEN_START);
  TokenSet (&Tokens0, &Tokens1, 1, I2C_TOKEN_ADDR_WRITE);
  TokenSet (&Tokens0, &Tokens1, 2, I2C_TOKEN_DATA);
  for (Index = 0; Index < Length; Index++) {
    //
    // Byte 0 of the write window is the register index, so payload byte N
    // lands at window offset N + 1.
    //
    Data[(Index + 1) / 4] |= (UINT32)Buffer[Index] << (((Index + 1) % 4) * 8);
    TokenSet (&Tokens0, &Tokens1, 3 + Index, I2C_TOKEN_DATA);
  }

  TokenSet (&Tokens0, &Tokens1, 3 + Length, I2C_TOKEN_STOP);

  return I2cRun (SlaveAddress, Tokens0, Tokens1, Data[0], Data[1]);
}
