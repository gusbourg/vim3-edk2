# Firmware patches (EDK2 / edk2-platforms)

Downstream fixes carried against the pinned upstream submodules
`third_party/edk2` and `third_party/edk2-platforms`.

These are **firmware** patches, applied at build time to code we do not own.
They are separate from `patches/linux/` (GPL-2.0, applied to an OS kernel we
do not ship) and `patches/userspace/`.

## Why patches rather than vendored source

The submodules stay pristine and keep tracking tianocore, so a patch that
lands upstream is deleted here and nothing else changes. This is the
convention among out-of-tree EDK2 board ports — edk2-rk3588 carries an
`edk2-patches/` directory, pftf/RPi4 carries an MdeModulePkg patch — and it
is the reason upstream `edk2-platforms` itself contains no patch directories
at all: there, a needed edk2 change simply goes to edk2.

Note the distinction from a *derived driver*. `Vim3XhciDxe`,
`Vim3BootManagerLib` and `PrePi` are deliberate platform-specific variants
with their own names and INFs, living in our own tree. That is also normal
EDK2 practice. A patch here means "this shared upstream module has a bug or
gap"; a derived driver means "this platform needs its own variant".

## How they are applied

`scripts/build.sh` applies each set into the submodule work tree and **leaves
it applied**. A stamp under `out/` records the state; the tree is re-synced
only when a patch file changes, or when the patches are found missing (e.g.
after `git submodule update`). Nothing is reversed on exit, so an interrupted
build cannot strand a half-reverted submodule.

**A dirty submodule is therefore the normal steady state, not an error.**

Constraint enforced by `patchset_guard`: a patch may only **modify** tracked
files. Re-sync uses `git checkout -- .`, which restores modified files but
would not remove an added one. A patch that adds or deletes files fails the
build with a clear message rather than corrupting the tree silently.

To return the submodules to pristine upstream:

```sh
git -C third_party/edk2 checkout -- .
git -C third_party/edk2-platforms checkout -- .
rm -f out/.patchset-*.stamp
```

## Inventory

### `edk2/` — against `edk2-stable202605` (`b03a21a6`)

| Patch | Module | Destiny |
|---|---|---|
| `0001-MmcDxe-fall-back-to-4-bit-eMMC` | `EmbeddedPkg/Universal/MmcDxe` | **Local forever.** The VIM3 reuses eMMC D4–D7 for the SPI NOR that holds UEFI variables, so eight-bit is physically unavailable. Board wiring, not a bug. |
| `0002-UsbBusDxe-improve-USB-enumerating-process` | `MdeModulePkg/Bus/Usb/UsbBusDxe` | **Upstream candidate.** Robustness for slow or unusual enumeration; benefits any platform. |
| `0003-RngDxe-reject-unavailable-raw-algorithm` | `SecurityPkg/RandomNumberGenerator/RngDxe` | **Upstream candidate.** Generic correctness fix. |
| `0004-UiApp-show-both-CPU-clusters-on-the-front-page` | `MdeModulePkg/Application/UiApp` | **Cosmetic.** Carried for big.LITTLE readability; reconsider whether it earns its keep. |

### `edk2-platforms/` — against `ae058185`

| Patch | Module | Destiny |
|---|---|---|
| `0001-DwEmac-support-generic-Clause-22-PHYs` | `Silicon/Synopsys/DesignWare/Drivers/DwEmacSnpDxe` | **Upstream candidate.** Generic Clause-22 PHY support in a shared Synopsys driver. |
| `0002-DwEmac-use-normal-descriptor-layout` | same | **Upstream candidate.** Descriptor-format correction. |

Four of the six are generic fixes that belong upstream. Carrying a patch you
intend to submit is normal; carrying one you never submit is how a fork rots.

### A note on line endings

The pinned edk2-platforms DwEmac sources are CRLF. **Both patches are
generated against CRLF**, apply with plain `git apply`, and leave every file
pure CRLF — so the working diff shows only real changes (125 insertions / 59
deletions across the set) and there is no normalization step anywhere.

It did not start that way. `0002` was generated from LF-normalized sources,
forcing the build to rewrite all eight DwEmac files to LF before it would
apply — inflating the working diff to ~4000 lines of pure line-ending churn.
`0001` was cleaner but still left mixed endings, because `git apply` writes LF
on the lines it adds.

**If you regenerate either patch, regenerate BOTH, in order, and keep the CRLF
terminators.** `EmacDxeUtil.c` is touched by both, so changing `0001`'s output
invalidates `0002`'s context lines and silently breaks the chain. The
procedure that produced the current pair:

1. Capture a golden reference: apply the current patches, strip `\r`, save.
2. From pristine, apply `0001`, re-emit its five files as CRLF, and take
   `git diff` — that is the new `0001`.
3. Snapshot that tree, write the golden content back as CRLF, and `diff -u`
   the three `0002` targets against the snapshot — that is the new `0002`.
4. Verify from pristine: `git apply --check` both, then compare every file
   against golden with `\r` stripped.

Content lines carry `\r`; the `diff`/`---`/`+++`/`@@` metadata lines do not.
