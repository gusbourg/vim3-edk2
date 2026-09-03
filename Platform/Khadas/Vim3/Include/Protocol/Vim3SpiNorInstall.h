/** @file
  Install the running firmware into the SPI NOR, on demand.

  Vim3FmpDxe already owns everything this needs - the eMMC boot-partition
  access, the raw NOR protocol, and the erase/program/verify loop that
  capsule updates use in production.  This protocol exposes that one
  operation so BDS can drive it from a setup-menu request, without a
  capsule and without duplicating the NOR write anywhere else.

  The source is the eMMC boot partition, i.e. the firmware the board is
  already running from (or would fall through to).  That makes the copy
  self-consistent by construction: there is no payload to supply, no
  version to match, and nothing staged on an ESP.

  Boot-services only.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef VIM3_SPI_NOR_INSTALL_H_
#define VIM3_SPI_NOR_INSTALL_H_

#define VIM3_SPI_NOR_INSTALL_PROTOCOL_GUID \
  { 0xb2d44920, 0x6b06, 0x46ad, { 0xb0, 0x4e, 0x84, 0x0b, 0x4b, 0x29, 0x58, 0x82 } }

typedef struct _VIM3_SPI_NOR_INSTALL_PROTOCOL VIM3_SPI_NOR_INSTALL_PROTOCOL;

/**
  Copy the eMMC boot-partition firmware into the SPI NOR and point the
  BootROM at it.

  Reads boot0, refuses anything without the FIP table signature (the boot
  partition holds whatever was last written there - unlike a capsule it is
  not validated on the way in), then erases, programs and read-back verifies
  the NOR.  Only after that does it set the MCU to SPI-first.

  ERASES THE NOR BOOT REGION, including oowow if the board still carries it.

  A power cut during the write leaves the NOR invalid and the BootROM falls
  through to the eMMC copy this read from - which is why the source is the
  eMMC copy and not a supplied payload.

  @param[in] This  This protocol instance.

  @retval EFI_SUCCESS            NOR written, verified, and selected.
  @retval EFI_NOT_FOUND          No eMMC, or no NOR flash protocol.
  @retval EFI_VOLUME_CORRUPTED   boot0 carries no FIP signature.
  @retval EFI_DEVICE_ERROR       Write or read-back verify failed.
  @retval EFI_ACCESS_DENIED      NOR is good, but the MCU refused SPI-first.
**/
typedef
EFI_STATUS
(EFIAPI *VIM3_SPI_NOR_INSTALL_FROM_EMMC)(
  IN VIM3_SPI_NOR_INSTALL_PROTOCOL  *This
  );

struct _VIM3_SPI_NOR_INSTALL_PROTOCOL {
  VIM3_SPI_NOR_INSTALL_FROM_EMMC    InstallFromEmmc;
};

extern EFI_GUID  gVim3SpiNorInstallProtocolGuid;

#endif // VIM3_SPI_NOR_INSTALL_H_
