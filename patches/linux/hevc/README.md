# HEVC decode on G12A/G12B

## What is in this directory

**Two patches that are not our work**, kept verbatim as LibreELEC carries
them:

| Patch | Author | What it is |
|---|---|---|
| `amlogic-0015-FROMLIST-v2-…-10bit-bitstre…` | Benjamin Roszak `<benjamin545@gmail.com>` | 10-bit bitstream handling |
| `amlogic-0016-FROMLIST-v2-…-HEVC-decode-codec` | Maxime Jourdan `<mjourdan@baylibre.com>` | the HEVC codec itself |

Both were posted to linux-media as `FROMLIST v2` and never landed upstream.
They are reproduced here unmodified so that what we added on top stays
visible as a separate thing. Patch `patches/linux/0024` is the squash of
these two plus our own deltas, and that is the patch you apply — this
directory is provenance, not a second copy to apply.

`codec_hevc.c` is roughly 1500 lines of third-party codec logic that never
merged. Adopting it means maintaining it; that is a real cost and it is
worth saying out loud.

## Why HEVC at all

The A311D's H.264 engine is capped at 4K30, so 4K60 H.264 cannot play:

| Stream | Block | Measured | Needed |
|---|---|---|---|
| H.264 4K60 | `vdec_1` | **36 fps** (0.60x) | 60 |
| **HEVC 4K60** | `vdec_hevc` | **64 fps** (1.06x) | 60 |
| HEVC 1080p | `vdec_hevc` | 182 fps (6.03x) | - |
| VP9 4K60 (already worked) | `vdec_hevc` | 74 fps (1.24x) | 60 |

HEVC is the only in-spec path to 4K60 on this SoC, matching the manual
(`H.265 HEVC up to 4Kx2K@60fps`, `H.264 AVC up to 4Kx2K@30fps`).

Kodi's `stream stalled` + `receive frame failed: EAGAIN` on 4K60 H.264 was
the downstream symptom: decode at 0.6x realtime backs the input queue up,
the demuxer blocks, audio starves.

## What we found

**`g12a_hevc_mmu.bin` decodes 8-bit HEVC on the non-MMU path.** That was the
open question. The upstream-proposed series wires HEVC into gxbb/gxl/gxlx/gxm
only, always with the non-MMU `gxl_hevc.bin`; the only G12A blob Amlogic ships
is the `_mmu` build, while `codec_hevc_use_mmu()` enables the frame-buffer MMU
for 10-bit only. So 8-bit loads an MMU-named firmware and runs the non-MMU
path against it. It works.

**The 10-bit MMU path had a structural bug that crashed the SoC.** As
inherited, a Main10 stream produced:

```
meson-vdec: esparser: input parsing error
meson-vdec: Buffer 5 done but can't match offset (0002C560)
Internal error: SP/PC alignment exception: 000000008a000000 [#1] SMP
```

— unrecoverable without a power cycle. Cause: the MMU registers
(`HEVC_ASSIST_MMU_MAP_ADDR`, `HEVC_SAO_MMU_VH0/VH1_ADDR`) are written only in
`codec_hevc_setup_workspace()`, which ran exactly once from `start()` — when
`is_10bit` was still 0 and the MMU map unallocated. When the first Main10
frame flipped the session into MMU mode via `resume()`, nothing re-armed
them: MMU decompress mode enabled with a map at address zero, the ucode walks
garbage, the parser stalls. `codec_vp9_resume()` does re-run its workspace
setup; HEVC's simply did not.

This was never a silicon limit. The A311D manual specifies
`H.265 HEVC MP-10@L5.1 up to 4Kx2K@60fps` — MP-10 *is* Main10 — and the
driver's own `codec_hevc_common.h` carries
`/* TOFIX: Handle Amlogic Compressed buffer for 8bit also */`, i.e. FBC/MMU
is the intended 10-bit mechanism and was known-incomplete.

## What patch `0024` adds on top

- **G12A/G12B wiring** — a `vdec_formats_g12a[]` entry and a matching
  `MODULE_FIRMWARE("meson/vdec/g12a_hevc_mmu.bin")`. Not part of any
  upstream-proposed series.
- **A working 10-bit frame-buffer MMU path** — split workspace *allocation*
  from register *programming* and call the latter from `resume()` after
  buffer setup, mirroring VP9's proven ordering. Plus hardening:
  `DECOMP_CTL2 = 0` in MMU mode, one-shot map allocation across resumes, a
  `fill_mmu_map` bounds check (the map fits 4608 pages; 4K10 needs 4096),
  and freeing FBC buffers with their recorded size. Cross-checked against
  `vh265.c` from the Amlogic media_modules tree, for reading only — no
  vendor code is reproduced.
- **A use-after-free fix.** I-slices kept a stale `col_frame` across the
  per-frame free pass; slab reuse turned it into a NULL-vbuf oops in the IRQ
  thread. Reachable by fast teardown, and present in the inherited code for
  8-bit and 10-bit alike.
- **The esparser throttle merge.** LibreELEC adds HEVC to the VP9 throttle;
  our `patches/upstream/0003` had already added a `streamon_cap` gate to the
  same line. They merge conceptually, not textually — keep both, or the
  throttle is defeated entirely:

```c
	if ((sess->fmt_out->pixfmt == V4L2_PIX_FMT_VP9 ||
	     sess->fmt_out->pixfmt == V4L2_PIX_FMT_HEVC) &&
	    sess->streamon_cap) {
```

If you apply `amlogic-0015` and `-0016` by hand instead of using `0024`,
those are the two hunks that reject, and the Makefile one rejects only
because our ACPI patch added `vdec_acpi.o` to the line above.

## Measured

8-bit: 1080p 182 fps; 4K60 real content 722 frames at 64 fps. Main10:
1080p 193 fps; 4K60 95 fps; real UHD-BD HDR10 rips at 2x realtime; PSNR
44 dB against software decode (the comparison downsamples to 8-bit, so it is
not bit-exact by construction). Truncated-file crash test aborts cleanly and
the board survives. Regression clean: H.264 1080p60 131 fps, VP9 unchanged.

## Honest caveats

- **1.06x on 4K60 8-bit is thin.** 64 fps against 60 needed is 6% headroom
  before Kodi's render path takes its share.
- The 4K60 test clip was encoded `x265 -preset ultrafast`, which produces
  simpler bitstreams than real-world encodes. A demanding stream may decode
  slower.
- The one dropped frame in a 240-frame 1080p run is the pre-existing EOS
  drain quirk — VP9 shows 299/300 the same way — not something this work
  introduced.
