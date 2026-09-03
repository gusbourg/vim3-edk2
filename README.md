# Khadas VIM3 EDK2

UEFI firmware for the Khadas VIM3 Pro (Amlogic A311D, G12B). EDK2 replaces
U-Boot as BL33; the vendor BL2/DDR, BL30 and BL31 firmware is retained.

The board boots Linux as an ordinary UEFI machine — GRUB or the EFI stub from
an ESP, EFI variables, capsule update, SMBIOS — and can present its hardware
to the OS as **either a DeviceTree or a full ACPI description**, selectable
from the setup menu.

## Read this first

**This is a hobby port with very limited testing.** It has been exercised by
one person, on one board (a VIM3 Pro), against a handful of Linux
distributions. It is not a product, it has not been tested broadly, and it
has no meaningful user base to have shaken bugs out of it. Whole classes of
configuration — other VIM3 variants, other peripherals, other operating
systems, anything Windows — are simply unknown rather than known-good.

Installing this replaces your bootloader. Read the recovery section below
*before* you write anything to the board, and make sure you can get back.

The VIM3's BootROM can always be reached over USB, which is what makes this
relatively safe to experiment with — but "relatively" is doing real work in
that sentence.

## What works

Exercised in both description modes: boot from eMMC/SD/USB/SPI, eMMC HS200,
SD, SDIO Wi-Fi, Ethernet, USB2, USB3 (mass storage — see below), PCIe/NVMe,
Mali G52 GPU, meson-drm KMS to 4K, hardware video decode (H.264, HEVC 8/10-bit,
VP9, MPEG1/2), HDMI audio, HDMI CEC, GPIO, LEDs, buttons, SARADC, I²C, RTC,
fan, CPU and DDR thermal, CPU DVFS, UART console, EFI runtime services, EFI
variables in SPI NOR, and capsule update.

ACPI mode additionally provides CPU deep idle (`_LPI`), which DeviceTree mode
cannot — no upstream Amlogic DT describes idle states on this SoC.

### Known limitations

**USB3 is mass-storage-only (BOT, not UAS).** The G12 DWC3 xHCI wedges its
command ring under concurrent bulk-stream traffic: UAS sync-write bursts
stall completions, Stop Endpoint times out, and `xhci_hc_died` takes down
both buses. We believe this is a defect in the SoC's stream engine rather
than anything firmware does — it reproduces in DeviceTree mode on a stock
kernel too — but we have no vendor confirmation, so treat that as our
best reading of the evidence and not established fact. The ACPI description
advertises `xhci-broken-streams-quirk`; patch `0013` reads it and sets
`XHCI_BROKEN_STREAMS`, so `uas` declines the device and `usb-storage` takes
it. In DeviceTree mode the same quirk is applied from the Amlogic glue
compatible.

Also: USB-C is left in host mode under ACPI. CVBS, DSI and suspend/resume are
not implemented and are not planned.

## Hardware description: ACPI or DeviceTree

Set from the firmware setup menu, or via the `HwDescription` EFI variable
(`0` = DeviceTree, `1` = ACPI). **DeviceTree is the default.**

Only one description is ever published — in ACPI mode the DeviceTree
configuration table is withheld, because an arm64 kernel offered both will
silently prefer DT. This also makes the firmware EBBR-conformant, which
requires exactly one.

| | DeviceTree | ACPI |
|---|---|---|
| Kernel | **Stock, unmodified** | **Requires the patches in `patches/linux/`** |
| Status | Default | Out-of-tree, more thinly tested |

**ACPI mode does not work on an unpatched kernel.** This is not a bug to be
reported — mainline has no ACPI support for any of this hardware, and the
firmware cannot supply it. If you boot ACPI mode on a stock kernel you will
get a machine with no storage, no display and no network. Use DeviceTree mode
unless you specifically want to work on the ACPI side.

Every ACPI patch is gated on `has_acpi_companion()` or the absence of
`dev->of_node`, so the DeviceTree paths stay byte-identical and applying the
series cannot regress DT mode.

## Building

Requires an EDK2 build environment and an aarch64 cross toolchain.

```sh
git submodule update --init --recursive
./scripts/build.sh RELEASE
./scripts/package-fip.sh RELEASE
```

These are **one step** — packaging alone will ship a stale firmware volume.
Use `DEBUG` in place of `RELEASE` for a debug build.

Output lands in `out/RELEASE/`:

| File | What it is |
|---|---|
| `VIM3_EFI.fd` | flat 4 MiB EDK2 BL33 |
| `fip/u-boot.bin.sd.bin` | wrapped Amlogic SD/eMMC boot image |
| `fip/u-boot.bin.sd.emmc.bin` | sector-padded boot-partition image |
| `fip/u-boot.bin` | intermediate unwrapped payload — **do not write to boot media** |

That last warning is load-bearing: the BootROM rejects the unwrapped payload
with `CHK:1F`. The `.sd.bin` suffix marks the wrapped image, and packaging
verifies the FIP-table signature at offset `0x10210`.

Inputs are pinned to EDK2 `edk2-stable202605` and LibreELEC
`amlogic-boot-fip` commit `42d3721`. Packaging refuses mismatched revisions
or altered vendor-blob hashes. Because `aml_encrypt_g12b` produces randomised
encrypted wrappers, a byte-identical FD does not reproduce the wrapper hash;
each is recorded in `SHA256SUMS` instead.

`build.sh` applies `patches/firmware/` into the pinned EDK2 and edk2-platforms
submodules and **leaves them applied**, recording a stamp; the trees are
re-synced only when a patch file changes or the patches are found missing.
A dirty submodule after a build is therefore expected, not a problem. Patches
may only modify tracked files — a guard enforces this, because re-sync could
not undo an added or deleted file.

## Installing

Boot media, recovery and promotion are guarded by scripts under `scripts/`;
each verifies the board, the target device identity and the candidate hash
before writing. **Read the script before running it.** Scripts that act on a
board over SSH require `VIM3_SSH_HOST=user@board-ip` in the environment and
refuse to run without it.

- `flash-vim3-sd.sh` — SD, using the Amlogic MBR write split
- `promote-vim3-emmc-boot-partitions.sh` — eMMC boot0/boot1, hash-checked;
  never opens the eMMC user area for writing
- `promote-vim3-spi.sh` / `spi-backup.sh` / `spi-restore.sh` /
  `provision-nor.sh` — SPI NOR maintenance
- `stage-spi-maintenance-boot.sh` — arms a one-shot boot with `spifc` enabled,
  needed before SPI NOR is visible as an mtd device
- `gate-emmc-boot-partitions.sh` — BootROM gate check

Capsule update is supported via `Vim3FmpDxe`.

## Recovery

The VIM3 can always be returned to USB MaskROM mode. With a data-capable USB-C
cable to the host, **press the Function button three times in quick
succession** — three fast presses within about two seconds. Do not hold it.
Confirm on the host:

```sh
lsusb -d 1b8e:c003        # Amlogic GX-CHIP
```

If that device does not appear, the board is not in MaskROM — try the three
presses again, faster. A charge-only USB-C cable is the other common cause.

A candidate can then be tested from RAM with no persistent write at all:

```sh
sudo env PYTHONPATH="$PWD/third_party/pyamlboot" \
  python3 third_party/pyamlboot/boot-g12.py --timeout 10 \
  out/RELEASE/fip/u-boot.bin
```

A successful transfer ends with `[BL2 END]`, and the board comes up in the UEFI
boot manager. Note this takes the **unwrapped** `u-boot.bin`, not `.sd.bin`.
Power-cycle to get your previous bootloader back. **Do this before writing
anything** — if the firmware runs from RAM it will run from flash.

This is the reason bricking is difficult: the BootROM path does not depend on
anything you can overwrite. `recover-vim3-uboot.sh` will write a known-good
vendor U-Boot back to eMMC via Khadas' flash tool; pass it the image path, as
the pinned image is not redistributed here.

## Patches

`patches/` is not firmware — it is what the OS side needs.

### `patches/firmware/` — applied to EDK2 at build time

| Patch | What it does |
|---|---|
| `edk2/0001` | `MmcDxe`: fall back to a 4-bit eMMC bus. The VIM3 reuses eMMC D4–D7 for the SPI NOR that holds UEFI variables, so 8-bit is not available and every timing mode was being rejected. |
| `edk2/0002` | `UsbBusDxe`: retry enumeration for slow or unusual devices. **Not our work** — by Jared Pan (Dell). |
| `edk2/0003` | `RngDxe`: reject the raw algorithm when unavailable rather than reporting success. |
| `edk2/0004` | `UiApp`: show both CPU clusters on the front page. |
| `edk2-platforms/0001` | `DwEmac`: support generic Clause 22 PHYs. |
| `edk2-platforms/0002` | `DwEmac`: use the normal descriptor layout. |

### `patches/linux/` — required for ACPI mode

42 patches against 7.0.x, with 37 of them rebased for 6.18.x under
`patches/linux/6.18/`. Broadly:

**ACPI enablement (PRP0001 probe support).** Each PRP0001 device the firmware
publishes needs its driver taught to probe without DeviceTree. Where a DT
phandle cannot be expressed, the firmware pre-configures the hardware and
states the facts as `_DSD` scalars.

| Patch | Driver / device |
|---|---|
| `0001` | mmc meson-gx — eMMC, SD, SDIO |
| `0002` | thermal amlogic — CPU and DDR zones |
| `0003` | i2c meson — plus RTC and fan children |
| `0006` | net stmmac — Ethernet. **Not our work** — Armbian's Phytium ACPI glue, by Ricardo Pardini |
| `0007` | drm/panfrost — Mali G52 |
| `0008` | drm/meson — KMS master |
| `0009` | drm/meson encoder-hdmi — bridge attach and CEC notifier |
| `0010`, `0011` | drm dw-hdmi glue and core — optional clocks, bridge accessor |
| `0012` | soc/amlogic meson-canvas |
| `0014`–`0018` | clk and ASoC: axg-audio, axg-fifo, axg-tdm-formatter, axg-tdm-interface, g12a-tohdmitx |
| `0019` | media/cec meson ao-cec-g12a |
| `0020` | tty/serial meson — `ttyAML0` in both modes |
| `0021` | media meson-vdec |
| `0022` | pinctrl meson |
| `0023` | iio/adc meson-saradc |
| `0031` | Bluetooth hci_bcm — honour `max-speed` under ACPI |

**Hardware quirks and fixes** (these matter in DeviceTree mode too):

| Patch | What it fixes |
|---|---|
| `0005` | arm64/efi: claim FPSIMD state before installing the EFI mm. Fixes an EFI runtime-services regression under software PAN. **The fix is Will Deacon's** and is now upstream — `e98a9d014637` in **v7.3-rc1**. Keep this patch only for **6.19 ≤ kernel < 7.3**; drop it on v7.3+. Not needed on 6.18 (the regression is v6.19+). The upstream commit has no `Cc: stable`, so it will not reach stable trees by itself. |
| `0013` | xhci-plat: mark streams broken on G12 DWC3 hosts (the UAS problem above) |
| `0026` | meson-vdec: 64-byte aligned canvas strides. Widths that are not a multiple of 64 (DVD 720, VCD 352) sheared into columns on zero-copy display. |
| `0030` | drm/meson venc: generate wide DMT modes in the CEA frame phase |
| `0032` | drm/meson: scrambled-domain deep color (the 1/40 TMDS clock ratio) |

**Video decode capability:**

| Patch | What it adds |
|---|---|
| `0024` | HEVC decode on G12A/G12B, 8-bit and 10-bit. Squashes two never-merged FROMLIST patches (kept verbatim in `patches/linux/hevc/`) plus the G12 wiring, a working 10-bit frame-buffer MMU path, and a use-after-free fix. HEVC is the only in-spec 4K60 path — the H.264 engine is capped around 4K30. |
| `0025` | mpeg12: emit the initial `SOURCE_CHANGE` event, which the driver never did. Without it, spec-flow clients (ffmpeg, hence Kodi) wait forever. |
| `0029` | zero-copy AM21C FBC capture format |
| `0033`–`0041` | meson-vdec robustness: reset discipline, racy core claim, FBC buffer recycling, firmware decode-stall recovery, null MMU maps, pooled FBC chunks |
| `0027`, `0028` | HDMI BT.2020 colorimetry and 10/12-bit deep colour |
| `0042`, `0043` | dw-hdmi: non-PCM IEC61937 and HBR bitstream passthrough on the I2S path |

**Out-of-tree modules** (`patches/linux/*/`): `g12b-cpufreq` (CPU DVFS under
ACPI), `g12b-pcie-acpi` (the conditional PCIe device), `vim3-hdmi-snd` (the
ACPI machine driver replacing the OF-only `axg-card`).

### `patches/upstream/` — not ACPI-specific

Four fixes to real bugs that also affect DeviceTree mode, kept in kernel
submission form because they belong upstream rather than in this port: the
mpeg12 canvas-at-start collapse, a VP9 esparser `u32` underflow, spurious
pre-negotiation `EPOLLERR`, and re-pulsing H.264 pipeline resets. **None have
been submitted** to linux-media. Each patch states its own status below its
`---` tear line.

A fifth, the arm64/efi SW-PAN fix, **has landed upstream** in Linux
**v7.3-rc1** as `e98a9d014637`, so its submission-form copy has been removed
from that directory. The numbering there now starts at `0002`. See that
directory's README for the whole thread.

### `patches/userspace/ffmpeg/` — needed for hardware decode in ffmpeg/Kodi

libavcodec V4L2 stateful-decoder rework, colour-metadata fallback, and
zero-copy Amlogic FBC capture for DRMPRIME output.

## Porting to another board

Same SoC family (Odroid N2/N2+ is also G12B): the kernel patch series applies
unchanged; what you rewrite is the firmware's ACPI tables, GPIO and LED
descriptions, the cpufreq module's voltage mapping, and the sound card
routing. Different SoC: only the generic patches and the *techniques* carry
over.

ACPI does not eliminate per-board firmware — it moves the hardware description
out of the OS image and into the firmware image. What becomes board-independent
is the OS.

## Two standing warnings

**Do not enable the SPIFC node in an operating-system device tree.** Firmware
owns that controller exclusively for runtime UEFI variables. Enabling it in
the OS will corrupt the variable store. The published DTB keeps it disabled;
`stage-spi-maintenance-boot.sh` exists precisely so that enabling it is a
deliberate, one-shot act.

**Vendor blobs are redistributed under their own terms.** BL2, BL30 and BL31
come from LibreELEC's `amlogic-boot-fip` and are not covered by this
repository's licence.

## Licence and attribution

**BSD-2-Clause-Patent**, matching EDK2 — see [LICENSE](LICENSE). Copyright
(c) 2026, Gus Bourg. Every source file carries an SPDX identifier.

This repository is a mix of licences, because it is a mix of provenance:

| Path | Licence | Notes |
|---|---|---|
| `Platform/`, `Silicon/`, `scripts/`, `patches/firmware/` | BSD-2-Clause-Patent | Portions derived from EDK2 and edk2-platforms retain their original authors' copyright notices — Intel, Arm, Linaro, Red Hat, Semihalf, AMD, Qualcomm, HPE, Jeremy Linton, Microsoft |
| `patches/linux/`, `patches/upstream/` | GPL-2.0 | Derivatives of the Linux kernel |
| `patches/linux/g12b-cpufreq/`, `g12b-pcie-acpi/`, `vim3-hdmi-snd/` | GPL-2.0 | Out-of-tree modules, written for this port |
| `patches/linux/hevc/` | GPL-2.0 | **Not our work** — two never-merged patches by Maxime Jourdan and Benjamin Roszak, reproduced as carried by LibreELEC |
| `patches/userspace/ffmpeg/` | LGPL-2.1+ / GPL-2.0+ | Inherits the licence of the ffmpeg files modified |

**Every patch in `patches/` carries a `From:` line naming its author.** Five
of them are not mine:

| Patch | Author |
|---|---|
| `patches/userspace/ffmpeg/0001` | John Cox `<jc@kynesim.co.uk>` |
| `patches/linux/hevc/…0016` | Maxime Jourdan `<mjourdan@baylibre.com>` |
| `patches/linux/hevc/…0015` | Benjamin Roszak `<benjamin545@gmail.com>` |
| `patches/firmware/edk2/0002` | Jared Pan `<jared.pan@dell.com>` |
| `patches/linux/0006` | Ricardo Pardini `<ricardo@pardini.net>` |

A sixth case is not visible in a `From:` line, because the kernel does not
record it that way: **the diff in `patches/linux/0005` is Will Deacon's**, not
mine. I posted a different fix for the same bug, he proposed the simpler
reorder in review, and I reverted mine in favour of his. It is now upstream as
`e98a9d014637`, authored by him, crediting me as reporter and tester — which
is the right split. Our carried patch is authored by me with
`Suggested-by: Will Deacon <will@kernel.org>`, the correct trailer for this
situation, but it understates things, so it is said plainly here.

Debts worth naming explicitly, because they are other people's work and this
port would not exist without them:

- **`patches/userspace/ffmpeg/0001` is John Cox's**, not ours — a subset of
  the V4L2 M2M stateful-decoder rework from
  [jc-kynesim/rpi-ffmpeg](https://github.com/jc-kynesim/rpi-ffmpeg). Not a
  line of his code is altered; what is ours is the choice of subset, the
  attribution header, and the two `---`/`+++` diff timestamps, which are
  metadata about when the diff was generated rather than authorship. Full
  provenance, including the exact base and branch commits, is at the top of
  the patch and in that directory's README.
- **HEVC decode comes from Maxime Jourdan** (Baylibre) and **Benjamin
  Roszak**, whose FROMLIST patches never merged upstream. They are kept
  verbatim in `patches/linux/hevc/`; patch `0024` squashes them together
  with our G12 wiring, 10-bit MMU fixes and a use-after-free fix, and
  credits them in its commit message.
- **The arm64/efi SW-PAN fix is Will Deacon's**, as described above, and is
  upstream in Linux v7.3-rc1 (`e98a9d014637`, reviewed by Ard Biesheuvel).
  The bug report, the reproduction, the ftrace work and the 6.4 M-call
  validation are mine; the change itself is his.
- **The display bring-up leaned on Fuchsia's** BSD-3-Clause Amlogic display
  and DesignWare HDMI drivers (Copyright The Fuchsia Authors). No Fuchsia
  code is reproduced — the sequences were reimplemented against the
  datasheet and verified on hardware — but the debt is real.

**Vendor blobs are not in this repository.** BL2/DDR, BL30 and BL31 are
fetched at build time from the pinned LibreELEC `amlogic-boot-fip`
submodule, are redistributed under their own terms, and are not licensed by
this project.
