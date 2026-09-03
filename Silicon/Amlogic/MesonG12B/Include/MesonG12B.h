/** @file
  Amlogic Meson G12B constants used by the VIM3 firmware port.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef MESON_G12B_H_
#define MESON_G12B_H_

#define MESON_G12B_UART_AO_BASE       0xFF803000ULL
#define MESON_G12B_UART_CLOCK_HZ      24000000U
#define MESON_G12B_AO_SEC_GP_CFG0     0xFF800240ULL

#define MESON_G12B_GICD_BASE          0xFFC01000ULL
#define MESON_G12B_GICC_BASE          0xFFC02000ULL

#define MESON_G12B_DRAM_LOW_BASE      0x00000000ULL
#define MESON_G12B_DRAM_LOW_SIZE      0x05000000ULL
#define MESON_G12B_SECURE_BASE        0x05000000ULL
#define MESON_G12B_SECURE_SIZE        0x02300000ULL
#define MESON_G12B_DRAM_HIGH_BASE     0x07300000ULL
#define MESON_G12B_DRAM_TOP           0xF4E5B000ULL
#define MESON_G12B_DRAM_HIGH_SIZE     \
  (MESON_G12B_DRAM_TOP - MESON_G12B_DRAM_HIGH_BASE)

#define MESON_G12B_FD_BASE            0x01000000ULL
#define MESON_G12B_DTB_BASE           0x01001000ULL
#define MESON_G12B_FV_BASE            0x01020000ULL

//
// VIM3 W25Q128 SPI NOR layout.  Firmware exposes a runtime RAM shadow for the
// final 768 KiB window because Meson SPIFC is command-driven, not directly
// memory mapped.
//
#define VIM3_NOR_SIZE                  0x01000000U
#define VIM3_NOR_FUTURE_BOOT_BASE      0x00000000U
#define VIM3_NOR_FUTURE_BOOT_SIZE      0x00C00000U
#define VIM3_NOR_SCRATCH_BASE          0x00C00000U
#define VIM3_NOR_SCRATCH_SIZE          0x00300000U
#define VIM3_NOR_RUNTIME_BASE          0x00F00000U
#define VIM3_NOR_RUNTIME_SIZE          0x000C0000U
#define VIM3_NOR_VARIABLE_BASE         0x00F00000U
#define VIM3_NOR_VARIABLE_SIZE         0x00040000U
#define VIM3_NOR_FTW_WORKING_BASE      0x00F40000U
#define VIM3_NOR_FTW_WORKING_SIZE      0x00002000U
#define VIM3_NOR_FTW_SPARE_BASE        0x00F80000U
#define VIM3_NOR_FTW_SPARE_SIZE        0x00040000U
#define VIM3_NOR_BOARD_DATA_BASE       0x00FC0000U
#define VIM3_NOR_BOARD_DATA_SIZE       0x00040000U
#define VIM3_NOR_ERASE_SIZE            0x00001000U
#define VIM3_NOR_PAGE_SIZE             0x00000100U

#define MESON_G12B_SD_EMMC_A_BASE     0xFFE03000ULL
#define MESON_G12B_SD_EMMC_B_BASE     0xFFE05000ULL
#define MESON_G12B_SD_EMMC_C_BASE     0xFFE07000ULL
#define MESON_G12B_SPIFC_BASE         0xFFD14000ULL
#define MESON_G12B_SPIFC_SIZE         0x00001000ULL
#define MESON_G12B_USB2_PHY0_BASE     0xFF636000ULL
#define MESON_G12B_USB2_PHY1_BASE     0xFF63A000ULL
//
// Combined USB3 / PCIe PHY (amlogic,g12a-usb3-pcie-phy).  The SuperSpeed port
// is unusable until this block is switched to USB3 and configured, even when
// the controller glue has already released the port.
//
#define MESON_G12B_USB3_PHY_BASE      0xFF646000ULL
#define MESON_G12B_USB_GLUE_BASE      0xFFE09000ULL
#define MESON_G12B_XHCI_BASE          0xFF500000ULL
#define MESON_G12B_XHCI_SIZE          0x00100000ULL

//
// G12B clock, reset, and peripheral GPIO register blocks.
//
#define MESON_G12B_HHI_BASE            0xFF63C000ULL
#define MESON_G12B_HHI_GCLK_MPEG0      (MESON_G12B_HHI_BASE + 0x140)
#define MESON_G12B_HHI_GCLK_MPEG1      (MESON_G12B_HHI_BASE + 0x144)
#define MESON_G12B_HHI_GCLK_MPEG2      (MESON_G12B_HHI_BASE + 0x148)
#define MESON_G12B_HHI_MEM_PD_REG0     (MESON_G12B_HHI_BASE + 0x100)
#define MESON_G12B_HHI_NAND_CLK_CNTL   (MESON_G12B_HHI_BASE + 0x25C)
#define MESON_G12B_HHI_SD_EMMC_CLK_CNTL \
  (MESON_G12B_HHI_BASE + 0x264)

//
// PCIe PLL.  This is the "ref_clk" of the shared USB3/PCIe PHY
// (DT: clocks = <&clkc CLKID_PCIE_PLL>, assigned-clock-rates = 100 MHz).
// Without it the SuperSpeed PHY has no reference and the port never leaves
// RxDetect, so firmware sees no SuperSpeed device at all.
//
#define MESON_G12B_HHI_PCIE_PLL_CNTL0  (MESON_G12B_HHI_BASE + 0x098)
#define MESON_G12B_HHI_PCIE_PLL_CNTL1  (MESON_G12B_HHI_BASE + 0x09C)
#define MESON_G12B_HHI_PCIE_PLL_CNTL2  (MESON_G12B_HHI_BASE + 0x0A0)
#define MESON_G12B_HHI_PCIE_PLL_CNTL3  (MESON_G12B_HHI_BASE + 0x0A4)
#define MESON_G12B_HHI_PCIE_PLL_CNTL4  (MESON_G12B_HHI_BASE + 0x0A8)
#define MESON_G12B_HHI_PCIE_PLL_CNTL5  (MESON_G12B_HHI_BASE + 0x0AC)
#define MESON_G12B_HHI_PCIE_PLL_LOCK   BIT31

#define MESON_G12B_RESET_LEVEL0        0xFFD01080ULL
#define MESON_G12B_RESET_LEVEL1        0xFFD01084ULL
#define MESON_G12B_RESET_LEVEL2        0xFFD01088ULL
//
// Self-clearing pulse registers of the same EE reset controller (base
// 0xFFD01004, one 32-bit bank per 32 reset lines; the level bank above is
// the same controller at offset 0x7C).  RESET_AUDIO is line 65 = bank 2
// bit 1.
//
#define MESON_G12B_RESET_PULSE2        0xFFD0100CULL
#define MESON_G12B_RESET_AUDIO         BIT1
#define MESON_G12B_WATCHDOG_BASE       0xFFD0F0D0ULL

//
// Audio subsystem.  MPLL0/1/2 feed the audio clock controller's master
// clock muxes; the SDM words below are programmed by MesonAudioDxe from
// the DT-mode register oracle captured on hardware, and the
// per-family enable bits (bit 31 of CNTL1/3/5) stay kernel-owned.
//
#define MESON_G12B_HHI_MPLL_CNTL0      (MESON_G12B_HHI_BASE + 0x278)
#define MESON_G12B_HHI_MPLL_CNTL1      (MESON_G12B_HHI_BASE + 0x27C)
#define MESON_G12B_HHI_MPLL_CNTL2      (MESON_G12B_HHI_BASE + 0x280)
#define MESON_G12B_HHI_MPLL_CNTL3      (MESON_G12B_HHI_BASE + 0x284)
#define MESON_G12B_HHI_MPLL_CNTL4      (MESON_G12B_HHI_BASE + 0x288)
#define MESON_G12B_HHI_MPLL_CNTL5      (MESON_G12B_HHI_BASE + 0x28C)
#define MESON_G12B_HHI_MPLL_CNTL6      (MESON_G12B_HHI_BASE + 0x290)

#define MESON_G12B_AUDIO_BASE          0xFF642000ULL
#define MESON_G12B_AUDIO_SW_RESET      (MESON_G12B_AUDIO_BASE + 0x024)
#define MESON_G12B_AUDIO_ARB_CTRL      (MESON_G12B_AUDIO_BASE + 0x280)

//
// Mali G52 clock.  Same layout as the kernel's g12a clock controller:
// mali_0 select bits [11:9], divider bits [6:0], gate bit 8; mali_1 mirrors
// at [27:25]/[22:16]/24; glitch-free top mux bit 31.  Source index 3 is
// fclk_div2p5 (fixed_pll / 5 = 800 MHz), whose own gate is
// HHI_FIX_PLL_CNTL1 bit 25.
//
#define MESON_G12B_HHI_MALI_CLK_CNTL   (MESON_G12B_HHI_BASE + 0x1B0)
#define MESON_G12B_HHI_FIX_PLL_CNTL1   (MESON_G12B_HHI_BASE + 0x2A4)

#define MESON_G12B_PERIPHS_GPIO_BASE   0xFF634440ULL
#define MESON_G12B_PERIPHS_PULL_BASE   0xFF6344E8ULL
#define MESON_G12B_PERIPHS_PULLEN_BASE 0xFF634520ULL
#define MESON_G12B_PERIPHS_MUX_BASE    0xFF6346C0ULL
#define MESON_G12B_PERIPHS_DS_BASE     0xFF634740ULL

#endif
