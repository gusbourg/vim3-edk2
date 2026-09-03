/** @file
  Khadas VIM3 MCU fan and setup driver.

  MCU register access goes through Vim3McuLib (the register map, the
  no-auto-increment reads, the write whitelist and the identity/trust probe all
  live there).  This driver owns the setup HII page, the config-callback
  question-to-register mapping, the McuSettings mirror, the USB3/PCIe DT mux
  patch, and the A311D on-die temperature reader (which is direct MMIO, not the
  MCU).

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FdtLib.h>
#include <Library/HiiLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/Vim3McuLib.h>

#include <Guid/Fdt.h>
#include <Protocol/HiiConfigAccess.h>

#include <Vim3Mcu.h>
#include <Vim3PlatformConfig.h>

//
// A311D on-die CPU temperature sensor (Meson G12A/G12B "PLL" TSENSOR).  The
// register block and bit fields are from the A311D datasheet section 15.2; the
// clock gate, the AO-secure per-chip trim mirror, and the raw-code -> Celsius
// calibration constants (A/B/M/N) are from the Linux amlogic_thermal.c driver,
// which uses the G12A calibration for G12B verbatim.
//
#define TS_CLK_CNTL_REG        0xFF63C190U   // HHI_TS_CLK_CNTL
#define TS_CLK_CNTL_ENABLE     0x0000012FU   // gate on (bit8) + div 47 => ~500 kHz
#define TS_PLL_CFG_REG1        0xFF634804U
#define TS_PLL_CFG_ENABLE      0x0000062BU   // FILTER_EN|VCM_EN|VBG_EN|DEM_EN|CH_SEL(3)
#define TS_PLL_STAT0           0xFF634840U
#define TS_STAT0_LOCK          BIT16         // "filter lock" => a reading is ready
#define TS_STAT0_CODE_MASK     0xFFFFU
#define TS_TRIM_REG            0xFF800268U   // AO-secure CPU-sensor trim mirror
#define TS_CAL_A               9411
#define TS_CAL_B               3159
#define TS_CAL_M               424
#define TS_CAL_N               324

#define VIM3_DTB_REGION_SIZE   0x1F000U

#define FAN_POLICY_ATTRIBUTES \
  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS)

extern UINT8  Vim3McuDxeHiiBin[];
extern UINT8  Vim3McuDxeStrings[];

#ifndef VIM3_MCU_TEST_APP
typedef struct {
  EFI_HII_CONFIG_ACCESS_PROTOCOL  ConfigAccess;
  EFI_HANDLE                      DriverHandle;
  EFI_HII_HANDLE                  HiiHandle;
} VIM3_MCU_PRIVATE;

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} VIM3_HII_DEVICE_PATH;

STATIC VIM3_HII_DEVICE_PATH  mHiiDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        sizeof (VENDOR_DEVICE_PATH),
        sizeof (VENDOR_DEVICE_PATH) >> 8
      }
    },
    VIM3_PLATFORM_FORMSET_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      END_DEVICE_PATH_LENGTH,
      0
    }
  }
};

STATIC VIM3_MCU_PRIVATE  mPrivate;

//
// Write an MCU configuration register and read it back to confirm.  Used for
// the mux and the settings registers (not the fan, which is write-only, or the
// IR codes).
//
STATIC
EFI_STATUS
McuWriteVerify (
  IN UINT8  Register,
  IN UINT8  Value
  )
{
  EFI_STATUS  Status;
  UINT8       ReadBack;

  Status = Vim3McuWriteByte (Register, Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Vim3McuReadByte (Register, &ReadBack);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return (ReadBack == Value) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

//
// Write a 4-byte IR power-key code.  The MCU does not auto-increment its
// register pointer, so each byte is written to its own register (base..base+3).
// Little-endian: the code's low byte lands at the base register.
//
STATIC
EFI_STATUS
McuWriteIrCode (
  IN UINT8   BaseRegister,
  IN UINT32  Code
  )
{
  EFI_STATUS  Status;
  UINTN       Index;

  if ((BaseRegister != MCU_REG_IR_CODE1) &&
      (BaseRegister != MCU_REG_IR_CODE2))
  {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < sizeof (UINT32); Index++) {
    Status = Vim3McuWriteByte (
               (UINT8)(BaseRegister + Index),
               (UINT8)(Code >> (Index * 8))
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

//
// Read one settings register, clamp it to a valid range, and return the value
// (or the given default on a read failure) so the setup oneof always has a
// value it can display.
//
STATIC
UINT8
McuReadSettingByte (
  IN UINT8  Register,
  IN UINT8  Max,
  IN UINT8  Default
  )
{
  UINT8  Value;

  if (EFI_ERROR (Vim3McuReadByte (Register, &Value))) {
    return Default;
  }

  return (Value <= Max) ? Value : Default;
}

//
// Read the four bytes of an IR-code register and assemble the little-endian
// 32-bit value.
//
STATIC
UINT32
McuReadIrCode (
  IN UINT8  BaseRegister
  )
{
  UINT8  Bytes[4];

  if (EFI_ERROR (Vim3McuReadBlock (BaseRegister, Bytes, sizeof (Bytes)))) {
    return 0;
  }

  return (UINT32)Bytes[0] | ((UINT32)Bytes[1] << 8) |
         ((UINT32)Bytes[2] << 16) | ((UINT32)Bytes[3] << 24);
}

//
// Mirror the live MCU configuration registers into the McuSettings variable so
// the setup browser shows the current values.  The MCU stays the source of
// truth: this runs every boot and a browser change writes the MCU back via
// ConfigCallback.
//
STATIC
VOID
MirrorMcuSettings (
  VOID
  )
{
  VIM3_MCU_SETTINGS_VARSTORE_DATA  Settings;
  EFI_STATUS                       Status;

  Settings.WolMode  = McuReadSettingByte (MCU_REG_BOOT_EN_WOL, VIM3_WAKE_RESET, VIM3_WAKE_DISABLED);
  Settings.RtcWake  = McuReadSettingByte (MCU_REG_BOOT_EN_RTC, VIM3_WAKE_ENABLED, VIM3_WAKE_ENABLED);
  Settings.ExpWake  = McuReadSettingByte (MCU_REG_BOOT_EN_EXP, VIM3_WAKE_RESET, VIM3_WAKE_ENABLED);
  Settings.IrWake   = McuReadSettingByte (MCU_REG_BOOT_EN_IR, VIM3_WAKE_ENABLED, VIM3_WAKE_ENABLED);
  Settings.DcinWake = McuReadSettingByte (MCU_REG_BOOT_EN_DCIN, VIM3_WAKE_ENABLED, VIM3_WAKE_ENABLED);
  Settings.KeyWake  = McuReadSettingByte (MCU_REG_BOOT_EN_KEY, VIM3_WAKE_ENABLED, VIM3_WAKE_ENABLED);
  Settings.KeyMode  = McuReadSettingByte (MCU_REG_KEY_MODE, VIM3_KEY_LONG_PRESS, VIM3_KEY_SHORT_PRESS);
  Settings.LedOn    = McuReadSettingByte (MCU_REG_LED_MODE_ON, VIM3_LED_HEARTBEAT, VIM3_LED_ON);
  Settings.LedOff   = McuReadSettingByte (MCU_REG_LED_MODE_OFF, VIM3_LED_HEARTBEAT, VIM3_LED_OFF);
  Settings.McuSleep = McuReadSettingByte (MCU_REG_MCU_SLEEP, VIM3_MCU_SLEEP_ENABLED, VIM3_MCU_SLEEP_DISABLED);
  Settings.IrCode1  = McuReadIrCode (MCU_REG_IR_CODE1);
  Settings.IrCode2  = McuReadIrCode (MCU_REG_IR_CODE2);

  Status = gRT->SetVariable (
                  VIM3_MCU_SETTINGS_VARIABLE_NAME,
                  &gVim3PlatformFormSetGuid,
                  FAN_POLICY_ATTRIBUTES,
                  sizeof (Settings),
                  &Settings
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "Vim3Mcu: failed to mirror MCU settings: %r\n", Status));
  }
}

STATIC
EFI_STATUS
PatchPublishedDtb (
  IN UINT8  Mode
  )
{
  EFI_STATUS  Status;
  VOID        *Dtb;
  INT32       Node;
  INT32       Result;
  CONST VOID  *Property;
  INT32       PropertyLength;
  UINT32      Usb2Phys[2];
  CONST CHAR8 Usb2PhyNames[] = "usb2-phy0\0usb2-phy1";

  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Dtb);
  if (EFI_ERROR (Status) || (Dtb == NULL) || (FdtCheckHeader (Dtb) != 0)) {
    return EFI_NOT_FOUND;
  }

  if (Mode == MCU_MUX_USB3) {
    DEBUG ((DEBUG_INFO, "Vim3Mcu: DT mux policy USB3; PCIe remains disabled\n"));
    return EFI_SUCCESS;
  }

  //
  // The DTB occupies a fixed 124 KiB slot in the FD.  Open it to the full
  // slot before replacing "disabled" with the longer "okay" property.
  //
  Result = FdtOpenInto (Dtb, Dtb, VIM3_DTB_REGION_SIZE);
  if (Result != 0) {
    DEBUG ((DEBUG_ERROR, "Vim3Mcu: cannot expand published DTB: %d\n", Result));
    return EFI_DEVICE_ERROR;
  }

  Node = FdtNodeOffsetByCompatible (Dtb, -1, "amlogic,meson-g12a-usb-ctrl");
  if (Node < 0) {
    return EFI_NOT_FOUND;
  }

  Property = FdtGetProp (Dtb, Node, "phys", &PropertyLength);
  if ((Property == NULL) || (PropertyLength < (INT32)(2 * sizeof (UINT32)))) {
    return EFI_COMPROMISED_DATA;
  }

  //
  // FdtSetProp() may relocate the property block.  Do not use a pointer into
  // that same block as its source buffer.
  //
  CopyMem (Usb2Phys, Property, sizeof (Usb2Phys));
  Result = FdtSetProp (
             Dtb,
             Node,
             "phys",
             Usb2Phys,
             sizeof (Usb2Phys)
             );
  if (Result == 0) {
    Result = FdtSetProp (
               Dtb,
               Node,
               "phy-names",
               Usb2PhyNames,
               sizeof (Usb2PhyNames)
               );
  }

  if (Result != 0) {
    return EFI_DEVICE_ERROR;
  }

  Node = FdtNodeOffsetByCompatible (Dtb, -1, "amlogic,g12a-pcie");
  if (Node < 0) {
    return EFI_NOT_FOUND;
  }

  Result = FdtSetPropString (Dtb, Node, "status", "okay");
  if (Result != 0) {
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((
    DEBUG_INFO,
    "Vim3Mcu: DT mux policy PCIe; USB3 PHY removed, USB2 retained\n"
    ));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
ExtractConfig (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN  CONST EFI_STRING                      Request,
  OUT EFI_STRING                            *Progress,
  OUT EFI_STRING                            *Results
  )
{
  if ((Progress == NULL) || (Results == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Progress = Request;
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
EFIAPI
RouteConfig (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN  CONST EFI_STRING                      Configuration,
  OUT EFI_STRING                            *Progress
  )
{
  if ((Configuration == NULL) || (Progress == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Progress = Configuration;
  return EFI_NOT_FOUND;
}

//
// Read the A311D CPU temperature sensor, returning millidegrees Celsius.
// Enables the sensor clock and analog front-end (both idempotent), applies the
// per-chip trim from the AO-secure mirror, waits for the filter to lock, then
// converts the raw code with the G12A calibration.  All-integer, 64-bit
// intermediates in the driver's exact order (verified: raw 0x201E -> 40 C).
//
STATIC
EFI_STATUS
ReadSocTemperatureMilliC (
  OUT INT32  *MilliC
  )
{
  UINT32  Trim;
  INT32   UEfuse;
  UINT32  Raw;
  UINTN   Retry;
  INT64   Factor;
  INT64   Uptat;
  INT64   Temp;

  if (MilliC == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  MmioWrite32 (TS_CLK_CNTL_REG, TS_CLK_CNTL_ENABLE);

  //
  // Per-chip trim.  The AO-secure mirror holds a validity/version field in the
  // top byte; when it is unprogrammed the driver refuses to calibrate, but we
  // fall back to a zero trim (worst case ~0.5-3 C off) rather than showing
  // nothing.
  //
  UEfuse = 0;
  Trim   = MmioRead32 (TS_TRIM_REG);
  if (((Trim >> 24) & 0x8C) != 0) {
    if ((Trim & 0x8000) != 0) {
      UEfuse = -(INT32)(Trim & 0x7FFF);
    } else {
      UEfuse = (INT32)(Trim & 0x7FFF);
    }
  }

  MmioOr32 (TS_PLL_CFG_REG1, TS_PLL_CFG_ENABLE);

  for (Retry = 0; Retry < 50; Retry++) {
    if ((MmioRead32 (TS_PLL_STAT0) & TS_STAT0_LOCK) != 0) {
      break;
    }

    gBS->Stall (1000);
  }

  if ((MmioRead32 (TS_PLL_STAT0) & TS_STAT0_LOCK) == 0) {
    return EFI_TIMEOUT;
  }

  Raw = MmioRead32 (TS_PLL_STAT0) & TS_STAT0_CODE_MASK;
  if ((Raw < 0x1000) || (Raw > 0x3000)) {
    return EFI_DEVICE_ERROR;
  }

  Factor  = ((INT64)TS_CAL_N * Raw) / 100;
  Uptat   = ((INT64)TS_CAL_M * Raw) / 100;
  Uptat   = (Uptat << 16) / (((INT64)BIT16) + Factor);
  Temp    = (Uptat + UEfuse) * TS_CAL_A;
  Temp    = Temp >> 16;
  *MilliC = (INT32)((Temp - TS_CAL_B) * 100);

  return EFI_SUCCESS;
}

//
// Re-read the sensor and publish it into the setup page's temperature field.
//
STATIC
VOID
RefreshSocTemperature (
  VOID
  )
{
  EFI_STATUS  Status;
  INT32       MilliC;
  CHAR16      Text[24];

  if (mPrivate.HiiHandle == NULL) {
    return;
  }

  Status = ReadSocTemperatureMilliC (&MilliC);
  if (EFI_ERROR (Status)) {
    UnicodeSPrint (Text, sizeof (Text), L"unavailable (%r)", Status);
  } else {
    UnicodeSPrint (
      Text,
      sizeof (Text),
      L"%d.%d C",
      MilliC / 1000,
      (MilliC % 1000) / 100
      );
  }

  HiiSetString (
    mPrivate.HiiHandle,
    STRING_TOKEN (STR_SOC_TEMP_VALUE),
    Text,
    NULL
    );
}

STATIC
EFI_STATUS
EFIAPI
ConfigCallback (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN  EFI_BROWSER_ACTION                    Action,
  IN  EFI_QUESTION_ID                       QuestionId,
  IN  UINT8                                 Type,
  IN  EFI_IFR_TYPE_VALUE                    *Value,
  OUT EFI_BROWSER_ACTION_REQUEST            *ActionRequest
  )
{
  EFI_STATUS  Status;
  UINT8       Mode;
  UINT8       Register;

  if (ActionRequest == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // The browser sends FORM_OPEN for the interactive questions on this page when
  // it is entered; use it to refresh the live SoC temperature reading.
  //
  if (Action == EFI_BROWSER_ACTION_FORM_OPEN) {
    RefreshSocTemperature ();
    return EFI_SUCCESS;
  }

  if (Action != EFI_BROWSER_ACTION_CHANGED) {
    return EFI_UNSUPPORTED;
  }

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!Vim3McuWritesTrusted ()) {
    return EFI_ACCESS_DENIED;
  }

  //
  // USB3/PCIe mux: changing it re-muxes hardware, so a reset is required
  // before the new mode is usable.  The reset is requested by the
  // RESET_REQUIRED flag on the question in the VFR, which makes the browser
  // prompt after the form is saved.  Do NOT set
  // EFI_BROWSER_ACTION_REQUEST_RESET here: the browser implements that as
  // "discard every uncommitted form edit and exit the formset"
  // (SetupBrowserDxe Presentation.c, DiscardFormIsRequired + NeedExit), which
  // ejected the user straight back to Device Manager with the on-screen
  // selection reverted - even though the MCU write below had already
  // succeeded.  That mismatch made a working mux switch look broken.
  //
  if (QuestionId == VIM3_QUESTION_USB_PCIE_MUX) {
    Mode = Value->u8;
    if (Mode > MCU_MUX_PCIE) {
      return EFI_INVALID_PARAMETER;
    }

    Status = McuWriteVerify (MCU_REG_USB_PCIE, Mode);
    DEBUG ((
      EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
      "Vim3Mcu: select %a mode: %r\n",
      Mode == MCU_MUX_USB3 ? "USB3" : "PCIe",
      Status
      ));

    return Status;
  }

  //
  // The two 4-byte IR power-key codes.  No reset needed - the MCU applies them
  // immediately.
  //
  if ((QuestionId == VIM3_QUESTION_IR_CODE1) ||
      (QuestionId == VIM3_QUESTION_IR_CODE2))
  {
    Register = (QuestionId == VIM3_QUESTION_IR_CODE1) ?
               MCU_REG_IR_CODE1 : MCU_REG_IR_CODE2;
    Status   = McuWriteIrCode (Register, Value->u32);
    DEBUG ((
      EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
      "Vim3Mcu: IR code @0x%02x = 0x%08x: %r\n",
      Register,
      Value->u32,
      Status
      ));
    return Status;
  }

  //
  // Single-byte MCU settings (wake sources, key mode, LED modes, MCU sleep).
  // Vim3McuWriteByte's whitelist keeps this to the configuration registers.
  // These take effect in the MCU immediately, so no firmware reset is needed.
  //
  switch (QuestionId) {
    case VIM3_QUESTION_WAKE_WOL:   Register = MCU_REG_BOOT_EN_WOL;  break;
    case VIM3_QUESTION_WAKE_RTC:   Register = MCU_REG_BOOT_EN_RTC;  break;
    case VIM3_QUESTION_WAKE_EXP:   Register = MCU_REG_BOOT_EN_EXP;  break;
    case VIM3_QUESTION_WAKE_IR:    Register = MCU_REG_BOOT_EN_IR;   break;
    case VIM3_QUESTION_WAKE_DCIN:  Register = MCU_REG_BOOT_EN_DCIN; break;
    case VIM3_QUESTION_WAKE_KEY:   Register = MCU_REG_BOOT_EN_KEY;  break;
    case VIM3_QUESTION_KEY_MODE:   Register = MCU_REG_KEY_MODE;     break;
    case VIM3_QUESTION_LED_ON:     Register = MCU_REG_LED_MODE_ON;  break;
    case VIM3_QUESTION_LED_OFF:    Register = MCU_REG_LED_MODE_OFF; break;
    case VIM3_QUESTION_MCU_SLEEP:  Register = MCU_REG_MCU_SLEEP;    break;
    default:
      return EFI_UNSUPPORTED;
  }

  Status = McuWriteVerify (Register, Value->u8);
  DEBUG ((
    EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
    "Vim3Mcu: settings reg 0x%02x = 0x%02x: %r\n",
    Register,
    Value->u8,
    Status
    ));

  return Status;
}

STATIC
EFI_STATUS
InstallHiiPage (
  VOID
  )
{
  EFI_STATUS      Status;

  ZeroMem (&mPrivate, sizeof (mPrivate));
  mPrivate.ConfigAccess.ExtractConfig = ExtractConfig;
  mPrivate.ConfigAccess.RouteConfig   = RouteConfig;
  mPrivate.ConfigAccess.Callback      = ConfigCallback;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mPrivate.DriverHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mHiiDevicePath,
                  &gEfiHiiConfigAccessProtocolGuid,
                  &mPrivate.ConfigAccess,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mPrivate.HiiHandle = HiiAddPackages (
                         &gVim3PlatformFormSetGuid,
                         mPrivate.DriverHandle,
                         Vim3McuDxeStrings,
                         Vim3McuDxeHiiBin,
                         NULL
                         );
  if (mPrivate.HiiHandle == NULL) {
    gBS->UninstallMultipleProtocolInterfaces (
           mPrivate.DriverHandle,
           &gEfiDevicePathProtocolGuid,
           &mHiiDevicePath,
           &gEfiHiiConfigAccessProtocolGuid,
           &mPrivate.ConfigAccess,
           NULL
           );
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

//
// Publish the MCU firmware version and the last-power-off status on the page.
//
STATIC
VOID
PublishMcuInfo (
  VOID
  )
{
  UINT16  Version;
  UINT8   Shutdown;
  CHAR16  Text[24];

  if (mPrivate.HiiHandle == NULL) {
    return;
  }

  if (!EFI_ERROR (Vim3McuGetVersion (&Version))) {
    //
    // Shown verbatim: the production MCU on this board reports 00.00, and
    // inventing a prettier number would be lying.
    //
    UnicodeSPrint (Text, sizeof (Text), L"%02x.%02x", Version >> 8, Version & 0xFF);
    HiiSetString (mPrivate.HiiHandle, STRING_TOKEN (STR_MCU_VERSION_VALUE), Text, NULL);
  }

  if (!EFI_ERROR (Vim3McuGetShutdownStatus (&Shutdown))) {
    HiiSetString (
      mPrivate.HiiHandle,
      STRING_TOKEN (STR_SHUTDOWN_STATUS_VALUE),
      (Shutdown == 0) ? L"Normal" : L"Aborted",
      NULL
      );
  }
}
#endif // VIM3_MCU_TEST_APP

EFI_STATUS
EFIAPI
Vim3McuDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                     Status;
  VIM3_FAN_POLICY_VARSTORE_DATA  Policy;
  UINTN                          Size;
#ifndef VIM3_MCU_TEST_APP
  UINT8                              UsbPcieMode;
  VIM3_HW_DESCRIPTION_VARSTORE_DATA  HwDescription;
  VIM3_SPI_NOR_INSTALL_VARSTORE_DATA  SpiNorVar;
  VIM3_USB3_PCIE_VARSTORE_DATA       MuxVar;
#endif

  //
  // Vim3McuInit runs the identity/trust probe (register map, DEVICE_NO and the
  // BOOT_MODE == 1 anti-stale signature) once.  If it fails, still install the
  // setup page (it just cannot write the MCU) - except in the test app, which
  // must fail loudly.
  //
  Status = Vim3McuInit ();
  if (EFI_ERROR (Status) || !Vim3McuWritesTrusted ()) {
    DEBUG ((DEBUG_ERROR, "Vim3Mcu: MCU not trusted (%r); no MCU writes\n", Status));
#ifdef VIM3_MCU_TEST_APP
    return EFI_COMPROMISED_DATA;
#else
    return InstallHiiPage ();
#endif
  }

  //
  // Fan policy: persist the default if unset, then apply the level.
  //
  Size   = sizeof (Policy);
  Status = gRT->GetVariable (
                  VIM3_FAN_POLICY_VARIABLE_NAME,
                  &gVim3PlatformFormSetGuid,
                  NULL,
                  &Size,
                  &Policy
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (Policy)) ||
      (Policy.Level > VIM3_FAN_LEVEL_3))
  {
    Policy.Level = VIM3_FAN_LEVEL_1;
    Status       = gRT->SetVariable (
                          VIM3_FAN_POLICY_VARIABLE_NAME,
                          &gVim3PlatformFormSetGuid,
                          FAN_POLICY_ATTRIBUTES,
                          sizeof (Policy),
                          &Policy
                          );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "Vim3Mcu: failed to persist default fan policy: %r\n", Status));
    }
  }

  Status = Vim3McuWriteByte (MCU_REG_FAN_CONTROL, Policy.Level);
  DEBUG ((
    EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
    "Vim3Mcu: apply fan level %u: %r\n",
    Policy.Level,
    Status
    ));

#ifdef VIM3_MCU_TEST_APP
  //
  // BootNext diagnostics must not return into BDS: an application is torn down
  // on return, while a production DXE driver's HII packages remain resident.
  // A warm reset consumes BootNext and resumes the normal order.
  //
  gRT->ResetSystem (EfiResetWarm, Status, 0, NULL);
  CpuDeadLoop ();
  return EFI_DEVICE_ERROR;
#else
  //
  // Materialize the OS hardware-description selector with its default so the
  // setup browser always has a backing variable.  Vim3PlatformDxe consumes it.
  //
  Size   = sizeof (HwDescription);
  Status = gRT->GetVariable (
                  VIM3_HW_DESCRIPTION_VARIABLE_NAME,
                  &gVim3PlatformFormSetGuid,
                  NULL,
                  &Size,
                  &HwDescription
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (HwDescription)) ||
      (HwDescription.Mode > VIM3_HW_DESCRIPTION_ACPI))
  {
    HwDescription.Mode = VIM3_HW_DESCRIPTION_DEVICE_TREE;
    Status             = gRT->SetVariable (
                                VIM3_HW_DESCRIPTION_VARIABLE_NAME,
                                &gVim3PlatformFormSetGuid,
                                FAN_POLICY_ATTRIBUTES,
                                sizeof (HwDescription),
                                &HwDescription
                                );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "Vim3Mcu: failed to persist default HwDescription: %r\n", Status));
    }
  }

  //
  // Same for SpiNorInstall.  This one is NOT optional bookkeeping: every
  // efivarstore on the form must already exist as a variable, or the whole
  // form fails to save.
  //
  // HiiConfigRouting handles efivarstore submits itself
  // (RouteConfigRespForEfiVarStore, ConfigRouting.c) rather than calling this
  // driver's RouteConfig, and it begins with
  //
  //   Status = gRT->GetVariable (Name, Guid, NULL, &BufferSize, NULL);
  //   if (Status != EFI_BUFFER_TOO_SMALL) { goto Done; }
  //
  // A variable that does not exist returns EFI_NOT_FOUND, not
  // EFI_BUFFER_TOO_SMALL, so the route fails - and SubmitForForm() routes
  // EVERY storage on the form, not just the changed one.  One missing
  // variable therefore makes every setting on the form unsavable with
  // "Submit Fail For Form", which is exactly what SpiNorInstall did: it was
  // the only one of the five never materialized.
  //
  // The damage was masked because the INTERACTIVE questions (USB3/PCIe mux,
  // the wake sources) act from ConfigCallback on EFI_BROWSER_ACTION_CHANGED
  // and never needed the submit path, so the form looked half-working.
  //
  Size   = sizeof (SpiNorVar);
  Status = gRT->GetVariable (
                  VIM3_SPI_NOR_INSTALL_VARIABLE_NAME,
                  &gVim3PlatformFormSetGuid,
                  NULL,
                  &Size,
                  &SpiNorVar
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (SpiNorVar)) ||
      (SpiNorVar.Enable > VIM3_SPI_NOR_INSTALL_NOW))
  {
    SpiNorVar.Enable = VIM3_SPI_NOR_INSTALL_DISABLED;
    Status           = gRT->SetVariable (
                              VIM3_SPI_NOR_INSTALL_VARIABLE_NAME,
                              &gVim3PlatformFormSetGuid,
                              FAN_POLICY_ATTRIBUTES,
                              sizeof (SpiNorVar),
                              &SpiNorVar
                              );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "Vim3Mcu: failed to persist default SpiNorInstall: %r\n", Status));
    }
  }

  //
  // Read the persisted mux mode, patch the published DT to match, and mirror it
  // into the setup variable.  The MCU (0x33) stays the source of truth.
  //
  if (EFI_ERROR (Vim3McuReadByte (MCU_REG_USB_PCIE, &UsbPcieMode)) ||
      (UsbPcieMode > MCU_MUX_PCIE))
  {
    DEBUG ((DEBUG_ERROR, "Vim3Mcu: invalid USB3/PCIe selector; leaving factory DT policy\n"));
    MuxVar.Mode = VIM3_USB3_PCIE_USB3;
  } else {
    DEBUG ((
      DEBUG_INFO,
      "Vim3Mcu: persisted mux mode is %a\n",
      UsbPcieMode == MCU_MUX_USB3 ? "USB3" : "PCIe"
      ));
    MuxVar.Mode = UsbPcieMode;
    Status      = PatchPublishedDtb (UsbPcieMode);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Vim3Mcu: published DT mux patch failed: %r\n", Status));
    }
  }

  Status = gRT->SetVariable (
                  VIM3_USB3_PCIE_VARIABLE_NAME,
                  &gVim3PlatformFormSetGuid,
                  FAN_POLICY_ATTRIBUTES,
                  sizeof (MuxVar),
                  &MuxVar
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "Vim3Mcu: failed to mirror mux mode to setup variable: %r\n", Status));
  }

  //
  // Mirror the MCU wake/LED/key/IR configuration into the McuSettings variable.
  //
  MirrorMcuSettings ();

  Status = InstallHiiPage ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Mcu: HII installation failed: %r\n", Status));
    return Status;
  }

  PublishMcuInfo ();

  //
  // Publish an initial SoC temperature so the field is populated before the
  // browser first opens the page (it refreshes on FORM_OPEN thereafter).
  //
  RefreshSocTemperature ();

  return EFI_SUCCESS;
#endif
}
