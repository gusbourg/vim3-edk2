/** @file
  Raw access to the board's SPI NOR, below the FVB abstraction.

  MesonSpifcFvbDxe publishes this alongside its FVB so that the platform
  FMP can reflash the firmware boot image at the bottom of the chip.  The
  FVB protocol cannot serve that purpose: its LBA space is deliberately
  confined to the variable/FTW runtime window, and widening it would hand
  the generic variable stack a route into the boot image.

  The protocol is boot-services only.  The producing driver is a runtime
  driver, but these entry points are not converted at SetVirtualAddressMap
  and refuse to run after ExitBootServices.

  Program() does not erase and does not verify; callers erase first and
  prove their own writes with Read().  That mirrors the layering inside
  the FVB driver itself.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef MESON_NOR_FLASH_H_
#define MESON_NOR_FLASH_H_

#define MESON_NOR_FLASH_PROTOCOL_GUID \
  { 0x17cd2784, 0x1b3c, 0x4012, { 0xaf, 0x1a, 0xb8, 0x48, 0x42, 0x28, 0x2c, 0xdb } }

typedef struct _MESON_NOR_FLASH_PROTOCOL MESON_NOR_FLASH_PROTOCOL;

/**
  Read from the NOR.  Any range inside the chip is readable.

  @param[in]  This     This protocol instance.
  @param[in]  Address  Byte offset from the start of the chip.
  @param[out] Buffer   Destination.
  @param[in]  Length   Bytes to read.

  @retval EFI_SUCCESS            Buffer holds the flash contents.
  @retval EFI_INVALID_PARAMETER  Range leaves the chip, or Buffer is NULL.
  @retval EFI_UNSUPPORTED        Called after ExitBootServices.
**/
typedef
EFI_STATUS
(EFIAPI *MESON_NOR_FLASH_READ)(
  IN  MESON_NOR_FLASH_PROTOCOL  *This,
  IN  UINT32                    Address,
  OUT VOID                      *Buffer,
  IN  UINTN                     Length
  );

/**
  Program previously erased flash.  The range must end at or below the
  firmware-owned runtime region (VIM3_NOR_RUNTIME_BASE): the variable
  store and its RAM shadow are unreachable through this protocol by
  construction.

  @param[in] This     This protocol instance.
  @param[in] Address  Byte offset from the start of the chip.
  @param[in] Buffer   Bytes to program.
  @param[in] Length   Byte count.

  @retval EFI_SUCCESS            Programmed (not verified - read it back).
  @retval EFI_INVALID_PARAMETER  Range reaches the runtime region.
  @retval EFI_WRITE_PROTECTED    The flash has protection bits set.
  @retval EFI_UNSUPPORTED        Called after ExitBootServices.
**/
typedef
EFI_STATUS
(EFIAPI *MESON_NOR_FLASH_PROGRAM)(
  IN MESON_NOR_FLASH_PROTOCOL  *This,
  IN UINT32                    Address,
  IN CONST VOID                *Buffer,
  IN UINTN                     Length
  );

/**
  Erase whole sectors.  Address and Length must both be multiples of
  VIM3_NOR_ERASE_SIZE, with the same runtime-region bound as Program().

  @param[in] This     This protocol instance.
  @param[in] Address  First sector's byte offset.
  @param[in] Length   Bytes to erase.

  @retval EFI_SUCCESS            Sectors erased.
  @retval EFI_INVALID_PARAMETER  Misaligned, or range reaches the runtime
                                 region.
  @retval EFI_WRITE_PROTECTED    The flash has protection bits set.
  @retval EFI_UNSUPPORTED        Called after ExitBootServices.
**/
typedef
EFI_STATUS
(EFIAPI *MESON_NOR_FLASH_ERASE)(
  IN MESON_NOR_FLASH_PROTOCOL  *This,
  IN UINT32                    Address,
  IN UINTN                     Length
  );

struct _MESON_NOR_FLASH_PROTOCOL {
  MESON_NOR_FLASH_READ       Read;
  MESON_NOR_FLASH_PROGRAM    Program;
  MESON_NOR_FLASH_ERASE      Erase;
};

extern EFI_GUID  gMesonNorFlashProtocolGuid;

#endif // MESON_NOR_FLASH_H_
