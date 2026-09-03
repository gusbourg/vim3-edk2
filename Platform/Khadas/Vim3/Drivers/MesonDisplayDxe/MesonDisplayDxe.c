/** @file
  UEFI GOP and DT handoff for the Khadas VIM3 HDMI framebuffer.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "MesonDisplay.h"

//
// DxeServicesLib (GetSectionFromAnyFv) wants the PI firmware-volume types,
// which a UEFI_DRIVER's <Uefi.h> world does not provide.
//
#include <Pi/PiFirmwareFile.h>
#include <Pi/PiFirmwareVolume.h>

#include <Protocol/AcpiTable.h>
#include <Protocol/Cpu.h>
#include <Protocol/DevicePath.h>

#include <Guid/Fdt.h>

#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Vim3PlatformConfig.h>

#define DISPLAY_VENDOR_GUID \
  { 0x7d23f9ea, 0x908b, 0x4d48, { 0xbd, 0x56, 0x39, 0xfb, 0x74, 0x42, 0xe5, 0x4f } }

STATIC EFI_GUID  mDisplayVendorGuid = DISPLAY_VENDOR_GUID;

STATIC VOID  *mAcpiTableNotifyRegistration;

/**
  Fetch one RAW SSDT section by FFS file GUID and install it.
**/
STATIC
VOID
MesonDisplayInstallOneSsdt (
  IN EFI_ACPI_TABLE_PROTOCOL  *AcpiTable,
  IN EFI_GUID                 *FileGuid,
  IN CONST CHAR8              *Name
  )
{
  EFI_STATUS  Status;
  VOID        *Table;
  UINTN       TableSize;
  UINTN       TableKey;

  Table  = NULL;
  Status = GetSectionFromAnyFv (
             FileGuid,
             EFI_SECTION_RAW,
             0,
             &Table,
             &TableSize
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MesonDisplay: %a SSDT section not found: %r\n", Name, Status));
    return;
  }

  Status = AcpiTable->InstallAcpiTable (AcpiTable, Table, TableSize, &TableKey);
  FreePool (Table);
  DEBUG ((DEBUG_INFO, "MesonDisplay: %a SSDT install: %r\n", Name, Status));
}

/**
  Install the display-pipeline and audio-pipeline SSDTs once
  EFI_ACPI_TABLE_PROTOCOL appears.

  Registered only after MesonDisplayHardwareInit succeeded in ACPI mode:
  the tables describe the VPU, DW-HDMI, and canvas blocks this driver just
  powered and clocked, plus the audio path that terminates in them
  (TOHDMITX feeds \_SB.HDMI - audio without the display pipeline is
  meaningless, so it shares the gate).  On headless or failed bring-up
  neither is registered, keeping the DSDT allow-list promise (an
  unpowered VPU description is the SDA0 AXI-hang hazard).
**/
STATIC
VOID
EFIAPI
MesonDisplayInstallSsdt (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS               Status;
  EFI_ACPI_TABLE_PROTOCOL  *AcpiTable;

  Status = gBS->LocateProtocol (
                  &gEfiAcpiTableProtocolGuid,
                  mAcpiTableNotifyRegistration,
                  (VOID **)&AcpiTable
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  gBS->CloseEvent (Event);

  MesonDisplayInstallOneSsdt (AcpiTable, &gVim3SsdtDisplayFileGuid, "display");
  MesonDisplayInstallOneSsdt (AcpiTable, &gVim3SsdtAudioFileGuid, "audio");
  MesonDisplayInstallOneSsdt (AcpiTable, &gVim3SsdtVdecFileGuid, "vdec");
}

/**
  Register the display SSDT installer if the OS hardware description is
  ACPI.  Called exactly once, after successful hardware bring-up.
**/
STATIC
VOID
MesonDisplayMaybePublishAcpi (
  VOID
  )
{
  EFI_STATUS                         Status;
  VIM3_HW_DESCRIPTION_VARSTORE_DATA  HwDescription;
  UINTN                              Size;

  Size   = sizeof (HwDescription);
  Status = gRT->GetVariable (
                  VIM3_HW_DESCRIPTION_VARIABLE_NAME,
                  &gVim3PlatformFormSetGuid,
                  NULL,
                  &Size,
                  &HwDescription
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (HwDescription)) ||
      (HwDescription.Mode != VIM3_HW_DESCRIPTION_ACPI))
  {
    return;
  }

  EfiCreateProtocolNotifyEvent (
    &gEfiAcpiTableProtocolGuid,
    TPL_CALLBACK,
    MesonDisplayInstallSsdt,
    NULL,
    &mAcpiTableNotifyRegistration
    );
}

STATIC
EFI_STATUS
EFIAPI
MesonGopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL           *This,
  IN  UINT32                                 ModeNumber,
  OUT UINTN                                  *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION   **Info
  )
{
  MESON_DISPLAY_PRIVATE  *Private;

  if ((This == NULL) || (SizeOfInfo == NULL) || (Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Private = MESON_DISPLAY_FROM_GOP (This);
  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }

  *Info = AllocateCopyPool (sizeof (Private->ModeInfo), &Private->ModeInfo);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *SizeOfInfo = sizeof (Private->ModeInfo);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MesonGopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black;

  if ((This == NULL) || (ModeNumber != 0)) {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (&Black, sizeof (Black));
  return This->Blt (
                 This,
                 &Black,
                 EfiBltVideoFill,
                 0,
                 0,
                 0,
                 0,
                 This->Mode->Info->HorizontalResolution,
                 This->Mode->Info->VerticalResolution,
                 0
                 );
}

STATIC
EFI_STATUS
EFIAPI
MesonGopBlt (
  IN     EFI_GRAPHICS_OUTPUT_PROTOCOL            *This,
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL           *BltBuffer OPTIONAL,
  IN     EFI_GRAPHICS_OUTPUT_BLT_OPERATION       BltOperation,
  IN     UINTN                                   SourceX,
  IN     UINTN                                   SourceY,
  IN     UINTN                                   DestinationX,
  IN     UINTN                                   DestinationY,
  IN     UINTN                                   Width,
  IN     UINTN                                   Height,
  IN     UINTN                                   Delta OPTIONAL
  )
{
  MESON_DISPLAY_PRIVATE  *Private;
  EFI_STATUS             Status;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Private = MESON_DISPLAY_FROM_GOP (This);
  Status = (EFI_STATUS)FrameBufferBlt (
                         Private->BltConfigure,
                         BltBuffer,
                         BltOperation,
                         SourceX,
                         SourceY,
                         DestinationX,
                         DestinationY,
                         Width,
                         Height,
                         Delta
                         );
  if (!EFI_ERROR (Status) && (BltOperation != EfiBltVideoToBltBuffer)) {
    //
    // The Meson VPU reads the framebuffer outside the CPU cache-coherency
    // domain.  Make every successful write visible to scanout.  Flushing the
    // complete 3.5 MiB surface is deliberately conservative for the first
    // hardware-validated implementation; a later optimization may restrict
    // this to the destination rectangle.
    //
    WriteBackDataCacheRange (
      (VOID *)(UINTN)Private->FrameBufferBase,
      Private->FrameBufferSize
      );
  }

  if (Private->BltCount < 16) {
    DEBUG ((
      DEBUG_INFO,
      "MesonDisplay: BLT #%u op=%u dst=%ux%u+%u+%u status=%r\n",
      Private->BltCount,
      BltOperation,
      (UINT32)Width,
      (UINT32)Height,
      (UINT32)DestinationX,
      (UINT32)DestinationY,
      Status
      ));
  }

  Private->BltCount++;
  return Status;
}

STATIC
EFI_STATUS
SetFdtU32 (
  IN VOID         *Dtb,
  IN INT32        Node,
  IN CONST CHAR8  *Name,
  IN UINT32       Value
  )
{
  UINT32  BigEndianValue;

  BigEndianValue = SwapBytes32 (Value);
  return (FdtSetProp (Dtb, Node, Name, &BigEndianValue, sizeof (BigEndianValue)) == 0) ?
         EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
PatchFramebufferDtb (
  IN MESON_DISPLAY_PRIVATE  *Private
  )
{
  EFI_STATUS  Status;
  VOID        *Dtb;
  INT32       Node;
  INT32       Reserved;
  INT32       Framebuffer;
  INT32       Result;
  CHAR8       FramebufferName[48];
  UINT32      Reg[4];
  UINT32      Width;
  UINT32      Height;
  UINT32      Stride;

  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Dtb);
  if (EFI_ERROR (Status) || (Dtb == NULL) || (FdtCheckHeader (Dtb) != 0)) {
    return EFI_NOT_FOUND;
  }

  Result = FdtOpenInto (Dtb, Dtb, MESON_DTB_REGION_SIZE);
  if (Result != 0) {
    return EFI_DEVICE_ERROR;
  }

  Node = FdtPathOffset (Dtb, "/chosen/framebuffer@f4e5b000");
  if (Node >= 0) {
    FdtSetProp (Dtb, Node, "status", "disabled", sizeof ("disabled"));
    //
    // Linux selects only the first /chosen child compatible with
    // "simple-framebuffer", then asks the platform core to instantiate it.
    // It does not continue to a later compatible child when that first node
    // is disabled.  The archived runtime DT contains this stale CVBS node
    // ahead of framebuffer-hdmi, so remove its compatibility after disabling
    // it and leave the active HDMI node as the sole simplefb candidate.
    //
    FdtDelProp (Dtb, Node, "compatible");
  }

  Node = FdtPathOffset (Dtb, "/chosen/framebuffer-hdmi");
  if (Node < 0) {
    return EFI_NOT_FOUND;
  }

  Width  = Private->Timing.Width;
  Height = Private->Timing.Height;
  Stride = Private->Stride;
  Reg[0] = SwapBytes32 ((UINT32)RShiftU64 (Private->FrameBufferBase, 32));
  Reg[1] = SwapBytes32 ((UINT32)Private->FrameBufferBase);
  Reg[2] = SwapBytes32 ((UINT32)RShiftU64 (Private->FrameBufferSize, 32));
  Reg[3] = SwapBytes32 ((UINT32)Private->FrameBufferSize);

  if ((FdtSetProp (Dtb, Node, "status", "okay", sizeof ("okay")) != 0) ||
      EFI_ERROR (SetFdtU32 (Dtb, Node, "width", Width)) ||
      EFI_ERROR (SetFdtU32 (Dtb, Node, "height", Height)) ||
      EFI_ERROR (SetFdtU32 (Dtb, Node, "stride", Stride)) ||
      (FdtSetProp (Dtb, Node, "format", "x8r8g8b8", sizeof ("x8r8g8b8")) != 0) ||
      (FdtSetProp (Dtb, Node, "reg", Reg, sizeof (Reg)) != 0))
  {
    return EFI_DEVICE_ERROR;
  }

  Reserved = FdtPathOffset (Dtb, "/reserved-memory");
  if (Reserved < 0) {
    Reserved = FdtAddSubnode (Dtb, 0, "reserved-memory");
    if (Reserved < 0) {
      return EFI_DEVICE_ERROR;
    }

    SetFdtU32 (Dtb, Reserved, "#address-cells", 2);
    SetFdtU32 (Dtb, Reserved, "#size-cells", 2);
    FdtSetProp (Dtb, Reserved, "ranges", NULL, 0);
  }

  AsciiSPrint (
    FramebufferName,
    sizeof (FramebufferName),
    "uefi-framebuffer@%lx",
    Private->FrameBufferBase
    );
  Framebuffer = FdtSubnodeOffset (Dtb, Reserved, FramebufferName);
  if (Framebuffer < 0) {
    Framebuffer = FdtAddSubnode (Dtb, Reserved, FramebufferName);
  }

  if ((Framebuffer < 0) ||
      (FdtSetProp (Dtb, Framebuffer, "reg", Reg, sizeof (Reg)) != 0) ||
      (FdtSetProp (Dtb, Framebuffer, "no-map", NULL, 0) != 0))
  {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ConfigureFrameBuffer (
  IN OUT MESON_DISPLAY_PRIVATE  *Private
  )
{
  EFI_STATUS             Status;
  EFI_CPU_ARCH_PROTOCOL  *Cpu;
  RETURN_STATUS          ReturnStatus;
  UINTN                  ConfigureSize;
  UINTN                  Pages;
  UINTN                  PixelCount;

  Private->Stride          = Private->Timing.Width * sizeof (UINT32);
  Private->FrameBufferSize = (UINTN)Private->Stride * Private->Timing.Height;
  Pages                    = EFI_SIZE_TO_PAGES (Private->FrameBufferSize);
  Private->FrameBufferBase = MESON_SYSTEM_MEMORY_TOP - 1;

  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiReservedMemoryType,
                  Pages,
                  &Private->FrameBufferBase
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Cpu->SetMemoryAttributes (
                  Cpu,
                  Private->FrameBufferBase,
                  EFI_PAGES_TO_SIZE (Pages),
                  EFI_MEMORY_WC
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "MesonDisplay: WC framebuffer unavailable: %r; using UC\n", Status));
    Status = Cpu->SetMemoryAttributes (
                    Cpu,
                    Private->FrameBufferBase,
                    EFI_PAGES_TO_SIZE (Pages),
                    EFI_MEMORY_UC
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  //
  // A dark blue fill is both a deterministic initial state and an immediate
  // scanout diagnostic before the console starts drawing.
  //
  PixelCount = Private->FrameBufferSize / sizeof (UINT32);
  SetMem32 ((VOID *)(UINTN)Private->FrameBufferBase, PixelCount * sizeof (UINT32), 0x00204060);
  WriteBackDataCacheRange (
    (VOID *)(UINTN)Private->FrameBufferBase,
    Private->FrameBufferSize
    );
  DEBUG ((DEBUG_INFO, "MesonDisplay: initial framebuffer flushed for scanout\n"));

  Private->ModeInfo.Version              = 0;
  Private->ModeInfo.HorizontalResolution = Private->Timing.Width;
  Private->ModeInfo.VerticalResolution   = Private->Timing.Height;
  Private->ModeInfo.PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;
  Private->ModeInfo.PixelsPerScanLine    = Private->Timing.Width;
  ZeroMem (&Private->ModeInfo.PixelInformation, sizeof (Private->ModeInfo.PixelInformation));

  ConfigureSize = 0;
  ReturnStatus  = FrameBufferBltConfigure (
                    (VOID *)(UINTN)Private->FrameBufferBase,
                    &Private->ModeInfo,
                    NULL,
                    &ConfigureSize
                    );
  if (ReturnStatus != RETURN_BUFFER_TOO_SMALL) {
    return (EFI_STATUS)ReturnStatus;
  }

  Private->BltConfigure = AllocatePool (ConfigureSize);
  if (Private->BltConfigure == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ReturnStatus = FrameBufferBltConfigure (
                   (VOID *)(UINTN)Private->FrameBufferBase,
                   &Private->ModeInfo,
                   Private->BltConfigure,
                   &ConfigureSize
                   );
  return (EFI_STATUS)ReturnStatus;
}

STATIC
VOID
InitializeProtocols (
  IN OUT MESON_DISPLAY_PRIVATE  *Private
  )
{
  Private->Gop.QueryMode = MesonGopQueryMode;
  Private->Gop.SetMode   = MesonGopSetMode;
  Private->Gop.Blt       = MesonGopBlt;
  Private->Gop.Mode      = &Private->GopMode;

  Private->GopMode.MaxMode         = 1;
  Private->GopMode.Mode            = 0;
  Private->GopMode.Info            = &Private->ModeInfo;
  Private->GopMode.SizeOfInfo      = sizeof (Private->ModeInfo);
  Private->GopMode.FrameBufferBase = Private->FrameBufferBase;
  Private->GopMode.FrameBufferSize = Private->FrameBufferSize;

  Private->EdidDiscovered.SizeOfEdid = Private->EdidSize;
  Private->EdidDiscovered.Edid       = Private->Edid;
  Private->EdidActive.SizeOfEdid     = Private->EdidSize;
  Private->EdidActive.Edid           = Private->Edid;

  Private->DevicePath.Vendor.Header.Type    = HARDWARE_DEVICE_PATH;
  Private->DevicePath.Vendor.Header.SubType = HW_VENDOR_DP;
  SetDevicePathNodeLength (&Private->DevicePath.Vendor.Header, sizeof (VENDOR_DEVICE_PATH));
  CopyGuid (&Private->DevicePath.Vendor.Guid, &mDisplayVendorGuid);
  SetDevicePathEndNode (&Private->DevicePath.End);
}

EFI_STATUS
EFIAPI
MesonDisplayDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  EFI_STATUS             WakeStatus;
  MESON_DISPLAY_PRIVATE  *Private;

  DEBUG ((DEBUG_INFO, "MesonDisplay: probing HDMI hotplug\n"));
  //
  // Headless is a normal state.  HPD is sampled only after the lightweight
  // HDMI system clock/pinmux setup in the hardware module and is bounded.
  //
  if (!MesonDisplayHotPlugDetected ()) {
    DEBUG ((DEBUG_INFO, "MesonDisplay: no HDMI sink; serial-only boot\n"));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_INFO, "MesonDisplay: HDMI sink detected\n"));
  Private = AllocateZeroPool (sizeof (*Private));
  if (Private == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Private->Signature = MESON_DISPLAY_SIGNATURE;
  Status = MesonDisplayReadEdid (
             Private->Edid,
             sizeof (Private->Edid),
             &Private->EdidSize
             );
  if (EFI_ERROR (Status)) {
    //
    // Some active KVMs report receiver sense before HPD and NACK DDC until
    // they see an active TMDS link.  Wake such a sink with a bounded 720p
    // signal, then retry EDID once before applying the normal fallback.
    //
    DEBUG ((DEBUG_WARN, "MesonDisplay: initial EDID unavailable (%r); waking sink\n", Status));
    WakeStatus = MesonDisplayWakeSink ();
    if (!EFI_ERROR (WakeStatus)) {
      Status = MesonDisplayReadEdid (
                 Private->Edid,
                 sizeof (Private->Edid),
                 &Private->EdidSize
                 );
    }
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "MesonDisplay: EDID unavailable after link wake (%r), using 720p60\n", Status));
    Private->EdidSize = 0;
  } else {
    DEBUG ((DEBUG_INFO, "MesonDisplay: read %u EDID bytes\n", Private->EdidSize));
  }

  MesonDisplayChooseMode (
    Private->Edid,
    Private->EdidSize,
    &Private->Timing,
    &Private->HdmiSink
    );
  DEBUG ((
    DEBUG_INFO,
    "MesonDisplay: selected CTA VIC %u %ux%u%a %a mode\n",
    Private->Timing.CtaVic,
    Private->Timing.Width,
    Private->Timing.Height,
    Private->Timing.Interlaced ? "i" : "p",
    Private->HdmiSink ? "HDMI" : "DVI"
    ));

  DEBUG ((DEBUG_INFO, "MesonDisplay: allocating framebuffer\n"));
  Status = ConfigureFrameBuffer (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MesonDisplay: framebuffer setup failed: %r\n", Status));
    return EFI_SUCCESS;
  }

  Status = MesonDisplayHardwareInit (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MesonDisplay: hardware setup failed: %r; serial-only\n", Status));
    return EFI_SUCCESS;
  }

  //
  // The VPU/HDMI power-and-clock state is now exactly what the display
  // SSDT describes; in ACPI mode publish it (headless/failed boots never
  // reach this point, so the devices stay undescribed there).
  //
  MesonDisplayMaybePublishAcpi ();

  InitializeProtocols (Private);
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Private->Handle,
                  &gEfiDevicePathProtocolGuid,
                  &Private->DevicePath,
                  &gEfiGraphicsOutputProtocolGuid,
                  &Private->Gop,
                  &gEfiEdidDiscoveredProtocolGuid,
                  &Private->EdidDiscovered,
                  &gEfiEdidActiveProtocolGuid,
                  &Private->EdidActive,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MesonDisplay: GOP install failed: %r\n", Status));
    return EFI_SUCCESS;
  }

  Status = PatchFramebufferDtb (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "MesonDisplay: DT framebuffer handoff failed: %r\n", Status));
  }

  DEBUG ((
    DEBUG_INFO,
    "MesonDisplay: GOP %ux%u FB=0x%lx size=0x%lx EDID=%u %a\n",
    Private->Timing.Width,
    Private->Timing.Height,
    Private->FrameBufferBase,
    (UINT64)Private->FrameBufferSize,
    Private->EdidSize,
    Private->HdmiSink ? "HDMI" : "DVI"
    ));
  return EFI_SUCCESS;
}
