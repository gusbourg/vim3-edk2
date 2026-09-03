#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
utils_tree="${project_root}/third_party/khadas-utils"
flash_tool="${utils_tree}/aml-flash-tool/flash-tool"
image=${1:-"${project_root}/out/recovery/khadas-v14/VIM3.uboot-mainline.emmc.aml.img"}

expected_utils=ec65caddc35497a4c739c9977641d2233bc9264e
expected_image=a602d3bfbc627244b887d6123581d440e2e2a892e0d616b01ffe0b0d872dba30

test "$(git -C "${utils_tree}" rev-parse HEAD)" = "${expected_utils}"
test -x "${flash_tool}"
test -s "${image}"
echo "${expected_image}  ${image}" | sha256sum -c -
lsusb -d 1b8e:c003 >/dev/null

# This is Khadas's VIM3 v14-compatible recovery package.  Deliberately omit
# --wipe and select only bootloader: disk_initial 0 preserves the DOS user-area
# partition table while the vendor tool repairs user boot data plus boot0/1.
sudo "${flash_tool}" \
  "--img=${image}" \
  --parts=bootloader \
  --soc=g12a \
  --reset=y
