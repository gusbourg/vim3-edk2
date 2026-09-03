#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
target=${1:-DEBUG}
expected_fixture=${2:-}
fixture_arg=${3:-}
host=${VIM3_SSH_HOST:?set VIM3_SSH_HOST=user@board-ip}
if [[ -n "${fixture_arg}" ]]; then
  fixture=$(realpath -- "${fixture_arg}")
else
  fixture="${project_root}/out/${target}/fip/u-boot.bin.sd.emmc.bin"
fi
remote_fixture=/tmp/vim3-edk2-validated-media.bin
ssh_opts=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o UserKnownHostsFile=/dev/null
  -o StrictHostKeyChecking=accept-new
)

if [[ -z "${expected_fixture}" ]]; then
  echo "usage: VIM3_PROMOTE_EMMC=YES $0 [DEBUG|RELEASE] EXPECTED_SHA256 [FIXTURE]" >&2
  exit 2
fi
test "${VIM3_PROMOTE_EMMC:-}" = YES
test -s "${fixture}"
test "$(( $(stat -c %s "${fixture}") % 512 ))" -eq 0
echo "${expected_fixture}  ${fixture}" | sha256sum -c -

scp "${ssh_opts[@]}" "${fixture}" "${host}:${remote_fixture}"
ssh "${ssh_opts[@]}" "${host}" \
  "sudo bash -s -- '${remote_fixture}' '${expected_fixture}'" <<'REMOTE'
set -euo pipefail
fixture=$1
expected_fixture=$2
emmc_block=
for path in /sys/block/mmcblk*; do
  test -f "${path}/device/cid" || continue
  test "$(cat "${path}/device/cid")" = "150100424a5444345203c7564962c600" || continue
  emmc_block=${path##*/}
done
test -n "${emmc_block}"
emmc=/dev/${emmc_block}
boot0=/dev/${emmc_block}boot0
boot1=/dev/${emmc_block}boot1
fixture_size=$(stat -c %s "${fixture}")
tail_blocks=$((fixture_size / 512 - 1))
tail_bytes=$((tail_blocks * 512))

test "$(tr -d '\0' </proc/device-tree/model)" = "Khadas VIM3"
test "$(cat "/sys/block/${emmc_block}/device/cid")" = "150100424a5444345203c7564962c600"
test "$(blockdev --getsize64 "${emmc}")" = "31268536320"
# This is the legacy MBR layout, which embeds the Amlogic payload around a
# DOS MBR.  Refuse a GPT disk because sectors 1 onward contain the primary
# GPT and must never be overwritten by this script.
test "$(lsblk -dnro PTTYPE "${emmc}")" = dos
root_source=$(findmnt -no SOURCE /)
root_device=$(readlink -f "${root_source}")
root_parent=$(lsblk -no PKNAME "${root_device}")
test -n "${root_parent}"
test "${root_parent}" != "${emmc_block}"
test "$(findmnt -no UUID /)" = "e92a34bb-3bd5-457c-8816-6200a6e21fbf"
test "$(( fixture_size % 512 ))" -eq 0
test "${tail_blocks}" -gt 0
echo "${expected_fixture}  ${fixture}" | sha256sum -c -

# Preserve and later compare the disk-signature/reserved bytes and DOS
# partition table in the tail of sector zero.
dd if="${emmc}" of=/tmp/vim3-emmc-mbr-tail.bin \
  bs=1 skip=444 count=68 status=none

echo 0 >"/sys/block/${emmc_block}boot0/force_ro"
echo 0 >"/sys/block/${emmc_block}boot1/force_ro"
trap 'echo 1 >"/sys/block/${emmc_block}boot0/force_ro"; echo 1 >"/sys/block/${emmc_block}boot1/force_ro"' EXIT

# User area: canonical Amlogic MBR split.
dd if="${fixture}" of="${emmc}" bs=444 count=1 \
  conv=notrunc,fsync status=none
dd if="${fixture}" of="${emmc}" bs=512 skip=1 seek=1 \
  count="${tail_blocks}" conv=notrunc,fsync status=none

# Khadas's vendor recovery path stores the boot image starting at LBA 1 in
# each eMMC hardware boot partition.
for device in "${boot0}" "${boot1}"; do
  dd if="${fixture}" of="${device}" bs=512 skip=1 seek=1 \
    count="${tail_blocks}" conv=notrunc,fsync status=none
done
sync

cmp -n 444 "${fixture}" "${emmc}"
cmp -n "${tail_bytes}" \
  <(dd if="${fixture}" bs=512 skip=1 count="${tail_blocks}" status=none) \
  <(dd if="${emmc}" bs=512 skip=1 count="${tail_blocks}" status=none)
for device in "${boot0}" "${boot1}"; do
  cmp -n "${tail_bytes}" \
    <(dd if="${fixture}" bs=512 skip=1 count="${tail_blocks}" status=none) \
    <(dd if="${device}" bs=512 skip=1 count="${tail_blocks}" status=none)
done
cmp -n 68 /tmp/vim3-emmc-mbr-tail.bin \
  <(dd if="${emmc}" bs=1 skip=444 count=68 status=none)

test "$(sfdisk -d "${emmc}" | awk -v part="${emmc}p1" '$1 == part {print $4}')" = \
  "8192,"

rm -f "${fixture}" /tmp/vim3-emmc-mbr-tail.bin
echo "Verified eMMC user boot area, boot0, boot1, and preserved MBR tail."
REMOTE
