#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
expected_fixture=${1:-}
fixture_arg=${2:-}
host=${VIM3_SSH_HOST:?set VIM3_SSH_HOST=user@board-ip}
remote_fixture=/tmp/vim3-edk2-validated-media.bin
ssh_opts=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o UserKnownHostsFile=/dev/null
  -o StrictHostKeyChecking=accept-new
)

if [[ -z "${expected_fixture}" || -z "${fixture_arg}" ]]; then
  echo "usage: VIM3_PROMOTE_EMMC_BOOT_PARTITIONS=YES $0 EXPECTED_SHA256 FIXTURE" >&2
  exit 2
fi

fixture=$(realpath -- "${fixture_arg}")
test "${VIM3_PROMOTE_EMMC_BOOT_PARTITIONS:-}" = YES
test -s "${fixture}"
test "$(( $(stat -c %s "${fixture}") % 512 ))" -eq 0
echo "${expected_fixture}  ${fixture}" | sha256sum -c -

scp "${ssh_opts[@]}" "${fixture}" "${host}:${remote_fixture}"
ssh "${ssh_opts[@]}" "${host}" \
  "sudo bash -s -- '${remote_fixture}' '${expected_fixture}'" <<'REMOTE'
set -euo pipefail
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

fixture=$1
expected_fixture=$2
expected_cid=150100424a5444345203c7564962c600
expected_emmc_root_uuid=e92a34bb-3bd5-457c-8816-6200a6e21fbf
expected_usb_root_uuid=4ff4a0b3-0e47-40c1-b65e-785fe1c4d271
# The USB stick was reinstalled with Armbian Resolute:
expected_usb2_root_uuid=0f3fde59-c1b6-40c4-afe6-40ecaa2baeef
expected_sd_root_uuid=fcc90ce5-ad5c-4b30-a490-e7cc068e8d7d
expected_esp_partuuid=b4231214-3703-41a8-96df-d6514381b025

test "$(tr -d '\0' </proc/device-tree/model)" = "Khadas VIM3"
root_uuid=$(findmnt -no UUID /)
case "${root_uuid}" in
  "${expected_emmc_root_uuid}"|"${expected_usb_root_uuid}"|"${expected_usb2_root_uuid}"|"${expected_sd_root_uuid}") ;;
  *) echo "refusing unexpected root filesystem UUID: ${root_uuid}" >&2; exit 1 ;;
esac
echo "${expected_fixture}  ${fixture}" | sha256sum -c -

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
fixture_size=$(stat -c %s "${fixture}")
tail_blocks=$((fixture_size / 512 - 1))
tail_bytes=$((tail_blocks * 512))

test "$(blockdev --getsize64 "${emmc}")" = "31268536320"
test "$(blockdev --getsize64 "${boot0}")" = "4194304"
test "$(blockdev --getsize64 "${boot1}")" = "4194304"
test "$(lsblk -dnro PTTYPE "${emmc}")" = gpt
test "$(blkid -s PARTUUID -o value "${emmc}p1")" = "${expected_esp_partuuid}"
test "${tail_blocks}" -gt 0

# This layout deliberately keeps the GPT user area independent of the Amlogic
# boot payload. Hash its first 4 MiB before and after promotion and never open
# it for writing. This permits guarded promotion while Linux itself runs from
# the eMMC root partition.
gpt_prefix_before=$(
  dd if="${emmc}" bs=1M count=4 iflag=fullblock status=none |
    sha256sum |
    awk '{print $1}'
)

echo 0 >"/sys/block/${emmc_block}boot0/force_ro"
echo 0 >"/sys/block/${emmc_block}boot1/force_ro"
restore_force_ro() {
  echo 1 >"/sys/block/${emmc_block}boot0/force_ro"
  echo 1 >"/sys/block/${emmc_block}boot1/force_ro"
}
trap restore_force_ro EXIT

# Keep one known-good boot partition intact until the other contains and
# read-back-verifies the new payload. BootROM consumes the wrapped image from
# LBA 1; LBA 0 and the unused tail remain untouched.
for device in "${boot0}" "${boot1}"; do
  dd if="${fixture}" of="${device}" bs=512 skip=1 seek=1 \
    count="${tail_blocks}" conv=notrunc,fsync status=none
  cmp -n "${tail_bytes}" \
    <(dd if="${fixture}" bs=512 skip=1 count="${tail_blocks}" status=none) \
    <(dd if="${device}" bs=512 skip=1 count="${tail_blocks}" status=none)
done
sync

gpt_prefix_after=$(
  dd if="${emmc}" bs=1M count=4 iflag=fullblock status=none |
    sha256sum |
    awk '{print $1}'
)
test "${gpt_prefix_after}" = "${gpt_prefix_before}"

printf 'fixture_sha256=%s\n' "${expected_fixture}"
printf 'gpt_prefix_sha256=%s\n' "${gpt_prefix_after}"
sha256sum "${boot0}" "${boot1}"
echo "Verified boot0 and boot1; the eMMC GPT user area was not written."
rm -f -- "${fixture}"
REMOTE
