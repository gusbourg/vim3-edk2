# Upstream-bound fixes

Five fixes to real bugs, none of them ACPI-specific — they matter in
DeviceTree mode too. They are kept here in kernel submission form, separately
from the ACPI series, because they belong upstream rather than in this port.

| Patch | Fixes | Upstream status |
|---|---|---|
| `0001` | arm64/efi: EFI runtime-services regression under software PAN | **Submitted, awaiting merge** |
| `0002` | meson-vdec: mpeg12 canvas-at-start collapse | Not submitted |
| `0003` | meson-vdec: VP9 esparser throttle `u32` underflow | Not submitted |
| `0004` | meson-vdec: spurious pre-negotiation `EPOLLERR` | Not submitted |
| `0005` | meson-vdec: h264 first-GOP corruption | Not submitted |

Each patch repeats its own status below its `---` tear line, so the
information travels with the file.

## `0001` — arm64/efi SW-PAN

Under `CONFIG_ARM64_SW_TTBR0_PAN`, a voluntary reschedule between the EFI mm
being installed into `TTBR0_EL1` and the firmware call resumes with another
task's `TTBR0_EL1`, and the next `efi_mm` access takes a level 0 translation
fault. On a VIM3 that is an oops in `efi_call_rts` within a few hundred
thousand runtime calls.

**The history matters, because the fix is not ours.** A first version — a
different approach, keeping the runtime call non-preemptible — was posted to
linux-efi on 2026-08-05:

<https://lore.kernel.org/linux-efi/20260806000144.3388823-1-gus@bourg.net/T/>

Will Deacon declined that approach and proposed the minimal reorder instead,
moving `__efi_fpsimd_begin()` above `uaccess_ttbr0_enable()`:

<https://lore.kernel.org/linux-efi/30c5499e-8041-4e39-933b-c5220d53be77@app.fastmail.com/T/>

**That suggestion is the diff in `0001`** — hence the `Suggested-by:` trailer.
Our v1 was reverted rather than stacked on top, and a `Tested-by:` for the
reorder was sent to the list on 2026-08-10, backed by 6.4 M runtime calls
across both description modes with zero faults.

This is carried as `patches/linux/0005` and should be dropped once it reaches
stable. The 6.18 series does not carry it at all: the regression it fixes is
v6.19+, which is why there is no `patches/linux/6.18/0005`.

## `0002`–`0005` — meson-vdec

Four fixes to the staging meson vdec driver, all root-caused on hardware and
validated with a GStreamer 1.26 stateful-decode flow on G12B:

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

Base `0001` on a tree containing `a5baf582f4c0` (v6.19 or later). Recipients
come from `scripts/get_maintainer.pl`; Ard Biesheuvel authored the commit that
introduced the regression, and Catalin Marinas and Will Deacon acked it, so
all three have direct context.

```sh
git am 0001-*.patch
./scripts/checkpatch.pl --strict -g HEAD
git format-patch -1 -o outgoing/
```

The patches carry an `Assisted-by:` trailer in the format
`Documentation/process/coding-assistants.rst` requires. Maintainers are
explicitly entitled to apply extra scrutiny to, deprioritise, or reject
tool-assisted contributions — expect that, and be ready to answer detailed
questions about the mechanism.
