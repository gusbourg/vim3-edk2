/** @file
  Amlogic Meson G12B/A311D HDMI display definitions.

  Register programming is derived from the public A311D datasheet and from
  reading the BSD-licensed Amlogic display and DesignWare HDMI drivers in
  Fuchsia (Copyright The Fuchsia Authors, BSD-3-Clause).  No Fuchsia code is
  reproduced here - the sequences were reimplemented against the datasheet
  and verified on hardware - but the debt is real and worth naming.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef MESON_DISPLAY_H_
#define MESON_DISPLAY_H_

#include <Uefi.h>

#include <Protocol/EdidActive.h>
#include <Protocol/EdidDiscovered.h>
#include <Protocol/GraphicsOutput.h>

#include <Library/FrameBufferBltLib.h>

#define MESON_VPU_BASE       0xFF900000UL
#define MESON_HHI_BASE       0xFF63C000UL
#define MESON_DMC_BASE       0xFF638000UL
#define MESON_AO_RTI_BASE    0xFF800000UL
#define MESON_RESET_BASE     0xFFD01000UL
#define MESON_GPIO_BASE      0xFF634400UL
#define MESON_HDMI_DWC_BASE  0xFF600000UL
#define MESON_HDMI_TOP_BASE  0xFF608000UL
#define MESON_WDT_BASE       0xFFD0F0D0UL

#define MESON_SYSTEM_MEMORY_TOP  0xF4E5B000ULL
#define MESON_DTB_REGION_SIZE    0x0001F000

#define MESON_CANVAS_INDEX  0x40
#define MESON_MAX_EDID_SIZE 512

#define MESON_REG(_Index)  ((_Index) * sizeof (UINT32))

typedef enum {
  MesonVclkEnci54,
  MesonVclkDdr54,
  MesonVclkDdr148500,
  MesonVclk74250,
  MesonVclk148500,
  MesonVclk297000
} MESON_VCLK_PROFILE;

typedef enum {
  MesonEncoderEnci480i,
  MesonEncoderEnci576i,
  MesonEncoderEncp480p,
  MesonEncoderEncp576p,
  MesonEncoderEncp720p60,
  MesonEncoderEncp720p50,
  MesonEncoderEncp1080i60,
  MesonEncoderEncp1080i50,
  MesonEncoderEncp1080p24,
  MesonEncoderEncp1080p30,
  MesonEncoderEncp1080p50,
  MesonEncoderEncp1080p60,
  MesonEncoderEncp2160p24,
  MesonEncoderEncp2160p25,
  MesonEncoderEncp2160p30
} MESON_ENCODER_PROFILE;

typedef struct {
  UINT32  Width;
  UINT32  Height;
  UINT32  PixelClockKhz;
  UINT32  HFrontPorch;
  UINT32  HSync;
  UINT32  HBackPorch;
  UINT32  VFrontPorch;
  UINT32  VSync;
  UINT32  VBackPorch;
  UINT8   CtaVic;
  BOOLEAN Interlaced;
  BOOLEAN HSyncPositive;
  BOOLEAN VSyncPositive;
  BOOLEAN HdmiRepeat;
  BOOLEAN VencRepeat;
  BOOLEAN UseEnci;
  MESON_VCLK_PROFILE     ClockProfile;
  MESON_ENCODER_PROFILE  EncoderProfile;
} MESON_DISPLAY_MODE;

typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} MESON_DISPLAY_DEVICE_PATH;

typedef struct {
  UINT32                             Signature;
  EFI_HANDLE                         Handle;
  EFI_GRAPHICS_OUTPUT_PROTOCOL       Gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE  GopMode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  ModeInfo;
  EFI_EDID_DISCOVERED_PROTOCOL       EdidDiscovered;
  EFI_EDID_ACTIVE_PROTOCOL           EdidActive;
  MESON_DISPLAY_DEVICE_PATH          DevicePath;
  MESON_DISPLAY_MODE                 Timing;
  EFI_PHYSICAL_ADDRESS               FrameBufferBase;
  UINTN                              FrameBufferSize;
  UINT32                             Stride;
  VOID                               *BltConfigure;
  UINT32                             BltCount;
  UINT8                              Edid[MESON_MAX_EDID_SIZE];
  UINT32                             EdidSize;
  BOOLEAN                            HdmiSink;
} MESON_DISPLAY_PRIVATE;

#define MESON_DISPLAY_SIGNATURE  SIGNATURE_32 ('M', 'G', 'O', 'P')
#define MESON_DISPLAY_FROM_GOP(_Gop) \
  CR ((_Gop), MESON_DISPLAY_PRIVATE, Gop, MESON_DISPLAY_SIGNATURE)

extern CONST MESON_DISPLAY_MODE  gMesonMode1080p;
extern CONST MESON_DISPLAY_MODE  gMesonMode720p;

EFI_STATUS
MesonDisplayHardwareInit (
  IN OUT MESON_DISPLAY_PRIVATE  *Private
  );

EFI_STATUS
MesonDisplayWakeSink (
  VOID
  );

BOOLEAN
MesonDisplayHotPlugDetected (
  VOID
  );

EFI_STATUS
MesonDisplayReadEdid (
  OUT UINT8   *Edid,
  IN  UINT32  Capacity,
  OUT UINT32  *EdidSize
  );

VOID
MesonDisplayChooseMode (
  IN  CONST UINT8          *Edid,
  IN  UINT32               EdidSize,
  OUT MESON_DISPLAY_MODE   *Mode,
  OUT BOOLEAN              *HdmiSink
  );

#endif
