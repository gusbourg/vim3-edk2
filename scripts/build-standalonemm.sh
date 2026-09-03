#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
edk2_root="${project_root}/third_party/edk2"
platforms_root="${project_root}/third_party/edk2-platforms"
workspace="${project_root}/out/StandaloneMM/workspace"
build_fd="${workspace}/Build/MmStandaloneRpmb/RELEASE_CLANGDWARF/FV/BL32_AP_MM.fd"
output_dir="${project_root}/out/StandaloneMM/release"

expected_edk2=b03a21a63e3bd001f52c527e5a57feddb53a690b
expected_platforms=ae058185e12591a9a49e5895e90ca52936851973

test "$(git -C "${edk2_root}" rev-parse HEAD)" = "${expected_edk2}"
test "$(git -C "${platforms_root}" rev-parse HEAD)" = "${expected_platforms}"
git -C "${edk2_root}" diff --quiet --
git -C "${platforms_root}" diff --quiet --

command -v clang >/dev/null
if ! command -v ld.lld >/dev/null && ! command -v ld.lld-21 >/dev/null; then
  echo "LLVM lld is required" >&2
  exit 1
fi
command -v llvm-ar >/dev/null
command -v llvm-objcopy >/dev/null

mkdir -p "${workspace}" "${output_dir}"
export WORKSPACE="${workspace}"
export PACKAGES_PATH="${edk2_root}:${platforms_root}"
export EDK_TOOLS_PATH="${edk2_root}/BaseTools"
export CLANGDWARF_BIN
CLANGDWARF_BIN=$(dirname -- "$(command -v clang)")/
export SOURCE_DATE_EPOCH
SOURCE_DATE_EPOCH=$(git -C "${edk2_root}" show -s --format=%ct HEAD)

# shellcheck disable=SC1091
set +u
source "${edk2_root}/edksetup.sh" BaseTools
set -u

build \
  -a AARCH64 \
  -t CLANGDWARF \
  -b RELEASE \
  -p Platform/StandaloneMm/PlatformStandaloneMmPkg/PlatformStandaloneMmRpmb.dsc \
  -n "$(nproc)"

test -s "${build_fd}"
test "$(stat -c %s "${build_fd}")" -eq 2621440
cp "${build_fd}" "${output_dir}/BL32_AP_MM.fd"
sha256sum "${output_dir}/BL32_AP_MM.fd"
