/** @file
  Standards-based ACPI namespace for the Khadas VIM3 (Amlogic A311D / G12B).

  The CPU topology (PKG0/CL0x/CPUx) and the xHCI controller (XHC0) describe the
  minimum needed to boot.  The remaining Device() objects mirror the enabled
  leaf peripherals of the board's DeviceTree so an ACPI-mode OS can enumerate
  them.  Each peripheral uses the Linux DT-over-ACPI convention: _HID "PRP0001"
  plus a _DSD "compatible" property, which binds the device to the matching
  DeviceTree driver's of_match_table.

  These entries deliberately carry only MMIO + interrupt(s) + compatible: ACPI
  cannot express the DeviceTree clock/reset/pinctrl phandle graph, so the OS
  drivers still require adjustment (skip clk/reset/pinctrl acquisition) to bind.
  GSIV = DeviceTree "GIC_SPI n" + 32.  All A311D peripherals are non-coherent
  (_CCA Zero).

  POLICY: a device is enabled here (_STA = 0x0F) only once it has been shown to
  work on hardware in ACPI mode.  Everything else is _STA = Zero, with the
  reason recorded above it, and gets re-enabled one at a time as it is
  validated.

  This is not tidiness.  Describing a peripheral whose clock firmware never
  enabled is actively dangerous: probe reaches an MMIO access, the ungated
  peripheral stalls the AXI bus, and the SoC hard-hangs with even SysRq dead.
  That is what SDA0 did the moment meson-gx-mmc could probe far enough to
  touch SD_EMMC_CLOCK.  Under DeviceTree the OS programs the clock tree itself,
  so an unbacked description is harmless there and lethal here - which is why
  these entries can look correct for a long time before biting.

  So: describe only what firmware has actually powered, and only what has been
  observed working.

  Currently enabled and validated:
    PKG0/CL0x/CPUx  CPU topology
    XHC0            xHCI - USB2 + SuperSpeed, boot disk and HID
    ETH0            dwmac - RGMII to the external RTL8211F, ping + ssh proven
    SDC0 / SDB0     eMMC and SD - firmware gates and clocks both (under test)
    GPU0            Mali G52 - MesonGpuDxe clocks it at 800 MHz, panfrost
    UAR0 I2CA(+RTC0/FAN0/EXPD) CTMP DDRT CPUF  - see per-device notes
    GPIP/GPAO       pinctrl gpiochips (patch 0022) + LEDS/LEDR/BTNS consumers
    SARC            SAR ADC (patch 0023) + ADCK adc-keys consumer

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

DefinitionBlock ("Dsdt.aml", "DSDT", 2, "KHADAS", "VIM3ACPI", 1)
{
  Scope (\_SB)
  {
    // Platform-wide _OSC.  Required for CPU deep idle: Linux hard-gates
    // _LPI parsing on the Platform-Coordinated LPI bit (DWORD2 bit 7,
    // ACPI 6.x "Idle State Coordination") coming back SET from BOTH the
    // query and control passes.  Everything else in the OS's capability
    // mask is cleared - this platform supports exactly PC-LPI.
    // The return buffer must be the SAME length as the input.
    Name (SUPP, Zero)   // last OS-offered DWORD2, kept for debugging

    Method (_OSC, 4, Serialized)
    {
      CreateDWordField (Arg3, 0x00, CDW1)
      CreateDWordField (Arg3, 0x04, CDW2)

      If ((Arg0 == ToUUID ("0811b06e-4a27-44f9-8d60-3cbbc22e7b48")))
      {
        If ((Arg1 != One))
        {
          CDW1 |= 0x08          // OSC_INVALID_REVISION_ERROR
          Return (Arg3)
        }

        SUPP = CDW2
        CDW2 &= 0x00000080      // keep only Platform-Coordinated LPI

        If ((CDW2 != SUPP))
        {
          CDW1 |= 0x10          // OSC_CAPABILITIES_MASK_ERROR
        }

        Return (Arg3)
      }

      CDW1 |= 0x04              // OSC_INVALID_UUID_ERROR
      Return (Arg3)
    }

    // Shared _LPI state table, referenced by every CPU's _LPI method.
    //
    // The ASL contract is exacting: 3 header elements + Count state
    // packages, each state package EXACTLY 10 elements (a shorter one
    // makes the kernel read out of bounds).  Element[6] must be a
    // FFixedHW GenericRegister whose Address is the raw PSCI
    // CPU_SUSPEND power_state (bits 63:32 zero, Original StateID
    // format - PSCI_FEATURES(CPU_SUSPEND) on this BL31 returns bit1
    // clear).  Units are microseconds.
    //
    // State 0 (WFI): index 0 never issues PSCI - the cpuidle enter
    //   macro executes cpu_do_idle() - but the state must exist, be
    //   enabled, and be FFH-typed or every entry fails -EINVAL.
    // State 1 (RET, power_state 0x00000000): the ONE retention state
    //   Amlogic BL31 accepts - bench-proven by raw-SMC probe (returned
    //   0 after a 4.1 ms standby; every other retention param returns
    //   INVALID_PARAMETERS).  arch-flags 0 = retention path, no
    //   context save, timer keeps running.
    // State 2 (CPU-PD, power_state 0x00010000): core power-down
    //   EXPERIMENT - StateType bit16 set, arch-flags CORE_CTXT so the
    //   kernel takes the full cpu_suspend()/cpu_resume path and stops
    //   the local timer.  BL31's acceptance is unknown (no OS has ever
    //   used CPU_SUSPEND for idle on G12B): a rejection shows up as a
    //   growing stateN/rejected counter (harmless); acceptance is
    //   validated by usage+time advancing with residency.  REMOVE THIS
    //   STATE if the bench shows hangs.
    Name (LPIP, Package ()
    {
      Zero,                     // [0] Revision
      Zero,                     // [1] Level ID
      3,                        // [2] Count (must equal packages below)

      Package ()
      {
        One,                    // [0] min residency (us)
        One,                    // [1] wake latency (us)
        One,                    // [2] flags: enabled
        Zero,                   // [3] arch context lost flags
        Zero,                   // [4] residency counter frequency
        Zero,                   // [5] enabled parent state
        ResourceTemplate () { Register (FFixedHW, 0x20, 0x00, 0x0000000000000000, 0x03) },
        ResourceTemplate () { Register (SystemMemory, 0x00, 0x00, 0x0000000000000000, 0x00) },
        ResourceTemplate () { Register (SystemMemory, 0x00, 0x00, 0x0000000000000000, 0x00) },
        "WFI"                   // [9] name
      },

      Package ()
      {
        100,                    // min residency 100 us
        25,                     // wake latency 25 us (SMC round trip)
        One,                    // enabled
        Zero,                   // retention: no context lost
        Zero,
        Zero,
        ResourceTemplate () { Register (FFixedHW, 0x20, 0x00, 0x0000000000000000, 0x03) },
        ResourceTemplate () { Register (SystemMemory, 0x00, 0x00, 0x0000000000000000, 0x00) },
        ResourceTemplate () { Register (SystemMemory, 0x00, 0x00, 0x0000000000000000, 0x00) },
        "RET"
      },

      Package ()
      {
        2000,                   // min residency 2 ms - only genuinely idle periods
        800,                    // wake latency estimate
        One,                    // enabled
        One,                    // CPUIDLE_CORE_CTXT: core context lost
        Zero,
        Zero,
        ResourceTemplate () { Register (FFixedHW, 0x20, 0x00, 0x0000000000010000, 0x03) },
        ResourceTemplate () { Register (SystemMemory, 0x00, 0x00, 0x0000000000000000, 0x00) },
        ResourceTemplate () { Register (SystemMemory, 0x00, 0x00, 0x0000000000000000, 0x00) },
        "CPU-PD"
      }
    })

    Device (PKG0)
    {
      Name (_HID, "ACPI0010")
      Name (_UID, Zero)

      Device (CL00)
      {
        Name (_HID, "ACPI0010")
        Name (_UID, One)

        Device (CPU0)
        {
          Name (_HID, "ACPI0007")
          Name (_UID, Zero)
          Name (_STA, 0x0F)
          Method (_LPI) { Return (\_SB.LPIP) }
        }

        Device (CPU1)
        {
          Name (_HID, "ACPI0007")
          Name (_UID, One)
          Name (_STA, 0x0F)
          Method (_LPI) { Return (\_SB.LPIP) }
        }
      }

      Device (CL01)
      {
        Name (_HID, "ACPI0010")
        Name (_UID, 2)

        Device (CPU2)
        {
          Name (_HID, "ACPI0007")
          Name (_UID, 2)
          Name (_STA, 0x0F)
          Method (_LPI) { Return (\_SB.LPIP) }
        }

        Device (CPU3)
        {
          Name (_HID, "ACPI0007")
          Name (_UID, 3)
          Name (_STA, 0x0F)
          Method (_LPI) { Return (\_SB.LPIP) }
        }

        Device (CPU4)
        {
          Name (_HID, "ACPI0007")
          Name (_UID, 4)
          Name (_STA, 0x0F)
          Method (_LPI) { Return (\_SB.LPIP) }
        }

        Device (CPU5)
        {
          Name (_HID, "ACPI0007")
          Name (_UID, 5)
          Name (_STA, 0x0F)
          Method (_LPI) { Return (\_SB.LPIP) }
        }
      }
    }

    Device (XHC0)
    {
      Name (_HID, "PNP0D10")
      Name (_UID, Zero)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF500000, 0x00100000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive)
        {
          62
        }
      })
      // The G12B DWC3 xHCI wedges its command ring under concurrent
      // bulk-stream traffic (UAS sync-write bursts stall completions,
      // Stop Endpoint times out, xhci_hc_died takes down both buses).
      // Root-caused on the bench.  Streams must not
      // be used on this controller; patched xhci-plat (patch 0013)
      // reads this property and sets XHCI_BROKEN_STREAMS, making uas
      // fall back to usb-storage (BOT).
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package (2) { "xhci-broken-streams-quirk", One },
        }
      })
    }

    //
    // ---- Serial / UART ----
    //

    // uart_AO @ 0xff803000 - console ttyAML0 (SPI 193 edge)
    // ENABLED (was _STA Zero: "unvalidated, no SPCR type").
    // The Meson UART still has no SPCR subtype, so the console is selected
    // by console=ttyAML0 on the kernel command line; patch 0020 gives the
    // driver an ACPI probe path (clock-frequency baud reference, _UID as
    // the line number so the name matches DT mode).  Firmware owns the AO
    // pclk - it is firmware's own console - and shares the port: Linux
    // reprograms baud/state at open exactly as it does over DT.
    Device (UAR0)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 0)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF803000, 0x00000018)
        Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 225 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", Package () { "amlogic,meson-g12a-uart", "amlogic,meson-gx-uart", "amlogic,meson-ao-uart" } },
          // Baud reference stated as a firmware fact: the free-running
          // 24 MHz crystal (selects the XTAL divider path in the driver).
          Package () { "clock-frequency", 24000000 },
        }
      })
    }

    // uart_A @ 0xffd24000 - Bluetooth UART (SPI 26 edge)
    // ENABLED: the BCM4359's BT half.  MesonSdMmcDxe muxes
    // GPIOX_12..15 to uart_a and opens the EE UART0 pclk gate (the SDA0-class
    // hazard the old note warned about); the 32.768 kHz LPO is the same
    // pwm_e clock the Wi-Fi sequence already runs.  Validated end-to-end in
    // ACPI mode (2 Mbaud, hardware flow, patchram, A2DP audio) via a
    // runtime SSDT before landing here.
    Device (UARA)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 1)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFFD24000, 0x00000018)
        Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 58 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", Package () { "amlogic,meson-g12a-uart", "amlogic,meson-gx-uart" } },
          // Baud reference: the free-running 24 MHz crystal (same
          // firmware fact as UAR0).
          Package () { "clock-frequency", 24000000 },
          Package () { "uart-has-rtscts", 1 },
        }
      })

      // BCM4359 Bluetooth (AP6398S module), serdev child of uart_A.
      //
      // hci_bcm's ACPI probe maps GPIOs BY INDEX, not by _DSD name
      // (x86 heritage): first GpioIo = device-wakeup, second GpioIo =
      // shutdown, GpioInt = host-wakeup.  Wiring: BT_WAKE = GPIOX_18
      // (line 83), BT_EN = GPIOX_17 (82), HOST_WAKE = GPIOX_19 (84).
      //
      // The UartSerialBusV2 ConnectionSpeed is the INITIAL baud - the
      // chip's ROM listens at 115200; hci_bcm reads it as init_speed.
      // Putting the operating rate here breaks the first exchange
      // (verified live).  The operating rate is the "max-speed"
      // property (needs kernel patch 0031 on the ACPI path).
      Device (BTH0)
      {
        Name (_HID, "PRP0001")
        Name (_UID, 0)
        Name (_CCA, Zero)
        Name (_STA, 0x0F)
        Name (_CRS, ResourceTemplate ()
        {
          UartSerialBusV2 (115200, DataBitsEight, StopBitsOne, 0xC0,
                           LittleEndian, ParityTypeNone, FlowControlHardware,
                           64, 64, "\\_SB.UARA", 0, ResourceConsumer, , Exclusive)
          GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionOutputOnly,
                  "\\_SB.GPIP", 0, ResourceConsumer, , ) { 83 }
          GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionOutputOnly,
                  "\\_SB.GPIP", 0, ResourceConsumer, , ) { 82 }
          GpioInt (Edge, ActiveHigh, ExclusiveAndWake, PullNone, 0,
                   "\\_SB.GPIP", 0, ResourceConsumer, , ) { 84 }
        })
        Name (_DSD, Package ()
        {
          ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
          Package ()
          {
            Package () { "compatible", "brcm,bcm43438-bt" },
            Package () { "max-speed", 2000000 },
          }
        })
      }
    }

    //
    // ---- Ethernet ----
    //

    // ethmac @ 0xff3f0000 + PRG_ETH glue @ 0xff634540 (SPI 8 level; PHY polled)
    Device (ETH0)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 2)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF3F0000, 0x00010000)
        Memory32Fixed (ReadWrite, 0xFF634540, 0x00000008)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 40 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", Package () { "amlogic,meson-g12a-dwmac", "snps,dwmac-3.70a", "snps,dwmac" } },
          Package () { "phy-mode", "rgmii" },
          Package () { "amlogic,tx-delay-ns", 2 },
          Package () { "interrupt-names", Package () { "macirq" } },
        }
      })
    }

    // mdio-multiplexer @ 0xff64c000
    // DISABLED: MDIO mux, for selecting the internal 100M PHY. Never probed anyway
    // ("failed to get peripheral clock"), and ethernet does not need it: the
    // RTL8211F attaches on the stmmac's own bus as PHY [stmmac-2:00].
    Device (EMDX)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 3)
      Name (_CCA, Zero)
      Name (_STA, Zero)   // DISABLED - see policy note at top of scope
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF64C000, 0x000000A4)
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,g12a-mdio-mux" },
        }
      })
    }

    //
    // ---- MMC / SD / eMMC (amlogic,meson-axg-mmc) ----
    //

    // sd_emmc_c @ 0xffe07000 - eMMC (SPI 191 level, 8-bit)
    Device (SDC0)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 4)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFFE07000, 0x00000800)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 223 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,meson-axg-mmc" },
          //
          // FOUR bits, not eight - and this is a board-level exclusion, not a
          // shortfall.  eMMC D4-D7 are the BOOT_4..BOOT_7 pins, which are the
          // same pins the SPI NOR uses (pinctrl groups emmc_nand_d4..d7 vs
          // nor_d/nor_q/nor_c/nor_cs).  Mainline's VIM3 DeviceTree takes the
          // other side of that trade: it sets bus-width = <8> and marks
          // &spifc "disabled".
          //
          // This firmware needs the NOR: MesonSpifcFvbDxe keeps the UEFI
          // variable store there (BootOrder, HwDescription, Secure Boot keys),
          // so the upper four data lines are not ours to take.  Advertising 8
          // here only makes the OS attempt a switch that cannot succeed and
          // then fall back, so state the truth.
          //
          Package () { "bus-width", 4 },
          Package () { "non-removable", 1 },
          Package () { "cap-mmc-highspeed", 1 },
          //
          // Rates of the two SD_EMMC clock mux parents, which an ACPI OS
          // cannot discover for itself: there is no clock provider, so the
          // "clkin0"/"clkin1" references a DeviceTree boot resolves do not
          // exist.  MesonSdMmcDxe programs both and leaves them running -
          // clkin0 is SD_EMMC_C_CLK0 (HHI_NAND_CLK_CNTL = enable, divide by
          // one, mux = crystal) and clkin1 is FCLK_DIV2.  Keep these in step
          // with that driver: the controller's own divider is computed from
          // them, so a wrong value yields an out-of-spec bus clock rather
          // than a clean failure.
          //
          Package () { "clkin0-frequency", 24000000 },
          Package () { "clkin1-frequency", 1000000000 },
          //
          // Without max-frequency the OS leaves mmc_host.f_max at zero, and
          // mmc_set_clock()'s "if (hz > f_max) hz = f_max" then silently
          // programs 0 Hz the moment a transfer speed is selected - card
          // identification succeeds and every command after it times out.
          //
          // 200 MHz matches DeviceTree and is the HS200 rate.  It is
          // deliberately higher than MesonSdMmcDxe's MAX_TRANSFER_CLOCK
          // (50 MHz): that is the firmware's own conservative ceiling for its
          // simple PIO-ish driver, not a limit of the controller.  Raising it
          // is required or HS200 gets clamped straight back to 50 MHz.
          //
          Package () { "max-frequency", 200000000 },
          //
          // The I/O rail (vqmmc) on this board is "emmc_1v8" in DeviceTree:
          // regulator-fixed, 1800000 uV, regulator-always-on.  It cannot be
          // switched and does not need to be - the eMMC is already signalling
          // at 1.8 V.  ACPI has no way to describe a regulator, so state the
          // rail voltage directly and let the driver accept the core's 1.8 V
          // request instead of rejecting it (its default assumption for a
          // regulator-less board is a fixed 3.3 V rail, which is wrong here).
          //
          // This is the I/O rail only.  The card supply (vmmc) is separate and
          // is genuinely 3.3 V - that is what ocr_avail describes.
          //
          Package () { "fixed-signal-voltage-microvolt", 1800000 },
          Package () { "mmc-hs200-1_8v", 1 },
          //
          // mmc-ddr-1_8v is deliberately NOT set: HS200 supersedes it here and
          // adding both only widens what must be re-validated.
          //
        }
      })
    }

    // sd_emmc_b @ 0xffe05000 - SD card slot (SPI 190 level, 4-bit)
    Device (SDB0)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 5)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFFE05000, 0x00000800)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 222 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,meson-axg-mmc" },
          Package () { "bus-width", 4 },
          Package () { "cap-sd-highspeed", 1 },
          // See SDC0: clkin0 here is SD_EMMC_B_CLK0 (HHI_SD_EMMC_CLK_CNTL).
          Package () { "clkin0-frequency", 24000000 },
          Package () { "clkin1-frequency", 1000000000 },
          //
          // Without max-frequency the OS leaves mmc_host.f_max at zero, and
          // mmc_set_clock()'s "if (hz > f_max) hz = f_max" then silently
          // programs 0 Hz the moment a transfer speed is selected - card
          // identification succeeds and every command after it times out.
          // 50 MHz is MAX_TRANSFER_CLOCK in MesonSdMmcDxe, i.e. the fastest
          // rate this controller is known good at on this board; faster modes
          // need 1.8 V signalling, which has no regulator here.
          //
          Package () { "max-frequency", 50000000 },
          //
          // The card-detect GPIO is not describable in ACPI, so ask the MMC
          // core to poll for media instead of waiting for a CD interrupt
          // that can never arrive.
          //
          Package () { "broken-cd", 1 },
        }
      })
    }

    // sd_emmc_a @ 0xffe03000 - SDIO, onboard Wi-Fi (SPI 189 level, 4-bit)
    //
    // ENABLED as of the MesonSdMmcDxe change that powers SD_EMMC_A: its
    // peripheral gate (HHI_GCLK_MPEG0 bit 24), CLK0 (HHI_SD_EMMC_CLK_CNTL
    // bit 7) and reset (RESET_LEVEL1 bit 12) are now all released.  Before
    // that, firmware gated only B and C - and because A and B share
    // HHI_SD_EMMC_CLK_CNTL, writing B alone actively cleared A's clock, so
    // probing this device stalled the AXI bus and hard-hung the SoC.
    // The allow-list precondition (describe only what firmware has powered)
    // is now genuinely satisfied.
    Device (SDA0)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 6)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFFE03000, 0x00000800)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 221 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,meson-axg-mmc" },
          Package () { "bus-width", 4 },
          Package () { "non-removable", 1 },
          // The bounce buffer this quirk selects is inside the controller's
          // own register window, so it needs nothing extra from ACPI.
          Package () { "amlogic,dram-access-quirk", 1 },
          // See SDC0: clkin0 here is SD_EMMC_A_CLK0.
          Package () { "clkin0-frequency", 24000000 },
          Package () { "clkin1-frequency", 1000000000 },
          // Matching DeviceTree's sd_emmc_a: SD high speed, 100 MHz ceiling.
          Package () { "cap-sd-highspeed", 1 },
          Package () { "max-frequency", 100000000 },
          // Wi-Fi firmware needs the card powered across suspend.
          Package () { "keep-power-in-suspend", 1 },
        }
      })
    }

    //
    // ---- GPU ----
    //

    // gpu @ 0xffe40000 - Mali G52 (Bifrost), SPI 162/161/160 level.
    //
    // Enabled only together with MesonGpuDxe, which programs
    // HHI_MALI_CLK_CNTL (mali_0 = fclk_div2p5 = 800 MHz) and releases
    // DVALIN / DVALIN_CAPB3 in RESET_LEVEL0/2 before this device may be
    // described - otherwise panfrost's first register access stalls the AXI
    // bus (the SDA0 hazard, see the policy note above).  Validated live on
    // fw 52055ba: post-capsule ACPI boot reads MALI_CLK 0x00000700 (sel 3,
    // div /1, gate open) and the GPU_ID readback under the bring-up
    // watchdog did not trip.
    //
    // INTERRUPT ORDER IS LOAD-BEARING and mirrors DeviceTree: job, mmu, gpu.
    // panfrost looks each up by NAME (platform_get_irq_byname), which under
    // ACPI resolves through the "interrupt-names" _DSD package to the Nth
    // Interrupt() in this _CRS (fwnode_irq_get_byname); ETH0's "macirq" is
    // the proven precedent on this board.
    //
    // There is no clock provider and no devfreq under ACPI, so the GPU runs
    // permanently at the firmware-programmed rate.  "clock-frequency" states
    // that rate for the driver's fixed-rate stand-in clock (patch 0007);
    // keep it in step with MesonGpuDxe, as SDC0's clkin values are kept in
    // step with MesonSdMmcDxe.
    //
    // No "mali" regulator is described because none exists to describe: the
    // rail is fixed 0.8 V on this board (no amlogic DT sets mali-supply);
    // the patched driver skips regulator acquisition under ACPI.
    //
    // _CCA is ONE - the single exception to this DSDT's all-peripherals-
    // non-coherent rule.  The Mali sits on the ACE-lite IO-coherent port and
    // the DeviceTree marks it dma-coherent; panfrost derives cache
    // attributes, the GPU COHERENCY_ENABLE protocol, and its page-table
    // walk mode from this flag.  Describing it non-coherent makes every job
    // fault DATA_INVALID (GPU parses garbage descriptors) while probe still
    // succeeds - measured live on fw 4d382f6, root-caused by comparing
    // dev->dma_coherent between the working DT boot (1) and ACPI (0).
    Device (GPU0)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 7)
      Name (_CCA, One)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFFE40000, 0x00040000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 194 }  // 0: job (SPI 162)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 193 }  // 1: mmu (SPI 161)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 192 }  // 2: gpu (SPI 160)
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", Package () { "amlogic,meson-g12a-mali", "arm,mali-bifrost" } },
          Package () { "interrupt-names", Package () { "job", "mmu", "gpu" } },
          Package () { "clock-frequency", 800000000 },
        }
      })
    }

    //
    // ---- I2C (amlogic,meson-axg-i2c) ----
    //

    // i2c_AO @ 0xff805000 (SPI 195 edge) - onboard MCU/RTC/GPIO-expander bus.
    //
    // The old "can't get device clock" blocker is fixed in
    // patches/linux/0003: i2c-meson only needs the clock for its RATE (the SCL
    // divider), so the driver synthesises a fixed-rate parent from
    // clkin-frequency below.  No firmware clock or pinmux work was needed -
    // BL2 already enables every gate in HHI_GCLK_MPEG0 (it reads all-ones,
    // verified against structured registers in the same page) and already muxes
    // GPIOAO_2/GPIOAO_3 to i2c_ao_sck/sda (function 1).
    //
    // Slaves are child devices, so adding the RTC (hym8563 @0x51) or the GPIO
    // expander (tca6408 @0x20) later is one more Device() block and nothing
    // else.  They are omitted for now per the validate-one-at-a-time policy.
    Device (I2CA)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 12)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF805000, 0x00000020)
        Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 227 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,meson-axg-i2c" },
          // clk81.  Measured, not assumed: a DT boot programs PWM_AO_D with
          // clk_sel=1 and 208 counts for a 1250 ns period, i.e. 166.4 MHz.
          Package () { "clkin-frequency", 166666666 },
        }
      })

      // The Khadas MCU is deliberately NOT described as a PRP0001 device.
      //
      // Binding khadas-mcu would create a second cooling device for the same
      // fan, and its child khadas_mcu_fan registers that cooling device with
      // devm on dev->PARENT (the MFD), not on its own platform device - so
      // unloading the fan module never unregisters it and the thermal core is
      // left calling into freed code.  That is a use-after-free on any VIM3,
      // DeviceTree or ACPI; it oopsed this board in step_wise_manage() and in
      // cur_state_show().  FAN0 below drives register 0x88 directly, so the
      // driver is not needed and neither is that failure mode.
      //
      // The MCU's other functions (power off at 0x80, USB3/PCIe mux at 0x33,
      // wake sources) are owned by firmware's own Vim3McuDxe, not by Linux.

      // Haoyu HYM8563 - the board's battery-backed real-time clock.
      //
      // No interrupt and no wakeup-source, matching DeviceTree, which declares
      // neither: the alarm line is not wired on this board, so the driver takes
      // its client->irq == 0 path and provides plain timekeeping.  DT also
      // carries #clock-cells for the CLKOUT pin; that is omitted here because
      // nothing in ACPI can consume it and the driver's clkout provider
      // registration is a no-op without an OF node.
      //
      // rtc-hym8563 needs no patch: its only firmware lookups are
      // device_property_read_bool() and of_clk_add_provider(), which is
      // explicitly NULL-safe.
      Device (RTC0)
      {
        Name (_HID, "PRP0001")
        Name (_UID, 24)
        Name (_CCA, Zero)
        Name (_STA, 0x0F)
        Name (_CRS, ResourceTemplate ()
        {
          I2cSerialBusV2 (0x0051, ControllerInitiated, 100000,
                          AddressingMode7Bit, "\\_SB.I2CA",
                          0x00, ResourceConsumer, , Exclusive,)
        })
        Name (_DSD, Package ()
        {
          ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
          Package ()
          {
            Package () { "compatible", "haoyu,hym8563" },
          }
        })
      }

      // Fan, as a standard ACPI 4.0 fine-grained fan device rather than a
      // DeviceTree-compat shim.  Linux's acpi_fan binds PNP0C0B and registers a
      // thermal cooling device named "Fan", which amlogic_thermal's should_bind
      // attaches to the active trip at 80 C - the same governor, trip and
      // hysteresis khadas-mcu-fan used, so the ramp behaviour is unchanged.
      // The knowledge that the fan is MCU register 0x88 with four levels moves
      // out of a kernel driver and into here, which is the point.
      //
      // MUST BE A CHILD OF I2CA.  Linux installs the GenericSerialBus handler
      // on the controller's handle, and ACPICA resolves a region's handler by
      // walking UP from the region to an ancestor.  Declared as a sibling of
      // I2CA the handler never applies and every access fails.
      //
      // NOTE ON _FPS ORDERING - load-bearing.  Linux sorts _FPS by the *speed*
      // field (acpi_fan_speed_cmp) and then maps cooling state N to
      // fps[N].control.  sort() is not stable, so equal speeds would make the
      // state->level mapping arbitrary.  The speeds below must therefore be
      // strictly increasing.  Level 0 really is 0 (fan off); 1000/2000/3000 are
      // ORDERING KEYS, not measured RPM - CMD_FAN_STATUS_CTRL is write-only
      // levels 0-3 with no tachometer, so real speeds are not knowable.  Noise
      // and power are reported unknown rather than invented.
      Device (FAN0)
      {
        Name (_HID, "PNP0C0B")
        Name (_UID, 25)
        Name (_STA, 0x0F)

        // Revision, FineGrainControl (0 = discrete states), StepSize,
        // LowSpeedNotificationSupport
        Name (_FIF, Package () { 0, 0, 0, 0 })

        // Revision, then { Control, TripPoint, Speed, NoiseLevel, Power }
        Name (_FPS, Package ()
        {
          0,
          Package () { 0, 0xFFFFFFFF,    0, 0xFFFFFFFF, 0xFFFFFFFF },
          Package () { 1, 0xFFFFFFFF, 1000, 0xFFFFFFFF, 0xFFFFFFFF },
          Package () { 2, 0xFFFFFFFF, 2000, 0xFFFFFFFF, 0xFFFFFFFF },
          Package () { 3, 0xFFFFFFFF, 3000, 0xFFFFFFFF, 0xFFFFFFFF }
        })

        // The offset within a GenericSerialBus field is the command value, so
        // this addresses MCU register 0x88 (CMD_FAN_STATUS_CTRL).
        OperationRegion (MCUR, GenericSerialBus, 0x00, 0x100)
        Field (MCUR, BufferAcc, NoLock, Preserve)
        {
          Connection (I2cSerialBusV2 (0x0018, ControllerInitiated, 100000,
                      AddressingMode7Bit, "\\_SB.I2CA",
                      0x00, ResourceConsumer, , Exclusive,)),
          AccessAs (BufferAcc, AttribByte),
          Offset (0x88),
          FANL, 8
        }

        // The region is only serviceable once the OS has installed its
        // GenericSerialBus handler, which happens when i2c-meson registers the
        // adapter - well after acpi_fan probes.  _DEP does NOT solve this:
        // Linux only enforces _DEP for an allow-list of supplier HIDs
        // (acpi_honor_dep_ids in drivers/acpi/scan.c), and PRP0001 is not on
        // it, so the dependency is recorded and then ignored.  _REG is the
        // mechanism that actually works: the OS calls it on handler install.
        Name (RGRD, Zero)   // region serviceable?
        Name (FLVL, Zero)   // last level the OS asked for

        // MUST NOT touch the bus.  The OS calls _REG from inside
        // i2c_register_adapter(), before the adapter is fully registered, so a
        // transfer here re-enters the i2c core on a half-built adapter.  Doing
        // that wedged the board: the kernel booted normally and then systemd
        // took SIGILL and froze.  Set the flag and nothing else - the thermal
        // governor re-issues _FSL on its next poll, so no fan state is lost.
        Method (_REG, 2, NotSerialized)
        {
          If ((Arg0 == 0x09))   // GenericSerialBus
          {
            RGRD = Arg1
          }
        }

        // GenericSerialBus buffers are { Status, Length, Data... }.
        Method (FSET, 1, Serialized)
        {
          If ((RGRD == Zero))
          {
            Return (Zero)
          }

          Name (FBUF, Buffer (3) { 0x00, 0x01, 0x00 })
          CreateByteField (FBUF, 0x02, FDAT)
          FDAT = Arg0
          FANL = FBUF
          Return (Zero)
        }

        Method (_FSL, 1, Serialized)
        {
          FLVL = Arg0
          FSET (Arg0)
        }

        // Reports the last commanded level rather than reading the chip.  The
        // MCU's fan register is effectively write-only - a read returns 0xFF,
        // which matches no _FPS entry and makes Linux fail cur_state with
        // -EINVAL.  The in-tree khadas_mcu_fan driver does the same thing,
        // caching ctx->level instead of reading back.  A happy consequence is
        // that _FST never touches the bus at all; only _FSL does.
        Method (_FST, 0, Serialized)
        {
          Return (Package () { 0, FLVL, 0xFFFFFFFF })
        }
      }

      // TCA6408 GPIO expander @0x20 - drives the red status LED (pin 5).
      //
      // Binds gpio-pca953x with no patch: matching happens through the
      // _DSD compatible (acpi_of_match_device), and the driver data comes
      // from the "tca6408" i2c_device_id fallback in i2c_get_match_data()
      // - the ACPI client is named after the compatible's suffix.  No IRQ
      // is described (the INT line is not wired to anything we use), so
      // the driver takes its no-irq path.  LEDR below consumes pin 5 by
      // GpioIo reference; the other expander pins stay untouched.
      Device (EXPD)
      {
        Name (_HID, "PRP0001")
        Name (_UID, 31)
        Name (_CCA, Zero)
        Name (_STA, 0x0F)
        Name (_CRS, ResourceTemplate ()
        {
          I2cSerialBusV2 (0x0020, ControllerInitiated, 100000,
                          AddressingMode7Bit, "\\_SB.I2CA",
                          0x00, ResourceConsumer, , Exclusive,)
        })
        Name (_DSD, Package ()
        {
          ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
          Package ()
          {
            Package () { "compatible", "ti,tca6408" },
          }
        })
      }
    }

    // i2c3 @ 0xffd1c000 (SPI 39 edge) - 40-pin header
    // DISABLED: Same as I2CA - i2c-meson cannot probe without the clock framework.
    Device (I2C3)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 13)
      Name (_CCA, Zero)
      Name (_STA, Zero)   // DISABLED - see policy note at top of scope
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFFD1C000, 0x00000020)
        Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 71 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,meson-axg-i2c" },
        }
      })
    }

    //
    // ---- Watchdog / RTC / ADC / RNG / thermal / IR / CEC / PWM ----
    //

    // watchdog @ 0xffd0f0d0
    // DISABLED: meson-gxbb-wdt requires a clock handle. Never probed.
    Device (WDT0)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 14)
      Name (_CCA, Zero)
      Name (_STA, Zero)   // DISABLED - see policy note at top of scope
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFFD0F0D0, 0x00000010)
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,meson-gxbb-wdt" },
        }
      })
    }

    // vrtc @ 0xff8000a8
    // DISABLED: Believed to bind, but not validated. Re-enable early - it is harmless
    // and useful.
    Device (VRTC)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 15)
      Name (_CCA, Zero)
      Name (_STA, Zero)   // DISABLED - see policy note at top of scope
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF8000A8, 0x00000004)
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,meson-vrtc" },
        }
      })
    }

    // saradc @ 0xff809000 (SPI 200 edge)
    //
    // ENABLED with patches/linux/0023.  Resource order is the driver ABI:
    // [0] the SAR ADC block, [1] AO_CLK_GATE0 (pclk gate bit 8), [2]
    // AO_SAR_CLK (converter mux[10:9]/div[7:0]/gate[8]) - the patched
    // driver rebuilds DT's AO clock chain as a local island over [1]+[2],
    // mapped WITHOUT claiming (both registers sit inside VDEC's AO-sysctrl
    // window).
    //
    // "#io-channel-cells" is LOAD-BEARING for consumers: without it the
    // ACPI reference parser consumes zero args and an "io-channels"
    // reference silently resolves to channel 0 instead of the stated
    // channel (acpi_fwnode_get_args_count returns 0 for a missing
    // property instead of erroring like DT).
    //
    // "vref-microvolt" replaces DT's vref-supply phandle: the reference
    // is VDDAO_1V8, a fixed always-on rail, so the fact is stated for
    // the IIO scale instead of describing a regulator ACPI cannot
    // express.
    Device (SARC)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 16)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF809000, 0x00000048)   // 0: SAR ADC
        Memory32Fixed (ReadWrite, 0xFF80004C, 0x00000004)   // 1: AO_CLK_GATE0
        Memory32Fixed (ReadWrite, 0xFF800090, 0x00000004)   // 2: AO_SAR_CLK
        Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 232 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", Package () { "amlogic,meson-g12a-saradc", "amlogic,meson-saradc" } },
          Package () { "#io-channel-cells", 1 },
          Package () { "clock-frequency", 24000000 },
          Package () { "vref-microvolt", 1800000 },
        }
      })
    }

    // Function button - a resistor ladder on SAR ADC channel 2 (adc-keys).
    // The io-channels reference MUST be the inner-Package form with the
    // channel index as a trailing integer; a bare reference would force
    // zero args.  No poll-interval, matching DT (driver default 500 ms).
    // Physical-press validation needs a finger on the bench; the idle
    // state is deterministic (channel 2 reads full-scale 1.8 V, above
    // the 1.71 V keyup threshold).
    Device (ADCK)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 32)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "adc-keys" },
          Package () { "io-channels", Package () { ^SARC, 2 } },
          Package () { "io-channel-names", Package () { "buttons" } },
          Package () { "keyup-threshold-microvolt", 1710000 },
        },
        ToUUID ("dbb8e3e6-5886-4ba6-8795-1319f52a966b"),
        Package ()
        {
          Package () { "button-function", "BTNF" },
        }
      })
      Name (BTNF, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "press-threshold-microvolt", 10000 },
          Package () { "linux,code", 0x1D0 },                // KEY_FN
        }
      })
    }

    // hwrng @ 0xff630218
    // DISABLED: Believed to bind, but not validated. Re-enable early - harmless.
    Device (RNG0)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 17)
      Name (_CCA, Zero)
      Name (_STA, Zero)   // DISABLED - see policy note at top of scope
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF630218, 0x00000004)
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,meson-rng" },
        }
      })
    }

    // cpu_temp @ 0xff634800 (SPI 35 edge)
    //
    // ENABLED.  Firmware gates the sensor on via HHI_TS_CLK_CNTL, so the
    // hardware is genuinely powered - the allow-list precondition holds.
    //
    // Requires the matching amlogic_thermal patch in patches/linux/.  Without
    // it this device is NOT safe to enable: the stock driver takes its match
    // data from of_device_get_match_data(), which returns NULL under PRP0001,
    // and dereferences it without a check - a NULL-pointer Oops *inside*
    // probe, which deadlocks all module loading because the module lock is
    // held across probe.
    //
    // Two resources, and the order is load-bearing: the driver maps resource
    // 0 as the tsensor and resource 1 as the AO-secure block.  AO-secure holds
    // the eFuse calibration word (offset 0x128 for g12a-cpu) that DeviceTree
    // reaches through an "amlogic,ao-secure" syscon phandle - which ACPI
    // cannot express, so it is passed as plain MMIO instead.
    // aobus is at 0xff800000 and sec_AO sits at +0x140, size 0x140.
    Device (CTMP)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 18)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF634800, 0x00000050)   // 0: tsensor
        Memory32Fixed (ReadWrite, 0xFF800140, 0x00000140)   // 1: AO-secure
        Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 67 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", Package () { "amlogic,g12a-cpu-thermal", "amlogic,g12a-thermal" } },
        }
      })
    }

    // ddr_temp @ 0xff634c00 (SPI 36 edge)
    //
    // ENABLED.  Same amlogic_thermal driver and the same two-resource ABI as
    // CTMP; only the compatible (and with it the eFuse trim offset, 0xf0)
    // differs.  The patched driver takes the zone name and trip table from
    // its match data: "ddr-thermal", passive 85 C / critical 110 C, mirroring
    // ddr_thermal in meson-g12-common.dtsi.  The DT zone's passive cooling
    // map targets the GPU devfreq cooling device, which does not exist under
    // ACPI, so this zone monitors and provides the critical shutdown only -
    // the driver's should_bind refuses every cooling device for it.  Both
    // sensors share CLKID_TS, which firmware already gates on for CTMP.
    Device (DDRT)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 19)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF634C00, 0x00000050)   // 0: tsensor
        Memory32Fixed (ReadWrite, 0xFF800140, 0x00000140)   // 1: AO-secure
        Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 68 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", Package () { "amlogic,g12a-ddr-thermal", "amlogic,g12a-thermal" } },
        }
      })
    }

    // periphs pinctrl, gpio bank @ 0xff634440
    //
    // ENABLED with patches/linux/0022.  The DT layout is a parent pinctrl
    // node with a gpio-controller child holding named reg windows; ACPI has
    // no child node, so the patched driver takes the windows from this
    // device's own _CRS and maps names to indexes through the "reg-names"
    // _DSD string array - same contract as the DT child's reg-names.
    //
    // The windows are mapped WITHOUT request_mem_region under ACPI: firmware
    // assigns single registers inside such windows to other devices where a
    // DT phandle cannot be expressed (see GPAO/CPUF below), so a claim by
    // whichever driver probes second would fail.  Every runtime consumer
    // RMWs disjoint registers.
    //
    // No GPIO interrupts: the meson GPIO irqchip is a separate OF-only
    // module the gpiochip does not depend on, and no described consumer
    // uses gpiod_to_irq (the power button is gpio-keys-POLLED).
    Device (GPIP)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 26)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF634440, 0x0000004C)   // 0: gpio
        Memory32Fixed (ReadWrite, 0xFF6344E8, 0x00000018)   // 1: pull
        Memory32Fixed (ReadWrite, 0xFF634520, 0x00000018)   // 2: pull-enable
        Memory32Fixed (ReadWrite, 0xFF6346C0, 0x00000040)   // 3: mux
        Memory32Fixed (ReadWrite, 0xFF634740, 0x0000001C)   // 4: ds
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,meson-g12a-periphs-pinctrl" },
          Package () { "reg-names", Package () { "gpio", "pull", "pull-enable", "mux", "ds" } },
        }
      })
    }

    // AO pinctrl, gpio bank @ 0xff800024 (15 lines: GPIOAO_0-11 + GPIOE_0-2)
    //
    // Register-sharing note (the reason for map-without-claim): the mux
    // window 0xff800014-0x1b contains AO pinmux reg1 (0xff800018), which is
    // also resource 2 of CPUF - g12b-cpufreq RMWs it at runtime for the
    // VDDCPU_A PWM pads (GPIOE_1/2).  The pinctrl driver only ever RMWs the
    // mux bits of pins that get requested, and the only described consumers
    // are GPIOAO_4 (white LED) and GPIOAO_7 (power button), both in reg0
    // (0xff800014) - disjoint from CPUF's register.  VDEC's AO-sysctrl
    // window also covers this range but touches only SLEEP0/ISO0
    // (0xe8/0xec).  Do not describe consumers on GPIOE pins.
    Device (GPAO)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 27)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF800014, 0x00000008)   // 0: mux
        Memory32Fixed (ReadWrite, 0xFF80001C, 0x00000008)   // 1: ds
        Memory32Fixed (ReadWrite, 0xFF800024, 0x00000014)   // 2: gpio
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,meson-g12a-aobus-pinctrl" },
          Package () { "reg-names", Package () { "mux", "ds", "gpio" } },
        }
      })
    }

    // White status LED - GPIOAO_4, active high (meson-khadas-vim3.dtsi leds/
    // led-white).  gpio-leds binds via PRP0001; each LED is a hierarchical
    // _DSD data subnode whose "gpios" package points back at THIS device
    // (the one whose _CRS holds the GpioIo), per the ACPI _DSD GPIO format:
    // { ref, _CRS GpioIo index, pin index within it, active-low }.
    // "color" 0 (white) + "function" "status" compose the same "white:status"
    // name DT derives.  The red LED is a SEPARATE device (LEDR) so a
    // gpio-pca953x probe failure cannot take the white LED down with it
    // (leds-gpio fails the whole probe if any child's GPIO is unavailable).
    //
    // GpioIo fields that are load-bearing: IoRestrictionOutputOnly is safe
    // here only because leds-gpio forces the direction itself; PullNone
    // matches DT (no bias is configured on these pins); DebounceTimeout
    // must be 0 - the meson gpiochip has no debounce support and every
    // non-zero value would warn per line.
    Device (LEDS)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 28)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionOutputOnly,
                "\\_SB.GPAO", 0, ResourceConsumer, ,) { 4 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "gpio-leds" },
        },
        ToUUID ("dbb8e3e6-5886-4ba6-8795-1319f52a966b"),
        Package ()
        {
          Package () { "led-white", "LEDW" },
        }
      })
      Name (LEDW, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "gpios", Package () { ^LEDS, 0, 0, 0 } },
          Package () { "color", 0 },                            // LED_COLOR_ID_WHITE
          Package () { "function", "status" },
          Package () { "linux,default-trigger", "heartbeat" },
        }
      })
    }

    // Red status LED - TCA6408 expander pin 5, active high (led-red in DT).
    // Separate gpio-leds instance; the GpioIo references the expander's
    // gpiochip, which gpiolib resolves once gpio-pca953x binds EXPD
    // (-EPROBE_DEFER until then).
    Device (LEDR)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 30)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionOutputOnly,
                "\\_SB.I2CA.EXPD", 0, ResourceConsumer, ,) { 5 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "gpio-leds" },
        },
        ToUUID ("dbb8e3e6-5886-4ba6-8795-1319f52a966b"),
        Package ()
        {
          Package () { "led-red", "LRED" },
        }
      })
      Name (LRED, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "gpios", Package () { ^LEDR, 0, 0, 0 } },
          Package () { "color", 1 },                            // LED_COLOR_ID_RED
          Package () { "function", "status" },
        }
      })
    }

    // Power button - GPIOAO_7, active low, gpio-keys-polled (DT poll-interval
    // 100 ms, KEY_POWER).  IoRestrictionInputOnly is load-bearing: firmware
    // io-restriction OVERRIDES the driver's requested direction
    // (gpiolib-acpi), and gpio-keys-polled never calls
    // gpiod_direction_input() afterwards - OutputOnly here would leave the
    // pin driving and the button reading garbage.  "wakeup-source" must NOT
    // be added: the polled driver hard-rejects it.
    Device (BTNS)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 29)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionInputOnly,
                "\\_SB.GPAO", 0, ResourceConsumer, ,) { 7 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "gpio-keys-polled" },
          Package () { "poll-interval", 100 },
        },
        ToUUID ("dbb8e3e6-5886-4ba6-8795-1319f52a966b"),
        Package ()
        {
          Package () { "power-button", "BTNP" },
        }
      })
      Name (BTNP, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "gpios", Package () { ^BTNS, 0, 0, 1 } },  // active low
          Package () { "linux,code", 116 },                      // KEY_POWER
          Package () { "label", "power" },
        }
      })
    }

    // ir @ 0xff808000 (SPI 196 edge)
    // DISABLED: IR receiver. Unvalidated; not needed.
    Device (IR00)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 20)
      Name (_CCA, Zero)
      Name (_STA, Zero)   // DISABLED - see policy note at top of scope
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF808000, 0x00000020)
        Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 228 }
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,meson-gxbb-ir" },
        }
      })
    }

    // cecb_AO: lives in SsdtDisplay.asl, not here.  CEC
    // references \_SB.HDMI for its cec-notifier linkage and is
    // meaningless without a display sink, so the device lives in the
    // display-gated conditional SSDT - a static-DSDT reference to a
    // conditionally-installed device would break headless boots.

    // CPU frequency scaling for the A73 (big) cluster.
    //
    // There is no stock ACPI path for this on arm64: cpufreq-dt needs the
    // clock/regulator/pinctrl phandle graph, and cppc_cpufreq needs a PCC
    // mailbox serviced by an always-on agent this SoC does not have (we emit
    // no PCCT and no _CPC).  So patches/linux/g12b-cpufreq drives the three
    // register blocks below directly, and this device exists purely to hand it
    // those regions and let it bind.
    //
    // Without it the A73 cluster is stuck at 1000 MHz - 45% of its 2208 MHz
    // rating - because firmware leaves HHI_SYS_CPUB_CLK_CNTL bit 11 clear.
    // That bit cannot be set here: it lives in the A73 cluster power domain,
    // which is off at UEFI time, so the write is lost when PSCI powers the
    // cores on.  It has to be done by the OS.
    //
    // The A53 cluster is described too.  It is not stuck the same way - it does
    // run from its PLL at 1200 MHz - but firmware leaves VDDCPU_B at ~723 mV
    // while that OPP wants 781 mV, so it boots undervolted by ~58 mV.
    //
    // Note the clusters are CROSSED relative to their names: the A73 (big)
    // cluster runs off sys_pll and the A53 (little) off sys1_pll.
    //
    // RESOURCE ORDER IS LOAD-BEARING - the driver indexes them (as CTMP does).
    // Resource 3 is optional: a driver built before it still drives the A73.
    // The AO window is deliberately just the one pinmux register rather than
    // the whole 0xFF800000 page, so it cannot collide with VRTC at 0xFF8000A8
    // when that is re-enabled.  That single register carries both rails' pins:
    // GPIOE_1 = VDDCPU_B (already muxed by firmware), GPIOE_2 = VDDCPU_A.
    Device (CPUF)
    {
      Name (_HID, "PRP0001")
      Name (_UID, 22)
      Name (_CCA, Zero)
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0xFF63C000, 0x00000400)   // 0: HHI - both PLLs + both cluster muxes
        Memory32Fixed (ReadWrite, 0xFFD1B000, 0x00000020)   // 1: PWM_AB - ch A = VDDCPU_A (A73)
        Memory32Fixed (ReadWrite, 0xFF800018, 0x00000004)   // 2: AO pinmux reg1 - GPIOE_1/GPIOE_2
        Memory32Fixed (ReadWrite, 0xFF802000, 0x00000020)   // 3: PWM_AO_CD - ch D = VDDCPU_B (A53)
      })
      Name (_DSD, Package ()
      {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
          Package () { "compatible", "amlogic,g12b-cpufreq" },
        }
      })
    }
  }
}
