/** @file
  eMMC partition-switch diagnostic for the capsule path.

  Build-only shell application - never in the production FV.  It replays
  Vim3FmpDxe's SelectPartition sequence step by step and reports each
  status, because on a SPI-booted board the capsule update fails at the
  boot0 stage while plain eMMC reads work, and RELEASE firmware carries
  no DEBUG output to say which command is at fault.

  Prime suspect: the hardcoded RCA of 1.  MmcDxe hands out relative card
  addresses from a counter, and if identification on a SPI-cold card took
  a retry the eMMC answers on 2 or higher - block reads (which use
  MmcDxe's own record) keep working while our CMD13 polls a dead address.
  So this probe walks CMD13 across RCAs 1..8 first.

  Everything is logged to probe.log on the eMMC ESP so the run can be
  driven blind from startup.nsh and read back from Linux afterwards.
  The partition-access dry run switches EXT_CSD PARTITION_CONFIG to
  boot0 and back to the user area; it writes no data blocks.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/MmcHost.h>
#include <Protocol/SimpleFileSystem.h>

#define EXTCSD_PARTITION_CONFIG  179
#define PARTITION_ACCESS_MASK    0x7
#define PARTITION_ACCESS_USER    0
#define PARTITION_ACCESS_BOOT0   1

#define EMMC_CMD6_ARG_ACCESS(x)   (((x) & 0x3) << 24)
#define EMMC_CMD6_ARG_INDEX(x)    (((x) & 0xFF) << 16)
#define EMMC_CMD6_ARG_VALUE(x)    (((x) & 0xFF) << 8)
#define EMMC_CMD6_ARG_CMD_SET(x)  (((x) & 0x7) << 0)

#define DEVICE_STATE(x)    (((x) >> 9) & 0xF)
#define EMMC_PRG_STATE     7
#define EMMC_SWITCH_ERROR  (1 << 7)

STATIC EFI_GUID  mMesonEmmcDevicePathGuid =
  { 0x9ec8ef97, 0x39d8, 0x48a3, { 0x84, 0xaf, 0xa1, 0x94, 0xd9, 0x92, 0xc6, 0x37 } };

STATIC CHAR16  mLog[8192];
STATIC UINTN   mLogLen;

STATIC
VOID
EFIAPI
Log (
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Line[200];

  VA_START (Args, Format);
  UnicodeVSPrint (Line, sizeof (Line), Format, Args);
  VA_END (Args);

  Print (L"%s", Line);

  if (mLogLen + StrLen (Line) + 1 < ARRAY_SIZE (mLog)) {
    StrCpyS (&mLog[mLogLen], ARRAY_SIZE (mLog) - mLogLen, Line);
    mLogLen += StrLen (Line);
  }
}

STATIC
BOOLEAN
IsEmmcVendorNode (
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  VENDOR_DEVICE_PATH  *Vendor;

  if ((DevicePath == NULL) ||
      (DevicePathType (DevicePath) != HARDWARE_DEVICE_PATH) ||
      (DevicePathSubType (DevicePath) != HW_VENDOR_DP))
  {
    return FALSE;
  }

  Vendor = (VENDOR_DEVICE_PATH *)DevicePath;
  return CompareGuid (&Vendor->Guid, &mMesonEmmcDevicePathGuid);
}

STATIC
EFI_STATUS
LocateEmmcHost (
  OUT EFI_MMC_HOST_PROTOCOL  **MmcHost
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                *Handles;
  UINTN                     HandleCount;
  UINTN                     Index;
  EFI_MMC_HOST_PROTOCOL     *Host;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;

  *MmcHost = NULL;

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
  return (*MmcHost != NULL) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
SendStatus (
  IN  EFI_MMC_HOST_PROTOCOL  *Host,
  IN  UINT32                 Rca,
  OUT UINT32                 *Response
  )
{
  EFI_STATUS  Status;

  *Response = 0;
  Status    = Host->SendCommand (Host, MMC_CMD13, Rca << 16);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Host->ReceiveResponse (Host, MMC_RESPONSE_TYPE_R1, Response);
}

STATIC
EFI_STATUS
WaitNotProgramming (
  IN EFI_MMC_HOST_PROTOCOL  *Host,
  IN UINT32                 Rca
  )
{
  EFI_STATUS  Status;
  UINT32      Data;
  UINTN       Retry;

  for (Retry = 0; Retry < 10000; Retry++) {
    Status = SendStatus (Host, Rca, &Data);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Data & EMMC_SWITCH_ERROR) != 0) {
      return EFI_DEVICE_ERROR;
    }

    if (DEVICE_STATE (Data) != EMMC_PRG_STATE) {
      return EFI_SUCCESS;
    }

    gBS->Stall (100);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
ReadExtCsdByte179 (
  IN  EFI_MMC_HOST_PROTOCOL  *Host,
  OUT UINT8                  *Config
  )
{
  EFI_STATUS  Status;
  UINT8       *ExtCsd;

  ExtCsd = AllocateZeroPool (512);
  if (ExtCsd == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = Host->SendCommand (Host, MMC_CMD8, 0);
  Log (L"  CMD8 (SEND_EXT_CSD)      : %r\n", Status);
  if (!EFI_ERROR (Status)) {
    Status = Host->ReadBlockData (Host, 0, 512, (UINT32 *)ExtCsd);
    Log (L"  ReadBlockData 512        : %r\n", Status);
  }

  if (!EFI_ERROR (Status)) {
    *Config = ExtCsd[EXTCSD_PARTITION_CONFIG];
    Log (L"  EXT_CSD[179]             : 0x%02x\n", *Config);
  }

  FreePool (ExtCsd);
  return Status;
}

STATIC
VOID
WriteLogToEmmcEsp (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  EFI_DEVICE_PATH_PROTOCOL         *DevicePath;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  UINTN                            Size;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiDevicePathProtocolGuid,
                    (VOID **)&DevicePath
                    );
    if (EFI_ERROR (Status) || !IsEmmcVendorNode (DevicePath)) {
      continue;
    }

    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Sfs
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Sfs->OpenVolume (Sfs, &Root);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Root->Open (
                     Root,
                     &File,
                     L"probe.log",
                     EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                     0
                     );
    if (!EFI_ERROR (Status)) {
      Size = mLogLen * sizeof (CHAR16);
      File->Write (File, &Size, mLog);
      File->Close (File);
      Print (L"probe.log written to eMMC ESP\n");
    }

    Root->Close (Root);
    break;
  }

  FreePool (Handles);
}

EFI_STATUS
EFIAPI
Vim3EmmcProbeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  EFI_MMC_HOST_PROTOCOL  *Host;
  UINT32                 Rca;
  UINT32                 Response;
  UINT32                 GoodRca;
  UINT8                  Config;
  UINT32                 Argument;

  Log (L"Vim3EmmcProbe: capsule eMMC path diagnostic\n");

  Status = LocateEmmcHost (&Host);
  Log (L"locate eMMC MMC host       : %r\n", Status);
  if (EFI_ERROR (Status)) {
    WriteLogToEmmcEsp ();
    return Status;
  }

  //
  // Which RCA does the card actually answer on?
  //
  GoodRca = 0;
  for (Rca = 0; Rca <= 8; Rca++) {
    Status = SendStatus (Host, Rca, &Response);
    Log (L"  CMD13 rca=%u             : %r resp=0x%08x\n", Rca, Status, Response);
    if (!EFI_ERROR (Status) && (Response != 0) && (GoodRca == 0) && (Rca != 0)) {
      GoodRca = Rca;
    }
  }

  if (GoodRca == 0) {
    Log (L"no RCA answered; stopping before EXT_CSD\n");
    WriteLogToEmmcEsp ();
    return EFI_DEVICE_ERROR;
  }

  Log (L"using rca=%u\n", GoodRca);

  Log (L"EXT_CSD read:\n");
  Status = ReadExtCsdByte179 (Host, &Config);
  if (EFI_ERROR (Status)) {
    WriteLogToEmmcEsp ();
    return Status;
  }

  //
  // Dry-run the partition switch: to boot0, verify, back to user.
  // No data blocks are written.
  //
  Argument = EMMC_CMD6_ARG_ACCESS (3) |
             EMMC_CMD6_ARG_INDEX (EXTCSD_PARTITION_CONFIG) |
             EMMC_CMD6_ARG_VALUE ((UINT8)((Config & ~PARTITION_ACCESS_MASK) | PARTITION_ACCESS_BOOT0)) |
             EMMC_CMD6_ARG_CMD_SET (1);
  Status = Host->SendCommand (Host, MMC_CMD6, Argument);
  Log (L"CMD6 -> boot0              : %r\n", Status);
  if (!EFI_ERROR (Status)) {
    Status = WaitNotProgramming (Host, GoodRca);
    Log (L"CMD13 poll after CMD6      : %r\n", Status);
  }

  Log (L"EXT_CSD re-read (expect boot0 bit):\n");
  ReadExtCsdByte179 (Host, &Config);

  Argument = EMMC_CMD6_ARG_ACCESS (3) |
             EMMC_CMD6_ARG_INDEX (EXTCSD_PARTITION_CONFIG) |
             EMMC_CMD6_ARG_VALUE ((UINT8)(Config & ~PARTITION_ACCESS_MASK)) |
             EMMC_CMD6_ARG_CMD_SET (1);
  Status = Host->SendCommand (Host, MMC_CMD6, Argument);
  Log (L"CMD6 -> user               : %r\n", Status);
  if (!EFI_ERROR (Status)) {
    Status = WaitNotProgramming (Host, GoodRca);
    Log (L"CMD13 poll after CMD6      : %r\n", Status);
  }

  Log (L"EXT_CSD re-read (expect user):\n");
  ReadExtCsdByte179 (Host, &Config);

  Log (L"done\n");
  WriteLogToEmmcEsp ();
  return EFI_SUCCESS;
}
