#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
edk2_root="${project_root}/third_party/edk2"
edk2_platforms_root="${project_root}/third_party/edk2-platforms"

#
# Downstream patch sets.
#
# EDK2 and edk2-platforms are pinned, PRISTINE upstream submodules.  Fixes we
# carry against them live in patches/firmware/ and are applied into the
# submodule work tree at build time.  This is the convention used by the other
# out-of-tree EDK2 board ports (edk2-rk3588's edk2-patches/, pftf/RPi4's
# MdeModulePkg patch); it keeps the submodules tracking tianocore so a patch
# that lands upstream is simply deleted here.
#
# Patches are applied and LEFT APPLIED.  A stamp records the state and the tree
# is re-synced only when a patch file changes or the patches are found missing.
# Nothing is reversed on exit, so an interrupted build cannot leave a
# half-reverted submodule -- and a dirty submodule is the normal steady state,
# not an error.
#
# CONSTRAINT: a patch may only MODIFY tracked files.  Re-sync uses
# `git checkout -- .`, which restores modified files but would not remove an
# added one; patchset_guard below enforces this rather than trusting it.
#
edk2_patches=("${project_root}"/patches/firmware/edk2/*.patch)
edk2_platforms_patches=("${project_root}"/patches/firmware/edk2-platforms/*.patch)
edk2_stamp="${project_root}/out/.patchset-edk2.stamp"
edk2_platforms_stamp="${project_root}/out/.patchset-edk2-platforms.stamp"

target=${1:-DEBUG}
base_dtb="${project_root}/Platform/Khadas/Vim3/DeviceTree/meson-g12b-a311d-khadas-vim3.dtb"
runtime_overlay="${project_root}/Platform/Khadas/Vim3/DeviceTree/meson-g12b-a311d-khadas-vim3-spifc.dtso"
generated_dir="${project_root}/out/${target}/generated"
runtime_dtbo="${generated_dir}/meson-g12b-a311d-khadas-vim3-spifc.dtbo"
runtime_dtb="${generated_dir}/meson-g12b-a311d-khadas-vim3.dtb"
# Dirtiness must IGNORE submodules.  The firmware patch sets are applied to
# third_party/edk2* and left applied, which makes plain `describe --dirty`
# report "-dirty" on every build regardless of whether our own tree is clean.
if git -C "${project_root}" rev-parse --git-dir >/dev/null 2>&1; then
  if git -C "${project_root}" diff --quiet --ignore-submodules=all HEAD -- &&
     git -C "${project_root}" diff --quiet --ignore-submodules=all --cached HEAD --
  then
    firmware_dirty=
  else
    firmware_dirty=-dirty
  fi
  firmware_version="$(git -C "${project_root}" describe --always)${firmware_dirty}"
  # Monotonic 32-bit revision for FMP/ESRT; the commit count moves with every
  # commit where the describe string is only human-readable.
  firmware_revision=$(git -C "${project_root}" rev-list --count HEAD)
else
  # Not a git checkout -- a source archive, e.g. GitHub's "Download ZIP".
  # Build anyway, but say so in the version string, and pin the FMP/ESRT
  # revision to 1 so such a build can never out-rank a real one in a capsule
  # version comparison.
  firmware_version=unknown-archive
  firmware_revision=1
fi
firmware_release_date=$(date -u +%m/%d/%Y)

case "${target}" in
  DEBUG|RELEASE) ;;
  *) echo "usage: $0 [DEBUG|RELEASE]" >&2; exit 2 ;;
esac

test "$(git -C "${edk2_root}" rev-parse HEAD)" = \
  b03a21a63e3bd001f52c527e5a57feddb53a690b
test "$(git -C "${edk2_platforms_root}" rev-parse HEAD)" = \
  ae058185e12591a9a49e5895e90ca52936851973

# Refuse a patch that adds or deletes files; re-sync could not undo it.
patchset_guard() {
  local p
  for p in "$@"; do
    if grep -qE '^(---|\+\+\+) /dev/null' "${p}"; then
      echo "patch adds or removes files, which re-sync cannot undo: ${p}" >&2
      exit 1
    fi
  done
}

# Sync needed when: no stamp, any patch newer than the stamp, or the work tree
# is clean (patches absent, e.g. after `git submodule update`).
patchset_needs_sync() {
  local repo=$1 stamp=$2 p
  shift 2
  [[ -f ${stamp} ]] || return 0
  # `git diff --quiet` exits 0 when CLEAN, i.e. when the patches are absent.
  if git -C "${repo}" diff --quiet --; then
    return 0
  fi
  for p in "$@"; do
    if [[ ${p} -nt ${stamp} ]]; then
      return 0
    fi
  done
  return 1
}

patchset_guard "${edk2_patches[@]}" "${edk2_platforms_patches[@]}"
mkdir -p "${project_root}/out"

if patchset_needs_sync "${edk2_root}" "${edk2_stamp}" "${edk2_patches[@]}"; then
  echo "==> syncing EDK2 patch set (${#edk2_patches[@]} patches)"
  git -C "${edk2_root}" checkout --quiet -- .
  for patch_file in "${edk2_patches[@]}"; do
    echo "    ${patch_file##*/}"
    git -C "${edk2_root}" apply --check --ignore-space-change "${patch_file}"
    git -C "${edk2_root}" apply --ignore-space-change "${patch_file}"
  done
  touch "${edk2_stamp}"
fi

if patchset_needs_sync "${edk2_platforms_root}" "${edk2_platforms_stamp}" \
  "${edk2_platforms_patches[@]}"; then
  echo "==> syncing edk2-platforms patch set"
  git -C "${edk2_platforms_root}" checkout --quiet -- .
  for patch_file in "${edk2_platforms_patches[@]}"; do
    echo "    ${patch_file##*/}"
    git -C "${edk2_platforms_root}" apply --check --ignore-space-change "${patch_file}"
    git -C "${edk2_platforms_root}" apply --ignore-space-change "${patch_file}"
  done
  touch "${edk2_platforms_stamp}"
fi

command -v dtc >/dev/null
command -v fdtoverlay >/dev/null

mkdir -p "${generated_dir}"
dtc -@ -I dts -O dtb -o "${runtime_dtbo}" "${runtime_overlay}"
fdtoverlay -i "${base_dtb}" -o "${runtime_dtb}" "${runtime_dtbo}"

test "$(fdtget -t i "${runtime_dtb}" /soc/mmc@ffe07000 bus-width)" = "4"
test "$(fdtget -t s "${runtime_dtb}" /soc/bus@ffd00000/spi@14000 status)" = \
  "disabled"

export WORKSPACE="${edk2_root}"
export PACKAGES_PATH="${project_root}:${edk2_root}:${project_root}/third_party:${edk2_platforms_root}"
export GCC_AARCH64_PREFIX=aarch64-linux-gnu-

if [[ ! -x "${edk2_root}/BaseTools/Source/C/bin/GenFv" ]]; then
  make -C "${edk2_root}/BaseTools" -j"$(nproc)"
fi
# shellcheck disable=SC1091
set +u
source "${edk2_root}/edksetup.sh" BaseTools
set -u

build \
  -a AARCH64 \
  -t GCC \
  -b "${target}" \
  -p Platform/Khadas/Vim3/Vim3.dsc \
  -D "VIM3_DTB_FILE=${runtime_dtb}" \
  -D "FIRMWARE_VERSION=${firmware_version}" \
  -D "FIRMWARE_RELEASE_DATE=${firmware_release_date}" \
  -D "FIRMWARE_REVISION=${firmware_revision}" \
  -n "$(nproc)"

fd="${edk2_root}/Build/KhadasVim3-AARCH64/${target}_GCC/FV/VIM3_EFI.fd"
test -s "${fd}"
mkdir -p "${project_root}/out/${target}"
cp "${fd}" "${project_root}/out/${target}/VIM3_EFI.fd"
sha256sum "${project_root}/out/${target}/VIM3_EFI.fd"
