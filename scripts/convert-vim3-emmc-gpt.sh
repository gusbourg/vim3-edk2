#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

host=${VIM3_SSH_HOST:?set VIM3_SSH_HOST=user@board-ip}
ssh_opts=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o UserKnownHostsFile=/dev/null
  -o StrictHostKeyChecking=accept-new
)

test "${VIM3_CONVERT_EMMC_GPT:-}" = YES

ssh "${ssh_opts[@]}" "${host}" "sudo bash -s" <<'REMOTE'
set -euo pipefail

expected_cid=150100424a5444345203c7564962c600
expected_root_uuid=e92a34bb-3bd5-457c-8816-6200a6e21fbf

test "$(tr -d '\0' </proc/device-tree/model)" = "Khadas VIM3"
test "$(findmnt -no UUID /)" = "${expected_root_uuid}"

emmc_block=
for path in /sys/block/mmcblk*; do
  test -f "${path}/device/cid" || continue
  test "$(cat "${path}/device/cid")" = "${expected_cid}" || continue
  emmc_block=${path##*/}
done
test -n "${emmc_block}"

emmc=/dev/${emmc_block}
boot0=/dev/${emmc_block}boot0
boot1=/dev/${emmc_block}boot1

test "$(blockdev --getsize64 "${emmc}")" = "31268536320"
test "$(blockdev --getsize64 "${boot0}")" = "4194304"
test "$(blockdev --getsize64 "${boot1}")" = "4194304"
test -z "$(lsblk -nrpo MOUNTPOINTS "${emmc}" | tr -d '[:space:]')"

boot0_before=$(sha256sum "${boot0}" | awk '{print $1}')
boot1_before=$(sha256sum "${boot1}" | awk '{print $1}')
test "${boot0_before}" = "${boot1_before}"

# The boot0/boot1 gate must have erased every user-area boot byte before this
# conversion. Refuse to create GPT unless that load-bearing experiment ran.
test "$(
  dd if="${emmc}" bs=1 count=444 status=none |
    LC_ALL=C tr -d '\000' |
    wc -c
)" -eq 0
test "$(
  dd if="${emmc}" bs=512 skip=1 count=2047 status=none |
    LC_ALL=C tr -d '\000' |
    wc -c
)" -eq 0

printf '%s\n' \
  'label: gpt' \
  'unit: sectors' \
  '' \
  "${emmc}p1 : start=2048, type=linux, name=\"Linux filesystem\"" |
  sfdisk --wipe always "${emmc}"
sync
blockdev --rereadpt "${emmc}"

test "$(sfdisk -d "${emmc}" | sed -n '1s/label: //p')" = gpt
test "$(sfdisk -d "${emmc}" | awk -v part="${emmc}p1" \
  '$1 == part {print $4}')" = "2048,"
test "$(sha256sum "${boot0}" | awk '{print $1}')" = "${boot0_before}"
test "$(sha256sum "${boot1}" | awk '{print $1}')" = "${boot1_before}"

echo "Converted ${emmc} to GPT; boot0/boot1 hashes are unchanged."
sfdisk -d "${emmc}"
REMOTE
