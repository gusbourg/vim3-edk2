/** @file
  Amlogic Meson G12B secure-monitor access.  See Vim3MesonSmLib.h.

  SMC IDs are the Amlogic "gxbb-sm" table, which G12 reuses verbatim (the G12
  DT binds compatible = "amlogic,meson-gxbb-sm").  Verified against mainline
  Linux drivers/firmware/meson/meson_sm.c and U-Boot drivers/sm/meson-sm.c.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/Vim3MesonSmLib.h>

#include <MesonG12B.h>

#define AML_SMC_GET_SHMEM_OUTPUT_BASE  0x82000021U
#define AML_SMC_EFUSE_READ             0x82000030U
#define AML_SMC_GET_CHIP_ID            0x82000044U

//
// GET_CHIP_ID output blob: u32 version at 0, then the 12-byte serial at 4.
//
#define CHIP_ID_SERIAL_OFFSET  4
#define CHIP_ID_SERIAL_SIZE    12

#define VIM3_EFUSE_MAC_ASCII_SIZE  12

STATIC BOOLEAN  mShmemProbed;
STATIC UINTN    mOutShmem;

//
// Fetch (once) the BL31 output shared-memory physical address and confirm it
// is inside the secure-reserved region the EDK2 memory map covers.  A value
// outside that window would fault when read, so treat it as "no shared mem".
//
STATIC
UINTN
MesonSmOutputShmem (
  VOID
  )
{
  ARM_SMC_ARGS  Args;
  UINT64        Base;

  if (mShmemProbed) {
    return mOutShmem;
  }

  mShmemProbed = TRUE;
  mOutShmem    = 0;

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = AML_SMC_GET_SHMEM_OUTPUT_BASE;
  ArmCallSmc (&Args);
  Base = Args.Arg0;

  if ((Base >= MESON_G12B_SECURE_BASE) &&
      (Base < MESON_G12B_SECURE_BASE + MESON_G12B_SECURE_SIZE))
  {
    mOutShmem = (UINTN)Base;
  } else {
    DEBUG ((
      DEBUG_ERROR,
      "MesonSm: output shmem 0x%lx outside secure region; SM reads disabled\n",
      Base
      ));
  }

  return mOutShmem;
}

STATIC
UINT8
MesonSmReadShmem8 (
  IN UINTN  Base,
  IN UINTN  Index
  )
{
  //
  // The shared mem is DEVICE-mapped; read byte-wise to avoid unaligned faults.
  //
  return MmioRead8 (Base + Index);
}

EFI_STATUS
EFIAPI
MesonSmReadEfuse (
  IN  UINT32  Offset,
  OUT UINT8   *Buffer,
  IN  UINTN   Size
  )
{
  ARM_SMC_ARGS  Args;
  UINTN         Shmem;
  UINTN         Index;

  if ((Buffer == NULL) || (Size == 0) || (Size > SIZE_4KB)) {
    return EFI_INVALID_PARAMETER;
  }

  Shmem = MesonSmOutputShmem ();
  if (Shmem == 0) {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = AML_SMC_EFUSE_READ;
  Args.Arg1 = Offset;
  Args.Arg2 = Size;
  ArmCallSmc (&Args);

  //
  // EFUSE_READ returns the byte count in x0; it must equal what we asked for.
  //
  if (Args.Arg0 != Size) {
    return EFI_DEVICE_ERROR;
  }

  for (Index = 0; Index < Size; Index++) {
    Buffer[Index] = MesonSmReadShmem8 (Shmem, Index);
  }

  return EFI_SUCCESS;
}

STATIC
BOOLEAN
HexNibble (
  IN  UINT8  Char,
  OUT UINT8  *Nibble
  )
{
  if ((Char >= '0') && (Char <= '9')) {
    *Nibble = (UINT8)(Char - '0');
  } else if ((Char >= 'a') && (Char <= 'f')) {
    *Nibble = (UINT8)(Char - 'a' + 10);
  } else if ((Char >= 'A') && (Char <= 'F')) {
    *Nibble = (UINT8)(Char - 'A' + 10);
  } else {
    return FALSE;
  }

  return TRUE;
}

EFI_STATUS
EFIAPI
MesonSmGetMac (
  OUT UINT8  Mac[6]
  )
{
  UINT8       Ascii[VIM3_EFUSE_MAC_ASCII_SIZE];
  EFI_STATUS  Status;
  UINTN       Index;
  UINT8       Hi;
  UINT8       Lo;
  UINT8       OrAll;
  UINT8       AndAll;

  Status = MesonSmReadEfuse (0, Ascii, sizeof (Ascii));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < 6; Index++) {
    if (!HexNibble (Ascii[Index * 2], &Hi) ||
        !HexNibble (Ascii[Index * 2 + 1], &Lo))
    {
      return EFI_NOT_FOUND;
    }

    Mac[Index] = (UINT8)((Hi << 4) | Lo);
  }

  //
  // Reject multicast/broadcast (bit 0 of the first octet), all-zero and
  // all-FF.
  //
  if ((Mac[0] & 0x01) != 0) {
    return EFI_NOT_FOUND;
  }

  OrAll  = 0;
  AndAll = 0xFF;
  for (Index = 0; Index < 6; Index++) {
    OrAll  |= Mac[Index];
    AndAll &= Mac[Index];
  }

  if ((OrAll == 0) || (AndAll == 0xFF)) {
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MesonSmGetChipSerial (
  OUT UINT8  Serial[12]
  )
{
  ARM_SMC_ARGS  Args;
  UINTN         Shmem;
  UINTN         Index;
  UINT8         OrAll;

  Shmem = MesonSmOutputShmem ();
  if (Shmem == 0) {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = AML_SMC_GET_CHIP_ID;
  ArmCallSmc (&Args);

  //
  // GET_CHIP_ID fills the output shmem but returns 0 in x0 - do not treat that
  // as a failure.  The 12-byte serial sits at offset 4.
  //
  OrAll = 0;
  for (Index = 0; Index < CHIP_ID_SERIAL_SIZE; Index++) {
    Serial[Index] = MesonSmReadShmem8 (Shmem, CHIP_ID_SERIAL_OFFSET + Index);
    OrAll        |= Serial[Index];
  }

  return (OrAll != 0) ? EFI_SUCCESS : EFI_NOT_FOUND;
}
