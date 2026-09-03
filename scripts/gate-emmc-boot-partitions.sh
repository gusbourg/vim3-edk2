#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
host=${VIM3_SSH_HOST:?set VIM3_SSH_HOST=user@board-ip}
output_dir=${1:-"${project_root}/out/emmc-boot-gate"}
expected_cid=150100424a5444345203c7564962c600
remote_dir=/tmp/vim3-emmc-boot-gate
ssh_opts=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o UserKnownHostsFile=/dev/null
  -o StrictHostKeyChecking=accept-new
)

test "${VIM3_EMMC_BOOT_GATE:-}" = YES
mkdir -p "${output_dir}"

ssh "${ssh_opts[@]}" "${host}" \
  "sudo bash -s -- '${remote_dir}' '${expected_cid}'" <<'REMOTE'
set -euo pipefail
remote_dir=$1
expected_cid=$2

test "$(tr -d '\0' </proc/device-tree/model)" = "Khadas VIM3"
case "$(findmnt -no SOURCE /)" in /dev/sd*) ;; *) exit 1 ;; esac
test "$(findmnt -no UUID /)" = "e92a34bb-3bd5-457c-8816-6200a6e21fbf"

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

rm -rf -- "${remote_dir}"
mkdir -m 0700 -- "${remote_dir}"
dd if="${emmc}" of="${remote_dir}/user-first1MiB.bin" \
  bs=1M count=1 iflag=fullblock status=none
dd if="${boot0}" of="${remote_dir}/boot0.bin" \
  bs=1M count=4 iflag=fullblock status=none
dd if="${boot1}" of="${remote_dir}/boot1.bin" \
  bs=1M count=4 iflag=fullblock status=none

{
  printf 'model=%s\n' "$(tr -d '\0' </proc/device-tree/model)"
  printf 'root=%s\n' "$(findmnt -no SOURCE /)"
  printf 'emmc=%s\n' "${emmc}"
  printf 'emmc_cid=%s\n' "$(cat "/sys/block/${emmc_block}/device/cid")"
  sfdisk -d "${emmc}"
  sha256sum \
    "${remote_dir}/user-first1MiB.bin" \
    "${remote_dir}/boot0.bin" \
    "${remote_dir}/boot1.bin"
} >"${remote_dir}/manifest-before.txt"

# Preserve the canonical Amlogic MBR tail (bytes 444..511), but remove every
# user-area boot byte the G12 BootROM could consume before the first partition.
dd if=/dev/zero of="${emmc}" bs=1 count=444 conv=notrunc status=none
dd if=/dev/zero of="${emmc}" bs=512 seek=1 count=2047 \
  conv=notrunc,fsync status=none
sync

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
test "$(sfdisk -d "${emmc}" | awk -v part="${emmc}p1" \
  '$1 == part {print $4}')" = "8192,"

chmod 0644 "${remote_dir}"/*
chown "${SUDO_UID}:${SUDO_GID}" "${remote_dir}" "${remote_dir}"/*
REMOTE

scp "${ssh_opts[@]}" \
  "${host}:${remote_dir}/user-first1MiB.bin" \
  "${host}:${remote_dir}/boot0.bin" \
  "${host}:${remote_dir}/boot1.bin" \
  "${host}:${remote_dir}/manifest-before.txt" \
  "${output_dir}/"

(
  cd "${output_dir}"
  sha256sum -c <(
    sed -n \
      -e 's#  .*/user-first1MiB.bin$#  user-first1MiB.bin#p' \
      -e 's#  .*/boot0.bin$#  boot0.bin#p' \
      -e 's#  .*/boot1.bin$#  boot1.bin#p' \
      manifest-before.txt
  )
)
ssh "${ssh_opts[@]}" "${host}" "sudo rm -rf -- '${remote_dir}'"
echo "User-area boot payload removed and boot-partition backups copied off-board."
echo "Cold-power-cycle the VIM3 now; success proves boot0/boot1-only startup."
