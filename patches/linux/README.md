# VIM3 ACPI-mode kernel patches

Paired with the firmware's ACPI description (DSDT allow-list + the
conditional SSDTs): each PRP0001 device the firmware publishes needs the
matching driver below to probe without DeviceTree.  Convention across the
whole series: every deviation sits behind `has_acpi_companion()` /
`!dev->of_node`, the DT paths stay byte-identical, and where a DT phandle
cannot be expressed the firmware pre-configures the hardware and states
the facts as `_DSD` scalars.

| Patch | Driver | ACPI device |
|---|---|---|
| 0001 | mmc: meson-gx | SDC0/SDB0/SDA0 (eMMC/SD/SDIO) |
| 0002 | thermal: amlogic | CTMP + DDRT (zone name + trips from match data; DDR = monitor + critical only, its DT cooling target is the GPU devfreq cdev which is absent under ACPI) |
| 0003 | i2c: meson | I2CA (+ RTC0/FAN0 children) |
| 0005 | arm64/efi | (SW-PAN preemption fix: FPSIMD claim before EFI-mm install. **Will Deacon's fix**, now UPSTREAM as `e98a9d014637` in v7.3-rc1. Keep for 6.19 <= kernel < 7.3, drop on v7.3+; no `Cc: stable` upstream, so stable trees will not get it automatically. Replaces our v1 non-preemptible approach.) |
| 0006 | net: stmmac | ETH0 (Phytium ACPI glue, 6.18-family).  **Not our work** - by Ricardo Pardini <ricardo@pardini.net> for Armbian |
| 0007 | drm/panfrost | GPU0 (Mali G52; _CCA=One is load-bearing) |
| 0008 | drm/meson | VPU0 (KMS master; component match by fwnode) |
| 0009 | drm/meson encoder-hdmi | (bridge from priv->hdmi_bridge; CEC notifier under ACPI registered against the dw-hdmi device found by _DSD compatible scan — pointer identity with 0019's hdmi-phandle resolution) |
| 0010 | drm/meson dw-hdmi glue | HDMI |
| 0011 | drm/bridge dw-hdmi core | (accessor + NO_CONNECTOR fix + optional clks) |
| 0012 | soc/amlogic canvas | CNVS (_DSD device reference lookup) |
| 0013 | usb/xhci-plat | XHC0 (G12 stream-engine erratum: XHCI_BROKEN_STREAMS via `xhci-broken-streams-quirk` _DSD + amlogic glue compatible default; uas auto-falls-back to BOT) |
| 0014 | clk/meson axg-audio | CLKA (clkdev island: `amlogic,mst-inN-rate` fixed-rate inputs + consumer lookups by compatible scan; pclk firmware-managed; aux reset skipped) |
| 0015 | ASoC/meson axg-fifo | FRDA (arb reset skipped — firmware + CLKA `_RST` own the arbiter grant; pclk ENOENT→DEFER) |
| 0016 | ASoC/meson axg-tdm-formatter | TDMA (5 clock gets ENOENT→DEFER) |
| 0017 | ASoC/meson axg-tdm-interface | TIFA (sclk/lrclk ENOENT→DEFER) |
| 0018 | ASoC/meson g12a-tohdmitx | THTX (device_reset_optional under ACPI — only the optional variant stops after evaluating AML `_RST`; the non-optional one still demands an OF/lookup reset control) |
| 0019 | media/cec meson ao-cec-g12a | CECB (device_get_match_data; "hdmi-phandle" as _DSD device reference → bus_find_device_by_fwnode, key-only per the notifier contract; fixed-rate oscin island from `oscin-frequency` — the DT clock is a read-only xtal gate). Lives in SsdtDisplay (references \_SB.HDMI); GPIOH_3 muxed by firmware |
| 0020 | tty/serial meson_uart | UAR0 (`clock-frequency` baud reference, `_UID` as line number ⇒ ttyAML0 in both modes; console via cmdline — no SPCR subtype exists for this UART) |
| 0021 | staging/media meson vdec | VDEC (by-index 5-region _CRS ABI, private AO regmap, RESET_PARSER pulse write, HHI clk island with fixed-parent-only muxes — no PLL escape; requires the upstream/0002-0005 driver fixes underneath) |
| 0022 | pinctrl: meson | GPIP/GPAO (device_get_match_data; regions from own _CRS via `reg-names` _DSD name→index map, mapped WITHOUT claiming — AO mux window shares a register with CPUF; ACPI-only add_pin_ranges so a requested line is muxed to GPIO like DT; GPIO IRQs out of scope). Consumers LEDS/LEDR/BTNS bind stock leds-gpio / gpio-keys-polled via PRP0001 — no kernel patch |
| 0023 | iio/adc meson-saradc | SARC (3-region by-index _CRS ABI: adc + AO_CLK_GATE0 + AO_SAR_CLK; AO clock island mapped without claiming; vref from `vref-microvolt` _DSD — the regulator core refuses dummy supplies under ACPI; `#io-channel-cells` on SARC is LOAD-BEARING for io-channels consumers, else they silently get channel 0). ADCK = stock adc-keys via PRP0001 — no kernel patch |
| 0024 | media meson-vdec HEVC | **NOT an ACPI patch** - a capability addition. **Not all our work** - squashes two never-merged FROMLIST patches carried by LibreELEC (10-bit handling by Benjamin Roszak, the HEVC codec by Maxime Jourdan; both kept verbatim in `hevc/`) plus our deltas: the **G12A/G12B wiring** (the original series wires gxbb/gxl/gxlx/gxm only, always with the non-MMU `gxl_hevc.bin`; the only G12 blob that exists is `g12a_hevc_mmu.bin`, and 8-bit runs the non-MMU path against it - verified on hardware), **making the 10-bit/Main10 frame-buffer MMU path work** (as inherited it crashed the SoC: the MMU registers were programmed only at start() before is_10bit was known - resume() now re-runs workspace setup after buffer allocation, mirroring VP9, plus DECOMP_CTL2=0 in MMU mode, one-shot map allocation, map bounds check, recorded-size frees - see `hevc/README.md`), and the esparser throttle merge that keeps our `streamon_cap` gate from upstream/0003 while adding HEVC to it. **v3 also fixes a latent use-after-free** (I-slices kept a stale `col_frame` across the per-frame free pass; slab reuse turned it into a NULL-vbuf oops in the IRQ thread - hit by fast teardown, present in the FROMLIST code for 8-bit and 10-bit alike). **Why:** the H.264 engine is capped at 4K30 (measured 36 fps on 4K60), so HEVC is the only in-spec 4K60 path - measured 64 fps 8-bit, 95 fps Main10 (ACPI; DT measures ~8% lower - vdec clk parent difference). Applies clean to BOTH 7.0.12 and 6.18.35 |
| 0025 | media meson-vdec mpeg12 SOURCE_CHANGE | Upstream candidate (spec-compliance fix). mpeg12 never emitted the initial `V4L2_EVENT_SOURCE_CHANGE` (no firmware header-parse interrupt), while the driver waits for CAPTURE streamon before starting no-resume codecs - a mutual wait that parks spec-flow clients (ffmpeg, hence Kodi) forever; GStreamer only worked because it configures CAPTURE from its own parser. The driver now scans the first queued OUTPUT buffers for the MPEG sequence header (00 00 01 B3 + 12-bit w/h) and answers with the real coded size. Kodi hardware-decodes MPEG1/2 for the first time; PSNR vs software 61.5/60.9 dB. Applies clean to BOTH kernels |
| 0026 | media meson-vdec 64-byte strides | Upstream candidate (scanout correctness). Canvas/bytesperline were 32-byte aligned but both the VDEC write and VD1 scanout canvases run in 64-byte swap mode - any width not a multiple of 64 (DVD 720, VCD 352) sheared into columns on DRMPRIME zero-copy display while CPU consumers saw correct frames. 64-multiple widths are byte-identical before/after, so validated HD/UHD behavior is untouched (goldens re-verified). Applies clean to BOTH kernels |
| 0027 | drm meson+dw-hdmi BT.2020 Colorspace | Upstream candidate (HDR correctness). meson never attached the HDMI `Colorspace` property and dw-hdmi never read `conn_state->colorspace` for the AVI infoframe (601/709 only), so HDR output carried PQ EOTF but no BT.2020 colorimetry - wrong gamut on TVs that key off the AVI frame. Attaches the property (BT2020_RGB\|BT2020_YCC), forces re-config on colorspace change, overrides colorimetry via `drm_hdmi_avi_infoframe_colorimetry()`. Bench-validated: Kodi sets Colorspace=BT2020_YCC on HDR10 playback. Applies clean to BOTH kernels |

Out-of-tree modules: `g12b-cpufreq/` (CPUF), `g12b-pcie-acpi/` (the
conditional PCIe SSDT device), `vim3-hdmi-snd/` (CARD - the ACPI
machine driver replacing OF-only axg-card; resolves every component by
platform-bus compatible scan, name prefixes via codec_conf).

Base tree: the top-level patches are generated against **7.0.12** (the
bench Resolute kernel, Armbian uefi-arm64 edge).  The `6.18/`
subdirectory holds the same series rebased for **6.18.35** (the Trixie
eMMC install); differences are mechanical API drift only
(`pfdev->dev` vs `base.dev`, `next_bridge` member location, no bridge
refcounting).  Both sets are verified with `git apply --check` against
their pristine trees; both sets are bench-qualified end-to-end: the 7.0 set on the
Resolute install and the 6.18 set live on the Trixie install
(module-swap against linux-headers-current-arm64; kmscube 60.000 fps,
glmark2 921, 60.00 Hz flips, DPMS + reboot soak, DT regression clean).
