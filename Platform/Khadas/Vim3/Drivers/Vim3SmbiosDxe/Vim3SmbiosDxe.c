/** @file
  Truthful SMBIOS records for the Khadas VIM3 Pro.

  The serial number and system UUID are derived from the SoC's real unique id
  (secure-monitor chip serial, or the eFUSE MAC as a fallback) - not random,
  and left empty if the SoC yields no unique id.  The Type 19 records describe
  the two actual UEFI system-memory ranges on either side of the secure carveout.
  Cache geometry is read from CCSIDR on the boot Cortex-A53 instead of using
  disputed, implementation-dependent cache sizes from secondary sources.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <IndustryStandard/ArmCache.h>
#include <IndustryStandard/SmBios.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/Vim3MesonSmLib.h>
#include <Guid/FdtHob.h>
#include <Protocol/Smbios.h>

#include <MesonG12B.h>

#define VIM3_DRAM_PHYSICAL_MIB  4096U
#define VIM3_DRAM_LOW_BASE      0x00000000ULL
#define VIM3_DRAM_LOW_TOP       0x05000000ULL

//
// BL2 programs both cluster PLLs to 1200 MHz before BL33 runs and firmware
// never rescales them ("A53 clk: 1200 MHz" / "A73 clk: 1200 MHz" on every
// vendor BL2 boot transcript).  The external oscillator is the 24 MHz xtal.
//
#define VIM3_BOOT_CPU_SPEED_MHZ  1200U
#define VIM3_XTAL_MHZ            24U

STATIC
EFI_STATUS
AddRecord (
  IN     EFI_SMBIOS_PROTOCOL    *Smbios,
  IN     VOID                   *Formatted,
  IN     UINTN                  FormattedSize,
  IN     CONST CHAR8 * CONST    *Strings OPTIONAL,
  IN OUT EFI_SMBIOS_HANDLE      *Handle
  )
{
  EFI_SMBIOS_TABLE_HEADER  *Record;
  EFI_STATUS               Status;
  UINTN                    Index;
  UINTN                    RecordSize;
  CHAR8                    *Walker;

  RecordSize = FormattedSize + 2;
  if (Strings != NULL) {
    for (Index = 0; Strings[Index] != NULL; Index++) {
      RecordSize += AsciiStrSize (Strings[Index]);
    }
  }

  Record = AllocateZeroPool (RecordSize);
  if (Record == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Record, Formatted, FormattedSize);
  Record->Length = (UINT8)FormattedSize;
  Record->Handle = *Handle;

  Walker = (CHAR8 *)Record + FormattedSize;
  if (Strings != NULL) {
    for (Index = 0; Strings[Index] != NULL; Index++) {
      AsciiStrCpyS (Walker, RecordSize - (Walker - (CHAR8 *)Record), Strings[Index]);
      Walker += AsciiStrSize (Strings[Index]);
    }
  }

  Status = Smbios->Add (Smbios, NULL, Handle, Record);
  FreePool (Record);
  return Status;
}

STATIC
EFI_STATUS
AddRecordAutoHandle (
  IN  EFI_SMBIOS_PROTOCOL  *Smbios,
  IN  VOID                 *Formatted,
  IN  UINTN                FormattedSize,
  IN  CONST CHAR8 * CONST  *Strings OPTIONAL,
  OUT EFI_SMBIOS_HANDLE    *AssignedHandle OPTIONAL
  )
{
  EFI_SMBIOS_HANDLE  Handle;
  EFI_STATUS         Status;

  Handle = SMBIOS_HANDLE_PI_RESERVED;
  Status = AddRecord (Smbios, Formatted, FormattedSize, Strings, &Handle);
  if (!EFI_ERROR (Status) && (AssignedHandle != NULL)) {
    *AssignedHandle = Handle;
  }

  return Status;
}

//
// Build a stable board serial string + system UUID from the SoC's unique ID:
// the secure-monitor chip serial, or the eFUSE MAC as a fallback.  Both come
// from the SoC (not the MCU, whose USID reads zero on this board).  Returns
// FALSE - leaving serial/UUID empty - only if neither source yields an ID.
//
STATIC
BOOLEAN
BuildBoardIdentity (
  OUT CHAR8  *Serial,
  IN  UINTN  SerialSize,
  OUT GUID   *Uuid
  )
{
  UINT8  Id[12];
  UINT8  Uuid16[16];
  UINTN  IdLen;
  UINTN  Index;

  if (!EFI_ERROR (MesonSmGetChipSerial (Id))) {
    IdLen = 12;
  } else if (!EFI_ERROR (MesonSmGetMac (Id))) {
    IdLen = 6;
  } else {
    return FALSE;
  }

  for (Index = 0; Index < IdLen; Index++) {
    AsciiSPrint (&Serial[Index * 2], SerialSize - (Index * 2), "%02X", Id[Index]);
  }

  //
  // Deterministic vendor UUID: a "VIM3" prefix plus the unique id, with the
  // RFC-4122 version (5) and variant nibbles fixed.  Reproducible per board,
  // not random.
  //
  ZeroMem (Uuid16, sizeof (Uuid16));
  Uuid16[0] = 'V';
  Uuid16[1] = 'I';
  Uuid16[2] = 'M';
  Uuid16[3] = '3';
  for (Index = 0; (Index < IdLen) && ((4 + Index) < sizeof (Uuid16)); Index++) {
    Uuid16[4 + Index] = Id[Index];
  }

  Uuid16[6] = (UINT8)((Uuid16[6] & 0x0F) | 0x50);
  Uuid16[8] = (UINT8)((Uuid16[8] & 0x3F) | 0x80);
  CopyMem (Uuid, Uuid16, sizeof (Uuid16));

  return TRUE;
}

STATIC
EFI_STATUS
AddIdentityRecords (
  IN EFI_SMBIOS_PROTOCOL  *Smbios
  )
{
  SMBIOS_TABLE_TYPE0  Type0;
  SMBIOS_TABLE_TYPE1  Type1;
  SMBIOS_TABLE_TYPE2  Type2;
  SMBIOS_TABLE_TYPE3  Type3;
  CHAR8               Vendor[64];
  CHAR8               Version[96];
  CHAR8               ReleaseDate[32];
  CONST CHAR8         *Type0Strings[4];
  CONST CHAR8         *Type1Strings[] = {
    "Khadas",
    "VIM3 Pro",
    "VIM3",
    NULL,        // slot 4: board serial (filled if the SoC has a unique id)
    NULL
  };
  CONST CHAR8         *Type2Strings[] = {
    "Khadas",
    "VIM3 Pro",
    NULL,        // slot 3: board serial
    NULL
  };
  CHAR8               BoardSerial[40];
  BOOLEAN             HaveIdentity;
  CONST CHAR8         *Type3Strings[] = {
    "Khadas",
    NULL
  };
  EFI_SMBIOS_HANDLE   ChassisHandle;
  EFI_STATUS          Status;

  UnicodeStrToAsciiStrS (
    (CHAR16 *)FixedPcdGetPtr (PcdFirmwareVendor),
    Vendor,
    ARRAY_SIZE (Vendor)
    );
  UnicodeStrToAsciiStrS (
    (CHAR16 *)FixedPcdGetPtr (PcdFirmwareVersionString),
    Version,
    ARRAY_SIZE (Version)
    );
  UnicodeStrToAsciiStrS (
    (CHAR16 *)FixedPcdGetPtr (PcdFirmwareReleaseDateString),
    ReleaseDate,
    ARRAY_SIZE (ReleaseDate)
    );
  Type0Strings[0] = Vendor;
  Type0Strings[1] = Version;
  Type0Strings[2] = ReleaseDate;
  Type0Strings[3] = NULL;

  ZeroMem (&Type0, sizeof (Type0));
  Type0.Hdr.Type                              = EFI_SMBIOS_TYPE_BIOS_INFORMATION;
  Type0.Vendor                                = 1;
  Type0.BiosVersion                           = 2;
  Type0.BiosReleaseDate                       = 3;
  Type0.BiosSize                              = (SIZE_4MB / SIZE_64KB) - 1;
  Type0.BiosCharacteristics.BiosCharacteristicsNotSupported = 1;
  Type0.BIOSCharacteristicsExtensionBytes[1]  = BIT3; // UEFI Specification supported.
  Type0.EmbeddedControllerFirmwareMajorRelease = 0xFF;
  Type0.EmbeddedControllerFirmwareMinorRelease = 0xFF;
  Status = AddRecordAutoHandle (
             Smbios,
             &Type0,
             sizeof (Type0),
             Type0Strings,
             NULL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Type1, sizeof (Type1));
  Type1.Hdr.Type     = EFI_SMBIOS_TYPE_SYSTEM_INFORMATION;
  Type1.Manufacturer = 1;
  Type1.ProductName  = 2;
  Type1.SKUNumber    = 3;
  Type1.WakeUpType   = SystemWakeupTypePowerSwitch;
  HaveIdentity       = BuildBoardIdentity (BoardSerial, sizeof (BoardSerial), &Type1.Uuid);
  if (HaveIdentity) {
    Type1Strings[3]    = BoardSerial;
    Type1.SerialNumber = 4;
  }

  Status = AddRecordAutoHandle (
             Smbios,
             &Type1,
             sizeof (Type1),
             Type1Strings,
             NULL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Type3, sizeof (Type3));
  Type3.Hdr.Type       = EFI_SMBIOS_TYPE_SYSTEM_ENCLOSURE;
  Type3.Manufacturer   = 1;
  Type3.Type           = MiscChassisEmbeddedPc;
  Type3.BootupState    = ChassisStateSafe;
  Type3.PowerSupplyState = ChassisStateSafe;
  Type3.ThermalState   = ChassisStateSafe;
  Type3.SecurityStatus = ChassisSecurityStatusNone;
  Status = AddRecordAutoHandle (
             Smbios,
             &Type3,
             sizeof (Type3),
             Type3Strings,
             &ChassisHandle
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Type2, sizeof (Type2));
  Type2.Hdr.Type              = EFI_SMBIOS_TYPE_BASEBOARD_INFORMATION;
  Type2.Manufacturer          = 1;
  Type2.ProductName           = 2;
  Type2.FeatureFlag.Motherboard = 1;
  Type2.ChassisHandle         = ChassisHandle;
  Type2.BoardType             = BaseBoardTypeMotherBoard;
  if (HaveIdentity) {
    Type2Strings[2]    = BoardSerial;
    Type2.SerialNumber = 3;
  }

  return AddRecordAutoHandle (
           Smbios,
           &Type2,
           sizeof (Type2),
           Type2Strings,
           NULL
           );
}

STATIC
UINT64
GetBootClusterL2Size (
  OUT UINT32  *Ways
  )
{
  CCSIDR_DATA  Ccsidr;
  CSSELR_DATA  Csselr;
  UINT64       Sets;
  UINT64       LineSize;

  ZeroMem (&Csselr, sizeof (Csselr));
  Csselr.Bits.Level = 1;
  Csselr.Bits.InD   = CsselrCacheTypeDataOrUnified;
  Ccsidr.Data       = ReadCCSIDR (Csselr.Data);

  if (ArmHasCcidx ()) {
    *Ways    = (UINT32)Ccsidr.BitsCcidxAA64.Associativity + 1;
    Sets     = Ccsidr.BitsCcidxAA64.NumSets + 1;
    LineSize = LShiftU64 (1, Ccsidr.BitsCcidxAA64.LineSize + 4);
  } else {
    *Ways    = (UINT32)Ccsidr.BitsNonCcidx.Associativity + 1;
    Sets     = Ccsidr.BitsNonCcidx.NumSets + 1;
    LineSize = LShiftU64 (1, Ccsidr.BitsNonCcidx.LineSize + 4);
  }

  return LineSize * *Ways * Sets;
}

STATIC
UINT8
SmbiosAssociativity (
  IN UINT32  Ways
  )
{
  switch (Ways) {
    case 2:  return CacheAssociativity2Way;
    case 4:  return CacheAssociativity4Way;
    case 8:  return CacheAssociativity8Way;
    case 12: return CacheAssociativity12Way;
    case 16: return CacheAssociativity16Way;
    case 20: return CacheAssociativity20Way;
    case 24: return CacheAssociativity24Way;
    case 32: return CacheAssociativity32Way;
    case 48: return CacheAssociativity48Way;
    case 64: return CacheAssociativity64Way;
    default: return CacheAssociativityOther;
  }
}

STATIC
EFI_STATUS
AddProcessorRecord (
  IN EFI_SMBIOS_PROTOCOL  *Smbios,
  IN CONST CHAR8          *Socket,
  IN CONST CHAR8          *Version,
  IN UINT16               MaximumSpeed,
  IN UINT8                CoreCount,
  IN EFI_SMBIOS_HANDLE    L2Handle
  )
{
  SMBIOS_TABLE_TYPE4  Type4;
  CONST CHAR8         *Strings[5];

  Strings[0] = Socket;
  Strings[1] = "Amlogic";
  Strings[2] = Version;
  Strings[3] = "A311D";
  Strings[4] = NULL;

  ZeroMem (&Type4, sizeof (Type4));
  Type4.Hdr.Type              = EFI_SMBIOS_TYPE_PROCESSOR_INFORMATION;
  Type4.Socket                = 1;
  Type4.ProcessorType         = CentralProcessor;
  Type4.ProcessorFamily       = ProcessorFamilyIndicatorFamily2;
  Type4.ProcessorManufacturer = 2;
  Type4.ProcessorVersion      = 3;
  Type4.PartNumber            = 4;
  Type4.ExternalClock         = VIM3_XTAL_MHZ;
  Type4.MaxSpeed              = MaximumSpeed;
  Type4.CurrentSpeed          = VIM3_BOOT_CPU_SPEED_MHZ;
  Type4.Status                = 0x41; // Populated and enabled.
  Type4.ProcessorUpgrade      = ProcessorUpgradeNone;
  Type4.L1CacheHandle         = 0xFFFF;
  Type4.L2CacheHandle         = L2Handle;
  Type4.L3CacheHandle         = 0xFFFF;
  Type4.CoreCount             = CoreCount;
  Type4.EnabledCoreCount      = CoreCount;
  Type4.ThreadCount           = CoreCount;
  Type4.ProcessorCharacteristics = BIT2 | BIT3 | BIT5 | BIT6;
  Type4.ProcessorFamily2      = ProcessorFamilyARMv8;
  Type4.CoreCount2            = CoreCount;
  Type4.EnabledCoreCount2     = CoreCount;
  Type4.ThreadCount2          = CoreCount;
  Type4.ThreadEnabled         = CoreCount;

  return AddRecordAutoHandle (
           Smbios,
           &Type4,
           sizeof (Type4),
           Strings,
           NULL
           );
}

STATIC
EFI_STATUS
AddL2CacheRecord (
  IN  EFI_SMBIOS_PROTOCOL  *Smbios,
  IN  CONST CHAR8          *Designation,
  IN  UINT64               CacheKiB,
  IN  UINT32               Ways,
  OUT EFI_SMBIOS_HANDLE    *L2Handle
  )
{
  SMBIOS_TABLE_TYPE7  Type7;
  CONST CHAR8         *CacheStrings[2];

  if ((CacheKiB == 0) || (CacheKiB > SMBIOS_CACHE_SIZE_MAX_SIZE_1K_GRANULARITY)) {
    return EFI_DEVICE_ERROR;
  }

  CacheStrings[0] = Designation;
  CacheStrings[1] = NULL;

  ZeroMem (&Type7, sizeof (Type7));
  Type7.Hdr.Type          = EFI_SMBIOS_TYPE_CACHE_INFORMATION;
  Type7.SocketDesignation = 1;
  Type7.CacheConfiguration = (3U << 8) | BIT7 | 1U;
  Type7.MaximumCacheSize.Size = (UINT16)CacheKiB;
  Type7.InstalledSize.Size    = (UINT16)CacheKiB;
  Type7.SupportedSRAMType.Unknown = 1;
  Type7.CurrentSRAMType.Unknown   = 1;
  Type7.ErrorCorrectionType       = CacheErrorUnknown;
  Type7.SystemCacheType           = CacheTypeUnified;
  Type7.Associativity             = SmbiosAssociativity (Ways);
  Type7.MaximumCacheSize2.Size    = (UINT32)CacheKiB;
  Type7.InstalledSize2.Size       = (UINT32)CacheKiB;
  return AddRecordAutoHandle (
           Smbios,
           &Type7,
           sizeof (Type7),
           CacheStrings,
           L2Handle
           );
}

//
// Return the highest opp-hz of a top-level operating-points-v2 table in the
// published device tree, in MHz.  The DTB is read from the PEI FDT HOB (the
// same source FdtClientDxe publishes) so the answer does not depend on
// whether the DT configuration table is exposed to the OS.  Falls back to
// the caller's constant when the table cannot be parsed.
//
STATIC
UINT16
GetClusterMaxSpeedMhz (
  IN CONST CHAR8  *OppTableName,
  IN UINT16       FallbackMhz
  )
{
  VOID         *Hob;
  CONST VOID   *Fdt;
  INT32        TableNode;
  INT32        OppNode;
  CONST VOID   *Prop;
  INT32        PropSize;
  UINT64       Hz;
  UINT64       MaxHz;

  Hob = GetFirstGuidHob (&gFdtHobGuid);
  if (Hob == NULL) {
    return FallbackMhz;
  }

  Fdt = (CONST VOID *)(UINTN)*(UINT64 *)GET_GUID_HOB_DATA (Hob);
  if ((Fdt == NULL) || (FdtCheckHeader (Fdt) != 0)) {
    return FallbackMhz;
  }

  TableNode = FdtSubnodeOffset (Fdt, 0, OppTableName);
  if (TableNode < 0) {
    return FallbackMhz;
  }

  MaxHz = 0;
  for (OppNode = FdtFirstSubnode (Fdt, TableNode);
       OppNode >= 0;
       OppNode = FdtNextSubnode (Fdt, OppNode))
  {
    Prop = FdtGetProp (Fdt, OppNode, "opp-hz", &PropSize);
    if ((Prop == NULL) || (PropSize != sizeof (UINT64))) {
      continue;
    }

    Hz = SwapBytes64 (ReadUnaligned64 ((CONST UINT64 *)Prop));
    if (Hz > MaxHz) {
      MaxHz = Hz;
    }
  }

  if ((MaxHz == 0) || (DivU64x32 (MaxHz, 1000000) > MAX_UINT16)) {
    return FallbackMhz;
  }

  return (UINT16)DivU64x32 (MaxHz, 1000000);
}

STATIC
EFI_STATUS
AddProcessorRecords (
  IN EFI_SMBIOS_PROTOCOL  *Smbios
  )
{
  EFI_SMBIOS_HANDLE  A53L2Handle;
  EFI_SMBIOS_HANDLE  A73L2Handle;
  EFI_STATUS         Status;
  UINT64             CacheKiB;
  UINT32             Ways;
  UINT16             A53MaxMhz;
  UINT16             A73MaxMhz;
  CHAR8              A53Version[48];
  CHAR8              A73Version[48];

  A53MaxMhz = GetClusterMaxSpeedMhz ("opp-table-0", 1800);
  A73MaxMhz = GetClusterMaxSpeedMhz ("opp-table-1", 2200);

  //
  // The front-page banner (carried UiApp patch 0004) renders a header line
  // built from the manufacturer and part-number strings, then one bullet
  // line per cluster from these version strings.
  //
  AsciiSPrint (
    A53Version,
    sizeof (A53Version),
    "2x Cortex-A53 @%u.%uGHz",
    A53MaxMhz / 1000U,
    (A53MaxMhz % 1000U) / 100U
    );
  AsciiSPrint (
    A73Version,
    sizeof (A73Version),
    "4x Cortex-A73 @%u.%uGHz",
    A73MaxMhz / 1000U,
    (A73MaxMhz % 1000U) / 100U
    );

  CacheKiB = GetBootClusterL2Size (&Ways) / SIZE_1KB;
  Status   = AddL2CacheRecord (
               Smbios,
               "Cortex-A53 cluster unified L2",
               CacheKiB,
               Ways,
               &A53L2Handle
               );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AddProcessorRecord (
             Smbios,
             "CPU cluster 0",
             A53Version,
             A53MaxMhz,
             2,
             A53L2Handle
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The A73 cluster geometry was captured from CCSIDR_EL1 on all four A73
  // cores under stock Debian 6.12.94. Keep this record synchronized with the
  // measured PPTT data.
  //
  Status = AddL2CacheRecord (
             Smbios,
             "Cortex-A73 cluster unified L2",
             SIZE_1MB / SIZE_1KB,
             16,
             &A73L2Handle
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return AddProcessorRecord (
           Smbios,
           "CPU cluster 1",
           A73Version,
           A73MaxMhz,
           4,
           A73L2Handle
           );
}

STATIC
EFI_STATUS
AddMemoryRecords (
  IN EFI_SMBIOS_PROTOCOL  *Smbios
  )
{
  SMBIOS_TABLE_TYPE16  Type16;
  SMBIOS_TABLE_TYPE17  Type17;
  SMBIOS_TABLE_TYPE19  Type19;
  CONST CHAR8          *Type17Strings[] = {
    "System board",
    "LPDDR4",
    NULL
  };
  EFI_SMBIOS_HANDLE    ArrayHandle;
  EFI_STATUS           Status;

  ZeroMem (&Type16, sizeof (Type16));
  Type16.Hdr.Type                 = EFI_SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY;
  Type16.Location                 = MemoryArrayLocationSystemBoard;
  Type16.Use                      = MemoryArrayUseSystemMemory;
  Type16.MemoryErrorCorrection    = MemoryErrorCorrectionNone;
  Type16.MaximumCapacity          = VIM3_DRAM_PHYSICAL_MIB * SIZE_1KB;
  Type16.MemoryErrorInformationHandle = 0xFFFE;
  Type16.NumberOfMemoryDevices    = 1;
  Status = AddRecordAutoHandle (
             Smbios,
             &Type16,
             sizeof (Type16),
             NULL,
             &ArrayHandle
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Type17, sizeof (Type17));
  Type17.Hdr.Type                     = EFI_SMBIOS_TYPE_MEMORY_DEVICE;
  Type17.MemoryArrayHandle            = ArrayHandle;
  Type17.MemoryErrorInformationHandle = 0xFFFE;
  Type17.TotalWidth                   = 0xFFFF;
  Type17.DataWidth                    = 0xFFFF;
  Type17.Size                         = VIM3_DRAM_PHYSICAL_MIB;
  Type17.FormFactor                   = MemoryFormFactorRowOfChips;
  Type17.DeviceLocator                = 1;
  Type17.BankLocator                  = 2;
  Type17.MemoryType                   = MemoryTypeLpddr4;
  Type17.TypeDetail.Synchronous       = 1;
  Type17.MemoryTechnology             = MemoryTechnologyDram;
  Type17.MemoryOperatingModeCapability.Bits.VolatileMemory = 1;
  Type17.VolatileSize                 = LShiftU64 (VIM3_DRAM_PHYSICAL_MIB, 20);
  Status = AddRecordAutoHandle (
             Smbios,
             &Type17,
             sizeof (Type17),
             Type17Strings,
             NULL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Type19, sizeof (Type19));
  Type19.Hdr.Type          = EFI_SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS;
  Type19.StartingAddress   = (UINT32)(VIM3_DRAM_LOW_BASE / SIZE_1KB);
  Type19.EndingAddress     = (UINT32)((VIM3_DRAM_LOW_TOP - 1) / SIZE_1KB);
  Type19.MemoryArrayHandle = ArrayHandle;
  Type19.PartitionWidth    = 1;
  Status = AddRecordAutoHandle (
             Smbios,
             &Type19,
             sizeof (Type19),
             NULL,
             NULL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Type19, sizeof (Type19));
  Type19.Hdr.Type          = EFI_SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS;
  Type19.StartingAddress   = (UINT32)(MESON_G12B_DRAM_HIGH_BASE / SIZE_1KB);
  Type19.EndingAddress     = (UINT32)((MESON_G12B_DRAM_TOP - 1) / SIZE_1KB);
  Type19.MemoryArrayHandle = ArrayHandle;
  Type19.PartitionWidth    = 1;
  return AddRecordAutoHandle (
           Smbios,
           &Type19,
           sizeof (Type19),
           NULL,
           NULL
           );
}

EFI_STATUS
EFIAPI
Vim3SmbiosEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_SMBIOS_PROTOCOL  *Smbios;
  SMBIOS_TABLE_TYPE32  Type32;
  EFI_STATUS           Status;

  Status = gBS->LocateProtocol (
                  &gEfiSmbiosProtocolGuid,
                  NULL,
                  (VOID **)&Smbios
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AddIdentityRecords (Smbios);
  if (!EFI_ERROR (Status)) {
    Status = AddProcessorRecords (Smbios);
  }

  if (!EFI_ERROR (Status)) {
    Status = AddMemoryRecords (Smbios);
  }

  if (!EFI_ERROR (Status)) {
    ZeroMem (&Type32, sizeof (Type32));
    Type32.Hdr.Type   = EFI_SMBIOS_TYPE_SYSTEM_BOOT_INFORMATION;
    Type32.BootStatus = BootInformationStatusNoError;
    Status = AddRecordAutoHandle (
               Smbios,
               &Type32,
               sizeof (Type32),
               NULL,
               NULL
               );
  }

  DEBUG ((DEBUG_INFO, "VIM3 SMBIOS installation: %r\n", Status));
  return Status;
}
