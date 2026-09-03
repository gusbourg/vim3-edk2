/** @file
  Firmware Management Protocol for the VIM3's firmware image.

  This moves the promotion discipline that has lived in
  scripts/promote-vim3-emmc-boot-partitions.sh into firmware, so a capsule
  can replace the boot chain without an SSH session.

  TWO PHYSICAL COPIES, ONE CAPSULE.  Since the SPI-NOR move the firmware
  has a primary copy in the NOR (raw image at offset 0) and a rescue copy
  in the eMMC boot partitions, and they must never diverge.  The capsule
  payload is the eMMC media image; its 512-byte BL2 envelope is all that
  separates the two forms, so bytes [512..) are exactly the NOR image and
  one capsule feeds both targets.  The NOR is written only when the board
  actually booted from it (BootROM source nibble in AO_SEC_GP_CFG0):
  eMMC-booted boards - every beta tester, whose NOR holds oowow - keep
  the historical eMMC-only behaviour untouched.

  The eMMC rescue is written before the NOR primary, so a power cut
  during the slow NOR write leaves the BootROM's fall-through pointed at
  firmware that is already new.

  WHY THE eMMC BOOT PARTITIONS ARE REACHABLE FROM HERE.  The card exposes
  boot0 and boot1 only while EXT_CSD PARTITION_CONFIG selects them, and
  nothing in the generic MMC stack ever switches it.  Rather than patch
  MmcDxe, this driver issues CMD6 itself through the host protocol and then
  reuses MmcDxe's BlockIo for the transfer: while a boot partition is
  selected, the same BlockIo addresses that partition.  It can do so because
  MmcDxe assigns eMMC relative addresses from a counter starting at one
  (MmcIdentification.c, mEmmcRcaCount) and this board has exactly one eMMC,
  so the RCA is deterministic.

  SAFETY.  The image cannot be validated by content: aml_encrypt_g12b
  randomizes the wrapper, so two builds of identical firmware share no
  header bytes and there is no magic to check.  The safety net is therefore
  ordering, not inspection.  boot0 is written and read back before boot1 is
  touched, so a power cut can leave at most one partition damaged, and the
  BootROM tries boot0 then boot1.  Never write both blind.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/FirmwareManagement.h>
#include <Protocol/MesonNorFlash.h>
#include <Protocol/MmcHost.h>
#include <Protocol/Vim3SpiNorInstall.h>

#include <Guid/SystemResourceTable.h>

#include <Library/Vim3McuLib.h>

#include <MesonG12B.h>
#include <Vim3Mcu.h>
#include <Vim3PlatformConfig.h>

///
/// The eMMC hardware boot partitions are 4 MiB each on this part
/// (/sys/block/mmcblk1boot0/size reports 8192 sectors).
///
#define VIM3_BOOT_PARTITION_BLOCKS  8192
#define VIM3_BLOCK_SIZE             512

///
/// EXT_CSD byte 179.  Its low three bits select which partition subsequent
/// block traffic addresses.
///
#define EXTCSD_PARTITION_CONFIG  179
#define PARTITION_ACCESS_MASK    0x7
#define PARTITION_ACCESS_USER    0
#define PARTITION_ACCESS_BOOT0   1
#define PARTITION_ACCESS_BOOT1   2

///
/// CMD6 argument layout, mirroring EmbeddedPkg/Universal/MmcDxe/Mmc.h.  The
/// command set value of 1 is what MmcDxe already uses successfully on this
/// board for bus-width selection.
///
#define EMMC_CMD6_ARG_ACCESS(x)   (((x) & 0x3) << 24)
#define EMMC_CMD6_ARG_INDEX(x)    (((x) & 0xFF) << 16)
#define EMMC_CMD6_ARG_VALUE(x)    (((x) & 0xFF) << 8)
#define EMMC_CMD6_ARG_CMD_SET(x)  (((x) & 0x7) << 0)

#define DEVICE_STATE(x)     (((x) >> 9) & 0xF)
#define EMMC_PRG_STATE      7
#define EMMC_SWITCH_ERROR   (1 << 7)
#define RCA_SHIFT_OFFSET    16

///
/// MmcDxe hands the first eMMC an RCA of 1; this board has one eMMC.
///
#define VIM3_EMMC_RCA  1

///
/// BL2 records the BootROM source in AO_SEC_GP_CFG0 (locally defined per
/// driver, as in MesonSdMmcDxe).  3 is SPI NOR - measured on the
/// SPI-booted bench board and matching U-Boot's BOOT_DEVICE_SPI.
///
#define BOOT_DEVICE_MASK     0xFU
#define BOOT_DEVICE_SPI_NOR  3U

///
/// The NOR write is chunked so a read-back verify buffer stays small and
/// the progress bar moves during the slow PIO phase.
///
#define VIM3_NOR_WRITE_CHUNK  SIZE_64KB

#define VIM3_FMP_IMAGE_INDEX  1
#define VIM3_FMP_IMAGE_ID     1

#define LAST_ATTEMPT_VARIABLE  L"Vim3FmpLastAttempt"

///
/// Vendor-range LastAttemptStatus codes (UEFI spec reserves
/// 0x1000-0x3FFF for vendor use).  RELEASE builds carry no DEBUG output,
/// so ESRT's last_attempt_status is the only after-the-fact witness of
/// WHERE an update failed - one code per stage.
///
#define VIM3_LAS_NO_NOR_PROTOCOL \
  (LAST_ATTEMPT_STATUS_ERROR_UNSUCCESSFUL_VENDOR_RANGE_MIN + 0)
#define VIM3_LAS_NO_EMMC \
  (LAST_ATTEMPT_STATUS_ERROR_UNSUCCESSFUL_VENDOR_RANGE_MIN + 1)
#define VIM3_LAS_BOOT0_FAILED \
  (LAST_ATTEMPT_STATUS_ERROR_UNSUCCESSFUL_VENDOR_RANGE_MIN + 2)
#define VIM3_LAS_BOOT1_FAILED \
  (LAST_ATTEMPT_STATUS_ERROR_UNSUCCESSFUL_VENDOR_RANGE_MIN + 3)
#define VIM3_LAS_NOR_FAILED \
  (LAST_ATTEMPT_STATUS_ERROR_UNSUCCESSFUL_VENDOR_RANGE_MIN + 4)
///
/// The NOR image verified but the MCU would not take SPI-first.  Both copies
/// are good; the board simply keeps booting the eMMC one.  Distinct from
/// VIM3_LAS_NOR_FAILED because the recovery is different: retry the MCU
/// write, do not rewrite the flash.
///
#define VIM3_LAS_MCU_FLIP_FAILED \
  (LAST_ATTEMPT_STATUS_ERROR_UNSUCCESSFUL_VENDOR_RANGE_MIN + 5)

typedef struct {
  UINT32    Version;
  UINT32    Status;
} VIM3_LAST_ATTEMPT;

STATIC EFI_GUID  mMesonEmmcDevicePathGuid =
  { 0x9ec8ef97, 0x39d8, 0x48a3, { 0x84, 0xaf, 0xa1, 0x94, 0xd9, 0x92, 0xc6, 0x37 } };

STATIC EFI_FIRMWARE_MANAGEMENT_PROTOCOL  mFmp;
STATIC EFI_HANDLE                        mFmpHandle;
STATIC VIM3_LAST_ATTEMPT                 mLastAttempt;
STATIC CHAR16                            mImageIdName[] = L"VIM3 System Firmware";

/**
  Test whether a device path node is the vendor node MesonSdMmcDxe uses for
  the eMMC controller.
**/
STATIC
BOOLEAN
IsEmmcVendorNode (
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  VENDOR_DEVICE_PATH  *Vendor;

  if (DevicePath == NULL) {
    return FALSE;
  }

  if ((DevicePathType (DevicePath) != HARDWARE_DEVICE_PATH) ||
      (DevicePathSubType (DevicePath) != HW_VENDOR_DP))
  {
    return FALSE;
  }

  Vendor = (VENDOR_DEVICE_PATH *)DevicePath;
  return CompareGuid (&Vendor->Guid, &mMesonEmmcDevicePathGuid);
}

/**
  Test whether a device path is exactly the eMMC vendor node and nothing
  else.

  A partition's device path carries the same node followed by more, so the
  end-of-path check is what keeps this from matching a partition handle.
  This applies only to handle device paths; the MMC host protocol's
  BuildDevicePath returns a BARE node with no end node after it (MmcDxe
  appends the end itself), so the host-side match must use IsEmmcVendorNode
  alone - requiring an end node there walks the node's Length into
  whatever follows the allocation and fails.
**/
STATIC
BOOLEAN
IsEmmcWholeDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  if (!IsEmmcVendorNode (DevicePath)) {
    return FALSE;
  }

  return IsDevicePathEnd (NextDevicePathNode (DevicePath));
}

/**
  Locate the eMMC's BlockIo and its MMC host protocol.
**/
STATIC
EFI_STATUS
LocateEmmc (
  OUT EFI_BLOCK_IO_PROTOCOL  **BlockIo,
  OUT EFI_MMC_HOST_PROTOCOL  **MmcHost
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                *Handles;
  UINTN                     HandleCount;
  UINTN                     Index;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_MMC_HOST_PROTOCOL     *Host;

  *BlockIo = NULL;
  *MmcHost = NULL;

  //
  // The BlockIo whose device path is the bare eMMC vendor node is MmcDxe's
  // whole-device handle for that controller.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiDevicePathProtocolGuid,
                    (VOID **)&DevicePath
                    );
    if (EFI_ERROR (Status) || !IsEmmcWholeDevicePath (DevicePath)) {
      continue;
    }

    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiBlockIoProtocolGuid,
                    (VOID **)BlockIo
                    );
    if (!EFI_ERROR (Status)) {
      break;
    }
  }

  FreePool (Handles);

  if (*BlockIo == NULL) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: no eMMC BlockIo handle\n"));
    return EFI_NOT_FOUND;
  }

  //
  // MesonSdMmcDxe installs one host protocol per controller; ask each which
  // device path it describes.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEmbeddedMmcHostProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEmbeddedMmcHostProtocolGuid,
                    (VOID **)&Host
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Host->BuildDevicePath (Host, &DevicePath);
    if (EFI_ERROR (Status)) {
      continue;
    }

    if (IsEmmcVendorNode (DevicePath)) {
      *MmcHost = Host;
      break;
    }
  }

  FreePool (Handles);

  if (*MmcHost == NULL) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: no eMMC MMC host protocol\n"));
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

/**
  Wait for the card to leave programming state after a CMD6.
**/
STATIC
EFI_STATUS
WaitForTransferState (
  IN EFI_MMC_HOST_PROTOCOL  *MmcHost
  )
{
  EFI_STATUS  Status;
  UINT32      Data;
  UINTN       Retry;

  for (Retry = 0; Retry < 10000; Retry++) {
    Status = MmcHost->SendCommand (
                        MmcHost,
                        MMC_CMD13,
                        VIM3_EMMC_RCA << RCA_SHIFT_OFFSET
                        );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_R1, &Data);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Data & EMMC_SWITCH_ERROR) != 0) {
      DEBUG ((DEBUG_ERROR, "Vim3Fmp: eMMC rejected the partition switch\n"));
      return EFI_DEVICE_ERROR;
    }

    if (DEVICE_STATE (Data) != EMMC_PRG_STATE) {
      return EFI_SUCCESS;
    }

    gBS->Stall (100);
  }

  return EFI_TIMEOUT;
}

/**
  Read EXT_CSD byte PARTITION_CONFIG from the card.

  The byte carries more than the access bits: BOOT_PARTITION_ENABLE (5:3)
  tells the BootROM which partition to boot, and BOOT_ACK sits in bit 6.
  Guessing those and writing them back would be a good way to make the board
  stop booting, so the current value is always read first and preserved.
**/
STATIC
EFI_STATUS
ReadPartitionConfig (
  IN  EFI_MMC_HOST_PROTOCOL  *MmcHost,
  OUT UINT8                  *Config
  )
{
  EFI_STATUS  Status;
  UINT8       *ExtCsd;

  ExtCsd = AllocateZeroPool (VIM3_BLOCK_SIZE);
  if (ExtCsd == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = MmcHost->SendCommand (MmcHost, MMC_CMD8, 0);
  if (!EFI_ERROR (Status)) {
    Status = MmcHost->ReadBlockData (
                        MmcHost,
                        0,
                        VIM3_BLOCK_SIZE,
                        (UINT32 *)ExtCsd
                        );
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: EXT_CSD read failed: %r\n", Status));
  } else {
    *Config = ExtCsd[EXTCSD_PARTITION_CONFIG];
    DEBUG ((DEBUG_INFO, "Vim3Fmp: PARTITION_CONFIG = 0x%02x\n", *Config));
  }

  FreePool (ExtCsd);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  return WaitForTransferState (MmcHost);
}

/**
  Point subsequent block traffic at a hardware partition.

  @param[in] MmcHost  The eMMC host protocol.
  @param[in] Access   PARTITION_ACCESS_USER, _BOOT0 or _BOOT1.
**/
STATIC
EFI_STATUS
SelectPartition (
  IN EFI_MMC_HOST_PROTOCOL  *MmcHost,
  IN UINT8                  Access
  )
{
  EFI_STATUS  Status;
  UINT32      Argument;
  UINT8       Config;

  Status = ReadPartitionConfig (MmcHost, &Config);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Config = (UINT8)((Config & ~PARTITION_ACCESS_MASK) |
                   (Access & PARTITION_ACCESS_MASK));

  Argument = EMMC_CMD6_ARG_ACCESS (3) |
             EMMC_CMD6_ARG_INDEX (EXTCSD_PARTITION_CONFIG) |
             EMMC_CMD6_ARG_VALUE (Config) |
             EMMC_CMD6_ARG_CMD_SET (1);

  Status = MmcHost->SendCommand (MmcHost, MMC_CMD6, Argument);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: CMD6 failed: %r\n", Status));
    return Status;
  }

  Status = WaitForTransferState (MmcHost);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((DEBUG_INFO, "Vim3Fmp: eMMC partition access = %u\n", Access));
  return EFI_SUCCESS;
}

/**
  Write an image to the currently selected partition and prove it landed.

  The image is staged through a page-aligned bounce buffer first.  The MMC
  host DMA takes the buffer address directly (SD_EMMC_CMD_DAT), and a
  capsule payload arrives at whatever offset its headers dictate - in
  practice 4-byte-but-not-8-byte aligned, which the engine silently
  truncates.  That was hardware-observed as a deterministic corrupt write
  (identical corruption hash across two attempts) while the read-back into
  a pool-aligned buffer told the truth.  Page alignment also keeps the
  buffer from sharing cache lines with anything else during the write-back.
**/
STATIC
EFI_STATUS
WriteAndVerify (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN CONST VOID             *Image,
  IN UINTN                  ImageSize
  )
{
  EFI_STATUS  Status;
  VOID        *Aligned;
  VOID        *ReadBack;

  Aligned = AllocatePages (EFI_SIZE_TO_PAGES (ImageSize));
  if (Aligned == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Aligned, Image, ImageSize);

  Status = BlockIo->WriteBlocks (
                      BlockIo,
                      BlockIo->Media->MediaId,
                      0,
                      ImageSize,
                      Aligned
                      );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: write failed: %r\n", Status));
    FreePages (Aligned, EFI_SIZE_TO_PAGES (ImageSize));
    return Status;
  }

  Status = BlockIo->FlushBlocks (BlockIo);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: flush failed: %r\n", Status));
    FreePages (Aligned, EFI_SIZE_TO_PAGES (ImageSize));
    return Status;
  }

  ReadBack = AllocatePages (EFI_SIZE_TO_PAGES (ImageSize));
  if (ReadBack == NULL) {
    FreePages (Aligned, EFI_SIZE_TO_PAGES (ImageSize));
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BlockIo->ReadBlocks (
                      BlockIo,
                      BlockIo->Media->MediaId,
                      0,
                      ImageSize,
                      ReadBack
                      );
  if (!EFI_ERROR (Status) && (CompareMem (ReadBack, Image, ImageSize) != 0)) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: read-back mismatch\n"));
    Status = EFI_DEVICE_ERROR;
  }

  FreePages (ReadBack, EFI_SIZE_TO_PAGES (ImageSize));
  FreePages (Aligned, EFI_SIZE_TO_PAGES (ImageSize));
  return Status;
}

/**
  Whether this boot's firmware was fetched from the SPI NOR.
**/
STATIC
BOOLEAN
BootedFromSpiNor (
  VOID
  )
{
  return (MmioRead32 (MESON_G12B_AO_SEC_GP_CFG0) & BOOT_DEVICE_MASK) ==
         BOOT_DEVICE_SPI_NOR;
}

/**
  Has the operator opted in to installing the firmware into the NOR from an
  eMMC-booted board?

  Absent or unreadable means no: this must never be the default, because on a
  beta board the NOR still holds oowow.

  @retval TRUE  The setup menu asked for a NOR install.
**/
STATIC
BOOLEAN
SpiNorInstallRequested (
  VOID
  )
{
  EFI_STATUS                          Status;
  VIM3_SPI_NOR_INSTALL_VARSTORE_DATA  Request;
  UINTN                               Size;

  Size   = sizeof (Request);
  Status = gRT->GetVariable (
                  VIM3_SPI_NOR_INSTALL_VARIABLE_NAME,
                  &gVim3PlatformFormSetGuid,
                  NULL,
                  &Size,
                  &Request
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (Request))) {
    return FALSE;
  }

  return (BOOLEAN)(Request.Enable == VIM3_SPI_NOR_INSTALL_ENABLED);
}

/**
  Point the BootROM at the NOR copy and consume the one-shot request.

  Called only after a NOR image has been written AND read back verified.  The
  request is cleared on success so a later capsule cannot silently reflash the
  NOR, and kept on failure so a retry needs no second trip to setup.

  @retval EFI_SUCCESS  The MCU reports SPI-first.
**/
STATIC
EFI_STATUS
CompleteSpiNorInstall (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = Vim3McuWriteByte (MCU_REG_BOOT_MODE, VIM3_MCU_BOOT_MODE_SPI_FIRST);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: NOR written but MCU boot-mode flip failed: %r\n", Status));
    return Status;
  }

  //
  // Deleting the request is best-effort: the flash and the boot source are
  // already correct, and a stale request only costs one redundant NOR write
  // on the next capsule.  Never fail the update over it.
  //
  gRT->SetVariable (
         VIM3_SPI_NOR_INSTALL_VARIABLE_NAME,
         &gVim3PlatformFormSetGuid,
         0,
         0,
         NULL
         );

  DEBUG ((DEBUG_INFO, "Vim3Fmp: SPI-NOR install complete; MCU set to SPI-first\n"));
  return EFI_SUCCESS;
}

/**
  Erase, program and read-back-verify one chunk, with one retry.
**/
STATIC
EFI_STATUS
NorUpdateChunk (
  IN MESON_NOR_FLASH_PROTOCOL  *Nor,
  IN UINT32                    Offset,
  IN CONST UINT8               *Data,
  IN UINTN                     Length,
  IN UINT8                     *Verify
  )
{
  EFI_STATUS  Status;
  UINTN       Attempt;

  //
  // One retry from a fresh erase: the bench failure was a single
  // under-programmed byte (extra 1-bits) that programmed cleanly on the
  // next attempt - transient program events like that are how NOR ages.
  //
  for (Attempt = 1; Attempt <= 2; Attempt++) {
    Status = Nor->Erase (Nor, Offset, ALIGN_VALUE (Length, VIM3_NOR_ERASE_SIZE));
    if (!EFI_ERROR (Status)) {
      Status = Nor->Program (Nor, Offset, Data, Length);
    }

    if (!EFI_ERROR (Status)) {
      Status = Nor->Read (Nor, Offset, Verify, Length);
    }

    if (!EFI_ERROR (Status)) {
      if (CompareMem (Verify, Data, Length) == 0) {
        return EFI_SUCCESS;
      }

      Status = EFI_DEVICE_ERROR;
    }

    DEBUG ((DEBUG_ERROR, "Vim3Fmp: NOR chunk @%x attempt %u: %r\n",
      Offset, (UINT32)Attempt, Status));
  }

  return Status;
}

/**
  Rewrite the NOR primary from the capsule payload.

  The payload is the eMMC media image; stripping its 512-byte BL2
  envelope yields the raw NOR image (plus a short sector-padding tail,
  harmlessly written along).  Each 64 KiB slice is erased, programmed and
  proven by read-back before the next, with the progress bar advanced
  from ProgressStart to ProgressEnd - at ~12.8 MHz PIO this phase takes
  minutes, and a bar that visibly moves is what distinguishes "slow" from
  "hung".

  Failure ordering matters: the BootROM only falls through to the eMMC
  rescue when BL2 fails validation, so this function erases the BL2 chunk
  first, programs it last, and on any abort erases the BL2 sector again.
  In every failure or power-cut window the NOR is therefore unbootable
  rather than half-written, and the board comes back on the rescue that
  was written and verified before this phase began.
**/
STATIC
EFI_STATUS
WriteAndVerifyNor (
  IN MESON_NOR_FLASH_PROTOCOL                       *Nor,
  IN CONST VOID                                     *Image,
  IN UINTN                                          ImageSize,
  IN EFI_FIRMWARE_MANAGEMENT_UPDATE_IMAGE_PROGRESS  Progress,
  IN UINTN                                          ProgressStart,
  IN UINTN                                          ProgressEnd
  )
{
  EFI_STATUS   Status;
  CONST UINT8  *NorImage;
  UINTN        NorSize;
  UINT8        *Verify;
  UINTN        Offset;
  UINTN        Chunk;
  UINTN        FirstChunk;
  UINTN        Written;
  UINTN        Percent;
  UINTN        LastPercent;

  if (ImageSize <= VIM3_BLOCK_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  NorImage = (CONST UINT8 *)Image + VIM3_BLOCK_SIZE;
  NorSize  = ImageSize - VIM3_BLOCK_SIZE;

  if (ALIGN_VALUE (NorSize, VIM3_NOR_ERASE_SIZE) > VIM3_NOR_FUTURE_BOOT_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  Verify = AllocatePages (EFI_SIZE_TO_PAGES (VIM3_NOR_WRITE_CHUNK));
  if (Verify == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Erase the first chunk before touching anything else, and program it
  // LAST.  BL2 lives in that chunk, and the BootROM only falls through to
  // the eMMC rescue when BL2 itself fails validation: a half-written image
  // with a valid BL2 boot-loops instead (measured - one under-programmed
  // byte at 0x1d4c71 looped the bench board until manual recovery).  With
  // this ordering, a power cut or an abort anywhere in the window leaves
  // the NOR unbootable and the board falls through to the rescue, which
  // was already written and verified before this function ran.
  //
  FirstChunk = MIN (NorSize, (UINTN)VIM3_NOR_WRITE_CHUNK);

  Status = Nor->Erase (Nor, 0, ALIGN_VALUE (FirstChunk, VIM3_NOR_ERASE_SIZE));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: NOR erase @0 failed: %r\n", Status));
    goto Out;
  }

  Written     = 0;
  LastPercent = ProgressStart;

  for (Offset = FirstChunk; Offset < NorSize; Offset += Chunk) {
    Chunk = MIN (NorSize - Offset, (UINTN)VIM3_NOR_WRITE_CHUNK);

    Status = NorUpdateChunk (Nor, (UINT32)Offset, NorImage + Offset, Chunk, Verify);
    if (EFI_ERROR (Status)) {
      goto Abort;
    }

    Written += Chunk;
    if (Progress != NULL) {
      Percent = ProgressStart + (Written * (ProgressEnd - ProgressStart)) / NorSize;
      if (Percent != LastPercent) {
        Progress (Percent);
        LastPercent = Percent;
      }
    }
  }

  Status = NorUpdateChunk (Nor, 0, NorImage, FirstChunk, Verify);
  if (EFI_ERROR (Status)) {
    goto Abort;
  }

  if (Progress != NULL) {
    Progress (ProgressEnd);
  }

  goto Out;

Abort:
  //
  // Leave the NOR unbootable rather than half-written: erase the BL2
  // sector so the BootROM's fall-through covers this failure too.  Best
  // effort - if even this erase fails there is nothing more to be done,
  // and the last-attempt status already records the failure.
  //
  DEBUG ((DEBUG_ERROR, "Vim3Fmp: NOR update failed (%r); erasing BL2 sector for eMMC fall-through\n", Status));
  Nor->Erase (Nor, 0, VIM3_NOR_ERASE_SIZE);

Out:
  FreePages (Verify, EFI_SIZE_TO_PAGES (VIM3_NOR_WRITE_CHUNK));
  return Status;
}

/**
  Record the outcome so ESRT can report it after the reset.
**/
STATIC
VOID
SaveLastAttempt (
  IN UINT32  Version,
  IN UINT32  LastAttemptStatus
  )
{
  mLastAttempt.Version = Version;
  mLastAttempt.Status  = LastAttemptStatus;

  gRT->SetVariable (
         LAST_ATTEMPT_VARIABLE,
         &gVim3PlatformFormSetGuid,
         EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
         sizeof (mLastAttempt),
         &mLastAttempt
         );
}

/**
  Common sanity checks for a candidate image.

  Content cannot be validated - see the file header - so this only rejects
  images that certainly cannot be a boot-partition payload.
**/
STATIC
EFI_STATUS
ValidateImage (
  IN CONST VOID  *Image,
  IN UINTN       ImageSize,
  OUT UINT32     *ImageUpdatable
  )
{
  if ((Image == NULL) || (ImageSize == 0)) {
    *ImageUpdatable = IMAGE_UPDATABLE_INVALID;
    return EFI_INVALID_PARAMETER;
  }

  if ((ImageSize % VIM3_BLOCK_SIZE) != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "Vim3Fmp: image size %lu is not a whole number of sectors\n",
      (UINT64)ImageSize
      ));
    *ImageUpdatable = IMAGE_UPDATABLE_INVALID;
    return EFI_INVALID_PARAMETER;
  }

  if (ImageSize > (VIM3_BOOT_PARTITION_BLOCKS * VIM3_BLOCK_SIZE)) {
    DEBUG ((
      DEBUG_ERROR,
      "Vim3Fmp: image of %lu bytes exceeds the 4 MiB boot partition\n",
      (UINT64)ImageSize
      ));
    *ImageUpdatable = IMAGE_UPDATABLE_INVALID_TYPE;
    return EFI_INVALID_PARAMETER;
  }

  *ImageUpdatable = IMAGE_UPDATABLE_VALID;
  return EFI_SUCCESS;
}

/**
  Copy the eMMC boot-partition firmware into the NOR.  See
  Protocol/Vim3SpiNorInstall.h for the contract.

  The source is boot0 - the firmware this board already runs, or falls
  through to.  Nothing is supplied by the caller, so there is no payload to
  validate on the way in and no version to reconcile; the only check that
  matters is that boot0 actually holds a firmware image, because unlike a
  capsule its contents were never validated by anything.
**/
STATIC
EFI_STATUS
EFIAPI
Vim3SpiNorInstallFromEmmc (
  IN VIM3_SPI_NOR_INSTALL_PROTOCOL  *This
  )
{
  EFI_STATUS                 Status;
  EFI_BLOCK_IO_PROTOCOL      *BlockIo;
  EFI_MMC_HOST_PROTOCOL      *MmcHost;
  MESON_NOR_FLASH_PROTOCOL   *Nor;
  VOID                       *Image;
  UINTN                      ImageSize;
  UINT32                     Updatable;

  Status = gBS->LocateProtocol (&gMesonNorFlashProtocolGuid, NULL, (VOID **)&Nor);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: no NOR flash protocol: %r\n", Status));
    return EFI_NOT_FOUND;
  }

  Status = LocateEmmc (&BlockIo, &MmcHost);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: no eMMC for SPI-NOR install: %r\n", Status));
    return EFI_NOT_FOUND;
  }

  ImageSize = VIM3_BOOT_PARTITION_BLOCKS * VIM3_BLOCK_SIZE;
  Image     = AllocatePages (EFI_SIZE_TO_PAGES (ImageSize));
  if (Image == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = SelectPartition (MmcHost, PARTITION_ACCESS_BOOT0);
  if (!EFI_ERROR (Status)) {
    Status = BlockIo->ReadBlocks (
                        BlockIo,
                        BlockIo->Media->MediaId,
                        0,
                        ImageSize,
                        Image
                        );
    //
    // Always hand the card back to the user area, even on a read error:
    // leaving it pointed at boot0 would make every later block access on
    // this boot read firmware instead of the OS disk.
    //
    SelectPartition (MmcHost, PARTITION_ACCESS_USER);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: boot0 read failed: %r\n", Status));
    FreePages (Image, EFI_SIZE_TO_PAGES (ImageSize));
    return Status;
  }

  //
  // boot0 holds whatever was last written to it.  Refuse to erase the NOR
  // for anything that is not a firmware image.
  //
  Status = ValidateImage (Image, ImageSize, &Updatable);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: boot0 is not a firmware image: %r\n", Status));
    FreePages (Image, EFI_SIZE_TO_PAGES (ImageSize));
    return EFI_VOLUME_CORRUPTED;
  }

  //
  // The NOR write is minutes of PIO; the BDS watchdog must not fire.
  //
  gBS->SetWatchdogTimer (0, 0, 0, NULL);

  DEBUG ((
    DEBUG_INFO,
    "Vim3Fmp: installing eMMC boot0 into the NOR (%lu bytes)\n",
    (UINT64)(ImageSize - VIM3_BLOCK_SIZE)
    ));

  Status = WriteAndVerifyNor (Nor, Image, ImageSize, NULL, 0, 0);
  FreePages (Image, EFI_SIZE_TO_PAGES (ImageSize));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: NOR install failed: %r\n", Status));
    return EFI_DEVICE_ERROR;
  }

  return CompleteSpiNorInstall ();
}

STATIC VIM3_SPI_NOR_INSTALL_PROTOCOL  mSpiNorInstall = {
  Vim3SpiNorInstallFromEmmc
};

STATIC
EFI_STATUS
EFIAPI
FmpGetImageInfo (
  IN EFI_FIRMWARE_MANAGEMENT_PROTOCOL  *This,
  IN OUT UINTN                         *ImageInfoSize,
  IN OUT EFI_FIRMWARE_IMAGE_DESCRIPTOR *ImageInfo,
  OUT UINT32                           *DescriptorVersion,
  OUT UINT8                            *DescriptorCount,
  OUT UINTN                            *DescriptorSize,
  OUT UINT32                           *PackageVersion,
  OUT CHAR16                           **PackageVersionName
  )
{
  if (ImageInfoSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (*ImageInfoSize < sizeof (EFI_FIRMWARE_IMAGE_DESCRIPTOR)) {
    *ImageInfoSize = sizeof (EFI_FIRMWARE_IMAGE_DESCRIPTOR);
    return EFI_BUFFER_TOO_SMALL;
  }

  if ((ImageInfo == NULL) || (DescriptorVersion == NULL) ||
      (DescriptorCount == NULL) || (DescriptorSize == NULL) ||
      (PackageVersion == NULL) || (PackageVersionName == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *ImageInfoSize      = sizeof (EFI_FIRMWARE_IMAGE_DESCRIPTOR);
  *DescriptorVersion  = EFI_FIRMWARE_IMAGE_DESCRIPTOR_VERSION;
  *DescriptorCount    = 1;
  *DescriptorSize     = sizeof (EFI_FIRMWARE_IMAGE_DESCRIPTOR);
  *PackageVersion     = 0xFFFFFFFF;
  *PackageVersionName = NULL;

  ZeroMem (ImageInfo, sizeof (EFI_FIRMWARE_IMAGE_DESCRIPTOR));
  ImageInfo->ImageIndex  = VIM3_FMP_IMAGE_INDEX;
  CopyGuid (&ImageInfo->ImageTypeId, PcdGetPtr (PcdVim3SystemFirmwareImageTypeIdGuid));
  ImageInfo->ImageId     = VIM3_FMP_IMAGE_ID;
  ImageInfo->ImageIdName = mImageIdName;
  ImageInfo->Version     = PcdGet32 (PcdVim3FirmwareRevision);
  ImageInfo->VersionName = NULL;
  ImageInfo->Size        = 0;

  ImageInfo->AttributesSupported = IMAGE_ATTRIBUTE_IMAGE_UPDATABLE |
                                   IMAGE_ATTRIBUTE_RESET_REQUIRED |
                                   IMAGE_ATTRIBUTE_IN_USE;
  ImageInfo->AttributesSetting = IMAGE_ATTRIBUTE_IMAGE_UPDATABLE |
                                 IMAGE_ATTRIBUTE_RESET_REQUIRED |
                                 IMAGE_ATTRIBUTE_IN_USE;
  ImageInfo->Compatibilities = 0;

  ImageInfo->LowestSupportedImageVersion = PcdGet32 (PcdVim3FirmwareLowestSupportedVersion);
  ImageInfo->LastAttemptVersion          = mLastAttempt.Version;
  ImageInfo->LastAttemptStatus           = mLastAttempt.Status;
  ImageInfo->HardwareInstance            = 0;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FmpGetImage (
  IN EFI_FIRMWARE_MANAGEMENT_PROTOCOL  *This,
  IN UINT8                             ImageIndex,
  IN OUT VOID                          *Image,
  IN OUT UINTN                         *ImageSize
  )
{
  //
  // Reading the running firmware back out is not part of the update path and
  // would need a second 4 MiB buffer at exactly the wrong moment.
  //
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
FmpCheckImage (
  IN EFI_FIRMWARE_MANAGEMENT_PROTOCOL  *This,
  IN UINT8                             ImageIndex,
  IN CONST VOID                        *Image,
  IN UINTN                             ImageSize,
  OUT UINT32                           *ImageUpdatable
  )
{
  if ((ImageUpdatable == NULL) || (ImageIndex != VIM3_FMP_IMAGE_INDEX)) {
    return EFI_INVALID_PARAMETER;
  }

  return ValidateImage (Image, ImageSize, ImageUpdatable);
}

STATIC
EFI_STATUS
EFIAPI
FmpSetImage (
  IN EFI_FIRMWARE_MANAGEMENT_PROTOCOL               *This,
  IN UINT8                                          ImageIndex,
  IN CONST VOID                                     *Image,
  IN UINTN                                          ImageSize,
  IN CONST VOID                                     *VendorCode,
  IN EFI_FIRMWARE_MANAGEMENT_UPDATE_IMAGE_PROGRESS  Progress,
  OUT CHAR16                                        **AbortReason
  )
{
  EFI_STATUS                Status;
  EFI_BLOCK_IO_PROTOCOL     *BlockIo;
  EFI_MMC_HOST_PROTOCOL     *MmcHost;
  MESON_NOR_FLASH_PROTOCOL  *Nor;
  BOOLEAN                   WriteNor;
  BOOLEAN                   NorOptIn;
  UINT32                    Updatable;

  if (AbortReason != NULL) {
    *AbortReason = NULL;
  }

  if (ImageIndex != VIM3_FMP_IMAGE_INDEX) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // BdsDxe arms a 5-minute watchdog for the boot that processes the
  // capsule, and the PIO NOR write alone can take that long.  A watchdog
  // reset mid-flash is the one outcome this driver must never allow.
  //
  gBS->SetWatchdogTimer (0, 0, 0, NULL);

  Status = ValidateImage (Image, ImageSize, &Updatable);
  if (EFI_ERROR (Status)) {
    SaveLastAttempt (
      PcdGet32 (PcdVim3FirmwareRevision),
      LAST_ATTEMPT_STATUS_ERROR_INVALID_FORMAT
      );
    return Status;
  }

  //
  // The NOR primary is rewritten when the running firmware actually came
  // from it - or when the operator has explicitly opted in from the setup
  // menu.  Absent that opt-in, an eMMC boot leaves the NOR alone: it belongs
  // to someone else (oowow on every beta board) or holds a corrupt image the
  // BootROM already fell through.
  //
  Nor         = NULL;
  NorOptIn    = SpiNorInstallRequested ();
  WriteNor    = BootedFromSpiNor () || NorOptIn;
  if (WriteNor) {
    Status = gBS->LocateProtocol (&gMesonNorFlashProtocolGuid, NULL, (VOID **)&Nor);
    if (EFI_ERROR (Status)) {
      //
      // Unreachable in practice: the depex chain (FMP wants variable
      // services, which want the SPIFC FVB) means the producer already
      // ran.  Fail rather than half-update - an eMMC-only write on a
      // SPI-booted board would report success while the running copy
      // stays old, the exact lie this driver exists to prevent.
      //
      DEBUG ((DEBUG_ERROR, "Vim3Fmp: SPI-booted but no NOR protocol: %r\n", Status));
      SaveLastAttempt (
        PcdGet32 (PcdVim3FirmwareRevision),
        VIM3_LAS_NO_NOR_PROTOCOL
        );
      return Status;
    }
  }

  Status = LocateEmmc (&BlockIo, &MmcHost);
  if (EFI_ERROR (Status)) {
    SaveLastAttempt (
      PcdGet32 (PcdVim3FirmwareRevision),
      VIM3_LAS_NO_EMMC
      );
    return Status;
  }

  if (Progress != NULL) {
    Progress (5);
  }

  //
  // boot0 first, proven by read-back, before boot1 is touched.  A power cut
  // in the middle therefore leaves at least one good copy for the BootROM.
  //
  Status = SelectPartition (MmcHost, PARTITION_ACCESS_BOOT0);
  if (!EFI_ERROR (Status)) {
    Status = WriteAndVerify (BlockIo, Image, ImageSize);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: boot0 update failed: %r\n", Status));
    SelectPartition (MmcHost, PARTITION_ACCESS_USER);
    SaveLastAttempt (
      PcdGet32 (PcdVim3FirmwareRevision),
      VIM3_LAS_BOOT0_FAILED
      );
    return Status;
  }

  if (Progress != NULL) {
    Progress (20);
  }

  Status = SelectPartition (MmcHost, PARTITION_ACCESS_BOOT1);
  if (!EFI_ERROR (Status)) {
    Status = WriteAndVerify (BlockIo, Image, ImageSize);
  }

  //
  // Always hand the card back to the user area: everything else in the
  // system, including whatever mounted the ESP, assumes it.
  //
  SelectPartition (MmcHost, PARTITION_ACCESS_USER);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: boot1 update failed: %r\n", Status));
    SaveLastAttempt (
      PcdGet32 (PcdVim3FirmwareRevision),
      VIM3_LAS_BOOT1_FAILED
      );
    return Status;
  }

  if (Progress != NULL) {
    Progress (35);
  }

  //
  // NOR primary last: from here on the rescue already holds the new
  // firmware, so a failure or power cut below degrades to the measured
  // fall-through path, not a brick.
  //
  if (WriteNor) {
    DEBUG ((
      DEBUG_INFO,
      "Vim3Fmp: %a - updating NOR primary (%lu bytes)\n",
      NorOptIn ? "operator opted in" : "SPI-booted",
      (UINT64)(ImageSize - VIM3_BLOCK_SIZE)
      ));
    Status = WriteAndVerifyNor (Nor, Image, ImageSize, Progress, 35, 99);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Vim3Fmp: NOR update failed: %r\n", Status));
      SaveLastAttempt (
        PcdGet32 (PcdVim3FirmwareRevision),
        VIM3_LAS_NOR_FAILED
        );
      //
      // Leave the opt-in request in place: the operator asked for a NOR
      // install and did not get one, so the next capsule should try again.
      //
      return Status;
    }

    //
    // Only now is it safe to point the BootROM at the NOR: the image is
    // written and read-back verified.  On the opt-in path this also consumes
    // the request.
    //
    if (NorOptIn) {
      Status = CompleteSpiNorInstall ();
      if (EFI_ERROR (Status)) {
        SaveLastAttempt (
          PcdGet32 (PcdVim3FirmwareRevision),
          VIM3_LAS_MCU_FLIP_FAILED
          );
        return Status;
      }
    }
  }

  if (Progress != NULL) {
    Progress (100);
  }

  SaveLastAttempt (
    PcdGet32 (PcdVim3FirmwareRevision),
    LAST_ATTEMPT_STATUS_SUCCESS
    );

  DEBUG ((
    DEBUG_INFO,
    "Vim3Fmp: update verified (eMMC rescue%a)\n",
    WriteNor ? " + NOR primary" : " only; eMMC boot"
    ));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FmpGetPackageInfo (
  IN EFI_FIRMWARE_MANAGEMENT_PROTOCOL  *This,
  OUT UINT32                           *PackageVersion,
  OUT CHAR16                           **PackageVersionName,
  OUT UINT32                           *PackageVersionNameMaxLen,
  OUT UINT64                           *AttributesSupported,
  OUT UINT64                           *AttributesSetting
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
FmpSetPackageInfo (
  IN EFI_FIRMWARE_MANAGEMENT_PROTOCOL  *This,
  IN CONST VOID                        *Image,
  IN UINTN                             ImageSize,
  IN CONST VOID                        *VendorCode,
  IN UINT32                            PackageVersion,
  IN CONST CHAR16                      *PackageVersionName
  )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
Vim3FmpDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINTN       Size;

  Size   = sizeof (mLastAttempt);
  Status = gRT->GetVariable (
                  LAST_ATTEMPT_VARIABLE,
                  &gVim3PlatformFormSetGuid,
                  NULL,
                  &Size,
                  &mLastAttempt
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (mLastAttempt))) {
    mLastAttempt.Version = 0;
    mLastAttempt.Status  = LAST_ATTEMPT_STATUS_SUCCESS;
  }

  mFmp.GetImageInfo      = FmpGetImageInfo;
  mFmp.GetImage          = FmpGetImage;
  mFmp.SetImage          = FmpSetImage;
  mFmp.CheckImage        = FmpCheckImage;
  mFmp.GetPackageInfo    = FmpGetPackageInfo;
  mFmp.SetPackageInfo    = FmpSetPackageInfo;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mFmpHandle,
                  &gEfiFirmwareManagementProtocolGuid,
                  &mFmp,
                  &gVim3SpiNorInstallProtocolGuid,
                  &mSpiNorInstall,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Fmp: cannot install FMP: %r\n", Status));
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "Vim3Fmp: system firmware FMP installed, revision 0x%08x\n",
    PcdGet32 (PcdVim3FirmwareRevision)
    ));
  return EFI_SUCCESS;
}
