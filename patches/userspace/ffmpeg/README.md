# ffmpeg V4L2 M2M stateful-decoder rework (Kodi hardware decode)

Userspace counterpart to kernel patch `0021` (meson-vdec under ACPI).  The
kernel side was already bit-exact via GStreamer; this patch makes **ffmpeg**
— and therefore **Kodi** — able to drive the same decoder.

## The bug this fixes

Upstream ffmpeg's `libavcodec/v4l2_m2m*` was written in 2017, before the
V4L2 **stateful decoder** UAPI was finalised.  It configures and starts the
CAPTURE queue up-front from container metadata instead of waiting for the
driver's `V4L2_EVENT_SOURCE_CHANGE` after the hardware parser has read the
SPS.  meson-vdec (compliant H.264 support, merged 5.7) enforces the spec, so
stock ffmpeg parks forever on the first frame; Kodi surfaces it as
`CDVDVideoCodecDRMPRIME ... send packet failed: Invalid data found when
processing input`.

Nothing is wrong with the kernel driver, our ACPI port, or the V4L2 ABI:
GStreamer's spec-compliant client is frame-md5-exact on the same kernel in
both DT and ACPI modes.

## Provenance

Subset of **jc-kynesim/rpi-ffmpeg** (John Cox, Raspberry Pi) — the same tree
LibreELEC ships for Amlogic instead of stock ffmpeg:

| | |
|---|---|
| upstream base | `n7.1.5` = `eefb3654c445a9004712036be4ffb01a68b2c3e1` |
| fork branch | `test/7.1.5/main` = `89275ad059740f06bfe58cf60407e79c567bc828` |
| repo | https://github.com/jc-kynesim/rpi-ffmpeg |

7.1.5 is *exactly* the version Debian trixie ships (`7:7.1.5-0+deb13u1`), so
the patch applies to the Debian source package with no rebasing.

**Deliberately a subset.**  Only the stateful-m2m rework is taken:
`v4l2_buffers.[ch]`, `v4l2_context.[ch]`, `v4l2_fmt.c`, `v4l2_m2m*.[ch]`,
`v4l2_req_dmabufs.[ch]`, `v4l2_req_utils.h`, `weak_link.[ch]`, two inline
helpers in `libavutil/frame.h`, and one `libavcodec/Makefile` hunk.  The
fork's RPi-specific parts (SAND pixel formats, `rpi_sand_*`, the
v4l2-request stateless HEVC path) are **excluded** — they add public pixfmt
enums and would change the library ABI.  Nothing in the subset references
them (verified: zero `AV_PIX_FMT_SAND*`/`AV_PIX_FMT_RPI*` uses).

What it buys beyond "it works at all": proper SOURCE_CHANGE handshake, EOS
drain, and **DRM_PRIME zero-copy output** — decoded frames stay as dmabufs
and go straight onto a KMS plane, which is what makes 4K viable.

## The three patches

| Patch | Whose | What it does |
|---|---|---|
| `0001` | **John Cox's** (jc-kynesim) | the stateful-m2m rework itself |
| `0002` | ours | colour-metadata fallback |
| `0003` | ours | zero-copy Amlogic FBC capture |

**`0002` — colour metadata.** The rework's decode path never programs the
OUTPUT queue colorspace, the driver mirrors DEFAULT back, and decoded frames
come out with unspecified colour properties — so Kodi never sees PQ/BT.2020
and never engages HDR passthrough. `0002` falls back to the codec context's
colour properties and propagates the static HDR side data (mastering display,
content light level) onto frames when the driver reports them unspecified.
Verified on hardware: hw frames carry bt2020nc/bt2020/smpte2084 and Kodi sets
`HDR_OUTPUT_METADATA` with EOTF=PQ on the connector, against an HDR-capable
sink.

**`0003` — zero copy.** When the client wants DRMPRIME frames, prefer the
meson vdec `AM21` capture format (one plane = the AFBC scatter/MMU
compression header — kernel patch `0029`) and export it as
`DRM_FORMAT_YUV420_8BIT`/`10BIT` + `DRM_FORMAT_MOD_AMLOGIC_FBC(SCATTER)` for
direct VD1 overlay scanout. That is true 10-bit decode-to-display with no
NV12 downsample write. Software consumers (`-f md5`, transcode) still
negotiate NV12 exactly as before.

## Building

`build-ffmpeg-debs.sh` runs inside a native arm64 `debian:trixie` container,
for example:

    podman run --rm -v <workdir>:/w docker.io/library/debian:trixie \
      bash /w/build-ffmpeg-debs.sh

Budget roughly 1.5 hours on a 4-core board. The script takes the Debian
source package, drops these patches into `debian/patches/` (the stock package
has **no** patches directory — it creates one), stamps a `+vim3` local
version, and builds binary debs.

Install on the board (runtime packages only — Kodi links the system ffmpeg,
so no Kodi rebuild is needed):

    ffmpeg libavcodec61 libavdevice61 libavfilter10 libavformat61 \
    libavutil59 libpostproc58 libswresample5 libswscale8

## Validated (kernel 7.0.12, ACPI mode)

frame-md5 of hardware decode vs software-decode goldens:

| clip | result |
|---|---|
| H.264 1080p (`h264-g60.mp4`) | **MD5-exact**, 300 frames |
| H.264 1080p (`h264-1080p.mp4`) | **MD5-exact**, 300 frames |
| H.264 **4K** (`h264-4k-g60.mp4`) | **MD5-exact**, 180 frames |
| VP9 1080p | 299/300 exact — final frame differs (the pre-existing EOS drain quirk, not introduced here) |

Kodi 21.2: `CDVDVideoCodecDRMPRIME::Open - using decoder V4L2 mem2mem H.264
/ VP9 decoder wrapper`, full-length playback of all three clips, **8 % CPU
during 4K**.

## CMA is a hard requirement for 4K

4K NV12 capture buffers are ~12 MB each and ffmpeg allocates ~20 of them.
With `cma=256M` the CAPTURE `STREAMON` fails with
`EINVAL` (kernel log: `dma alloc of size 8323072 failed`).  Boot with
`cma=512M` — set it in `/etc/default/grub.d/99-cma.cfg`, which **overrides**
`/etc/default/grub`; edit the drop-in, not the main file.
