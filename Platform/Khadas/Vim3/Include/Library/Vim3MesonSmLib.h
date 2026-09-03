/** @file
  Amlogic Meson G12B secure-monitor (BL31) access: eFUSE read and chip serial.

  The vendor BL31 exposes the standard Amlogic "gxbb-sm" SMC interface (G12
  reuses the GXBB IDs). Results land in a BL31-owned output shared-memory
  buffer whose physical address BL31 returns from GET_SHARE_MEM_OUTPUT_BASE;
  that address is inside the secure-reserved region, mapped DEVICE in the EDK2
  memory map, so callers read it byte-wise.

  Boot-time / DXE only.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef VIM3_MESON_SM_LIB_H_
#define VIM3_MESON_SM_LIB_H_

#include <Uefi/UefiBaseType.h>

/**
  Read Size bytes from user-eFUSE at Offset into Buffer via the secure monitor.

  @retval EFI_SUCCESS            Size bytes were read.
  @retval EFI_DEVICE_ERROR       The SMC failed or returned an unexpected count.
  @retval EFI_UNSUPPORTED        BL31 returned no usable shared-memory buffer.
**/
EFI_STATUS
EFIAPI
MesonSmReadEfuse (
  IN  UINT32  Offset,
  OUT UINT8   *Buffer,
  IN  UINTN   Size
  );

/**
  Read the board's Ethernet MAC from eFUSE (VIM3 layout: offset 0, 12 ASCII
  hex characters), decode to 6 bytes, and validate it as a unicast address.

  @param[out] Mac  Six MAC bytes on success.

  @retval EFI_SUCCESS      A valid unicast MAC was read.
  @retval EFI_NOT_FOUND    The eFUSE MAC is blank/invalid (caller should fall
                           back to its own default).
**/
EFI_STATUS
EFIAPI
MesonSmGetMac (
  OUT UINT8  Mac[6]
  );

/**
  Read the 12-byte SoC unique chip serial via GET_CHIP_ID.

  @param[out] Serial  Twelve serial bytes on success.

  @retval EFI_SUCCESS    A non-zero serial was read.
  @retval EFI_NOT_FOUND  The serial read back all-zero.
**/
EFI_STATUS
EFIAPI
MesonSmGetChipSerial (
  OUT UINT8  Serial[12]
  );

#endif // VIM3_MESON_SM_LIB_H_
