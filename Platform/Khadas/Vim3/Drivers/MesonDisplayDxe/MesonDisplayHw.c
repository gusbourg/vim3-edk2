/** @file
  A311D VPU, OSD1, HDMI encoder, DesignWare TX, PHY, and EDID support.

  This is intentionally a fixed-mode boot-services-only implementation.  It
  supports CEA 1080p60 and 720p60 and contains no GPU, RDMA, audio, HDCP, CEC,
  or runtime code.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "MesonDisplay.h"

#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#define BIT(_Bit)  (1U << (_Bit))

#define HHI_MEM_PD_REG0       0x40
#define HHI_VPU_MEM_PD_REG0   0x41
#define HHI_VPU_MEM_PD_REG1   0x42
#define HHI_VPU_MEM_PD_REG2   0x4D
#define HHI_GCLK_MPEG2        0x52
#define HHI_VID_CLK_DIV       0x59
#define HHI_VID_CLK_CNTL      0x5F
#define HHI_VID_CLK_CNTL2     0x65
#define HHI_VID_PLL_CLK_DIV   0x68
#define HHI_VPU_CLK_CNTL      0x6F
#define HHI_HDMI_CLK_CNTL     0x73
#define HHI_VAPBCLK_CNTL      0x7D
#define HHI_VPU_CLKB_CNTL     0x83
#define HHI_HDMI_PLL_CNTL0    0xC8
#define HHI_HDMI_PLL_CNTL1    0xC9
#define HHI_HDMI_PLL_CNTL2    0xCA
#define HHI_HDMI_PLL_CNTL3    0xCB
#define HHI_HDMI_PLL_CNTL4    0xCC
#define HHI_HDMI_PLL_CNTL5    0xCD
#define HHI_HDMI_PLL_CNTL6    0xCE
#define HHI_HDMI_PHY_CNTL0    0xE8
#define HHI_HDMI_PHY_CNTL1    0xE9
#define HHI_HDMI_PHY_CNTL3    0xEB
#define HHI_HDMI_PHY_CNTL5    0xED
#define HHI_VDAC_CNTL0        0xBB
#define HHI_VDAC_CNTL1        0xBC

#define VPU_ENCI_VIDEO_MODE       0x1B00
#define VPU_ENCI_VIDEO_MODE_ADV   0x1B01
#define VPU_ENCI_VIDEO_SCH        0x1B07
#define VPU_ENCI_SYNC_MODE        0x1B08
#define VPU_ENCI_SYNC_HSO_BEG     0x1B0A
#define VPU_ENCI_SYNC_HSO_END     0x1B0B
#define VPU_ENCI_SYNC_VSO_EVNLN   0x1B0E
#define VPU_ENCI_SYNC_VSO_ODDLN   0x1B0F
#define VPU_ENCI_DE_H_BEG         0x1B16
#define VPU_ENCI_DE_H_END         0x1B17
#define VPU_ENCI_DE_V_BEG_EVN     0x1B18
#define VPU_ENCI_DE_V_END_EVN     0x1B19
#define VPU_ENCI_DE_V_BEG_ODD     0x1B1A
#define VPU_ENCI_DE_V_END_ODD     0x1B1B
#define VPU_ENCI_DBG_PX_RST       0x1B48
#define VPU_ENCI_MACV_MAX_AMP     0x1B50
#define VPU_ENCI_CFILT_CTRL       0x1B54
#define VPU_ENCI_YC_DELAY         0x1B56
#define VPU_ENCI_VIDEO_EN         0x1B57
#define VPU_VENC_DVI_SETTING      0x1B62
#define VPU_VENC_VIDEO_PROG_MODE  0x1B68
#define VPU_ENCP_VIDEO_EN         0x1B80
#define VPU_ENCP_VIDEO_SYNC_MODE  0x1B81
#define VPU_ENCP_VIDEO_MODE       0x1B8D
#define VPU_ENCP_VIDEO_MODE_ADV   0x1B8E
#define VPU_ENCP_VIDEO_YFP1_HTIME 0x1B94
#define VPU_ENCP_VIDEO_YFP2_HTIME 0x1B95
#define VPU_ENCP_VIDEO_YC_DLY      0x1B96
#define VPU_ENCP_VIDEO_MAX_PXCNT  0x1B97
#define VPU_ENCP_VIDEO_HSPULS_BEG 0x1B98
#define VPU_ENCP_VIDEO_HSPULS_END 0x1B99
#define VPU_ENCP_VIDEO_HSPULS_SW  0x1B9A
#define VPU_ENCP_VIDEO_VSPULS_BEG 0x1B9B
#define VPU_ENCP_VIDEO_VSPULS_END 0x1B9C
#define VPU_ENCP_VIDEO_VSPULS_BLN 0x1B9D
#define VPU_ENCP_VIDEO_VSPULS_ELN 0x1B9E
#define VPU_ENCP_VIDEO_EQPULS_BEG 0x1B9F
#define VPU_ENCP_VIDEO_EQPULS_END 0x1BA0
#define VPU_ENCP_VIDEO_EQPULS_BLN 0x1BA1
#define VPU_ENCP_VIDEO_EQPULS_ELN 0x1BA2
#define VPU_ENCP_VIDEO_HAVON_END  0x1BA3
#define VPU_ENCP_VIDEO_HAVON_BEG  0x1BA4
#define VPU_ENCP_VIDEO_VAVON_BEG  0x1BA6
#define VPU_ENCP_VIDEO_HSO_BEG    0x1BA7
#define VPU_ENCP_VIDEO_HSO_END    0x1BA8
#define VPU_ENCP_VIDEO_VSO_BEG    0x1BA9
#define VPU_ENCP_VIDEO_VSO_END    0x1BAA
#define VPU_ENCP_VIDEO_VSO_BLINE  0x1BAB
#define VPU_ENCP_VIDEO_VSO_ELINE  0x1BAC
#define VPU_ENCP_VIDEO_MAX_LNCNT  0x1BAE
#define VPU_ENCP_VIDEO_VAVON_END  0x1BAF
#define VPU_ENCP_VIDEO_SY_VAL     0x1BB0
#define VPU_ENCP_VIDEO_SY2_VAL    0x1BB1
#define VPU_ENCP_VIDEO_RGB_CTRL   0x1BB7
#define VPU_ENCP_VIDEO_FILT_CTRL  0x1BB8
#define VPU_ENCP_VIDEO_OFLD_VOAV  0x1BBA
#define VPU_ENCI_DVI_HSO_BEG      0x1C00
#define VPU_ENCI_DVI_HSO_END      0x1C01
#define VPU_ENCI_DVI_VSO_BLN_EVN  0x1C02
#define VPU_ENCI_DVI_VSO_BLN_ODD  0x1C03
#define VPU_ENCI_DVI_VSO_ELN_EVN  0x1C04
#define VPU_ENCI_DVI_VSO_ELN_ODD  0x1C05
#define VPU_ENCI_DVI_VSO_BEG_EVN  0x1C06
#define VPU_ENCI_DVI_VSO_BEG_ODD  0x1C07
#define VPU_ENCI_DVI_VSO_END_EVN  0x1C08
#define VPU_ENCI_DVI_VSO_END_ODD  0x1C09
#define VPU_ENCI_CFILT_CTRL2      0x1C0A
#define VPU_ENCI_VFIFO2VD_CTL     0x1C18
#define VPU_ENCI_VFIFO2VD_PX_BEG  0x1C19
#define VPU_ENCI_VFIFO2VD_PX_END  0x1C1A
#define VPU_ENCI_VFIFO2VD_LN_T_BEG 0x1C1B
#define VPU_ENCI_VFIFO2VD_LN_T_END 0x1C1C
#define VPU_ENCI_VFIFO2VD_LN_B_BEG 0x1C1D
#define VPU_ENCI_VFIFO2VD_LN_B_END 0x1C1E
#define VPU_ENCP_DVI_HSO_BEG      0x1C30
#define VPU_ENCP_DVI_HSO_END      0x1C31
#define VPU_ENCP_DVI_VSO_BLINE    0x1C32
#define VPU_ENCP_DVI_VSO_BLINE_ODD 0x1C33
#define VPU_ENCP_DVI_VSO_ELINE    0x1C34
#define VPU_ENCP_DVI_VSO_ELINE_ODD 0x1C35
#define VPU_ENCP_DVI_VSO_BEG      0x1C36
#define VPU_ENCP_DVI_VSO_BEG_ODD  0x1C37
#define VPU_ENCP_DVI_VSO_END      0x1C38
#define VPU_ENCP_DVI_VSO_END_ODD  0x1C39
#define VPU_ENCP_DE_H_BEG         0x1C3A
#define VPU_ENCP_DE_H_END         0x1C3B
#define VPU_ENCP_DE_V_BEG         0x1C3C
#define VPU_ENCP_DE_V_END         0x1C3D
#define VPU_ENCP_DE_V_BEG_ODD     0x1C3E
#define VPU_ENCP_DE_V_END_ODD     0x1C3F
#define VPU_VIU_VENC_MUX_CTRL     0x271A
#define VPU_HDMI_SETTING          0x271B
#define VPU_HDMI_FMT_CTRL         0x2743
#define VPU_HDMI_DITH_CNTL        0x27FC

#define VPU_OSD1_CTRL_STAT        0x1A10
#define VPU_OSD1_BLK0_CFG_W0      0x1A1B
#define VPU_OSD1_BLK0_CFG_W1      0x1A1C
#define VPU_OSD1_BLK0_CFG_W2      0x1A1D
#define VPU_OSD1_BLK0_CFG_W3      0x1A1E
#define VPU_OSD1_BLK0_CFG_W4      0x1A13
#define VPU_OSD1_FIFO_CTRL_STAT   0x1A2B
#define VPU_OSD1_CTRL_STAT2       0x1A2D
#define VPU_OSD1_MALI_UNPACK      0x1A2F
#define VPU_OSD_PATH_MISC_CTRL    0x1A0E
#define VPU_MAFBC_SURFACE_CFG     0x3A07

#define DOLBY_PATH_CTRL           0x1A0C
#define VPP_POSTBLEND_H_SIZE      0x1D21
#define VPP_HOLD_LINES            0x1D22
#define VPP_MISC                  0x1D26
#define VPP_OFIFO_SIZE            0x1D27
#define VPP_OSD_SC_CTRL0          0x1DC8
#define VPP_OSD_VSC_CTRL0         0x1DC2
#define VPP_OSD_HSC_CTRL0         0x1DC5
#define VPP_OSD1_IN_SIZE          0x1DF1
#define VPP_OSD1_BLD_H_SCOPE      0x1DF5
#define VPP_OSD1_BLD_V_SCOPE      0x1DF6
#define OSD1_BLEND_SRC_CTRL       0x1DFD
#define OSD2_BLEND_SRC_CTRL       0x1DFE
#define VPP_OUT_H_V_SIZE          0x1DA5
#define VPP_POST2_MATRIX_EN       0x39AD
#define VIU_OSD_BLEND_CTRL        0x39B0
#define VIU_OSD_BLEND_DIN0_H      0x39B1
#define VIU_OSD_BLEND_DIN0_V      0x39B2
#define VIU_OSD_BLEND_DUMMY0      0x39B9
#define VIU_OSD_BLEND_DUMMY_ALPHA 0x39BA
#define VIU_OSD_BLEND0_SIZE       0x39BB
#define VIU_OSD_BLEND1_SIZE       0x39BC
#define VPP_WRAP_OSD1_MATRIX_COEF00_01    0x3D60
#define VPP_WRAP_OSD1_MATRIX_COEF02_10    0x3D61
#define VPP_WRAP_OSD1_MATRIX_COEF11_12    0x3D62
#define VPP_WRAP_OSD1_MATRIX_COEF20_21    0x3D63
#define VPP_WRAP_OSD1_MATRIX_COEF22       0x3D64
#define VPP_WRAP_OSD1_MATRIX_OFFSET0_1    0x3D69
#define VPP_WRAP_OSD1_MATRIX_OFFSET2      0x3D6A
#define VPP_WRAP_OSD1_MATRIX_PRE_OFFSET0_1 0x3D6B
#define VPP_WRAP_OSD1_MATRIX_PRE_OFFSET2  0x3D6C
#define VPP_WRAP_OSD1_MATRIX_EN_CTRL      0x3D6D
#define OSD1_HDR2_CTRL                    0x38A0
#define VPU_RDARB_MODE_L1C1       0x2790
#define VPU_RDARB_MODE_L1C2       0x2799
#define VPU_RDARB_MODE_L2C1       0x279D
#define VPU_WRARB_MODE_L2C1       0x27A2

#define HDMITX_TOP_SW_RESET       0x000
#define HDMITX_TOP_CLK_CNTL       0x001
#define HDMITX_TOP_HPD_FILTER     0x002
#define HDMITX_TOP_INTR_MASKN     0x003
#define HDMITX_TOP_INTR_CLR       0x005
#define HDMITX_TOP_BIST_CNTL      0x006
#define HDMITX_TOP_TMDS_PTTN_01   0x00A
#define HDMITX_TOP_TMDS_PTTN_23   0x00B
#define HDMITX_TOP_TMDS_PTTN_CTL  0x00C
#define HDMITX_TOP_STAT0          0x00E

#define DWC_IH_FC_STAT0       0x0100
#define DWC_IH_FC_STAT1       0x0101
#define DWC_IH_FC_STAT2       0x0102
#define DWC_IH_AS_STAT0       0x0103
#define DWC_IH_PHY_STAT0      0x0104
#define DWC_IH_I2CM_STAT0     0x0105
#define DWC_IH_CEC_STAT0      0x0106
#define DWC_IH_VP_STAT0       0x0107
#define DWC_IH_I2CMPHY_STAT0  0x0108
#define DWC_IH_MUTE_I2CM      0x0185
#define DWC_IH_MUTE            0x01FF
#define DWC_TX_INVID0          0x0200
#define DWC_TX_INSTUFFING      0x0201
#define DWC_TX_GYDATA0         0x0202
#define DWC_TX_GYDATA1         0x0203
#define DWC_TX_RCRDATA0        0x0204
#define DWC_TX_RCRDATA1        0x0205
#define DWC_TX_BCBDATA0        0x0206
#define DWC_TX_BCBDATA1        0x0207
#define DWC_VP_PR_CD           0x0801
#define DWC_VP_STUFF           0x0802
#define DWC_VP_REMAP           0x0803
#define DWC_VP_CONF            0x0804
#define DWC_VP_MASK            0x0807
#define DWC_FC_INVIDCONF       0x1000
#define DWC_FC_INHACTV0        0x1001
#define DWC_FC_INHACTV1        0x1002
#define DWC_FC_INHBLANK0       0x1003
#define DWC_FC_INHBLANK1       0x1004
#define DWC_FC_INVACTV0        0x1005
#define DWC_FC_INVACTV1        0x1006
#define DWC_FC_INVBLANK        0x1007
#define DWC_FC_HSYNCDELAY0     0x1008
#define DWC_FC_HSYNCDELAY1     0x1009
#define DWC_FC_HSYNCWIDTH0     0x100A
#define DWC_FC_HSYNCWIDTH1     0x100B
#define DWC_FC_VSYNCDELAY      0x100C
#define DWC_FC_VSYNCWIDTH      0x100D
#define DWC_FC_CTRLDUR         0x1011
#define DWC_FC_EXCTRLDUR       0x1012
#define DWC_FC_EXCTRLSPAC      0x1013
#define DWC_FC_AVICONF3        0x1017
#define DWC_FC_GCP             0x1018
#define DWC_FC_AVICONF0        0x1019
#define DWC_FC_AVICONF1        0x101A
#define DWC_FC_AVICONF2        0x101B
#define DWC_FC_AVIVID          0x101C
#define DWC_FC_MASK0           0x10D2
#define DWC_FC_MASK1           0x10D6
#define DWC_FC_MASK2           0x10DA
#define DWC_FC_PRCONF          0x10E0
#define DWC_FC_ACTSPC          0x10E8
#define DWC_FC_INVACT2D0       0x10E9
#define DWC_FC_INVACT2D1       0x10EA
#define DWC_MC_CLKDIS          0x4001
#define DWC_MC_SWRSTZREQ       0x4002
#define DWC_MC_FLOWCTRL        0x4004
#define DWC_MC_LOCKONCLOCK     0x4006

#define DWC_MC_CLKDIS_HDCP     BIT (6)
#define DWC_MC_CLKDIS_CEC      BIT (5)
#define DWC_MC_CLKDIS_AUDIO    BIT (3)
#define DWC_MC_CLKDIS_PREP     BIT (2)
#define DWC_CSC_CFG            0x4100
#define DWC_CSC_SCALE          0x4101
#define DWC_CSC_COEF_A1_MSB    0x4102
#define DWC_I2CM_SLAVE         0x7E00
#define DWC_I2CM_ADDRESS       0x7E01
#define DWC_I2CM_OPERATION     0x7E04
#define DWC_I2CM_INT           0x7E05
#define DWC_I2CM_CTLINT        0x7E06
#define DWC_I2CM_DIV           0x7E07
#define DWC_I2CM_SEGADDR       0x7E08
#define DWC_I2CM_SEGPTR        0x7E0A
#define DWC_I2CM_SS_HCNT1      0x7E0B
#define DWC_I2CM_SS_HCNT0      0x7E0C
#define DWC_I2CM_SS_LCNT1      0x7E0D
#define DWC_I2CM_SS_LCNT0      0x7E0E
#define DWC_I2CM_FS_HCNT1      0x7E0F
#define DWC_I2CM_FS_HCNT0      0x7E10
#define DWC_I2CM_FS_LCNT1      0x7E11
#define DWC_I2CM_FS_LCNT0      0x7E12
#define DWC_I2CM_SDA_HOLD      0x7E13
#define DWC_I2CM_SCDC          0x7E14
#define DWC_I2CM_READ_BUFF0    0x7E20

#define RESET0_LEVEL  0x80
#define RESET1_LEVEL  0x84
#define RESET2_LEVEL  0x88
#define RESET4_LEVEL  0x90
#define RESET7_LEVEL  0x9C

#define GPIO_H_DIRECTION  (MESON_GPIO_BASE + MESON_REG (0x19))
#define GPIO_H_OUTPUT     (MESON_GPIO_BASE + MESON_REG (0x1A))
#define MUX_GPIOH_8       (MESON_GPIO_BASE + MESON_REG (0xBC))

#define WDT_CTRL  0x00
#define WDT_TCNT  0x08
#define WDT_RESET 0x0C

#define MESON_MODE(Width, Height, Clock, Hfp, Hsync, Hbp, Vfp, Vsync, Vbp, \
                   Vic, Interlace, Hpos, Vpos, Hrepeat, Vrepeat, Enci,      \
                   ClockProfile, EncoderProfile)                           \
  {                                                                        \
    Width, Height, Clock, Hfp, Hsync, Hbp, Vfp, Vsync, Vbp, Vic,          \
    Interlace, Hpos, Vpos, Hrepeat, Vrepeat, Enci, ClockProfile,           \
    EncoderProfile                                                         \
  }

CONST MESON_DISPLAY_MODE  gMesonMode1080p =
  MESON_MODE (
    1920, 1080, 148500, 88, 44, 148, 4, 5, 36, 16,
    FALSE, TRUE, TRUE, FALSE, FALSE, FALSE,
    MesonVclk148500, MesonEncoderEncp1080p60
    );

CONST MESON_DISPLAY_MODE  gMesonMode720p =
  MESON_MODE (
    1280, 720, 74250, 110, 40, 220, 5, 5, 20, 4,
    FALSE, TRUE, TRUE, FALSE, TRUE, FALSE,
    MesonVclkDdr148500, MesonEncoderEncp720p60
    );

//
// CTA timings with an explicit mainline Meson ENCI/ENCP hardware table and
// no more than the RM1's published 3840x2160p30 capture ceiling.  Aliases are
// separate entries so the selected VIC survives into the AVI InfoFrame.
//
STATIC CONST MESON_DISPLAY_MODE  mSupportedModes[] = {
  MESON_MODE (
    720, 480, 27000, 16, 62, 60, 9, 6, 30, 2,
    FALSE, FALSE, FALSE, FALSE, TRUE, FALSE,
    MesonVclkDdr54, MesonEncoderEncp480p
    ),
  MESON_MODE (
    720, 480, 27000, 16, 62, 60, 9, 6, 30, 3,
    FALSE, FALSE, FALSE, FALSE, TRUE, FALSE,
    MesonVclkDdr54, MesonEncoderEncp480p
    ),
  MESON_MODE (
    1280, 720, 74250, 110, 40, 220, 5, 5, 20, 4,
    FALSE, TRUE, TRUE, FALSE, TRUE, FALSE,
    MesonVclkDdr148500, MesonEncoderEncp720p60
    ),
  MESON_MODE (
    1920, 1080, 74250, 88, 44, 148, 4, 10, 31, 5,
    TRUE, TRUE, TRUE, FALSE, TRUE, FALSE,
    MesonVclkDdr148500, MesonEncoderEncp1080i60
    ),
  MESON_MODE (
    720, 480, 13500, 19, 62, 57, 8, 6, 31, 6,
    TRUE, FALSE, FALSE, TRUE, TRUE, TRUE,
    MesonVclkEnci54, MesonEncoderEnci480i
    ),
  MESON_MODE (
    720, 480, 13500, 19, 62, 57, 8, 6, 31, 7,
    TRUE, FALSE, FALSE, TRUE, TRUE, TRUE,
    MesonVclkEnci54, MesonEncoderEnci480i
    ),
  MESON_MODE (
    1920, 1080, 148500, 88, 44, 148, 4, 5, 36, 16,
    FALSE, TRUE, TRUE, FALSE, FALSE, FALSE,
    MesonVclk148500, MesonEncoderEncp1080p60
    ),
  MESON_MODE (
    720, 576, 27000, 12, 64, 68, 5, 5, 39, 17,
    FALSE, FALSE, FALSE, FALSE, TRUE, FALSE,
    MesonVclkDdr54, MesonEncoderEncp576p
    ),
  MESON_MODE (
    720, 576, 27000, 12, 64, 68, 5, 5, 39, 18,
    FALSE, FALSE, FALSE, FALSE, TRUE, FALSE,
    MesonVclkDdr54, MesonEncoderEncp576p
    ),
  MESON_MODE (
    1280, 720, 74250, 440, 40, 220, 5, 5, 20, 19,
    FALSE, TRUE, TRUE, FALSE, TRUE, FALSE,
    MesonVclkDdr148500, MesonEncoderEncp720p50
    ),
  MESON_MODE (
    1920, 1080, 74250, 528, 44, 148, 4, 10, 31, 20,
    TRUE, TRUE, TRUE, FALSE, TRUE, FALSE,
    MesonVclkDdr148500, MesonEncoderEncp1080i50
    ),
  MESON_MODE (
    720, 576, 13500, 12, 63, 69, 4, 6, 39, 21,
    TRUE, FALSE, FALSE, TRUE, TRUE, TRUE,
    MesonVclkEnci54, MesonEncoderEnci576i
    ),
  MESON_MODE (
    720, 576, 13500, 12, 63, 69, 4, 6, 39, 22,
    TRUE, FALSE, FALSE, TRUE, TRUE, TRUE,
    MesonVclkEnci54, MesonEncoderEnci576i
    ),
  MESON_MODE (
    1920, 1080, 148500, 528, 44, 148, 4, 5, 36, 31,
    FALSE, TRUE, TRUE, FALSE, FALSE, FALSE,
    MesonVclk148500, MesonEncoderEncp1080p50
    ),
  MESON_MODE (
    1920, 1080, 74250, 638, 44, 148, 4, 5, 36, 32,
    FALSE, TRUE, TRUE, FALSE, FALSE, FALSE,
    MesonVclk74250, MesonEncoderEncp1080p24
    ),
  MESON_MODE (
    1920, 1080, 74250, 528, 44, 148, 4, 5, 36, 33,
    FALSE, TRUE, TRUE, FALSE, FALSE, FALSE,
    MesonVclk74250, MesonEncoderEncp1080p50
    ),
  MESON_MODE (
    1920, 1080, 74250, 88, 44, 148, 4, 5, 36, 34,
    FALSE, TRUE, TRUE, FALSE, FALSE, FALSE,
    MesonVclk74250, MesonEncoderEncp1080p30
    ),
  MESON_MODE (
    3840, 2160, 297000, 1276, 88, 296, 8, 10, 72, 93,
    FALSE, TRUE, TRUE, FALSE, FALSE, FALSE,
    MesonVclk297000, MesonEncoderEncp2160p24
    ),
  MESON_MODE (
    3840, 2160, 297000, 1056, 88, 296, 8, 10, 72, 94,
    FALSE, TRUE, TRUE, FALSE, FALSE, FALSE,
    MesonVclk297000, MesonEncoderEncp2160p25
    ),
  MESON_MODE (
    3840, 2160, 297000, 176, 88, 296, 8, 10, 72, 95,
    FALSE, TRUE, TRUE, FALSE, FALSE, FALSE,
    MesonVclk297000, MesonEncoderEncp2160p30
    )
};

STATIC BOOLEAN  mVpuPowered;
STATIC BOOLEAN  mHostPrepared;

STATIC
VOID
PowerOnVpu (
  VOID
  );

STATIC
UINT32
HhiRead (
  IN UINT32  Index
  )
{
  return MmioRead32 (MESON_HHI_BASE + MESON_REG (Index));
}

STATIC
VOID
HhiWrite (
  IN UINT32  Index,
  IN UINT32  Value
  )
{
  MmioWrite32 (MESON_HHI_BASE + MESON_REG (Index), Value);
}

STATIC
UINT32
VpuRead (
  IN UINT32  Index
  )
{
  return MmioRead32 (MESON_VPU_BASE + MESON_REG (Index));
}

STATIC
VOID
VpuWrite (
  IN UINT32  Index,
  IN UINT32  Value
  )
{
  MmioWrite32 (MESON_VPU_BASE + MESON_REG (Index), Value);
}

STATIC
UINT8
DwcRead (
  IN UINT32  Register
  )
{
  return MmioRead8 (MESON_HDMI_DWC_BASE + Register);
}

STATIC
VOID
DwcWrite (
  IN UINT32  Register,
  IN UINT8   Value
  )
{
  MmioWrite8 (MESON_HDMI_DWC_BASE + Register, Value);
}

STATIC
VOID
TopWrite (
  IN UINT32  Index,
  IN UINT32  Value
  )
{
  MmioWrite32 (MESON_HDMI_TOP_BASE + MESON_REG (Index), Value);
}

STATIC
VOID
WatchdogStart (
  VOID
  )
{
  UINT32  Control;

  MmioAnd32 (MESON_WDT_BASE + WDT_CTRL, ~BIT (18));
  MmioWrite32 (MESON_WDT_BASE + WDT_RESET, 0);
  Control = 24000U | BIT (25) | BIT (24) | BIT (21);
  MmioWrite32 (MESON_WDT_BASE + WDT_CTRL, Control);
  MmioWrite32 (MESON_WDT_BASE + WDT_TCNT, 5000);
  MmioWrite32 (MESON_WDT_BASE + WDT_RESET, 0);
  MmioWrite32 (MESON_WDT_BASE + WDT_CTRL, Control | BIT (18));
  ArmDataSynchronizationBarrier ();
}

STATIC
VOID
WatchdogStop (
  VOID
  )
{
  MmioAnd32 (MESON_WDT_BASE + WDT_CTRL, ~BIT (18));
  MmioWrite32 (MESON_WDT_BASE + WDT_RESET, 0);
  ArmDataSynchronizationBarrier ();
}

STATIC
VOID
ConfigureDdc (
  VOID
  )
{
  DwcWrite (DWC_MC_LOCKONCLOCK, 0xFF);
  DwcWrite (DWC_MC_CLKDIS, 0);
  DwcWrite (DWC_I2CM_INT, 0);
  DwcWrite (DWC_I2CM_CTLINT, 0);
  DwcWrite (DWC_I2CM_DIV, 0);
  DwcWrite (DWC_I2CM_SS_HCNT1, 0);
  DwcWrite (DWC_I2CM_SS_HCNT0, 0xCF);
  DwcWrite (DWC_I2CM_SS_LCNT1, 0);
  DwcWrite (DWC_I2CM_SS_LCNT0, 0xFF);
  DwcWrite (DWC_I2CM_FS_HCNT1, 0);
  DwcWrite (DWC_I2CM_FS_HCNT0, 0x0F);
  DwcWrite (DWC_I2CM_FS_LCNT1, 0);
  DwcWrite (DWC_I2CM_FS_LCNT0, 0x20);
  DwcWrite (DWC_I2CM_SDA_HOLD, 8);
  DwcWrite (DWC_I2CM_SCDC, 0);
}

STATIC
UINT32
TraceMmioRead32 (
  IN CONST CHAR8  *Name,
  IN UINTN        Address
  )
{
  UINT32  Value;

  DEBUG ((DEBUG_INFO, "MesonDisplay: MMIO read  %a @ 0x%lx\n", Name, Address));
  Value = MmioRead32 (Address);
  ArmDataSynchronizationBarrier ();
  DEBUG ((DEBUG_INFO, "MesonDisplay: MMIO read  %a = 0x%08x complete\n", Name, Value));
  return Value;
}

STATIC
VOID
TraceMmioWrite32 (
  IN CONST CHAR8  *Name,
  IN UINTN        Address,
  IN UINT32       Value
  )
{
  DEBUG ((
    DEBUG_INFO,
    "MesonDisplay: MMIO write %a @ 0x%lx = 0x%08x\n",
    Name,
    Address,
    Value
    ));
  MmioWrite32 (Address, Value);
  ArmDataSynchronizationBarrier ();
  DEBUG ((DEBUG_INFO, "MesonDisplay: MMIO write %a complete\n", Name));
}

STATIC
VOID
TraceResetPulse (
  IN CONST CHAR8  *Name,
  IN UINTN        Address,
  IN UINT32       Mask
  )
{
  UINT32  Value;

  Value = TraceMmioRead32 (Name, Address);
  TraceMmioWrite32 (Name, Address, Value & ~Mask);
  MicroSecondDelay (2);
  TraceMmioWrite32 (Name, Address, Value | Mask);
  MicroSecondDelay (2);
}

STATIC
VOID
PrepareHdmiHost (
  VOID
  )
{
  UINT32  Value;

  if (mHostPrepared) {
    return;
  }

  //
  // The HDMI TOP window is part of the VPU power domain.  HPD probing reaches
  // this routine before full mode programming, so the domain must be powered
  // here rather than later in MesonDisplayHardwareInit().  Touching
  // HDMITX_TOP_SW_RESET while the domain is off raises an external abort on
  // A311D.
  //
  if (!mVpuPowered) {
    DEBUG ((DEBUG_INFO, "MesonDisplay: VPU power/reset before HDMI host access\n"));
    PowerOnVpu ();
    mVpuPowered = TRUE;
  }

  //
  // VIM3 routes the HDMI connector's +5 V source through the board VCC_5V
  // regulator controlled by open-drain GPIOH_8.  Program the output latch
  // high, then release the line (input) to enable the rail without a low
  // pulse.  The USB driver independently applies the same idempotent board
  // policy later, but HPD/EDID probing occurs before USB binding.
  //
  Value = TraceMmioRead32 ("MUX_GPIOH_8", MUX_GPIOH_8);
  TraceMmioWrite32 ("MUX_GPIOH_8", MUX_GPIOH_8, Value & ~0xFU);
  Value = TraceMmioRead32 ("GPIO_H_OUTPUT", GPIO_H_OUTPUT);
  TraceMmioWrite32 ("GPIO_H_OUTPUT", GPIO_H_OUTPUT, Value | BIT (8));
  Value = TraceMmioRead32 ("GPIO_H_DIRECTION", GPIO_H_DIRECTION);
  TraceMmioWrite32 ("GPIO_H_DIRECTION", GPIO_H_DIRECTION, Value | BIT (8));

  Value = TraceMmioRead32 ("PAD_PULL_UP_EN_REG3", MESON_GPIO_BASE + MESON_REG (0x4B));
  TraceMmioWrite32 ("PAD_PULL_UP_EN_REG3", MESON_GPIO_BASE + MESON_REG (0x4B), Value & ~0x3U);
  Value = TraceMmioRead32 ("PAD_PULL_UP_REG3", MESON_GPIO_BASE + MESON_REG (0x3D));
  TraceMmioWrite32 ("PAD_PULL_UP_REG3", MESON_GPIO_BASE + MESON_REG (0x3D), Value & ~0x3U);
  Value = TraceMmioRead32 ("PREG_PAD_GPIO3_EN_N", MESON_GPIO_BASE + MESON_REG (0x19));
  TraceMmioWrite32 ("PREG_PAD_GPIO3_EN_N", MESON_GPIO_BASE + MESON_REG (0x19), Value | 0x3);
  //
  // Nibbles 0/1 = GPIOH_0/H_1 -> HDMI SDA/SCL (function 1).  Nibble 3 =
  // GPIOH_3 -> cec_ao_b_h (function 5): the CEC-B pad.  DeviceTree muxes
  // GPIOH_3 through the cecb pinctrl state at driver probe; under ACPI
  // nothing applies a default pin state, so without this write the CEC
  // bus is electrically dead (verified live: nibble read 0 in ACPI mode).
  // Harmless in DT boots - pinctrl rewrites the same value.
  //
  Value = TraceMmioRead32 ("PERIPHS_PIN_MUX_B", MESON_GPIO_BASE + MESON_REG (0xBB));
  TraceMmioWrite32 (
    "PERIPHS_PIN_MUX_B",
    MESON_GPIO_BASE + MESON_REG (0xBB),
    (Value & ~0xF0FFU) | 0x5011
    );

  Value = TraceMmioRead32 ("HHI_HDMI_CLK_CNTL", MESON_HHI_BASE + MESON_REG (HHI_HDMI_CLK_CNTL));
  TraceMmioWrite32 (
    "HHI_HDMI_CLK_CNTL",
    MESON_HHI_BASE + MESON_REG (HHI_HDMI_CLK_CNTL),
    (Value & ~0x7FFU) | BIT (8)
    );
  //
  // Bit 4 = HTX_PCLK (the HDMI TX APB clock this driver needs).  Bit 25 =
  // VPU_INTR: the VENC vsync interrupt clock.  Firmware itself never takes
  // that interrupt, but the ACPI-mode meson-drm takeover depends on it and
  // there is no clock provider there to open it - kernel CLKID_VPU_INTR
  // (drivers/clk/meson/g12a.c HHI_GCLK_MPEG2 bit 25).
  //
  Value = TraceMmioRead32 ("HHI_GCLK_MPEG2", MESON_HHI_BASE + MESON_REG (HHI_GCLK_MPEG2));
  TraceMmioWrite32 (
    "HHI_GCLK_MPEG2",
    MESON_HHI_BASE + MESON_REG (HHI_GCLK_MPEG2),
    Value | BIT (4) | BIT (25)
    );
  Value = TraceMmioRead32 ("HHI_MEM_PD_REG0", MESON_HHI_BASE + MESON_REG (HHI_MEM_PD_REG0));
  TraceMmioWrite32 (
    "HHI_MEM_PD_REG0",
    MESON_HHI_BASE + MESON_REG (HHI_MEM_PD_REG0),
    Value & ~(0xFFU << 8)
    );
  //
  // Mainline Linux resets all three G12A HDMI domains before touching the
  // TOP register window.  Without these pulses the A311D reports an
  // asynchronous external abort when TOP_SW_RESET is written.
  //
  TraceResetPulse (
    "RESET_HDMITX_CAPB3",
    MESON_RESET_BASE + RESET0_LEVEL,
    BIT (19)
    );
  TraceResetPulse (
    "RESET_HDMITX_PHY",
    MESON_RESET_BASE + RESET2_LEVEL,
    BIT (2)
    );
  TraceResetPulse (
    "RESET_HDMITX",
    MESON_RESET_BASE + RESET2_LEVEL,
    BIT (15)
    );
  TraceMmioWrite32 (
    "HDMITX_TOP_SW_RESET",
    MESON_HDMI_TOP_BASE + MESON_REG (HDMITX_TOP_SW_RESET),
    0
    );
  MicroSecondDelay (200);
  TraceMmioWrite32 (
    "HDMITX_TOP_CLK_CNTL",
    MESON_HDMI_TOP_BASE + MESON_REG (HDMITX_TOP_CLK_CNTL),
    0xFF
    );
  //
  // Match the G12B HPD debounce setup used by mainline meson-dw-hdmi:
  // glitch width 0xA and valid width 0xA0.  Leaving the filter in its
  // reset state can hold filtered HPD low even while the sink asserts it.
  //
  TraceMmioWrite32 (
    "HDMITX_TOP_HPD_FILTER",
    MESON_HDMI_TOP_BASE + MESON_REG (HDMITX_TOP_HPD_FILTER),
    (0xAU << 12) | 0xA0U
    );
  //
  // Release the DesignWare controller from software reset before its DDC
  // engine is configured.  The modeset path also performs this sequence
  // after programming the frame composer, but EDID probing precedes modeset.
  //
  DwcWrite (DWC_MC_SWRSTZREQ, 0);
  MicroSecondDelay (10);
  DwcWrite (DWC_MC_SWRSTZREQ, 0x7D);
  DwcWrite (DWC_MC_CLKDIS, 0);
  ConfigureDdc ();
  //
  // Active KVM EDID emulators can need several hundred milliseconds after
  // source power, pinmux, and the HDMI host clocks become valid before HPD
  // or receiver sense is observable.  DEBUG builds previously supplied this
  // delay accidentally through register tracing; keep RELEASE behavior
  // deterministic with an explicit bounded settle interval.  The existing
  // hotplug loop below adds at most another 100 ms for a headless boot.
  //
  MicroSecondDelay (350000);
  mHostPrepared = TRUE;
}

BOOLEAN
MesonDisplayHotPlugDetected (
  VOID
  )
{
  UINTN   Retry;
  UINT32  Status;

  PrepareHdmiHost ();
  for (Retry = 0; Retry < 100; Retry++) {
    Status = MmioRead32 (MESON_HDMI_TOP_BASE + MESON_REG (HDMITX_TOP_STAT0));
    //
    // G12B TOP_STAT0 reports filtered HPD in bit 0 and receiver sense in
    // bit 1.  Match mainline meson-dw-hdmi and accept either indication:
    // some active KVM sinks assert receiver sense before HPD and only raise
    // HPD after transmitter initialization begins.
    //
    if (Status != 0) {
      DEBUG ((DEBUG_INFO, "MesonDisplay: HDMI sink sensed, TOP_STAT0=0x%08x\n", Status));
      return TRUE;
    }

    MicroSecondDelay (1000);
  }

  DEBUG ((DEBUG_INFO, "MesonDisplay: HDMI HPD absent, TOP_STAT0=0x%08x\n", Status));
  return FALSE;
}

STATIC
BOOLEAN
EdidBlockValid (
  IN CONST UINT8  *Block,
  IN BOOLEAN      Base
  )
{
  UINTN  Index;
  UINT8  Sum;

  if (Base) {
    STATIC CONST UINT8  Header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    if (CompareMem (Block, Header, sizeof (Header)) != 0) {
      return FALSE;
    }
  }

  Sum = 0;
  for (Index = 0; Index < 128; Index++) {
    Sum = (UINT8)(Sum + Block[Index]);
  }

  return Sum == 0;
}

STATIC
EFI_STATUS
ReadEdidBlock (
  IN  UINT32  BlockIndex,
  OUT UINT8   *Block
  )
{
  UINT32  Segment;
  UINT32  InitialOffset;
  UINT32  Offset;
  UINTN   Poll;
  UINTN   Byte;
  UINT8   Status;

  Segment       = BlockIndex / 2;
  InitialOffset = (BlockIndex & 1U) * 128;
  DwcWrite (DWC_I2CM_SLAVE, 0x50);
  DwcWrite (DWC_I2CM_SEGADDR, 0x30);
  DwcWrite (DWC_I2CM_SEGPTR, (UINT8)Segment);

  for (Offset = 0; Offset < 128; Offset += 8) {
    DwcWrite (DWC_IH_I2CM_STAT0, 0xFF);
    DwcWrite (DWC_I2CM_ADDRESS, (UINT8)(InitialOffset + Offset));
    //
    // The first 256 bytes fit in the DDC target's 8-bit word-offset space
    // and do not require an E-DDC segment-pointer transaction.  Some KVM
    // EDID emulators NACK that unnecessary transaction even though ordinary
    // DDC reads work.  Use extended reads only for later segments.
    //
    DwcWrite (
      DWC_I2CM_OPERATION,
      (BlockIndex < 2) ? BIT (2) : BIT (3)
      );
    Status = 0;
    for (Poll = 0; Poll < 8; Poll++) {
      MicroSecondDelay (1000);
      Status = DwcRead (DWC_IH_I2CM_STAT0);
      if ((Status & (BIT (1) | BIT (0))) != 0) {
        break;
      }
    }

    if (((Status & BIT (1)) == 0) || ((Status & BIT (0)) != 0)) {
      DEBUG ((
        DEBUG_WARN,
        "MesonDisplay: DDC block %u offset 0x%x failed, IH_I2CM_STAT0=0x%02x\n",
        BlockIndex,
        InitialOffset + Offset,
        Status
        ));
      return EFI_TIMEOUT;
    }

    DwcWrite (DWC_IH_I2CM_STAT0, BIT (1));
    for (Byte = 0; Byte < 8; Byte++) {
      Block[Offset + Byte] = DwcRead (DWC_I2CM_READ_BUFF0 + (UINT32)Byte);
    }
  }

  return EdidBlockValid (Block, BlockIndex == 0) ? EFI_SUCCESS : EFI_CRC_ERROR;
}

EFI_STATUS
MesonDisplayReadEdid (
  OUT UINT8   *Edid,
  IN  UINT32  Capacity,
  OUT UINT32  *EdidSize
  )
{
  EFI_STATUS  Status;
  UINT32      Blocks;
  UINT32      Index;

  if ((Edid == NULL) || (EdidSize == NULL) || (Capacity < 128)) {
    return EFI_INVALID_PARAMETER;
  }

  PrepareHdmiHost ();
  Status = ReadEdidBlock (0, Edid);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Blocks = 1U + Edid[126];
  if (Blocks > Capacity / 128) {
    Blocks = Capacity / 128;
  }

  for (Index = 1; Index < Blocks; Index++) {
    Status = ReadEdidBlock (Index, Edid + Index * 128);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  *EdidSize = Blocks * 128;
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
DetailedTimingMatches (
  IN CONST UINT8               *Descriptor,
  IN CONST MESON_DISPLAY_MODE  *Mode
  )
{
  BOOLEAN Interlaced;
  UINT32  HBlank;
  UINT32  PixelClockKhz;
  UINT32  VBlank;
  UINT32  Width;
  UINT32  Height;

  PixelClockKhz = (Descriptor[0] | (Descriptor[1] << 8)) * 10U;
  Width         = Descriptor[2] | ((Descriptor[4] & 0xF0U) << 4);
  HBlank        = Descriptor[3] | ((Descriptor[4] & 0x0FU) << 8);
  Height        = Descriptor[5] | ((Descriptor[7] & 0xF0U) << 4);
  VBlank        = Descriptor[6] | ((Descriptor[7] & 0x0FU) << 8);
  Interlaced    = (Descriptor[17] & BIT (7)) != 0;
  return (PixelClockKhz == Mode->PixelClockKhz) &&
         (Width == Mode->Width) &&
         (Height == Mode->Height) &&
         (HBlank == (Mode->HFrontPorch + Mode->HSync + Mode->HBackPorch)) &&
         (VBlank == (Mode->VFrontPorch + Mode->VSync + Mode->VBackPorch)) &&
         (Interlaced == Mode->Interlaced);
}

STATIC
CONST MESON_DISPLAY_MODE *
FindModeByVic (
  IN UINT8  Vic
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mSupportedModes); Index++) {
    if (mSupportedModes[Index].CtaVic == Vic) {
      return &mSupportedModes[Index];
    }
  }

  return NULL;
}

STATIC
CONST MESON_DISPLAY_MODE *
FindDetailedTimingMode (
  IN CONST UINT8  *Edid
  )
{
  CONST MESON_DISPLAY_MODE  *DefaultMatch;
  UINTN                     Descriptor;
  UINTN                     ModeIndex;

  DefaultMatch = NULL;
  for (Descriptor = 54; Descriptor < 126; Descriptor += 18) {
    if ((Edid[Descriptor] == 0) && (Edid[Descriptor + 1] == 0)) {
      continue;
    }

    for (ModeIndex = 0; ModeIndex < ARRAY_SIZE (mSupportedModes); ModeIndex++) {
      if (DetailedTimingMatches (Edid + Descriptor, &mSupportedModes[ModeIndex])) {
        //
        // Prefer the 16:9 alias where a DTD cannot express the distinction.
        //
        if ((mSupportedModes[ModeIndex].CtaVic == 3) ||
            (mSupportedModes[ModeIndex].CtaVic == 7) ||
            (mSupportedModes[ModeIndex].CtaVic == 18) ||
            (mSupportedModes[ModeIndex].CtaVic == 22))
        {
          return &mSupportedModes[ModeIndex];
        }

        if (DefaultMatch == NULL) {
          DefaultMatch = &mSupportedModes[ModeIndex];
        }
      }
    }
  }

  return DefaultMatch;
}

VOID
MesonDisplayChooseMode (
  IN  CONST UINT8         *Edid,
  IN  UINT32              EdidSize,
  OUT MESON_DISPLAY_MODE  *Mode,
  OUT BOOLEAN             *HdmiSink
  )
{
  CONST MESON_DISPLAY_MODE  *DetailedMode;
  CONST MESON_DISPLAY_MODE  *SelectedMode;
  UINT32                    End;
  UINT32                    Index;
  UINT8                     Length;
  UINT32                    Offset;
  UINT8                     Tag;

  *HdmiSink = FALSE;

  if ((Edid == NULL) || (EdidSize < 128)) {
    CopyMem (Mode, &gMesonMode720p, sizeof (*Mode));
    return;
  }

  DetailedMode = FindDetailedTimingMode (Edid);
  SelectedMode = NULL;

  for (Index = 1; Index < EdidSize / 128; Index++) {
    CONST UINT8  *Extension;

    Extension = Edid + Index * 128;
    if (Extension[0] != 0x02) {
      continue;
    }

    End = Extension[2];
    if ((End < 4) || (End > 127)) {
      End = 127;
    }

    for (Offset = 4; Offset < End; Offset += 1U + Length) {
      Tag    = Extension[Offset] >> 5;
      Length = Extension[Offset] & 0x1F;
      if ((Offset + 1U + Length) > End) {
        break;
      }

      if (Tag == 2) {
        UINT32  VicIndex;

        for (VicIndex = 0; VicIndex < Length; VicIndex++) {
          CONST MESON_DISPLAY_MODE  *VicMode;

          VicMode = FindModeByVic (Extension[Offset + 1 + VicIndex] & 0x7F);
          if ((SelectedMode == NULL) && (VicMode != NULL)) {
            //
            // CTA orders SVDs by sink preference.  Honoring the first
            // supported entry also makes generated single-VIC profiles
            // select that exact electrical timing and aspect-ratio alias.
            //
            SelectedMode = VicMode;
          }
        }
      } else if ((Tag == 3) && (Length >= 3) &&
                 (Extension[Offset + 1] == 0x03) &&
                 (Extension[Offset + 2] == 0x0C) &&
                 (Extension[Offset + 3] == 0x00))
      {
        *HdmiSink = TRUE;
      }
    }
  }

  if (SelectedMode == NULL) {
    SelectedMode = DetailedMode;
  }

  if (SelectedMode == NULL) {
    SelectedMode = &gMesonMode720p;
  }

  CopyMem (Mode, SelectedMode, sizeof (*Mode));
}

STATIC
VOID
PowerOnVpu (
  VOID
  )
{
  UINT32  Mask;
  UINT32  Value;

  MmioAnd32 (MESON_AO_RTI_BASE + MESON_REG (0x3A), ~BIT (8));
  MicroSecondDelay (20);
  HhiWrite (HHI_VPU_MEM_PD_REG0, 0);
  HhiWrite (HHI_VPU_MEM_PD_REG1, 0);
  HhiWrite (HHI_VPU_MEM_PD_REG2, 0);
  HhiWrite (HHI_MEM_PD_REG0, HhiRead (HHI_MEM_PD_REG0) & ~(0xFFU << 8));
  MicroSecondDelay (20);

  Mask = BIT (5) | BIT (10) | BIT (19) | BIT (13);
  Value = MmioRead32 (MESON_RESET_BASE + RESET0_LEVEL);
  MmioWrite32 (MESON_RESET_BASE + RESET0_LEVEL, Value & ~Mask);
  MmioAnd32 (MESON_RESET_BASE + RESET1_LEVEL, ~BIT (5));
  MmioAnd32 (MESON_RESET_BASE + RESET2_LEVEL, ~BIT (15));
  Mask = BIT (6) | BIT (7) | BIT (13) | BIT (5) | BIT (9) | BIT (4) | BIT (12);
  Value = MmioRead32 (MESON_RESET_BASE + RESET4_LEVEL);
  MmioWrite32 (MESON_RESET_BASE + RESET4_LEVEL, Value & ~Mask);
  MmioAnd32 (MESON_RESET_BASE + RESET7_LEVEL, ~BIT (7));

  MmioAnd32 (MESON_AO_RTI_BASE + MESON_REG (0x3A), ~BIT (9));
  MicroSecondDelay (20);

  Mask = BIT (5) | BIT (10) | BIT (19) | BIT (13);
  MmioOr32 (MESON_RESET_BASE + RESET0_LEVEL, Mask);
  MmioOr32 (MESON_RESET_BASE + RESET1_LEVEL, BIT (5));
  MmioOr32 (MESON_RESET_BASE + RESET2_LEVEL, BIT (15));
  Mask = BIT (6) | BIT (7) | BIT (13) | BIT (5) | BIT (9) | BIT (4) | BIT (12);
  MmioOr32 (MESON_RESET_BASE + RESET4_LEVEL, Mask);
  MmioOr32 (MESON_RESET_BASE + RESET7_LEVEL, BIT (7));

  HhiWrite (HHI_VPU_CLK_CNTL, 0x00000100);
  HhiWrite (HHI_VPU_CLKB_CNTL, 0x00100001);
  HhiWrite (HHI_VAPBCLK_CNTL, 0x40000101);
  VpuWrite (VPU_RDARB_MODE_L1C1, 0);
  VpuWrite (VPU_RDARB_MODE_L1C2, 0x10000);
  VpuWrite (VPU_RDARB_MODE_L2C1, 0x900000);
  VpuWrite (VPU_WRARB_MODE_L2C1, 0x20000);
}

STATIC
EFI_STATUS
ConfigurePll (
  IN CONST MESON_DISPLAY_MODE  *Mode
  )
{
  UINT32  Control0;
  UINT32  Fraction;
  UINT32  HdmiTxSelect;
  UINT32  Multiplier;
  UINT32  Od1;
  UINT32  Od2;
  UINT32  Od3;
  UINT32  VencSelect;
  UINT32  VclkDiv;
  UINTN   Attempt;
  UINTN   Poll;

  //
  // These are the six clock topologies used by mainline's explicit Meson CTA
  // tables.  OD values below are already encoded: 0=/1, 1=/2, 2=/4.  The
  // hardware-proven Candidate 27 74.25/148.5 MHz paths are kept byte-for-byte
  // equivalent while adding the 4.32 and 5.94 GHz VCO families.
  //
  switch (Mode->ClockProfile) {
    case MesonVclkEnci54:
      Multiplier  = 0xB4;
      Fraction    = 0;
      Od1         = 2;
      Od2         = 2;
      Od3         = 0;
      VclkDiv     = 0;
      HdmiTxSelect = 1;
      VencSelect   = 1;
      break;

    case MesonVclkDdr54:
      Multiplier  = 0xB4;
      Fraction    = 0;
      Od1         = 2;
      Od2         = 2;
      Od3         = 0;
      VclkDiv     = 0;
      HdmiTxSelect = 1;
      VencSelect   = 0;
      break;

    case MesonVclkDdr148500:
      Multiplier  = 0x7B;
      Fraction    = 0x18000;
      Od1         = 2;
      Od2         = 0;
      Od3         = 0;
      VclkDiv     = 0;
      HdmiTxSelect = 1;
      VencSelect   = 0;
      break;

    case MesonVclk74250:
      Multiplier  = 0x7B;
      Fraction    = 0x18000;
      Od1         = 2;
      Od2         = 0;
      Od3         = 0;
      VclkDiv     = 1;
      HdmiTxSelect = 0;
      VencSelect   = 0;
      break;

    case MesonVclk148500:
      Multiplier  = 0x7B;
      Fraction    = 0x18000;
      Od1         = 1;
      Od2         = 0;
      Od3         = 0;
      VclkDiv     = 1;
      HdmiTxSelect = 0;
      VencSelect   = 0;
      break;

    case MesonVclk297000:
      Multiplier  = 0xF7;
      Fraction    = 0x10000;
      Od1         = 1;
      Od2         = 0;
      Od3         = 0;
      VclkDiv     = 1;
      HdmiTxSelect = 0;
      VencSelect   = 0;
      break;

    default:
      return EFI_UNSUPPORTED;
  }

  Control0 = BIT (28) | BIT (27) | BIT (25) | BIT (24) |
             (Od3 << 20) | (Od2 << 18) | (Od1 << 16) |
             (1U << 10) | Multiplier;

  HhiWrite (HHI_HDMI_PLL_CNTL0, Control0 | BIT (29));
  HhiWrite (HHI_HDMI_PLL_CNTL1, Fraction);
  HhiWrite (HHI_HDMI_PLL_CNTL2, 0x00000000);
  if (Multiplier >= 0xF7) {
    HhiWrite (HHI_HDMI_PLL_CNTL3, 0xEA68DC00);
    HhiWrite (HHI_HDMI_PLL_CNTL4, 0x65771290);
    HhiWrite (HHI_HDMI_PLL_CNTL5, 0x39272000);
    HhiWrite (HHI_HDMI_PLL_CNTL6, 0x55540000);
  } else {
    HhiWrite (HHI_HDMI_PLL_CNTL3, 0x0A691C00);
    HhiWrite (HHI_HDMI_PLL_CNTL4, 0x33771290);
    HhiWrite (HHI_HDMI_PLL_CNTL5, 0x39272000);
  }

  HhiWrite (HHI_HDMI_PLL_CNTL0, Control0);

  for (Attempt = 0; Attempt < 8; Attempt++) {
    for (Poll = 0; Poll < 200; Poll++) {
      if ((HhiRead (HHI_HDMI_PLL_CNTL0) & (BIT (31) | BIT (30))) ==
          (BIT (31) | BIT (30)))
      {
        goto Locked;
      }

      MicroSecondDelay (5);
    }

    HhiWrite (HHI_HDMI_PLL_CNTL0, Control0 | BIT (29));
    MicroSecondDelay (10);
    HhiWrite (HHI_HDMI_PLL_CNTL0, Control0);
  }

  return EFI_TIMEOUT;

Locked:
  //
  // /5 HDMI pattern divider followed by the selected VCLK, transmitter, and
  // ENCI/ENCP dividers.
  //
  HhiWrite (HHI_VID_PLL_CLK_DIV, HhiRead (HHI_VID_PLL_CLK_DIV) & ~BIT (19));
  HhiWrite (HHI_VID_PLL_CLK_DIV, 0x000AF39C);
  HhiWrite (HHI_VID_PLL_CLK_DIV, 0x000A739C);
  HhiWrite (HHI_VID_PLL_CLK_DIV, 0x000A739C | BIT (19));

  HhiWrite (
    HHI_VID_CLK_DIV,
    BIT (16) |
    VclkDiv |
    (VencSelect << (Mode->UseEnci ? 28 : 24))
    );
  HhiWrite (HHI_VID_CLK_CNTL, BIT (19) | BIT (2) | BIT (1) | BIT (0));
  HhiWrite (
    HHI_HDMI_CLK_CNTL,
    (HhiRead (HHI_HDMI_CLK_CNTL) & ~0xF0000U) |
    BIT (8) |
    (HdmiTxSelect << 16)
    );
  HhiWrite (
    HHI_VID_CLK_CNTL2,
    (HhiRead (HHI_VID_CLK_CNTL2) & ~(BIT (2) | BIT (0))) |
    BIT (5) |
    (Mode->UseEnci ? BIT (0) : BIT (2))
    );
  return EFI_SUCCESS;
}

typedef struct {
  UINT32  Register;
  UINT32  Value;
} MESON_REGISTER_VALUE;

typedef struct {
  UINT32  HsoBegin;
  UINT32  HsoEnd;
  UINT32  VsoEven;
  UINT32  VsoOdd;
  UINT32  MacvMaxAmp;
  UINT32  VideoProgMode;
  UINT32  VideoMode;
  UINT32  SchAdjust;
  UINT32  YcDelay;
  UINT32  PixelStart;
  UINT32  PixelEnd;
  UINT32  TopLineStart;
  UINT32  TopLineEnd;
  UINT32  BottomLineStart;
  UINT32  BottomLineEnd;
} MESON_ENCI_TIMING;

STATIC CONST MESON_ENCI_TIMING  mEnci480i = {
  5, 129, 3, 260, 0xB, 0xF0, 0x08, 0x20, 0,
  227, 1667, 18, 258, 19, 259
};

STATIC CONST MESON_ENCI_TIMING  mEnci576i = {
  3, 129, 3, 260, 0x7, 0xFF, 0x13, 0x28, 0x333,
  251, 1691, 22, 310, 23, 311
};

STATIC CONST MESON_REGISTER_VALUE  mEncp480p[] = {
  { VPU_VENC_DVI_SETTING, 0x21 },
  { VPU_ENCP_VIDEO_MODE, 0x4000 },
  { VPU_ENCP_VIDEO_MODE_ADV, 0x09 },
  { VPU_VENC_VIDEO_PROG_MODE, 0 },
  { VPU_ENCP_VIDEO_SYNC_MODE, 7 },
  { VPU_ENCP_VIDEO_FILT_CTRL, 0x2052 },
  { VPU_ENCP_VIDEO_YFP1_HTIME, 244 },
  { VPU_ENCP_VIDEO_YFP2_HTIME, 1630 },
  { VPU_ENCP_VIDEO_MAX_PXCNT, 1715 },
  { VPU_ENCP_VIDEO_HSPULS_BEG, 0x22 },
  { VPU_ENCP_VIDEO_HSPULS_END, 0xA0 },
  { VPU_ENCP_VIDEO_HSPULS_SW, 88 },
  { VPU_ENCP_VIDEO_VSPULS_BEG, 0 },
  { VPU_ENCP_VIDEO_VSPULS_END, 1589 },
  { VPU_ENCP_VIDEO_VSPULS_BLN, 0 },
  { VPU_ENCP_VIDEO_VSPULS_ELN, 5 },
  { VPU_ENCP_VIDEO_HAVON_BEG, 249 },
  { VPU_ENCP_VIDEO_HAVON_END, 1689 },
  { VPU_ENCP_VIDEO_VAVON_BEG, 42 },
  { VPU_ENCP_VIDEO_VAVON_END, 521 },
  { VPU_ENCP_VIDEO_HSO_BEG, 3 },
  { VPU_ENCP_VIDEO_HSO_END, 5 },
  { VPU_ENCP_VIDEO_VSO_BEG, 3 },
  { VPU_ENCP_VIDEO_VSO_END, 5 },
  { VPU_ENCP_VIDEO_VSO_BLINE, 0 },
  { VPU_ENCP_VIDEO_SY_VAL, 8 },
  { VPU_ENCP_VIDEO_SY2_VAL, 0x1D8 },
  { VPU_ENCP_VIDEO_MAX_LNCNT, 524 },
  { 0, 0 }
};

STATIC CONST MESON_REGISTER_VALUE  mEncp576p[] = {
  { VPU_VENC_DVI_SETTING, 0x21 },
  { VPU_ENCP_VIDEO_MODE, 0x4000 },
  { VPU_ENCP_VIDEO_MODE_ADV, 0x09 },
  { VPU_VENC_VIDEO_PROG_MODE, 0 },
  { VPU_ENCP_VIDEO_SYNC_MODE, 7 },
  { VPU_ENCP_VIDEO_FILT_CTRL, 0x52 },
  { VPU_ENCP_VIDEO_YFP1_HTIME, 235 },
  { VPU_ENCP_VIDEO_YFP2_HTIME, 1674 },
  { VPU_ENCP_VIDEO_MAX_PXCNT, 1727 },
  { VPU_ENCP_VIDEO_HSPULS_BEG, 0 },
  { VPU_ENCP_VIDEO_HSPULS_END, 0x80 },
  { VPU_ENCP_VIDEO_HSPULS_SW, 88 },
  { VPU_ENCP_VIDEO_VSPULS_BEG, 0 },
  { VPU_ENCP_VIDEO_VSPULS_END, 1599 },
  { VPU_ENCP_VIDEO_VSPULS_BLN, 0 },
  { VPU_ENCP_VIDEO_VSPULS_ELN, 4 },
  { VPU_ENCP_VIDEO_HAVON_BEG, 235 },
  { VPU_ENCP_VIDEO_HAVON_END, 1674 },
  { VPU_ENCP_VIDEO_VAVON_BEG, 44 },
  { VPU_ENCP_VIDEO_VAVON_END, 619 },
  { VPU_ENCP_VIDEO_HSO_BEG, 0x80 },
  { VPU_ENCP_VIDEO_HSO_END, 0 },
  { VPU_ENCP_VIDEO_VSO_BEG, 0 },
  { VPU_ENCP_VIDEO_VSO_END, 5 },
  { VPU_ENCP_VIDEO_VSO_BLINE, 0 },
  { VPU_ENCP_VIDEO_SY_VAL, 8 },
  { VPU_ENCP_VIDEO_SY2_VAL, 0x1D8 },
  { VPU_ENCP_VIDEO_MAX_LNCNT, 624 },
  { 0, 0 }
};

STATIC CONST MESON_REGISTER_VALUE  mEncp720p60[] = {
  { VPU_VENC_DVI_SETTING, 0x2029 },
  { VPU_ENCP_VIDEO_MODE, 0x4040 },
  { VPU_ENCP_VIDEO_MODE_ADV, 0x19 },
  { VPU_ENCP_VIDEO_YFP1_HTIME, 648 },
  { VPU_ENCP_VIDEO_YFP2_HTIME, 3207 },
  { VPU_ENCP_VIDEO_MAX_PXCNT, 3299 },
  { VPU_ENCP_VIDEO_HSPULS_BEG, 80 },
  { VPU_ENCP_VIDEO_HSPULS_END, 240 },
  { VPU_ENCP_VIDEO_HSPULS_SW, 80 },
  { VPU_ENCP_VIDEO_VSPULS_BEG, 688 },
  { VPU_ENCP_VIDEO_VSPULS_END, 3248 },
  { VPU_ENCP_VIDEO_VSPULS_BLN, 4 },
  { VPU_ENCP_VIDEO_VSPULS_ELN, 8 },
  { VPU_ENCP_VIDEO_HAVON_BEG, 648 },
  { VPU_ENCP_VIDEO_HAVON_END, 3207 },
  { VPU_ENCP_VIDEO_VAVON_BEG, 29 },
  { VPU_ENCP_VIDEO_VAVON_END, 748 },
  { VPU_ENCP_VIDEO_HSO_BEG, 256 },
  { VPU_ENCP_VIDEO_HSO_END, 168 },
  { VPU_ENCP_VIDEO_VSO_BEG, 168 },
  { VPU_ENCP_VIDEO_VSO_END, 256 },
  { VPU_ENCP_VIDEO_VSO_BLINE, 0 },
  { VPU_ENCP_VIDEO_VSO_ELINE, 5 },
  { VPU_ENCP_VIDEO_MAX_LNCNT, 749 },
  { 0, 0 }
};

STATIC CONST MESON_REGISTER_VALUE  mEncp720p50[] = {
  { VPU_VENC_DVI_SETTING, 0x202D },
  { VPU_ENCP_VIDEO_MODE, 0x4040 },
  { VPU_ENCP_VIDEO_MODE_ADV, 0x19 },
  { VPU_VENC_VIDEO_PROG_MODE, 0x100 },
  { VPU_ENCP_VIDEO_SYNC_MODE, 0x407 },
  { VPU_ENCP_VIDEO_YC_DLY, 0 },
  { VPU_ENCP_VIDEO_YFP1_HTIME, 648 },
  { VPU_ENCP_VIDEO_YFP2_HTIME, 3207 },
  { VPU_ENCP_VIDEO_MAX_PXCNT, 3959 },
  { VPU_ENCP_VIDEO_HSPULS_BEG, 80 },
  { VPU_ENCP_VIDEO_HSPULS_END, 240 },
  { VPU_ENCP_VIDEO_HSPULS_SW, 80 },
  { VPU_ENCP_VIDEO_VSPULS_BEG, 688 },
  { VPU_ENCP_VIDEO_VSPULS_END, 3248 },
  { VPU_ENCP_VIDEO_VSPULS_BLN, 4 },
  { VPU_ENCP_VIDEO_VSPULS_ELN, 8 },
  { VPU_ENCP_VIDEO_HAVON_BEG, 648 },
  { VPU_ENCP_VIDEO_HAVON_END, 3207 },
  { VPU_ENCP_VIDEO_VAVON_BEG, 29 },
  { VPU_ENCP_VIDEO_VAVON_END, 748 },
  { VPU_ENCP_VIDEO_HSO_BEG, 128 },
  { VPU_ENCP_VIDEO_HSO_END, 208 },
  { VPU_ENCP_VIDEO_VSO_BEG, 128 },
  { VPU_ENCP_VIDEO_VSO_END, 128 },
  { VPU_ENCP_VIDEO_VSO_BLINE, 0 },
  { VPU_ENCP_VIDEO_VSO_ELINE, 5 },
  { VPU_ENCP_VIDEO_MAX_LNCNT, 749 },
  { 0, 0 }
};

STATIC CONST MESON_REGISTER_VALUE  mEncp1080i60[] = {
  { VPU_VENC_DVI_SETTING, 0x2029 },
  { VPU_ENCP_VIDEO_MODE, 0x5FFC },
  { VPU_ENCP_VIDEO_MODE_ADV, 0x19 },
  { VPU_VENC_VIDEO_PROG_MODE, 0x100 },
  { VPU_ENCP_VIDEO_SYNC_MODE, 0x207 },
  { VPU_ENCP_VIDEO_OFLD_VOAV, 0x11 },
  { VPU_ENCP_VIDEO_YFP1_HTIME, 516 },
  { VPU_ENCP_VIDEO_YFP2_HTIME, 4355 },
  { VPU_ENCP_VIDEO_MAX_PXCNT, 4399 },
  { VPU_ENCP_VIDEO_HSPULS_BEG, 88 },
  { VPU_ENCP_VIDEO_HSPULS_END, 264 },
  { VPU_ENCP_VIDEO_HSPULS_SW, 88 },
  { VPU_ENCP_VIDEO_VSPULS_BEG, 440 },
  { VPU_ENCP_VIDEO_VSPULS_END, 2200 },
  { VPU_ENCP_VIDEO_VSPULS_BLN, 0 },
  { VPU_ENCP_VIDEO_VSPULS_ELN, 4 },
  { VPU_ENCP_VIDEO_EQPULS_BEG, 2288 },
  { VPU_ENCP_VIDEO_EQPULS_END, 2464 },
  { VPU_ENCP_VIDEO_EQPULS_BLN, 0 },
  { VPU_ENCP_VIDEO_EQPULS_ELN, 4 },
  { VPU_ENCP_VIDEO_HAVON_BEG, 516 },
  { VPU_ENCP_VIDEO_HAVON_END, 4355 },
  { VPU_ENCP_VIDEO_VAVON_BEG, 20 },
  { VPU_ENCP_VIDEO_VAVON_END, 559 },
  { VPU_ENCP_VIDEO_HSO_BEG, 264 },
  { VPU_ENCP_VIDEO_HSO_END, 176 },
  { VPU_ENCP_VIDEO_VSO_BEG, 88 },
  { VPU_ENCP_VIDEO_VSO_END, 88 },
  { VPU_ENCP_VIDEO_VSO_BLINE, 0 },
  { VPU_ENCP_VIDEO_VSO_ELINE, 5 },
  { VPU_ENCP_VIDEO_MAX_LNCNT, 1124 },
  { 0, 0 }
};

STATIC CONST MESON_REGISTER_VALUE  mEncp1080i50[] = {
  { VPU_VENC_DVI_SETTING, 0x202D },
  { VPU_ENCP_VIDEO_MODE, 0x5FFC },
  { VPU_ENCP_VIDEO_MODE_ADV, 0x19 },
  { VPU_VENC_VIDEO_PROG_MODE, 0x100 },
  { VPU_ENCP_VIDEO_SYNC_MODE, 7 },
  { VPU_ENCP_VIDEO_OFLD_VOAV, 0x11 },
  { VPU_ENCP_VIDEO_YFP1_HTIME, 526 },
  { VPU_ENCP_VIDEO_YFP2_HTIME, 4365 },
  { VPU_ENCP_VIDEO_MAX_PXCNT, 5279 },
  { VPU_ENCP_VIDEO_HSPULS_BEG, 88 },
  { VPU_ENCP_VIDEO_HSPULS_END, 264 },
  { VPU_ENCP_VIDEO_HSPULS_SW, 88 },
  { VPU_ENCP_VIDEO_VSPULS_BEG, 440 },
  { VPU_ENCP_VIDEO_VSPULS_END, 2200 },
  { VPU_ENCP_VIDEO_VSPULS_BLN, 0 },
  { VPU_ENCP_VIDEO_VSPULS_ELN, 4 },
  { VPU_ENCP_VIDEO_EQPULS_BEG, 2288 },
  { VPU_ENCP_VIDEO_EQPULS_END, 2464 },
  { VPU_ENCP_VIDEO_EQPULS_BLN, 0 },
  { VPU_ENCP_VIDEO_EQPULS_ELN, 4 },
  { VPU_ENCP_VIDEO_HAVON_BEG, 526 },
  { VPU_ENCP_VIDEO_HAVON_END, 4365 },
  { VPU_ENCP_VIDEO_VAVON_BEG, 20 },
  { VPU_ENCP_VIDEO_VAVON_END, 559 },
  { VPU_ENCP_VIDEO_HSO_BEG, 142 },
  { VPU_ENCP_VIDEO_HSO_END, 230 },
  { VPU_ENCP_VIDEO_VSO_BEG, 142 },
  { VPU_ENCP_VIDEO_VSO_END, 142 },
  { VPU_ENCP_VIDEO_VSO_BLINE, 0 },
  { VPU_ENCP_VIDEO_VSO_ELINE, 5 },
  { VPU_ENCP_VIDEO_MAX_LNCNT, 1124 },
  { 0, 0 }
};

STATIC CONST MESON_REGISTER_VALUE  mEncp1080p24[] = {
  { VPU_VENC_DVI_SETTING, 0x0D },
  { VPU_ENCP_VIDEO_MODE, 0x4040 },
  { VPU_ENCP_VIDEO_MODE_ADV, 0x18 },
  { VPU_VENC_VIDEO_PROG_MODE, 0x100 },
  { VPU_ENCP_VIDEO_SYNC_MODE, 7 },
  { VPU_ENCP_VIDEO_YC_DLY, 0 },
  { VPU_ENCP_VIDEO_RGB_CTRL, 2 },
  { VPU_ENCP_VIDEO_FILT_CTRL, 0x1052 },
  { VPU_ENCP_VIDEO_YFP1_HTIME, 271 },
  { VPU_ENCP_VIDEO_YFP2_HTIME, 2190 },
  { VPU_ENCP_VIDEO_MAX_PXCNT, 2749 },
  { VPU_ENCP_VIDEO_HSPULS_BEG, 44 },
  { VPU_ENCP_VIDEO_HSPULS_END, 132 },
  { VPU_ENCP_VIDEO_HSPULS_SW, 44 },
  { VPU_ENCP_VIDEO_VSPULS_BEG, 220 },
  { VPU_ENCP_VIDEO_VSPULS_END, 2140 },
  { VPU_ENCP_VIDEO_VSPULS_BLN, 0 },
  { VPU_ENCP_VIDEO_VSPULS_ELN, 4 },
  { VPU_ENCP_VIDEO_EQPULS_BLN, 0 },
  { VPU_ENCP_VIDEO_EQPULS_ELN, 4 },
  { VPU_ENCP_VIDEO_HAVON_BEG, 271 },
  { VPU_ENCP_VIDEO_HAVON_END, 2190 },
  { VPU_ENCP_VIDEO_VAVON_BEG, 41 },
  { VPU_ENCP_VIDEO_VAVON_END, 1120 },
  { VPU_ENCP_VIDEO_HSO_BEG, 79 },
  { VPU_ENCP_VIDEO_HSO_END, 123 },
  { VPU_ENCP_VIDEO_VSO_BEG, 79 },
  { VPU_ENCP_VIDEO_VSO_END, 79 },
  { VPU_ENCP_VIDEO_VSO_BLINE, 0 },
  { VPU_ENCP_VIDEO_VSO_ELINE, 5 },
  { VPU_ENCP_VIDEO_MAX_LNCNT, 1124 },
  { 0, 0 }
};

STATIC CONST MESON_REGISTER_VALUE  mEncp1080p30[] = {
  { VPU_VENC_DVI_SETTING, 1 },
  { VPU_ENCP_VIDEO_MODE, 0x4040 },
  { VPU_ENCP_VIDEO_MODE_ADV, 0x18 },
  { VPU_VENC_VIDEO_PROG_MODE, 0x100 },
  { VPU_ENCP_VIDEO_FILT_CTRL, 0x1052 },
  { VPU_ENCP_VIDEO_YFP1_HTIME, 140 },
  { VPU_ENCP_VIDEO_YFP2_HTIME, 2060 },
  { VPU_ENCP_VIDEO_MAX_PXCNT, 2199 },
  { VPU_ENCP_VIDEO_HSPULS_BEG, 2156 },
  { VPU_ENCP_VIDEO_HSPULS_END, 44 },
  { VPU_ENCP_VIDEO_HSPULS_SW, 44 },
  { VPU_ENCP_VIDEO_VSPULS_BEG, 140 },
  { VPU_ENCP_VIDEO_VSPULS_END, 2059 },
  { VPU_ENCP_VIDEO_VSPULS_BLN, 0 },
  { VPU_ENCP_VIDEO_VSPULS_ELN, 4 },
  { VPU_ENCP_VIDEO_HAVON_BEG, 148 },
  { VPU_ENCP_VIDEO_HAVON_END, 2067 },
  { VPU_ENCP_VIDEO_VAVON_BEG, 41 },
  { VPU_ENCP_VIDEO_VAVON_END, 1120 },
  { VPU_ENCP_VIDEO_HSO_BEG, 44 },
  { VPU_ENCP_VIDEO_HSO_END, 2156 },
  { VPU_ENCP_VIDEO_VSO_BEG, 2100 },
  { VPU_ENCP_VIDEO_VSO_END, 2164 },
  { VPU_ENCP_VIDEO_VSO_BLINE, 0 },
  { VPU_ENCP_VIDEO_VSO_ELINE, 5 },
  { VPU_ENCP_VIDEO_MAX_LNCNT, 1124 },
  { 0, 0 }
};

STATIC CONST MESON_REGISTER_VALUE  mEncp1080p50[] = {
  { VPU_VENC_DVI_SETTING, 0x0D },
  { VPU_ENCP_VIDEO_MODE, 0x4040 },
  { VPU_ENCP_VIDEO_MODE_ADV, 0x18 },
  { VPU_VENC_VIDEO_PROG_MODE, 0x100 },
  { VPU_ENCP_VIDEO_SYNC_MODE, 7 },
  { VPU_ENCP_VIDEO_YC_DLY, 0 },
  { VPU_ENCP_VIDEO_RGB_CTRL, 2 },
  { VPU_ENCP_VIDEO_YFP1_HTIME, 271 },
  { VPU_ENCP_VIDEO_YFP2_HTIME, 2190 },
  { VPU_ENCP_VIDEO_MAX_PXCNT, 2639 },
  { VPU_ENCP_VIDEO_HSPULS_BEG, 44 },
  { VPU_ENCP_VIDEO_HSPULS_END, 132 },
  { VPU_ENCP_VIDEO_HSPULS_SW, 44 },
  { VPU_ENCP_VIDEO_VSPULS_BEG, 220 },
  { VPU_ENCP_VIDEO_VSPULS_END, 2140 },
  { VPU_ENCP_VIDEO_VSPULS_BLN, 0 },
  { VPU_ENCP_VIDEO_VSPULS_ELN, 4 },
  { VPU_ENCP_VIDEO_EQPULS_BLN, 0 },
  { VPU_ENCP_VIDEO_EQPULS_ELN, 4 },
  { VPU_ENCP_VIDEO_HAVON_BEG, 271 },
  { VPU_ENCP_VIDEO_HAVON_END, 2190 },
  { VPU_ENCP_VIDEO_VAVON_BEG, 41 },
  { VPU_ENCP_VIDEO_VAVON_END, 1120 },
  { VPU_ENCP_VIDEO_HSO_BEG, 79 },
  { VPU_ENCP_VIDEO_HSO_END, 123 },
  { VPU_ENCP_VIDEO_VSO_BEG, 79 },
  { VPU_ENCP_VIDEO_VSO_END, 79 },
  { VPU_ENCP_VIDEO_VSO_BLINE, 0 },
  { VPU_ENCP_VIDEO_VSO_ELINE, 5 },
  { VPU_ENCP_VIDEO_MAX_LNCNT, 1124 },
  { 0, 0 }
};

STATIC CONST MESON_REGISTER_VALUE  mEncp2160p24[] = {
  { VPU_VENC_DVI_SETTING, 1 },
  { VPU_ENCP_VIDEO_MODE, 0x4040 },
  { VPU_ENCP_VIDEO_MODE_ADV, 0x08 },
  { VPU_ENCP_VIDEO_FILT_CTRL, 0x1000 },
  { VPU_ENCP_VIDEO_YFP1_HTIME, 140 },
  { VPU_ENCP_VIDEO_YFP2_HTIME, 3980 },
  { VPU_ENCP_VIDEO_MAX_PXCNT, 5499 },
  { VPU_ENCP_VIDEO_HSPULS_BEG, 4076 },
  { VPU_ENCP_VIDEO_HSPULS_END, 44 },
  { VPU_ENCP_VIDEO_HSPULS_SW, 44 },
  { VPU_ENCP_VIDEO_VSPULS_BEG, 140 },
  { VPU_ENCP_VIDEO_VSPULS_END, 3979 },
  { VPU_ENCP_VIDEO_VSPULS_BLN, 0 },
  { VPU_ENCP_VIDEO_VSPULS_ELN, 4 },
  { VPU_ENCP_VIDEO_HAVON_BEG, 148 },
  { VPU_ENCP_VIDEO_HAVON_END, 3987 },
  { VPU_ENCP_VIDEO_VAVON_BEG, 89 },
  { VPU_ENCP_VIDEO_VAVON_END, 2248 },
  { VPU_ENCP_VIDEO_HSO_BEG, 44 },
  { VPU_ENCP_VIDEO_HSO_END, 4076 },
  { VPU_ENCP_VIDEO_VSO_BEG, 4020 },
  { VPU_ENCP_VIDEO_VSO_END, 4084 },
  { VPU_ENCP_VIDEO_VSO_BLINE, 51 },
  { VPU_ENCP_VIDEO_VSO_ELINE, 53 },
  { VPU_ENCP_VIDEO_MAX_LNCNT, 2249 },
  { 0, 0 }
};

STATIC CONST MESON_REGISTER_VALUE  mEncp2160p25[] = {
  { VPU_VENC_DVI_SETTING, 1 },
  { VPU_ENCP_VIDEO_MODE, 0x4040 },
  { VPU_ENCP_VIDEO_MODE_ADV, 0x08 },
  { VPU_ENCP_VIDEO_FILT_CTRL, 0x1000 },
  { VPU_ENCP_VIDEO_YFP1_HTIME, 140 },
  { VPU_ENCP_VIDEO_YFP2_HTIME, 3980 },
  { VPU_ENCP_VIDEO_MAX_PXCNT, 5279 },
  { VPU_ENCP_VIDEO_HSPULS_BEG, 4076 },
  { VPU_ENCP_VIDEO_HSPULS_END, 44 },
  { VPU_ENCP_VIDEO_HSPULS_SW, 44 },
  { VPU_ENCP_VIDEO_VSPULS_BEG, 140 },
  { VPU_ENCP_VIDEO_VSPULS_END, 3979 },
  { VPU_ENCP_VIDEO_VSPULS_BLN, 0 },
  { VPU_ENCP_VIDEO_VSPULS_ELN, 4 },
  { VPU_ENCP_VIDEO_HAVON_BEG, 148 },
  { VPU_ENCP_VIDEO_HAVON_END, 3987 },
  { VPU_ENCP_VIDEO_VAVON_BEG, 89 },
  { VPU_ENCP_VIDEO_VAVON_END, 2248 },
  { VPU_ENCP_VIDEO_HSO_BEG, 44 },
  { VPU_ENCP_VIDEO_HSO_END, 4076 },
  { VPU_ENCP_VIDEO_VSO_BEG, 4020 },
  { VPU_ENCP_VIDEO_VSO_END, 4084 },
  { VPU_ENCP_VIDEO_VSO_BLINE, 51 },
  { VPU_ENCP_VIDEO_VSO_ELINE, 53 },
  { VPU_ENCP_VIDEO_MAX_LNCNT, 2249 },
  { 0, 0 }
};

STATIC CONST MESON_REGISTER_VALUE  mEncp2160p30[] = {
  { VPU_VENC_DVI_SETTING, 1 },
  { VPU_ENCP_VIDEO_MODE, 0x4040 },
  { VPU_ENCP_VIDEO_MODE_ADV, 0x08 },
  { VPU_ENCP_VIDEO_FILT_CTRL, 0x1000 },
  { VPU_ENCP_VIDEO_YFP1_HTIME, 140 },
  { VPU_ENCP_VIDEO_YFP2_HTIME, 3980 },
  { VPU_ENCP_VIDEO_MAX_PXCNT, 4399 },
  { VPU_ENCP_VIDEO_HSPULS_BEG, 4076 },
  { VPU_ENCP_VIDEO_HSPULS_END, 44 },
  { VPU_ENCP_VIDEO_HSPULS_SW, 44 },
  { VPU_ENCP_VIDEO_VSPULS_BEG, 140 },
  { VPU_ENCP_VIDEO_VSPULS_END, 3979 },
  { VPU_ENCP_VIDEO_VSPULS_BLN, 0 },
  { VPU_ENCP_VIDEO_VSPULS_ELN, 4 },
  { VPU_ENCP_VIDEO_HAVON_BEG, 148 },
  { VPU_ENCP_VIDEO_HAVON_END, 3987 },
  { VPU_ENCP_VIDEO_VAVON_BEG, 89 },
  { VPU_ENCP_VIDEO_VAVON_END, 2248 },
  { VPU_ENCP_VIDEO_HSO_BEG, 44 },
  { VPU_ENCP_VIDEO_HSO_END, 4076 },
  { VPU_ENCP_VIDEO_VSO_BEG, 4020 },
  { VPU_ENCP_VIDEO_VSO_END, 4084 },
  { VPU_ENCP_VIDEO_VSO_BLINE, 51 },
  { VPU_ENCP_VIDEO_VSO_ELINE, 53 },
  { VPU_ENCP_VIDEO_MAX_LNCNT, 2249 },
  { 0, 0 }
};

STATIC
UINT32
Modulo (
  IN UINT32  Value,
  IN UINT32  Modulus
  )
{
  return Value % Modulus;
}

STATIC
INT32
SignedNibble (
  IN UINT32  Value
  )
{
  Value &= 0xF;
  return (Value <= 7) ? (INT32)Value : (INT32)Value - 16;
}

STATIC
CONST MESON_REGISTER_VALUE *
GetEncpTable (
  IN MESON_ENCODER_PROFILE  Profile
  )
{
  switch (Profile) {
    case MesonEncoderEncp480p:
      return mEncp480p;
    case MesonEncoderEncp576p:
      return mEncp576p;
    case MesonEncoderEncp720p60:
      return mEncp720p60;
    case MesonEncoderEncp720p50:
      return mEncp720p50;
    case MesonEncoderEncp1080i60:
      return mEncp1080i60;
    case MesonEncoderEncp1080i50:
      return mEncp1080i50;
    case MesonEncoderEncp1080p24:
      return mEncp1080p24;
    case MesonEncoderEncp1080p30:
    case MesonEncoderEncp1080p60:
      return mEncp1080p30;
    case MesonEncoderEncp1080p50:
      return mEncp1080p50;
    case MesonEncoderEncp2160p24:
      return mEncp2160p24;
    case MesonEncoderEncp2160p25:
      return mEncp2160p25;
    case MesonEncoderEncp2160p30:
      return mEncp2160p30;
    default:
      return NULL;
  }
}

STATIC
VOID
ConfigureEncpEncoder (
  IN CONST MESON_DISPLAY_MODE  *Mode
  )
{
  CONST MESON_REGISTER_VALUE  *Table;
  INT32                       FieldOffset;
  UINT32                      ActivePixels;
  UINT32                      DeHBegin;
  UINT32                      DeHEnd;
  UINT32                      DeVBeginEven;
  UINT32                      DeVBeginOdd;
  UINT32                      DeVEndEven;
  UINT32                      DeVEndOdd;
  UINT32                      EofLines;
  UINT32                      FrontPorch;
  UINT32                      HsBegin;
  UINT32                      HsEnd;
  UINT32                      HsyncPixels;
  UINT32                      Index;
  UINT32                      SofLines;
  UINT32                      TotalPixels;
  UINT32                      VTotal;
  UINT32                      VsAdjust;
  UINT32                      VsBeginEven;
  UINT32                      VsBeginOdd;
  UINT32                      VsEndEven;
  UINT32                      VsEndOdd;
  UINT32                      VsoBeginOdd;
  UINT32                      VsyncLines;

  Table = GetEncpTable (Mode->EncoderProfile);
  ASSERT (Table != NULL);
  for (Index = 0; Table[Index].Register != 0; Index++) {
    VpuWrite (Table[Index].Register, Table[Index].Value);
  }

  VpuWrite (VPU_ENCP_VIDEO_EN, 1);
  VpuWrite (VPU_ENCP_VIDEO_MODE, VpuRead (VPU_ENCP_VIDEO_MODE) | BIT (14));

  TotalPixels = Mode->Width + Mode->HFrontPorch + Mode->HSync +
                Mode->HBackPorch;
  ActivePixels = Mode->Width;
  FrontPorch   = Mode->HFrontPorch;
  HsyncPixels  = Mode->HSync;
  if (Mode->HdmiRepeat) {
    TotalPixels /= 2;
    ActivePixels /= 2;
    FrontPorch /= 2;
    HsyncPixels /= 2;
  }

  if (Mode->VencRepeat) {
    TotalPixels *= 2;
    ActivePixels *= 2;
    FrontPorch *= 2;
    HsyncPixels *= 2;
  }

  VTotal     = Mode->Height + Mode->VFrontPorch + Mode->VSync +
               Mode->VBackPorch;
  EofLines   = Mode->VFrontPorch;
  SofLines   = Mode->VBackPorch;
  VsyncLines = Mode->VSync;
  if (Mode->Interlaced) {
    EofLines   /= 2;
    SofLines   /= 2;
    VsyncLines /= 2;
  }

  DeHBegin = Modulo (VpuRead (VPU_ENCP_VIDEO_HAVON_BEG) + 2, TotalPixels);
  DeHEnd   = Modulo (DeHBegin + ActivePixels, TotalPixels);
  VpuWrite (VPU_ENCP_DE_H_BEG, DeHBegin);
  VpuWrite (VPU_ENCP_DE_H_END, DeHEnd);

  DeVBeginEven = VpuRead (VPU_ENCP_VIDEO_VAVON_BEG);
  DeVEndEven   = DeVBeginEven +
                 (Mode->Interlaced ? Mode->Height / 2 : Mode->Height);
  VpuWrite (VPU_ENCP_DE_V_BEG, DeVBeginEven);
  VpuWrite (VPU_ENCP_DE_V_END, DeVEndEven);

  DeVBeginOdd = 0;
  if (Mode->Interlaced) {
    FieldOffset = SignedNibble (VpuRead (VPU_ENCP_VIDEO_OFLD_VOAV) >> 4);
    DeVBeginOdd = (UINT32)((INT32)DeVBeginEven + FieldOffset) +
                  ((VTotal - 1) / 2);
    DeVEndOdd = DeVBeginOdd + (Mode->Height / 2);
    VpuWrite (VPU_ENCP_DE_V_BEG_ODD, DeVBeginOdd);
    VpuWrite (VPU_ENCP_DE_V_END_ODD, DeVEndOdd);
  }

  if ((DeHEnd + FrontPorch) >= TotalPixels) {
    HsBegin  = DeHEnd + FrontPorch - TotalPixels;
    VsAdjust = 1;
  } else {
    HsBegin  = DeHEnd + FrontPorch;
    VsAdjust = 0;
  }

  HsEnd = Modulo (HsBegin + HsyncPixels, TotalPixels);
  VpuWrite (VPU_ENCP_DVI_HSO_BEG, HsBegin);
  VpuWrite (VPU_ENCP_DVI_HSO_END, HsEnd);

  if (DeVBeginEven >= (SofLines + VsyncLines + (1 - VsAdjust))) {
    VsBeginEven = DeVBeginEven - SofLines - VsyncLines - (1 - VsAdjust);
  } else {
    VsBeginEven = VTotal + DeVBeginEven - SofLines - VsyncLines -
                  (1 - VsAdjust);
  }

  VsEndEven = Modulo (VsBeginEven + VsyncLines, VTotal);
  VpuWrite (VPU_ENCP_DVI_VSO_BLINE, VsBeginEven);
  VpuWrite (VPU_ENCP_DVI_VSO_ELINE, VsEndEven);
  VpuWrite (VPU_ENCP_DVI_VSO_BEG, HsBegin);
  VpuWrite (VPU_ENCP_DVI_VSO_END, HsBegin);

  if (Mode->Interlaced) {
    VsBeginOdd = (DeVBeginOdd - 1) - SofLines - VsyncLines;
    VsEndOdd   = (DeVBeginOdd - 1) - VsyncLines;
    VsoBeginOdd = Modulo (HsBegin + (TotalPixels >> 1), TotalPixels);
    VpuWrite (VPU_ENCP_DVI_VSO_BLINE_ODD, VsBeginOdd);
    VpuWrite (VPU_ENCP_DVI_VSO_ELINE_ODD, VsEndOdd);
    VpuWrite (VPU_ENCP_DVI_VSO_BEG_ODD, VsoBeginOdd);
    VpuWrite (VPU_ENCP_DVI_VSO_END_ODD, VsoBeginOdd);
  }

  VpuWrite (
    VPU_VIU_VENC_MUX_CTRL,
    (VpuRead (VPU_VIU_VENC_MUX_CTRL) & ~0xFU) | 0xAU
    );
}

STATIC
VOID
ConfigureEnciEncoder (
  IN CONST MESON_DISPLAY_MODE  *Mode
  )
{
  CONST MESON_ENCI_TIMING  *Timing;
  UINT32                   ActivePixels;
  UINT32                   DeHBegin;
  UINT32                   DeHEnd;
  UINT32                   DeVBeginEven;
  UINT32                   DeVBeginOdd;
  UINT32                   DeVEndEven;
  UINT32                   DeVEndOdd;
  UINT32                   EofLines;
  UINT32                   FrontPorch;
  UINT32                   HsBegin;
  UINT32                   HsEnd;
  UINT32                   HsyncPixels;
  UINT32                   LinesF0;
  UINT32                   LinesF1;
  UINT32                   TotalPixels;
  UINT32                   VsoBegin;
  UINT32                   VsAdjust;
  UINT32                   VsBeginEven;
  UINT32                   VsBeginOdd;
  UINT32                   VsEndEven;
  UINT32                   VsEndOdd;
  UINT32                   VsyncLines;
  UINT32                   VTotal;

  Timing = (Mode->EncoderProfile == MesonEncoderEnci480i) ?
           &mEnci480i : &mEnci576i;

  VpuWrite (VPU_ENCI_CFILT_CTRL, BIT (1) | 0x10);
  VpuWrite (VPU_ENCI_CFILT_CTRL2, 2 | (1U << 4));
  VpuWrite (VPU_VENC_DVI_SETTING, 0);
  VpuWrite (VPU_ENCI_VIDEO_MODE, 0);
  VpuWrite (VPU_ENCI_VIDEO_MODE_ADV, 0);
  VpuWrite (VPU_ENCI_SYNC_HSO_BEG, Timing->HsoBegin);
  VpuWrite (VPU_ENCI_SYNC_HSO_END, Timing->HsoEnd);
  VpuWrite (VPU_ENCI_SYNC_VSO_EVNLN, Timing->VsoEven);
  VpuWrite (VPU_ENCI_SYNC_VSO_ODDLN, Timing->VsoOdd);
  VpuWrite (VPU_ENCI_MACV_MAX_AMP, BIT (15) | Timing->MacvMaxAmp);
  VpuWrite (VPU_VENC_VIDEO_PROG_MODE, Timing->VideoProgMode);
  VpuWrite (VPU_ENCI_VIDEO_MODE, Timing->VideoMode);
  VpuWrite (VPU_ENCI_VIDEO_MODE_ADV, 2 | BIT (2) | (2U << 4));
  VpuWrite (VPU_ENCI_VIDEO_SCH, Timing->SchAdjust);
  VpuWrite (VPU_ENCI_SYNC_MODE, 7);
  if (Timing->YcDelay != 0) {
    VpuWrite (VPU_ENCI_YC_DELAY, Timing->YcDelay);
  }

  VpuWrite (VPU_ENCI_DBG_PX_RST, 0);
  VpuWrite (VPU_ENCI_VFIFO2VD_CTL, BIT (0) | (0x4EU << 8));
  VpuWrite (VPU_ENCI_VFIFO2VD_PX_BEG, Timing->PixelStart);
  VpuWrite (VPU_ENCI_VFIFO2VD_PX_END, Timing->PixelEnd);
  VpuWrite (VPU_ENCI_VFIFO2VD_LN_T_BEG, Timing->TopLineStart);
  VpuWrite (VPU_ENCI_VFIFO2VD_LN_T_END, Timing->TopLineEnd);
  VpuWrite (VPU_ENCI_VFIFO2VD_LN_B_BEG, Timing->BottomLineStart);
  VpuWrite (VPU_ENCI_VFIFO2VD_LN_B_END, Timing->BottomLineEnd);
  VpuWrite (
    VPU_VIU_VENC_MUX_CTRL,
    (VpuRead (VPU_VIU_VENC_MUX_CTRL) & ~0xFU) | 0x5U
    );
  VpuWrite (VPU_ENCI_VIDEO_EN, 1);

  TotalPixels = Mode->Width + Mode->HFrontPorch + Mode->HSync +
                Mode->HBackPorch;
  ActivePixels = Mode->Width;
  FrontPorch   = Mode->HFrontPorch;
  HsyncPixels  = Mode->HSync;
  if (Mode->HdmiRepeat) {
    TotalPixels /= 2;
    ActivePixels /= 2;
    FrontPorch /= 2;
    HsyncPixels /= 2;
  }

  if (Mode->VencRepeat) {
    TotalPixels *= 2;
    ActivePixels *= 2;
    FrontPorch *= 2;
    HsyncPixels *= 2;
  }

  VTotal     = Mode->Height + Mode->VFrontPorch + Mode->VSync +
               Mode->VBackPorch;
  EofLines   = Mode->VFrontPorch / 2;
  VsyncLines = Mode->VSync / 2;
  LinesF0    = VTotal >> 1;
  LinesF1    = LinesF0 + 1;

  DeHBegin = Modulo (VpuRead (VPU_ENCI_VFIFO2VD_PX_BEG) + 1, TotalPixels);
  DeHEnd   = Modulo (DeHBegin + ActivePixels, TotalPixels);
  VpuWrite (VPU_ENCI_DE_H_BEG, DeHBegin);
  VpuWrite (VPU_ENCI_DE_H_END, DeHEnd);

  DeVBeginEven = VpuRead (VPU_ENCI_VFIFO2VD_LN_T_BEG);
  DeVEndEven   = DeVBeginEven + Mode->Height;
  DeVBeginOdd  = VpuRead (VPU_ENCI_VFIFO2VD_LN_B_BEG);
  DeVEndOdd    = DeVBeginOdd + Mode->Height;
  VpuWrite (VPU_ENCI_DE_V_BEG_EVN, DeVBeginEven);
  VpuWrite (VPU_ENCI_DE_V_END_EVN, DeVEndEven);
  VpuWrite (VPU_ENCI_DE_V_BEG_ODD, DeVBeginOdd);
  VpuWrite (VPU_ENCI_DE_V_END_ODD, DeVEndOdd);

  if ((DeHEnd + FrontPorch) >= TotalPixels) {
    HsBegin  = DeHEnd + FrontPorch - TotalPixels;
    VsAdjust = 1;
  } else {
    HsBegin  = DeHEnd + FrontPorch;
    VsAdjust = 0;
  }

  HsEnd = Modulo (HsBegin + HsyncPixels, TotalPixels);
  VpuWrite (VPU_ENCI_DVI_HSO_BEG, HsBegin);
  VpuWrite (VPU_ENCI_DVI_HSO_END, HsEnd);

  if (((DeVEndOdd - 1) + EofLines + VsAdjust) >= LinesF1) {
    VsBeginEven = (DeVEndOdd - 1) + EofLines + VsAdjust - LinesF1;
    VsEndEven   = VsBeginEven + VsyncLines;
    VpuWrite (VPU_ENCI_DVI_VSO_BLN_EVN, VsBeginEven);
    VpuWrite (VPU_ENCI_DVI_VSO_ELN_EVN, VsEndEven);
    VpuWrite (VPU_ENCI_DVI_VSO_BEG_EVN, HsBegin);
    VpuWrite (VPU_ENCI_DVI_VSO_END_EVN, HsBegin);
  } else {
    VsBeginOdd = (DeVEndOdd - 1) + EofLines + VsAdjust;
    VpuWrite (VPU_ENCI_DVI_VSO_BLN_ODD, VsBeginOdd);
    VpuWrite (VPU_ENCI_DVI_VSO_BEG_ODD, HsBegin);
    if ((VsBeginOdd + VsyncLines) >= LinesF1) {
      VsEndEven = VsBeginOdd + VsyncLines - LinesF1;
      VpuWrite (VPU_ENCI_DVI_VSO_ELN_EVN, VsEndEven);
      VpuWrite (VPU_ENCI_DVI_VSO_END_EVN, HsBegin);
    } else {
      VsEndOdd = VsBeginOdd + VsyncLines;
      VpuWrite (VPU_ENCI_DVI_VSO_ELN_ODD, VsEndOdd);
      VpuWrite (VPU_ENCI_DVI_VSO_END_ODD, HsBegin);
    }
  }

  if (((DeVEndEven - 1) + EofLines + 1) >= LinesF0) {
    VsBeginOdd = (DeVEndEven - 1) + EofLines + 1 - LinesF0;
    VsEndOdd   = VsBeginOdd + VsyncLines;
    VsoBegin   = Modulo (HsBegin + (TotalPixels >> 1), TotalPixels);
    VpuWrite (VPU_ENCI_DVI_VSO_BLN_ODD, VsBeginOdd);
    VpuWrite (VPU_ENCI_DVI_VSO_ELN_ODD, VsEndOdd);
    VpuWrite (VPU_ENCI_DVI_VSO_BEG_ODD, VsoBegin);
    VpuWrite (VPU_ENCI_DVI_VSO_END_ODD, VsoBegin);
  } else {
    VsBeginEven = (DeVEndEven - 1) + EofLines + 1;
    VsoBegin    = Modulo (HsBegin + (TotalPixels >> 1), TotalPixels);
    VpuWrite (VPU_ENCI_DVI_VSO_BLN_EVN, VsBeginEven);
    VpuWrite (VPU_ENCI_DVI_VSO_BEG_EVN, VsoBegin);
    if ((VsBeginEven + VsyncLines) >= LinesF0) {
      VsEndOdd = VsBeginEven + VsyncLines - LinesF0;
      VpuWrite (VPU_ENCI_DVI_VSO_ELN_ODD, VsEndOdd);
      VpuWrite (VPU_ENCI_DVI_VSO_END_ODD, VsoBegin);
    } else {
      VsEndEven = VsBeginEven + VsyncLines;
      VpuWrite (VPU_ENCI_DVI_VSO_ELN_EVN, VsEndEven);
      VpuWrite (VPU_ENCI_DVI_VSO_END_EVN, VsoBegin);
    }
  }
}

STATIC
VOID
ConfigureEncoder (
  IN CONST MESON_DISPLAY_MODE  *Mode
  )
{
  UINT32  Bridge;

  VpuWrite (VPU_ENCP_VIDEO_EN, 0);
  VpuWrite (VPU_ENCI_VIDEO_EN, 0);
  if (Mode->UseEnci) {
    ConfigureEnciEncoder (Mode);
  } else {
    ConfigureEncpEncoder (Mode);
  }

  Bridge = (4U << 5) | (Mode->UseEnci ? 1U : 2U);
  if (Mode->HSyncPositive) {
    Bridge |= BIT (3);
  }

  if (Mode->VSyncPositive) {
    Bridge |= BIT (2);
  }

  if (Mode->VencRepeat) {
    Bridge |= BIT (8);
  }

  if (Mode->HdmiRepeat) {
    Bridge |= BIT (12);
  }

  VpuWrite (VPU_HDMI_SETTING, Bridge);
  HhiWrite (HHI_VDAC_CNTL0, 0);
  HhiWrite (HHI_VDAC_CNTL1, 8);
}

STATIC
VOID
ConfigureTx (
  IN CONST MESON_DISPLAY_PRIVATE  *Private
  )
{
  CONST MESON_DISPLAY_MODE  *Mode;
  UINT32                    HBlank;
  UINT32                    FcHeight;
  UINT32                    FcVBlank;
  UINT32                    FcVFrontPorch;
  UINT32                    FcVSync;
  UINT32                    VBlank;
  UINT8                     InVidConf;
  UINT8                     AviConf1;
  UINTN                     Register;
  STATIC CONST UINT8        CscYuvToRgb[24] = {
    0x10, 0x00, 0xF4, 0x93, 0xFA, 0x7F, 0x00, 0x87,
    0x10, 0x00, 0x16, 0x6E, 0x00, 0x00, 0xFF, 0x4D,
    0x10, 0x00, 0x00, 0x00, 0x1C, 0x5A, 0xFF, 0x1E
  };

  Mode   = &Private->Timing;
  HBlank = Mode->HFrontPorch + Mode->HSync + Mode->HBackPorch;
  VBlank = Mode->VFrontPorch + Mode->VSync + Mode->VBackPorch;
  FcHeight      = Mode->Interlaced ? Mode->Height / 2 : Mode->Height;
  FcVBlank      = Mode->Interlaced ? VBlank / 2 : VBlank;
  FcVFrontPorch = Mode->Interlaced ? Mode->VFrontPorch / 2 :
                                     Mode->VFrontPorch;
  FcVSync       = Mode->Interlaced ? Mode->VSync / 2 : Mode->VSync;

  DEBUG ((DEBUG_INFO, "MesonDisplay: TX video path\n"));
  TopWrite (HDMITX_TOP_BIST_CNTL, BIT (12));
  DwcWrite (DWC_TX_INVID0, 0x09);
  for (Register = DWC_TX_INSTUFFING; Register <= DWC_TX_BCBDATA1; Register++) {
    DwcWrite ((UINT32)Register, 0);
  }

  DwcWrite (DWC_MC_FLOWCTRL, BIT (0));
  DwcWrite (DWC_CSC_CFG, 0);
  for (Register = 0; Register < ARRAY_SIZE (CscYuvToRgb); Register++) {
    DwcWrite (DWC_CSC_COEF_A1_MSB + (UINT32)Register, CscYuvToRgb[Register]);
  }

  DwcWrite (DWC_CSC_SCALE, 0x42);
  DwcWrite (DWC_VP_PR_CD, 0);
  DwcWrite (DWC_VP_STUFF, 0);
  DwcWrite (DWC_VP_REMAP, 0);
  DwcWrite (DWC_VP_CONF, BIT (6) | BIT (2) | 2);
  DwcWrite (DWC_VP_MASK, 0xFF);

  InVidConf = BIT (4);
  if (Mode->VSyncPositive) {
    InVidConf |= BIT (6);
  }

  if (Mode->HSyncPositive) {
    InVidConf |= BIT (5);
  }

  if (Mode->Interlaced) {
    InVidConf |= BIT (1) | BIT (0);
  }

  if (Private->HdmiSink) {
    //
    // Keep the hardware-proven HDMI keepout policy on the primary path.  A
    // DVI sink has neither HDCP nor data islands; leaving keepout asserted
    // there differs from the working native 720p DVI state.
    //
    InVidConf |= BIT (7) | BIT (3);
  }

  DwcWrite (DWC_FC_INVIDCONF, InVidConf);
  DwcWrite (DWC_FC_INHACTV0, (UINT8)Mode->Width);
  DwcWrite (DWC_FC_INHACTV1, (UINT8)(Mode->Width >> 8));
  DwcWrite (DWC_FC_INHBLANK0, (UINT8)HBlank);
  DwcWrite (DWC_FC_INHBLANK1, (UINT8)(HBlank >> 8));
  DwcWrite (DWC_FC_INVACTV0, (UINT8)FcHeight);
  DwcWrite (DWC_FC_INVACTV1, (UINT8)(FcHeight >> 8));
  DwcWrite (DWC_FC_INVBLANK, (UINT8)FcVBlank);
  DwcWrite (DWC_FC_HSYNCDELAY0, (UINT8)Mode->HFrontPorch);
  DwcWrite (DWC_FC_HSYNCDELAY1, (UINT8)(Mode->HFrontPorch >> 8));
  DwcWrite (DWC_FC_HSYNCWIDTH0, (UINT8)Mode->HSync);
  DwcWrite (DWC_FC_HSYNCWIDTH1, (UINT8)(Mode->HSync >> 8));
  DwcWrite (DWC_FC_VSYNCDELAY, (UINT8)FcVFrontPorch);
  DwcWrite (DWC_FC_VSYNCWIDTH, (UINT8)FcVSync);
  DwcWrite (DWC_FC_CTRLDUR, 12);
  DwcWrite (DWC_FC_EXCTRLDUR, 32);
  DwcWrite (DWC_FC_EXCTRLSPAC, 1);
  DwcWrite (DWC_FC_GCP, Private->HdmiSink ? 1 : 0);
  DwcWrite (DWC_FC_AVICONF0, Private->HdmiSink ? BIT (6) : 0);
  AviConf1 = ((Mode->CtaVic == 2) || (Mode->CtaVic == 6) ||
              (Mode->CtaVic == 17) || (Mode->CtaVic == 21)) ?
             0x58 : 0x68;
  DwcWrite (DWC_FC_AVICONF1, AviConf1);
  DwcWrite (DWC_FC_AVICONF2, 0);
  DwcWrite (DWC_FC_AVICONF3, 0);
  DwcWrite (DWC_FC_AVIVID, Private->HdmiSink ? Mode->CtaVic : 0);
  DwcWrite (DWC_FC_ACTSPC, 0);
  DwcWrite (DWC_FC_INVACT2D0, (UINT8)FcHeight);
  DwcWrite (DWC_FC_INVACT2D1, (UINT8)(FcHeight >> 8));
  DwcWrite (DWC_FC_MASK0, 0xE7);
  DwcWrite (DWC_FC_MASK1, 0xFB);
  DwcWrite (DWC_FC_MASK2, 3);
  DwcWrite (DWC_FC_PRCONF, 0x10);

  //
  // This boot-services-only driver never installs an HDMI interrupt handler.
  // Keep the Amlogic TOP interrupt outputs masked instead of clearing and
  // unmasking every DW-HDMI sub-block.  Besides being unnecessary, touching
  // the CEC/HDCP-adjacent interrupt aperture is outside the phase scope and
  // caused an external abort on the VIM3's vendor secure-world stack.
  //
  DEBUG ((DEBUG_INFO, "MesonDisplay: mask TX interrupts\n"));
  TopWrite (HDMITX_TOP_INTR_MASKN, 0);
  TopWrite (HDMITX_TOP_INTR_CLR, 0x1F);
  ArmDataSynchronizationBarrier ();

  DEBUG ((DEBUG_INFO, "MesonDisplay: TX reset and TMDS\n"));
  DwcWrite (DWC_MC_SWRSTZREQ, 0);
  MicroSecondDelay (10);
  DwcWrite (DWC_MC_SWRSTZREQ, 0x7D);
  //
  // Leave clocks for unused blocks gated while keeping the pixel, TMDS, and
  // CSC paths running.  Enabling every DW-HDMI clock here stalls visible
  // output on G12B.  This mask matches the mainline dw-hdmi steady state and
  // was isolated on the VIM3 by changing only MC_CLKDIS: 0x00 blanked the
  // sink and 0x6C immediately restored the GOP scanout.
  //
  DwcWrite (
    DWC_MC_CLKDIS,
    DWC_MC_CLKDIS_HDCP |
    DWC_MC_CLKDIS_CEC |
    DWC_MC_CLKDIS_AUDIO |
    DWC_MC_CLKDIS_PREP
    );
  TopWrite (HDMITX_TOP_TMDS_PTTN_01, 0x001F001F);
  TopWrite (HDMITX_TOP_TMDS_PTTN_23, 0x001F001F);
  TopWrite (HDMITX_TOP_TMDS_PTTN_CTL, 1);
  MicroSecondDelay (2);
  TopWrite (HDMITX_TOP_TMDS_PTTN_CTL, 2);
}

STATIC
VOID
ConfigurePhy (
  IN CONST MESON_DISPLAY_MODE  *Mode
  )
{
  UINT32  ChannelMap;
  UINT32  PhyControl0;

  ChannelMap = (1U << 20) | (2U << 22) | (3U << 24);
  HhiWrite (HHI_HDMI_PHY_CNTL0, 0);
  HhiWrite (HHI_HDMI_PHY_CNTL1, ChannelMap | 0xF);
  MicroSecondDelay (2);
  HhiWrite (HHI_HDMI_PHY_CNTL1, ChannelMap | 0xE);
  MicroSecondDelay (2);
  HhiWrite (HHI_HDMI_PHY_CNTL1, ChannelMap | 0xF);
  MicroSecondDelay (2);
  HhiWrite (HHI_HDMI_PHY_CNTL1, ChannelMap | 0xE);
  MicroSecondDelay (2);
  PhyControl0 = (Mode->PixelClockKhz >= 297000) ?
                0x33EB6262 : 0x33EB4242;
  HhiWrite (HHI_HDMI_PHY_CNTL0, PhyControl0);
  HhiWrite (HHI_HDMI_PHY_CNTL3, 0x2AB0FF3B);
  HhiWrite (HHI_HDMI_PHY_CNTL5, 3);
  MicroSecondDelay (20);
}

STATIC
VOID
ConfigureG12aOsd1Matrix (
  VOID
  )
{
  //
  // Mainline's limited-range BT.709 RGB-to-YUV matrix.  G12A/G12B route
  // OSD1 through this wrapper before the VPP/HDMI path; bypassing it leaves
  // a valid timing stream whose active pixels can remain black.
  //
  VpuWrite (VPP_WRAP_OSD1_MATRIX_PRE_OFFSET0_1, 0);
  VpuWrite (VPP_WRAP_OSD1_MATRIX_PRE_OFFSET2, 0);
  VpuWrite (VPP_WRAP_OSD1_MATRIX_COEF00_01, 0x00BA0273);
  VpuWrite (VPP_WRAP_OSD1_MATRIX_COEF02_10, 0x003F1F9A);
  VpuWrite (VPP_WRAP_OSD1_MATRIX_COEF11_12, 0x1EA801C0);
  VpuWrite (VPP_WRAP_OSD1_MATRIX_COEF20_21, 0x01C01E6A);
  VpuWrite (VPP_WRAP_OSD1_MATRIX_COEF22, 0x00001FD8);
  VpuWrite (VPP_WRAP_OSD1_MATRIX_OFFSET0_1, 0x00400200);
  VpuWrite (VPP_WRAP_OSD1_MATRIX_OFFSET2, 0x00000200);
  VpuWrite (
    VPP_WRAP_OSD1_MATRIX_EN_CTRL,
    VpuRead (VPP_WRAP_OSD1_MATRIX_EN_CTRL) | BIT (0)
    );
}

STATIC
VOID
ConfigureCanvasAndOsd (
  IN CONST MESON_DISPLAY_PRIVATE  *Private
  )
{
  UINT64  AddressUnits;
  UINT32  WidthUnits;
  UINT32  DataLow;
  UINT32  DataHigh;
  UINT32  Width;
  UINT32  Height;
  UINT32  Fifo;

  AddressUnits = RShiftU64 (Private->FrameBufferBase + 7, 3);
  WidthUnits   = (Private->Stride + 7) >> 3;
  DataLow      = ((UINT32)AddressUnits & 0x1FFFFFFF) | ((WidthUnits & 7U) << 29);
  DataHigh     = (WidthUnits >> 3) | ((Private->Timing.Height & 0x1FFFU) << 9);
  MmioWrite32 (MESON_DMC_BASE + MESON_REG (0x12), DataLow);
  MmioWrite32 (MESON_DMC_BASE + MESON_REG (0x13), DataHigh);
  MmioWrite32 (MESON_DMC_BASE + MESON_REG (0x14), (2U << 8) | MESON_CANVAS_INDEX);
  MmioRead32 (MESON_DMC_BASE + MESON_REG (0x13));

  Width  = Private->Timing.Width;
  Height = Private->Timing.Height;
  VpuWrite (VPP_OSD_SC_CTRL0, 0);
  VpuWrite (VPP_OSD_VSC_CTRL0, 0);
  VpuWrite (VPP_OSD_HSC_CTRL0, 0);
  VpuWrite (VPU_OSD1_MALI_UNPACK, VpuRead (VPU_OSD1_MALI_UNPACK) & ~BIT (31));
  VpuWrite (VPU_OSD_PATH_MISC_CTRL, VpuRead (VPU_OSD_PATH_MISC_CTRL) & ~BIT (4));
  VpuWrite (VPU_MAFBC_SURFACE_CFG, VpuRead (VPU_MAFBC_SURFACE_CFG) & ~BIT (0));

  //
  // G12A/G12B use a 32-word burst and hold 31 FIFO lines.  BIT(31) would
  // select the older 128-word burst encoding used by pre-G12 hardware.
  //
  Fifo = 1U | (31U << 5) | BIT (10) | (32U << 12) | (2U << 22) |
         (2U << 24);
  VpuWrite (VPU_OSD1_FIFO_CTRL_STAT, Fifo);
  VpuWrite (
    VPU_OSD1_BLK0_CFG_W0,
    (MESON_CANVAS_INDEX << 16) | BIT (15) | (5U << 8) | (1U << 2)
    );
  VpuWrite (VPU_OSD1_BLK0_CFG_W1, (Width - 1) << 16);
  VpuWrite (VPU_OSD1_BLK0_CFG_W2, (Height - 1) << 16);
  VpuWrite (VPU_OSD1_BLK0_CFG_W3, (Width - 1) << 16);
  VpuWrite (VPU_OSD1_BLK0_CFG_W4, (Height - 1) << 16);
  VpuWrite (VPU_OSD1_CTRL_STAT2, BIT (14) | (0xFFU << 6) | BIT (1));
  VpuWrite (VPU_OSD1_CTRL_STAT, BIT (21) | (0x100U << 12) | BIT (0));

  VpuWrite (
    VIU_OSD_BLEND_CTRL,
    (4U << 29) | BIT (27) | BIT (26) | BIT (25) | BIT (24) |
    BIT (20) | 1U
    );
  VpuWrite (OSD1_BLEND_SRC_CTRL, (3U << 8) | BIT (20));
  VpuWrite (OSD2_BLEND_SRC_CTRL, BIT (20));
  VpuWrite (VIU_OSD_BLEND_DUMMY0, 0);
  VpuWrite (VIU_OSD_BLEND_DUMMY_ALPHA, 0);
  VpuWrite (VIU_OSD_BLEND_DIN0_H, (Width - 1) << 16);
  VpuWrite (VIU_OSD_BLEND_DIN0_V, (Height - 1) << 16);
  VpuWrite (VIU_OSD_BLEND0_SIZE, (Height << 16) | Width);
  VpuWrite (VIU_OSD_BLEND1_SIZE, (Height << 16) | Width);
  VpuWrite (DOLBY_PATH_CTRL, VpuRead (DOLBY_PATH_CTRL) | (3U << 2));
  VpuWrite (VPP_MISC, (VpuRead (VPP_MISC) | BIT (7)) & ~BIT (6));
  VpuWrite (VPP_OSD1_IN_SIZE, (Height << 16) | Width);
  VpuWrite (VPP_OSD1_BLD_H_SCOPE, Width - 1);
  VpuWrite (VPP_OSD1_BLD_V_SCOPE, Height - 1);
  VpuWrite (VPP_POSTBLEND_H_SIZE, Width);
  VpuWrite (VPP_OUT_H_V_SIZE, (Width << 16) | Height);
  //
  // G12B's VPP output FIFO needs both the 12-bit FIFO capacity in bits
  // 31:20 and the one-past-last word count in bits 13:0.  A low-only 0xfff
  // value leaves the postblend path producing a valid HDMI timing stream
  // whose active pixels are all black.
  //
  VpuWrite (VPP_OFIFO_SIZE, (0xFFFU << 20) | (0xFFFU + 1U));
  VpuWrite (VPP_HOLD_LINES, 0x08080808);
  ConfigureG12aOsd1Matrix ();
  VpuWrite (
    OSD1_HDR2_CTRL,
    VpuRead (OSD1_HDR2_CTRL) & ~(BIT (16) | BIT (13))
    );
  VpuWrite (VPP_POST2_MATRIX_EN, VpuRead (VPP_POST2_MATRIX_EN) & ~BIT (0));

  DEBUG ((
    DEBUG_INFO,
    "MesonDisplay: OSD canvas=%08x/%08x/%08x ctrl=%08x/%08x "
    "cfg=%08x fifo=%08x ofifo=%08x blend=%08x/%08x matrix=%08x hdr2=%08x\n",
    MmioRead32 (MESON_DMC_BASE + MESON_REG (0x12)),
    MmioRead32 (MESON_DMC_BASE + MESON_REG (0x13)),
    MmioRead32 (MESON_DMC_BASE + MESON_REG (0x14)),
    VpuRead (VPU_OSD1_CTRL_STAT),
    VpuRead (VPU_OSD1_CTRL_STAT2),
    VpuRead (VPU_OSD1_BLK0_CFG_W0),
    VpuRead (VPU_OSD1_FIFO_CTRL_STAT),
    VpuRead (VPP_OFIFO_SIZE),
    VpuRead (VIU_OSD_BLEND_CTRL),
    VpuRead (OSD1_BLEND_SRC_CTRL),
    VpuRead (VPP_WRAP_OSD1_MATRIX_EN_CTRL),
    VpuRead (OSD1_HDR2_CTRL)
    ));
}

STATIC
VOID
DumpDisplayState (
  VOID
  )
{
  DEBUG ((
    DEBUG_INFO,
    "MesonDisplay: state HHI PLL0=%08x VID_DIV=%08x VID_CNTL=%08x "
    "VID_CNTL2=%08x PLL_DIV=%08x HDMI_CLK=%08x\n",
    HhiRead (HHI_HDMI_PLL_CNTL0),
    HhiRead (HHI_VID_CLK_DIV),
    HhiRead (HHI_VID_CLK_CNTL),
    HhiRead (HHI_VID_CLK_CNTL2),
    HhiRead (HHI_VID_PLL_CLK_DIV),
    HhiRead (HHI_HDMI_CLK_CNTL)
    ));
  DEBUG ((
    DEBUG_INFO,
    "MesonDisplay: state PHY CNTL0=%08x CNTL1=%08x CNTL3=%08x "
    "CNTL5=%08x TOP_STAT0=%08x\n",
    HhiRead (HHI_HDMI_PHY_CNTL0),
    HhiRead (HHI_HDMI_PHY_CNTL1),
    HhiRead (HHI_HDMI_PHY_CNTL3),
    HhiRead (HHI_HDMI_PHY_CNTL5),
    MmioRead32 (MESON_HDMI_TOP_BASE + MESON_REG (HDMITX_TOP_STAT0))
    ));
  DEBUG ((
    DEBUG_INFO,
    "MesonDisplay: state ENCP MODE=%08x ADV=%08x MAX=%08x/%08x "
    "HAVON=%08x-%08x VAVON=%08x-%08x\n",
    VpuRead (VPU_ENCP_VIDEO_MODE),
    VpuRead (VPU_ENCP_VIDEO_MODE_ADV),
    VpuRead (VPU_ENCP_VIDEO_MAX_PXCNT),
    VpuRead (VPU_ENCP_VIDEO_MAX_LNCNT),
    VpuRead (VPU_ENCP_VIDEO_HAVON_BEG),
    VpuRead (VPU_ENCP_VIDEO_HAVON_END),
    VpuRead (VPU_ENCP_VIDEO_VAVON_BEG),
    VpuRead (VPU_ENCP_VIDEO_VAVON_END)
    ));
  DEBUG ((
    DEBUG_INFO,
    "MesonDisplay: state ENCP DE_H=%08x-%08x HSO=%08x-%08x "
    "DE_V=%08x-%08x VSO=%08x-%08x/%08x\n",
    VpuRead (VPU_ENCP_DE_H_BEG),
    VpuRead (VPU_ENCP_DE_H_END),
    VpuRead (VPU_ENCP_DVI_HSO_BEG),
    VpuRead (VPU_ENCP_DVI_HSO_END),
    VpuRead (VPU_ENCP_DE_V_BEG),
    VpuRead (VPU_ENCP_DE_V_END),
    VpuRead (VPU_ENCP_DVI_VSO_BLINE),
    VpuRead (VPU_ENCP_DVI_VSO_ELINE),
    VpuRead (VPU_ENCP_DVI_VSO_BEG)
    ));
  DEBUG ((
    DEBUG_INFO,
    "MesonDisplay: state ENCI MODE=%08x ADV=%08x DE_H=%08x-%08x "
    "DE_V=%08x-%08x/%08x-%08x HSO=%08x-%08x\n",
    VpuRead (VPU_ENCI_VIDEO_MODE),
    VpuRead (VPU_ENCI_VIDEO_MODE_ADV),
    VpuRead (VPU_ENCI_DE_H_BEG),
    VpuRead (VPU_ENCI_DE_H_END),
    VpuRead (VPU_ENCI_DE_V_BEG_EVN),
    VpuRead (VPU_ENCI_DE_V_END_EVN),
    VpuRead (VPU_ENCI_DE_V_BEG_ODD),
    VpuRead (VPU_ENCI_DE_V_END_ODD),
    VpuRead (VPU_ENCI_DVI_HSO_BEG),
    VpuRead (VPU_ENCI_DVI_HSO_END)
    ));
  DEBUG ((
    DEBUG_INFO,
    "MesonDisplay: state BRIDGE=%08x FC=%02x VIC=%02x "
    "ACTIVE=%02x%02x/%02x%02x "
    "BLANK=%02x%02x/%02x HS=%02x%02x+%02x%02x VS=%02x+%02x\n",
    VpuRead (VPU_HDMI_SETTING),
    DwcRead (DWC_FC_INVIDCONF),
    DwcRead (DWC_FC_AVIVID),
    DwcRead (DWC_FC_INHACTV1),
    DwcRead (DWC_FC_INHACTV0),
    DwcRead (DWC_FC_INVACTV1),
    DwcRead (DWC_FC_INVACTV0),
    DwcRead (DWC_FC_INHBLANK1),
    DwcRead (DWC_FC_INHBLANK0),
    DwcRead (DWC_FC_INVBLANK),
    DwcRead (DWC_FC_HSYNCDELAY1),
    DwcRead (DWC_FC_HSYNCDELAY0),
    DwcRead (DWC_FC_HSYNCWIDTH1),
    DwcRead (DWC_FC_HSYNCWIDTH0),
    DwcRead (DWC_FC_VSYNCDELAY),
    DwcRead (DWC_FC_VSYNCWIDTH)
    ));
}

EFI_STATUS
MesonDisplayWakeSink (
  VOID
  )
{
  EFI_STATUS             Status;
  MESON_DISPLAY_PRIVATE  Probe;
  UINT32                 BridgeRates;

  ZeroMem (&Probe, sizeof (Probe));
  Probe.Timing   = gMesonMode720p;
  Probe.HdmiSink = FALSE;

  WatchdogStart ();
  DEBUG ((DEBUG_INFO, "MesonDisplay: bounded 720p link wake begin\n"));
  PrepareHdmiHost ();
  Status = ConfigurePll (&Probe.Timing);
  if (EFI_ERROR (Status)) {
    WatchdogStop ();
    return Status;
  }

  ConfigureEncoder (&Probe.Timing);
  ConfigureTx (&Probe);
  VpuWrite (VPU_HDMI_FMT_CTRL, BIT (10) | (2U << 2));
  VpuWrite (VPU_HDMI_DITH_CNTL, VpuRead (VPU_HDMI_DITH_CNTL) | BIT (4));

  BridgeRates = VpuRead (VPU_HDMI_SETTING) & 0xFF00U;
  VpuWrite (Probe.Timing.UseEnci ? VPU_ENCI_VIDEO_EN : VPU_ENCP_VIDEO_EN, 0);
  VpuWrite (VPU_HDMI_SETTING, VpuRead (VPU_HDMI_SETTING) & ~0xFF03U);
  MicroSecondDelay (1);
  VpuWrite (Probe.Timing.UseEnci ? VPU_ENCI_VIDEO_EN : VPU_ENCP_VIDEO_EN, 1);
  MicroSecondDelay (1);
  VpuWrite (
    VPU_HDMI_SETTING,
    VpuRead (VPU_HDMI_SETTING) |
    BridgeRates |
    (Probe.Timing.UseEnci ? 1U : 2U)
    );
  ConfigurePhy (&Probe.Timing);

  //
  // Keep the wait bounded while allowing slower KVM EDID emulators to notice
  // the link and assert HPD before the caller performs its single retry.
  //
  MicroSecondDelay (250000);
  WatchdogStop ();
  DEBUG ((
    DEBUG_INFO,
    "MesonDisplay: link wake complete, TOP_STAT0=0x%08x\n",
    MmioRead32 (MESON_HDMI_TOP_BASE + MESON_REG (HDMITX_TOP_STAT0))
    ));
  return EFI_SUCCESS;
}

EFI_STATUS
MesonDisplayHardwareInit (
  IN OUT MESON_DISPLAY_PRIVATE  *Private
  )
{
  EFI_STATUS  Status;
  UINT32      BridgeRates;

  if (Private == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  WatchdogStart ();
  DEBUG ((DEBUG_INFO, "MesonDisplay: hardware init begin\n"));
  PrepareHdmiHost ();
  DEBUG ((DEBUG_INFO, "MesonDisplay: PLL\n"));
  Status = ConfigurePll (&Private->Timing);
  if (EFI_ERROR (Status)) {
    WatchdogStop ();
    return Status;
  }

  DEBUG ((DEBUG_INFO, "MesonDisplay: encoder\n"));
  ConfigureEncoder (&Private->Timing);
  DEBUG ((DEBUG_INFO, "MesonDisplay: transmitter\n"));
  ConfigureTx (Private);
  DEBUG ((DEBUG_INFO, "MesonDisplay: bridge\n"));
  VpuWrite (VPU_HDMI_FMT_CTRL, BIT (10) | (2U << 2));
  VpuWrite (VPU_HDMI_DITH_CNTL, VpuRead (VPU_HDMI_DITH_CNTL) | BIT (4));

  BridgeRates = VpuRead (VPU_HDMI_SETTING) & 0xFF00U;
  VpuWrite (
    Private->Timing.UseEnci ? VPU_ENCI_VIDEO_EN : VPU_ENCP_VIDEO_EN,
    0
    );
  VpuWrite (VPU_HDMI_SETTING, VpuRead (VPU_HDMI_SETTING) & ~0xFF03U);
  MicroSecondDelay (1);
  VpuWrite (
    Private->Timing.UseEnci ? VPU_ENCI_VIDEO_EN : VPU_ENCP_VIDEO_EN,
    1
    );
  MicroSecondDelay (1);
  VpuWrite (
    VPU_HDMI_SETTING,
    VpuRead (VPU_HDMI_SETTING) |
    BridgeRates |
    (Private->Timing.UseEnci ? 1U : 2U)
    );
  DEBUG ((DEBUG_INFO, "MesonDisplay: PHY\n"));
  ConfigurePhy (&Private->Timing);
  DEBUG ((DEBUG_INFO, "MesonDisplay: canvas/OSD\n"));
  ConfigureCanvasAndOsd (Private);
  DumpDisplayState ();
  WatchdogStop ();
  DEBUG ((DEBUG_INFO, "MesonDisplay: hardware init complete\n"));
  return EFI_SUCCESS;
}
