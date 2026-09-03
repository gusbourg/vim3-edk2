/** @file
  Polling SerialPortLib for the Amlogic Meson GX UART.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/SerialPortLib.h>

#define UART_WFIFO       0x00
#define UART_RFIFO       0x04
#define UART_CONTROL     0x08
#define UART_STATUS      0x0C
#define UART_REG5        0x14

#define UART_TX_ENABLE   BIT12
#define UART_RX_ENABLE   BIT13
#define UART_TWO_WIRE    BIT15
#define UART_TX_RESET    BIT22
#define UART_RX_RESET    BIT23
#define UART_CLEAR_ERROR BIT24

#define UART_PARITY_ERROR BIT16
#define UART_FRAME_ERROR  BIT17
#define UART_TX_FIFO_ERROR BIT18
#define UART_RX_EMPTY      BIT20
#define UART_TX_FULL       BIT21
#define UART_TX_EMPTY      BIT22
#define UART_ERROR_MASK    \
  (UART_PARITY_ERROR | UART_FRAME_ERROR | UART_TX_FIFO_ERROR)

#define UART_USE_NEW_BAUD BIT23
#define UART_USE_XTAL     BIT24
#define UART_BAUD_MASK    0x7FFFFF
#define UART_XTAL_HZ      24000000U

STATIC
UINTN
UartBase (
  VOID
  )
{
  return (UINTN)PcdGet64 (PcdSerialRegisterBase);
}

STATIC
VOID
ClearReceiveError (
  VOID
  )
{
  UINT32  Control;

  Control = MmioRead32 (UartBase () + UART_CONTROL);
  MmioWrite32 (UartBase () + UART_CONTROL, Control | UART_CLEAR_ERROR);
  MmioWrite32 (UartBase () + UART_CONTROL, Control & ~UART_CLEAR_ERROR);
  (VOID)MmioRead32 (UartBase () + UART_RFIFO);
}

STATIC
RETURN_STATUS
SetBaudRate (
  IN UINT64  BaudRate
  )
{
  UINT64  Divisor;

  if ((BaudRate == 0) || (BaudRate > 8000000)) {
    return RETURN_INVALID_PARAMETER;
  }

  Divisor = DivU64x64Remainder (
              (UART_XTAL_HZ / 3) + (BaudRate / 2),
              BaudRate,
              NULL
              );
  if ((Divisor == 0) || (Divisor > (UART_BAUD_MASK + 1ULL))) {
    return RETURN_INVALID_PARAMETER;
  }

  MmioWrite32 (
    UartBase () + UART_REG5,
    UART_USE_XTAL | UART_USE_NEW_BAUD | ((UINT32)(Divisor - 1) & UART_BAUD_MASK)
    );
  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
  UINT32         Control;
  RETURN_STATUS  Status;

  Status = SetBaudRate (PcdGet64 (PcdUartDefaultBaudRate));
  if (RETURN_ERROR (Status)) {
    return Status;
  }

  Control  = MmioRead32 (UartBase () + UART_CONTROL);
  Control |= UART_TX_RESET | UART_RX_RESET | UART_CLEAR_ERROR;
  MmioWrite32 (UartBase () + UART_CONTROL, Control);
  Control &= ~(UART_TX_RESET | UART_RX_RESET | UART_CLEAR_ERROR);
  Control |= UART_TX_ENABLE | UART_RX_ENABLE | UART_TWO_WIRE;
  MmioWrite32 (UartBase () + UART_CONTROL, Control);
  return RETURN_SUCCESS;
}

UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  UINTN  Index;

  if (Buffer == NULL) {
    return 0;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    while ((MmioRead32 (UartBase () + UART_STATUS) & UART_TX_FULL) != 0) {
      CpuPause ();
    }

    MmioWrite32 (UartBase () + UART_WFIFO, Buffer[Index]);
  }

  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN  NumberOfBytes
  )
{
  UINTN   Index;
  UINT32  Status;

  if (Buffer == NULL) {
    return 0;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    do {
      Status = MmioRead32 (UartBase () + UART_STATUS);
      if ((Status & UART_ERROR_MASK) != 0) {
        ClearReceiveError ();
      }
    } while ((Status & UART_RX_EMPTY) != 0);

    Buffer[Index] = (UINT8)MmioRead32 (UartBase () + UART_RFIFO);
  }

  return NumberOfBytes;
}

BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  UINT32  Status;

  Status = MmioRead32 (UartBase () + UART_STATUS);
  if ((Status & UART_ERROR_MASK) != 0) {
    ClearReceiveError ();
    return FALSE;
  }

  return (Status & UART_RX_EMPTY) == 0;
}

RETURN_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64              *BaudRate,
  IN OUT UINT32              *ReceiveFifoDepth,
  IN OUT UINT32              *Timeout,
  IN OUT EFI_PARITY_TYPE     *Parity,
  IN OUT UINT8               *DataBits,
  IN OUT EFI_STOP_BITS_TYPE  *StopBits
  )
{
  if ((BaudRate == NULL) || (ReceiveFifoDepth == NULL) ||
      (Timeout == NULL) || (Parity == NULL) || (DataBits == NULL) ||
      (StopBits == NULL))
  {
    return RETURN_INVALID_PARAMETER;
  }

  if (((*Parity != DefaultParity) && (*Parity != NoParity)) ||
      ((*DataBits != 0) && (*DataBits != 8)) ||
      ((*StopBits != DefaultStopBits) && (*StopBits != OneStopBit)))
  {
    return RETURN_UNSUPPORTED;
  }

  *Parity          = NoParity;
  *DataBits        = 8;
  *StopBits        = OneStopBit;
  *ReceiveFifoDepth = 64;
  return SetBaudRate (*BaudRate);
}

RETURN_STATUS
EFIAPI
SerialPortSetControl (
  IN UINT32  Control
  )
{
  return (Control == 0) ? RETURN_SUCCESS : RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl (
  OUT UINT32  *Control
  )
{
  UINT32  Status;

  if (Control == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  Status   = MmioRead32 (UartBase () + UART_STATUS);
  *Control = 0;
  if ((Status & UART_RX_EMPTY) != 0) {
    *Control |= EFI_SERIAL_INPUT_BUFFER_EMPTY;
  }

  if ((Status & UART_TX_EMPTY) != 0) {
    *Control |= EFI_SERIAL_OUTPUT_BUFFER_EMPTY;
  }

  return RETURN_SUCCESS;
}
