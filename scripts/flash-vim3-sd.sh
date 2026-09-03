#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
target=${1:-DEBUG}
host=${VIM3_SSH_HOST:?set VIM3_SSH_HOST=user@board-ip}
expected_cid=035344534832353680864d41dc019200
expected_size=255869321216
fip="${project_root}/out/${target}/fip/u-boot.bin.sd.validated-media.bin"
expected_fip=071954847b59ba1f4ad25134fffc71b5b148c3eabe604f9b1c7fb5d326496d67
fd="${project_root}/out/${target}/VIM3_EFI.fd"
startup_nsh="${project_root}/Platform/Khadas/Vim3/Boot/startup.nsh"
remote_fip=/tmp/vim3-edk2-fip.bin
remote_fd=/tmp/VIM3_EFI.fd
remote_startup_nsh=/tmp/startup.nsh
expected_root=/dev/sdb2
expected_root_uuid=e92a34bb-3bd5-457c-8816-6200a6e21fbf
ssh_opts=(-o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=accept-new)

test -s "${fip}"
test -s "${fd}"
test -s "${startup_nsh}"
echo "${expected_fip}  ${fip}" | sha256sum -c -
test "$(od -An -tx1 -j $((0x10210)) -N 8 "${fip}" | tr -d ' \n')" = \
  "010064aa78563412"

identity=$(
  ssh "${ssh_opts[@]}" "${host}" \
    "bash -s -- '${expected_cid}'" <<'REMOTE'
set -e
expected_cid=$1
tr -d '\0' </proc/device-tree/model
echo
for path in /sys/block/mmcblk*; do
  test -f "${path}/device/cid" || continue
  test "$(cat "${path}/device/cid")" = "${expected_cid}" || continue
  echo "/dev/${path##*/}"
  cat "${path}/device/cid"
  echo "$(( $(cat "${path}/size") * 512 ))"
done
REMOTE
)
mapfile -t identity_lines <<<"${identity}"
test "${identity_lines[0]}" = "Khadas VIM3"
device=${identity_lines[1]}
test "${device#/dev/mmcblk}" != "${device}"
test "${identity_lines[2]}" = "${expected_cid}"
test "${identity_lines[3]}" = "${expected_size}"

scp "${ssh_opts[@]}" "${fip}" "${host}:${remote_fip}"
scp "${ssh_opts[@]}" "${fd}" "${host}:${remote_fd}"
scp "${ssh_opts[@]}" "${startup_nsh}" "${host}:${remote_startup_nsh}"

ssh "${ssh_opts[@]}" "${host}" \
  "sudo bash -s -- '${device}' '${remote_fip}' '${remote_fd}' '${remote_startup_nsh}' '${expected_root}' '${expected_root_uuid}'" <<'REMOTE'
set -euo pipefail
device=$1
fip=$2
fd=$3
startup_nsh=$4
expected_root=$5
expected_root_uuid=$6
block=${device#/dev/}

test "$(cat "/sys/block/${block}/device/cid")" = 035344534832353680864d41dc019200
test "$(blockdev --getsize64 "${device}")" = 255869321216
test "$(findmnt -no SOURCE /)" = "${expected_root}"
test "$(findmnt -no UUID /)" = "${expected_root_uuid}"

kernel_release=$(uname -r)
kernel="/boot/vmlinuz-${kernel_release}"
initrd="/boot/initrd.img-${kernel_release}"
test -s "${kernel}"
test -s "${initrd}"

umount "${device}"p1 2>/dev/null || true
umount "${device}"p2 2>/dev/null || true
wipefs --all "${device}"

sfdisk --wipe always "${device}" <<'SFDISK'
label: dos
unit: sectors

start=32768, type=c, bootable
SFDISK

partprobe "${device}"
udevadm settle
mkfs.vfat -F 32 -n VIM3ESP "${device}"p1

# Canonical Amlogic recipe: preserve bytes 444-511 containing the MBR tail.
dd if="${fip}" of="${device}" bs=444 count=1 conv=fsync,notrunc status=none
dd if="${fip}" of="${device}" bs=512 skip=1 seek=1 conv=fsync,notrunc status=none
sync

cmp -n 444 "${fip}" "${device}"
fip_size=$(stat -c %s "${fip}")
tail_size=$((fip_size - 512))
cmp -n "${tail_size}" <(tail -c +513 "${fip}") <(dd if="${device}" bs=512 skip=1 status=none)

mkdir -p /mnt/vim3-edk2-esp
mount "${device}"p1 /mnt/vim3-edk2-esp
mkdir -p /mnt/vim3-edk2-esp/EFI/BOOT
install -m 0644 "${fd}" /mnt/vim3-edk2-esp/VIM3_EFI.fd
install -m 0644 "${startup_nsh}" /mnt/vim3-edk2-esp/startup.nsh
install -m 0644 "${kernel}" /mnt/vim3-edk2-esp/VIM3_LINUX.EFI
install -m 0644 "${initrd}" /mnt/vim3-edk2-esp/VIM3_INITRD
printf '%s\n' \
  'Khadas VIM3 EDK2 bring-up SD' \
  "Fallback EFI-stub kernel: ${kernel_release}" \
  "Root filesystem UUID: ${expected_root_uuid}" \
  'The UEFI Shell is embedded in BL33; startup.nsh is the SD fallback path.' \
  >/mnt/vim3-edk2-esp/README.TXT
sha256sum \
  /mnt/vim3-edk2-esp/VIM3_EFI.fd \
  /mnt/vim3-edk2-esp/VIM3_LINUX.EFI \
  /mnt/vim3-edk2-esp/VIM3_INITRD \
  >/mnt/vim3-edk2-esp/SHA256SUMS
umount /mnt/vim3-edk2-esp
rmdir /mnt/vim3-edk2-esp
rm -f "${fip}" "${fd}" "${startup_nsh}"
REMOTE

echo "Flashed ${fip} to ${host}:${device} after CID and size verification."
