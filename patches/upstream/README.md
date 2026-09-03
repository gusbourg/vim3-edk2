# Upstream-bound fixes

Four fixes to real bugs in the staging meson vdec driver, none of them
ACPI-specific — they matter in DeviceTree mode too. They are kept here in
kernel submission form, separately from the ACPI series, because they belong
upstream rather than in this port.

| Patch | Fixes | Upstream status |
|---|---|---|
| `0002` | meson-vdec: mpeg12 canvas-at-start collapse | Not submitted |
| `0003` | meson-vdec: VP9 esparser throttle `u32` underflow | Not submitted |
| `0004` | meson-vdec: spurious pre-negotiation `EPOLLERR` | Not submitted |
| `0005` | meson-vdec: h264 first-GOP corruption | Not submitted |

Each patch repeats its own status below its `---` tear line, so the
information travels with the file. The numbering starts at `0002` because
`0001` has already landed — see below.

## `0001` — arm64/efi SW-PAN: merged, and removed from this directory

**This one is upstream now**, so the submission-form copy has been deleted
rather than left here to be sent a second time.

Under `CONFIG_ARM64_SW_TTBR0_PAN`, a voluntary reschedule between the EFI mm
being installed into `TTBR0_EL1` and the firmware call resumes with another
task's `TTBR0_EL1`, and the next `efi_mm` access takes a level 0 translation
fault. On a VIM3 that is an oops in `efi_call_rts` within a few hundred
thousand runtime calls.

A first version — a different approach, keeping the runtime call
non-preemptible — was posted to linux-efi on 2026-08-05:

<https://lore.kernel.org/all/20260806000144.3388823-1-gus@bourg.net/>

Will Deacon declined that approach and proposed the minimal reorder instead,
moving `__efi_fpsimd_begin()` above `uaccess_ttbr0_enable()`. That is what
landed, in **Linux v7.3-rc1**:

```
commit e98a9d0146372b046d863164025a66ab4488b972
Author: Will Deacon <will@kernel.org>

    arm64/efi: Avoid voluntary preemption with efi_mm installed

    Reported-by: Gus Bourg <gus@bourg.net>
    Tested-by: Gus Bourg <gus@bourg.net>
    Reviewed-by: Ard Biesheuvel <ardb@kernel.org>
    Fixes: a5baf582f4c0 ("arm64/efi: Call EFI runtime services without
           disabling preemption")
```

**The fix is Will Deacon's work.** The bug report, the reproduction, the
ftrace analysis and the validation soak are mine; the change itself is his.

It is still carried as `patches/linux/0005` for older kernels — see that
patch's own note. In short: **needed for 6.19 ≤ kernel < 7.3, drop it on
v7.3 or later.** The upstream commit carries no `Cc: stable`, so it will not
reach stable trees on its own.

## `0002`–`0005` — meson-vdec

Four fixes, all root-caused on hardware and validated with a GStreamer 1.26
stateful-decode flow on G12B:

- **`0002`** mpeg12 canvas-at-start collapse — defer codec start until both
  queues are streaming.
- **`0003`** VP9 esparser throttle `u32` underflow — clamp, and gate the
  throttle on `streamon_cap`.
- **`0004`** spurious pre-negotiation `EPOLLERR` — the cause of random
  session failures; 10/10 clean afterwards.
- **`0005`** h264 first-GOP corruption — re-pulse the pipeline resets after
  power-up; bit-exact and deterministic afterwards.

These have not been posted to linux-media. They carry a `Signed-off-by:`
because that is a DCO attestation of origin, which is true whether or not a
patch is ever sent — it is not a claim that it was.

## If you want to send one

Recipients come from `scripts/get_maintainer.pl`.

```sh
git am 0002-*.patch
./scripts/checkpatch.pl --strict -g HEAD
git format-patch -1 -o outgoing/
```

The patches carry an `Assisted-by:` trailer in the format
`Documentation/process/coding-assistants.rst` requires. Maintainers are
explicitly entitled to apply extra scrutiny to, deprioritise, or reject
tool-assisted contributions — expect that, and be ready to answer detailed
questions about the mechanism.
