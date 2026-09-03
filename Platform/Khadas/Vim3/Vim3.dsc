#
#  Khadas VIM3 (Amlogic A311D/G12B) platform description.
#
#  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#

[Defines]
  PLATFORM_NAME                  = KhadasVim3
  PLATFORM_GUID                  = 0d5bf2db-d91b-49e7-a7db-8c97ddf70990
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x0001001B
  OUTPUT_DIRECTORY               = Build/KhadasVim3-AARCH64
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Platform/Khadas/Vim3/Vim3.fdf

  DEFINE TTY_TERMINAL            = TRUE
  DEFINE SECURE_BOOT_ENABLE      = TRUE
  DEFINE NETWORK_ENABLE                  = TRUE
  DEFINE NETWORK_SNP_ENABLE              = FALSE
  DEFINE NETWORK_IP4_ENABLE              = TRUE
  DEFINE NETWORK_IP6_ENABLE              = TRUE
  DEFINE NETWORK_VLAN_ENABLE             = TRUE
  DEFINE NETWORK_PXE_BOOT_ENABLE         = TRUE
  DEFINE NETWORK_HTTP_BOOT_ENABLE        = TRUE
  DEFINE NETWORK_TLS_ENABLE              = FALSE
  DEFINE NETWORK_ALLOW_HTTP_CONNECTIONS  = TRUE
  DEFINE NETWORK_ISCSI_ENABLE            = FALSE
  DEFINE TPM2_ENABLE             = FALSE
  DEFINE FIRMWARE_VERSION        = phase6-release
  DEFINE FIRMWARE_RELEASE_DATE   = 07/29/2026
  DEFINE FIRMWARE_REVISION       = 1

!include ArmVirtPkg/ArmVirtStackCookies.dsc.inc
!include MdePkg/MdeLibs.dsc.inc
!include ArmVirtPkg/ArmVirt.dsc.inc

[LibraryClasses.common]
  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf
  ArmMmuLib|UefiCpuPkg/Library/ArmMmuLib/ArmMmuBaseLib.inf
  ArmPlatformLib|ArmPlatformPkg/Library/ArmPlatformLibNull/ArmPlatformLibNull.inf
  ArmVirtMemInfoLib|Silicon/Amlogic/MesonG12B/Library/MesonVirtMemInfoLib/MesonVirtMemInfoLib.inf
  MemoryInitPeiLib|Silicon/Amlogic/MesonG12B/Library/MesonMemoryInitPeiLib/MesonMemoryInitPeiLib.inf
  PlatformPeiLib|Platform/Khadas/Vim3/Library/Vim3PlatformPeiLib/Vim3PlatformPeiLib.inf
  SerialPortLib|Silicon/Amlogic/MesonG12B/Library/MesonSerialPortLib/MesonSerialPortLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf
  ArmMonitorLib|ArmPkg/Library/ArmMonitorLib/ArmMonitorLib.inf
  ResetSystemLib|ArmPkg/Library/ArmPsciResetSystemLib/ArmPsciResetSystemLib.inf
  RealTimeClockLib|Platform/Khadas/Vim3/Library/Vim3RealTimeClockLib/Vim3RealTimeClockLib.inf
  Vim3AoI2cLib|Platform/Khadas/Vim3/Library/Vim3AoI2cLib/Vim3AoI2cLib.inf
  Vim3McuLib|Platform/Khadas/Vim3/Library/Vim3McuLib/Vim3McuLib.inf
  Vim3MesonSmLib|Platform/Khadas/Vim3/Library/Vim3MesonSmLib/Vim3MesonSmLib.inf
  TimeBaseLib|EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.inf
  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibFmp/DxeCapsuleLib.inf
  BmpSupportLib|MdeModulePkg/Library/BaseBmpSupportLib/BaseBmpSupportLib.inf
  DisplayUpdateProgressLib|MdeModulePkg/Library/DisplayUpdateProgressLibGraphics/DisplayUpdateProgressLibGraphics.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  UefiBootManagerLib|MdeModulePkg/Library/UefiBootManagerLib/UefiBootManagerLib.inf
  PlatformBootManagerLib|Platform/Khadas/Vim3/Library/Vim3BootManagerLib/Vim3BootManagerLib.inf
  BootLogoLib|MdeModulePkg/Library/BootLogoLib/BootLogoLib.inf
  CustomizedDisplayLib|MdeModulePkg/Library/CustomizedDisplayLib/CustomizedDisplayLib.inf
  FrameBufferBltLib|MdeModulePkg/Library/FrameBufferBltLib/FrameBufferBltLib.inf
  FileExplorerLib|MdeModulePkg/Library/FileExplorerLib/FileExplorerLib.inf
  DmaLib|EmbeddedPkg/Library/NonCoherentDmaLib/NonCoherentDmaLib.inf
  PciHostBridgeLib|Platform/Khadas/Vim3/Library/Vim3PciHostBridgeLib/Vim3PciHostBridgeLib.inf
  PciSegmentLib|Silicon/Amlogic/MesonG12B/Library/MesonPciSegmentLib/MesonPciSegmentLib.inf
  MesonPcieMuxLib|Platform/Khadas/Vim3/Library/Vim3PcieMuxLib/Vim3PcieMuxLib.inf
  NonDiscoverableDeviceRegistrationLib|MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.inf
  TpmMeasurementLib|MdeModulePkg/Library/TpmMeasurementLibNull/TpmMeasurementLibNull.inf
  TpmPlatformHierarchyLib|SecurityPkg/Library/PeiDxeTpmPlatformHierarchyLibNull/PeiDxeTpmPlatformHierarchyLib.inf

[LibraryClasses.common.SEC]
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf
  SerialPortLib|Silicon/Amlogic/MesonG12B/Library/MesonSerialPortLib/MesonSerialPortLib.inf

[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  # Runtime services execute under the OS virtual address map.  The ordinary
  # serial DebugLib uses the UART's physical MMIO address and can page fault
  # when ResetSystemLib logs a PSCI fallback after ExitBootServices().
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  # ArmVirt.dsc.inc scopes DxeCapsuleLibNull to runtime drivers, which
  # silently overrides the common DxeCapsuleLibFmp binding above - so
  # CapsuleRuntimeDxe's UpdateCapsule()/QueryCapsuleCapabilities() rejected
  # every capsule with a printless EFI_UNSUPPORTED (the null SupportCapsuleImage),
  # while the BDS-side CoD machinery worked.  Cost a full hardware iteration
  # to find.  DxeRuntimeCapsuleLib is the runtime-safe variant carrying the
  # real DxeCapsuleLib.c, so boot-time UpdateCapsule() actually processes.
  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibFmp/DxeRuntimeCapsuleLib.inf

[LibraryClasses.common.UEFI_DRIVER]
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf

[BuildOptions]
  GCC:*_*_*_CC_XIPFLAGS = -fno-jump-tables

[PcdsFeatureFlag.common]
  #
  # Capsule delivery.  UpdateCapsule() with a reset is how an OS hands a
  # firmware image to BDS, and capsule-on-disk lets that image arrive as a
  # file in \EFI\UpdateCapsule on the ESP rather than in RAM across a reset -
  # which matters here because the payload is 1.5 MiB of boot chain.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdSupportUpdateCapsuleReset|TRUE
  # ConSplitterDxe always requires its virtual GOP support, even on a
  # serial-only platform with no physical graphics output.
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutGopSupport|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdInstallAcpiSdtProtocol|TRUE

[PcdsFixedAtBuild.common]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8000004F
  # A311D implements neither FEAT_RNG nor the Arm firmware TRNG interface.
  # Permit the network stack to use RngDxe's explicitly-unsafe timer provider
  # for non-cryptographic DHCP/PXE identifiers.  TLS is disabled above.
  gEfiMdePkgTokenSpaceGuid.PcdEnforceSecureRngAlgorithms|FALSE
  gArmPlatformTokenSpaceGuid.PcdCoreCount|1
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000
  gArmPlatformTokenSpaceGuid.PcdSystemMemoryUefiRegionSize|0x04000000

  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterBase|0xFF803000
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultBaudRate|115200
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultDataBits|8
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultParity|1
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultStopBits|1
  gEfiMdePkgTokenSpaceGuid.PcdDefaultTerminalType|4

  gArmTokenSpaceGuid.PcdUefiShellDefaultBootEnable|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdResetOnMemoryTypeInformationChange|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxVariableSize|0x2000
  # Real-world Secure Boot payloads exceed the old 10 KiB ceiling: the
  # current Microsoft dbx alone is several times that.  The SPI store is
  # 256 KiB; a 64 KiB single-variable ceiling is the size check, not a
  # layout change.
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxAuthVariableSize|0x10000
  # Deny-execute images that fail verification regardless of where they
  # came from; the default policies trust fixed media.
  gEfiSecurityPkgTokenSpaceGuid.PcdOptionRomImageVerificationPolicy|0x04
  gEfiSecurityPkgTokenSpaceGuid.PcdFixedMediaImageVerificationPolicy|0x04
  gEfiSecurityPkgTokenSpaceGuid.PcdRemovableMediaImageVerificationPolicy|0x04
  gEmbeddedTokenSpaceGuid.PcdPrePiCpuIoSize|16
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetNxForStack|TRUE
  gEfiShellPkgTokenSpaceGuid.PcdShellFileOperationSize|0x20000

  gArmTokenSpaceGuid.PcdFdSize|0x00400000
  gArmTokenSpaceGuid.PcdFvSize|0x003E0000
  gArmTokenSpaceGuid.PcdMonitorConduitHvc|FALSE

  gEfiMdeModulePkgTokenSpaceGuid.PcdCapsuleOnDiskSupport|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableSize|0x40000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingSize|0x2000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareSize|0x40000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVendor|L"TianoCore / VIM3 EDK2"
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString|L"$(FIRMWARE_VERSION)"
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareReleaseDateString|L"$(FIRMWARE_RELEASE_DATE)"
  # build.sh supplies the repo's commit count, making the ESRT/FMP revision
  # advance with every build instead of sitting at the .dec default.
  gVim3TokenSpaceGuid.PcdVim3FirmwareRevision|$(FIRMWARE_REVISION)
  # AcpiTableDxe-generated RSDP/XSDT identity.  Platform tables carry the
  # same values in Platform/Khadas/Vim3/AcpiTables/AcpiTables.h.
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemId|"KHADAS"
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemTableId|0x49504341334D4956
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x00000001
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultCreatorId|0x324B4445
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultCreatorRevision|0x00000001
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiExposedTableVersions|0x20
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiTableStorageFile|{ 0x64, 0x8d, 0x8a, 0x0f, 0x30, 0x31, 0xea, 0x47, 0x9f, 0xa2, 0x4e, 0x29, 0x26, 0x2e, 0x56, 0xa8 }
  gEfiMdeModulePkgTokenSpaceGuid.PcdSmbiosVersion|0x0307
  # UiApp provides the firmware setup and boot-manager menu in this FV.
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootManagerMenuFile|{ 0x21, 0xaa, 0x2c, 0x46, 0x14, 0x76, 0x03, 0x45, 0x83, 0x6e, 0x8a, 0xb6, 0xf4, 0x66, 0x23, 0x31 }
  # Authenticated-format variables are stored in the on-board SPI NOR.
  # MesonSpifcFvbDxe supplies a runtime shadow plus FVB/FTW services.
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvStoreReserved|0

[PcdsPatchableInModule.common]
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x00000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0xF4E5B000
  gArmTokenSpaceGuid.PcdFdBaseAddress|0x01000000
  gArmTokenSpaceGuid.PcdFvBaseAddress|0x01020000
  gUefiOvmfPkgTokenSpaceGuid.PcdDeviceTreeInitialBaseAddress|0x01001000

[PcdsDynamicHii]
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|L"Timeout"|gEfiGlobalVariableGuid|0x0|3
  # ArmPkg's stock discovery handler deliberately returns without connecting
  # drivers when this PCD is not dynamic.  Default to the standard Connect All
  # policy so UEFI-model storage and network drivers are dispatched by BDS.
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootDiscoveryPolicy|L"BootDiscoveryPolicy"|gBootDiscoveryPolicyMgrFormsetGuid|0x0|2

[PcdsDynamicDefault.common]
  gArmTokenSpaceGuid.PcdGicDistributorBase|0xFFC01000
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0xFFC02000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableBase|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingBase|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareBase|0

[Components.common]
  Platform/Khadas/Vim3/PrePi/Vim3PrePi.inf {
    <LibraryClasses>
      ExtractGuidedSectionLib|EmbeddedPkg/Library/PrePiExtractGuidedSectionLib/PrePiExtractGuidedSectionLib.inf
      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
      PrePiLib|EmbeddedPkg/Library/PrePiLib/PrePiLib.inf
      HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
      PrePiHobListPointerLib|ArmPlatformPkg/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf
      MemoryAllocationLib|EmbeddedPkg/Library/PrePiMemoryAllocationLib/PrePiMemoryAllocationLib.inf
  }

  MdeModulePkg/Core/Dxe/DxeMain.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/DxeCrc32GuidedSectionExtractLib/DxeCrc32GuidedSectionExtractLib.inf
      DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  }
  MdeModulePkg/Universal/PCD/Dxe/Pcd.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  }
  ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  Silicon/Amlogic/MesonG12B/Drivers/MesonSpifcFvbDxe/MesonSpifcFvbDxe.inf
  MdeModulePkg/Universal/FaultTolerantWriteDxe/FaultTolerantWriteDxe.inf
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/VarCheckUefiLib/VarCheckUefiLib.inf
      BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  }
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf {
    <LibraryClasses>
      NULL|SecurityPkg/Library/DxeImageVerificationLib/DxeImageVerificationLib.inf
  }
  SecurityPkg/VariableAuthenticated/SecureBootConfigDxe/SecureBootConfigDxe.inf
  MdeModulePkg/Universal/MonotonicCounterRuntimeDxe/MonotonicCounterRuntimeDxe.inf
  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  MdeModulePkg/Universal/EsrtFmpDxe/EsrtFmpDxe.inf
  # Built for the ESP, not the FV: the canonical capsule debugging tool.
  MdeModulePkg/Application/CapsuleApp/CapsuleApp.inf
  Platform/Khadas/Vim3/Drivers/Vim3FmpDxe/Vim3FmpDxe.inf
  MdeModulePkg/Universal/Metronome/Metronome.inf
  EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf

  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  MdeModulePkg/Universal/Console/TerminalDxe/TerminalDxe.inf
  MdeModulePkg/Universal/SerialDxe/SerialDxe.inf
  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  MdeModulePkg/Universal/SmbiosDxe/SmbiosDxe.inf
  Platform/Khadas/Vim3/Drivers/Vim3SmbiosDxe/Vim3SmbiosDxe.inf
  MdeModulePkg/Universal/Acpi/AcpiTableDxe/AcpiTableDxe.inf
  MdeModulePkg/Universal/Acpi/AcpiPlatformDxe/AcpiPlatformDxe.inf
  Platform/Khadas/Vim3/AcpiTables/AcpiTables.inf
  Platform/Khadas/Vim3/AcpiTables/SsdtPcie.inf
  Platform/Khadas/Vim3/AcpiTables/SsdtDisplay.inf
  Platform/Khadas/Vim3/AcpiTables/SsdtAudio.inf
  Platform/Khadas/Vim3/AcpiTables/SsdtVdec.inf

  ArmPkg/Drivers/ArmGicDxe/ArmGicV2Dxe.inf
  ArmPkg/Drivers/TimerDxe/TimerDxe.inf
  MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf

  Platform/Khadas/Vim3/Drivers/Vim3PlatformDxe/Vim3PlatformDxe.inf
  Platform/Khadas/Vim3/Drivers/Vim3McuDxe/Vim3McuDxe.inf
  Platform/Khadas/Vim3/Drivers/Vim3McuDxe/Vim3McuTest.inf
  Platform/Khadas/Vim3/Applications/Vim3EmmcProbe/Vim3EmmcProbe.inf
  Platform/Khadas/Vim3/Applications/Vim3SdProbe/Vim3SdProbe.inf
  Platform/Khadas/Vim3/Applications/Vim3UsbBoot/Vim3UsbBoot.inf
  Platform/Khadas/Vim3/Applications/Vim3UsbBootHttp/Vim3UsbBootHttp.inf
  Platform/Khadas/Vim3/Applications/Vim3MaskRomBoot/Vim3MaskRomBoot.inf
  Platform/Khadas/Vim3/Applications/Vim3NormalBoot/Vim3NormalBoot.inf
  Platform/Khadas/Vim3/Drivers/MesonEthDxe/MesonEthDxe.inf
  Platform/Khadas/Vim3/Drivers/MesonGpuDxe/MesonGpuDxe.inf
  Platform/Khadas/Vim3/Drivers/MesonAudioDxe/MesonAudioDxe.inf
  # TcpDxe has an unconditional dependency on EFI_HASH2_SERVICE_BINDING.
  # Without this provider PXE (UDP) works, but TCP never dispatches and both
  # the HTTP shell command and HTTP Boot fail with EFI_UNSUPPORTED.
  SecurityPkg/Hash2DxeCrypto/Hash2DxeCrypto.inf
  SecurityPkg/RandomNumberGenerator/RngDxe/RngDxe.inf {
    <LibraryClasses>
      # A311D predates FEAT_RNG.  This provider is sufficient for DHCP/PXE
      # transaction IDs; it deliberately advertises the UEFI unsafe RNG GUID.
      RngLib|MdeModulePkg/Library/BaseRngLibTimerLib/BaseRngLibTimerLib.inf
      ArmTrngLib|MdePkg/Library/BaseArmTrngLibNull/BaseArmTrngLibNull.inf
  }
  edk2-platforms/Silicon/Synopsys/DesignWare/Drivers/DwEmacSnpDxe/DwEmacSnpDxe.inf
  EmbeddedPkg/Drivers/FdtClientDxe/FdtClientDxe.inf
  Platform/Khadas/Vim3/Drivers/MesonDisplayDxe/MesonDisplayDxe.inf
  Silicon/Amlogic/MesonG12B/Drivers/MesonSdMmcDxe/MesonSdMmcDxe.inf
  EmbeddedPkg/Universal/MmcDxe/MmcDxe.inf
  Silicon/Amlogic/MesonG12B/Drivers/MesonUsbDxe/MesonUsbDxe.inf
  MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf
  Silicon/Amlogic/MesonG12B/Drivers/Vim3XhciDxe/Vim3XhciDxe.inf
  ArmPkg/Drivers/ArmPciCpuIo2Dxe/ArmPciCpuIo2Dxe.inf
  EmbeddedPkg/Drivers/NonCoherentIoMmuDxe/NonCoherentIoMmuDxe.inf
  MdeModulePkg/Bus/Pci/PciHostBridgeDxe/PciHostBridgeDxe.inf
  MdeModulePkg/Bus/Pci/PciBusDxe/PciBusDxe.inf
  MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpressDxe.inf
  MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf
  # UDF read support: Windows ARM64 install media is a UDF (optical) image,
  # not FAT, so without this the firmware cannot read \efi\boot\bootaa64.efi
  # off the install USB.  PartitionDxe already carries UDF partition detection.
  MdeModulePkg/Universal/Disk/UdfDxe/UdfDxe.inf

!include NetworkPkg/Network.dsc.inc

  MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf {
    <LibraryClasses>
      DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
      PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  }
  MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf
  MdeModulePkg/Universal/SetupBrowserDxe/SetupBrowserDxe.inf
  MdeModulePkg/Universal/DriverHealthManagerDxe/DriverHealthManagerDxe.inf
  MdeModulePkg/Universal/BootManagerPolicyDxe/BootManagerPolicyDxe.inf
  MdeModulePkg/Universal/BdsDxe/BdsDxe.inf
  MdeModulePkg/Application/UiApp/UiApp.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/DeviceManagerUiLib/DeviceManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootManagerUiLib/BootManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootMaintenanceManagerUiLib/BootMaintenanceManagerUiLib.inf
  }
