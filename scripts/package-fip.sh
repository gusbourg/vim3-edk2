#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
target=${1:-DEBUG}
fip_tree="${project_root}/third_party/amlogic-boot-fip"
board_tree="${fip_tree}/khadas-vim3"
fd="${project_root}/out/${target}/VIM3_EFI.fd"
output_dir="${project_root}/out/${target}/fip"
work_dir="${project_root}/out/${target}/fip-work"

expected_edk2=b03a21a63e3bd001f52c527e5a57feddb53a690b
expected_fip=42d372123631066fb77fbcbb612dc3eb41a3f6f9

test "$(git -C "${project_root}/third_party/edk2" rev-parse HEAD)" = "${expected_edk2}"
test "$(git -C "${fip_tree}" rev-parse HEAD)" = "${expected_fip}"
test -s "${fd}"
test "$(stat -c %s "${fd}")" -eq 4194304

# These are the pinned public VIM3 boot-chain inputs.  The removable-media
# wrapper emitted by aml_encrypt_g12b is required; u-boot.bin alone is the
# unwrapped bootmk payload and is not directly bootable from SD/eMMC.
echo "53d99bde4e3b290c55b73ef5165673cb51649fd2fc095637b26365042d6b04a4  ${board_tree}/bl2.bin" | sha256sum -c -
echo "d965152eb6e8a61ec7081d8588be96ee07553428c1a69670f24556bde5b1c1f9  ${board_tree}/bl31.bin" | sha256sum -c -
echo "855541de280c041f1dba1c6f11fdbd4d8968f5ee4126cb00959b778b79492dbb  ${board_tree}/acs.bin" | sha256sum -c -
echo "c5d2ca3ce142af78a79ec776cc70ed3d04b56fb16d73602cedc2ecea81daec3f  ${board_tree}/aml_encrypt_g12b" | sha256sum -c -

mkdir -p "${output_dir}" "${work_dir}"
# The media and USB images are side effects of the u-boot.bin bootmk rule.
# Remove all four first so make can never accept a raw payload while leaving
# an absent or stale wrapper behind.
rm -f \
  "${output_dir}/u-boot.bin" \
  "${output_dir}/u-boot.bin.sd.bin" \
  "${output_dir}/u-boot.bin.usb.bl2" \
  "${output_dir}/u-boot.bin.usb.tpl"
make -C "${board_tree}" \
  BL33="${fd}" \
  COMPRESS_LZ4=1 \
  O="${output_dir}" \
  TMP="${work_dir}" \
  "${output_dir}/u-boot.bin"

encrypted_size=$(stat -c %s "${work_dir}/bl33.bin.enc")
fip="${output_dir}/u-boot.bin"
media_image="${output_dir}/u-boot.bin.sd.bin"
emmc_image="${output_dir}/u-boot.bin.sd.emmc.bin"
test -s "${media_image}"

# bootmk creates the SD/eMMC form by prepending a 512-byte BL2 envelope.  The
# FIP table signature is consequently at 0x10210, not 0x10010 as in the raw
# payload.  Flashing the latter makes the G12B BootROM fail with CHK:1F.
test "$(stat -c %s "${media_image}")" -eq "$(( $(stat -c %s "${fip}") + 512 ))"
test "$(od -An -tx1 -j $((0x10210)) -N 8 "${media_image}" | tr -d ' \n')" = \
  "010064aa78563412"

fip_size=$(stat -c %s "${media_image}")
if (( encrypted_size > 4194304 || fip_size > 16777216 )); then
  echo "packaged image exceeds the validated SD boot area" >&2
  exit 1
fi

# Hardware boot partitions are block devices.  Keep the vendor bootmk output
# byte-exact, and produce a separate zero-padded sector image for verified
# block writes.
cp "${media_image}" "${emmc_image}"
truncate -s "$(( (fip_size + 511) / 512 * 512 ))" "${emmc_image}"
test "$(( $(stat -c %s "${emmc_image}") % 512 ))" -eq 0
cmp -n "${fip_size}" "${media_image}" "${emmc_image}"

sha256sum \
  "${fd}" \
  "${work_dir}/bl33.bin.enc" \
  "${fip}" \
  "${media_image}" \
  "${emmc_image}" \
  "${output_dir}/u-boot.bin.usb.bl2" \
  "${output_dir}/u-boot.bin.usb.tpl" \
  >"${output_dir}/SHA256SUMS"
cat "${output_dir}/SHA256SUMS"
