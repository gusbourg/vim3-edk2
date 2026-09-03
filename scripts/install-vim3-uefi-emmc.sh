#!/usr/bin/env bash
# Install the VIM3 EDK2/UEFI firmware into the eMMC boot partitions.
#
# Run this ON THE BOARD, as root, from a Linux that is already booted on it.
#
# This is the beta-tester counterpart of promote-vim3-emmc-boot-partitions.sh.
# It keeps that script's safety properties - board model check, payload hash,
# one boot partition at a time with read-back verification, LBA 0 and the
# unused tail preserved, and proof that the eMMC user area (your GPT, ESP and
# rootfs) was never opened for writing - but identifies the eMMC generically
# instead of pinning one board's CID, and takes a backup first.
#
# SPDX-License-Identifier: BSD-2-Clause-Patent

set -euo pipefail
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

usage() {
  cat >&2 <<'EOF'
usage: sudo install-vim3-uefi-emmc.sh [--yes] PAYLOAD [EXPECTED_SHA256]

  PAYLOAD           u-boot.bin.sd.emmc.bin from the release package.
                    Do NOT substitute u-boot.bin or u-boot.bin.sd.bin - the
                    suffixes are different images and the BootROM rejects the
                    wrong one with CHK:1F.
  EXPECTED_SHA256   optional; if given, the payload must match it. The release
                    package ships SHA256SUMS - use it.
  --yes             skip the interactive confirmation.

Backups of the current boot partitions are written next to the payload before
anything is modified, so the vendor bootloader can be restored.
EOF
  exit 2
}

assume_yes=no
if [[ ${1:-} == --yes ]]; then assume_yes=yes; shift; fi
payload=${1:-}; expected_sha=${2:-}
[[ -n ${payload} ]] || usage
[[ $(id -u) -eq 0 ]] || { echo "must run as root" >&2; exit 1; }

# ---- board identity ------------------------------------------------------
# DeviceTree exposes the model directly; under our own ACPI firmware there is
# no /proc/device-tree, so fall back to SMBIOS.
model=
if [[ -r /proc/device-tree/model ]]; then
  model=$(tr -d '\0' </proc/device-tree/model)
elif [[ -r /sys/class/dmi/id/product_name ]]; then
  model=$(cat /sys/class/dmi/id/product_name)
fi
case "${model}" in
  *"VIM3"*) ;;
  *) echo "refusing: board model '${model:-unknown}' is not a Khadas VIM3" >&2; exit 1 ;;
esac

# ---- payload -------------------------------------------------------------
payload=$(realpath -- "${payload}")
[[ -s ${payload} ]] || { echo "payload is empty or missing" >&2; exit 1; }
payload_size=$(stat -c %s "${payload}")
if (( payload_size % 512 != 0 )); then
  echo "refusing: payload is not a whole number of 512-byte sectors" >&2
  echo "  (that is the signature of the wrong file - use u-boot.bin.sd.emmc.bin)" >&2
  exit 1
fi
actual_sha=$(sha256sum "${payload}" | awk '{print $1}')
if [[ -n ${expected_sha} && ${expected_sha} != "${actual_sha}" ]]; then
  echo "refusing: payload sha256 mismatch" >&2
  echo "  expected ${expected_sha}" >&2
  echo "  actual   ${actual_sha}" >&2
  exit 1
fi

# ---- locate the eMMC -----------------------------------------------------
# Only eMMC has boot0/boot1 hardware boot partitions, so this distinguishes it
# from an SD card without needing to know either device's number, which is not
# stable across boots or between DT and ACPI mode.
emmc_block=
for path in /sys/block/mmcblk*; do
  name=${path##*/}
  [[ -e /dev/${name}boot0 && -e /dev/${name}boot1 ]] || continue
  if [[ -n ${emmc_block} ]]; then
    echo "refusing: more than one device has boot partitions (${emmc_block}, ${name})" >&2
    exit 1
  fi
  emmc_block=${name}
done
[[ -n ${emmc_block} ]] || { echo "refusing: no eMMC with boot0/boot1 found" >&2; exit 1; }

emmc=/dev/${emmc_block}
boot0=/dev/${emmc_block}boot0
boot1=/dev/${emmc_block}boot1
boot_size=$(blockdev --getsize64 "${boot0}")
[[ $(blockdev --getsize64 "${boot1}") == "${boot_size}" ]] || {
  echo "refusing: boot0 and boot1 differ in size" >&2; exit 1; }

# The BootROM reads the wrapped image from LBA 1; LBA 0 and the tail stay put.
tail_blocks=$((payload_size / 512 - 1))
tail_bytes=$((tail_blocks * 512))
(( tail_blocks > 0 )) || { echo "refusing: payload too small" >&2; exit 1; }
(( payload_size <= boot_size )) || {
  echo "refusing: payload (${payload_size}) exceeds boot partition (${boot_size})" >&2
  exit 1; }

cid=$(cat "/sys/block/${emmc_block}/device/cid" 2>/dev/null || echo unknown)
cat <<EOF

  board            ${model}
  eMMC             ${emmc}  ($(numfmt --to=iec "$(blockdev --getsize64 "${emmc}")"))
  eMMC CID         ${cid}
  boot partitions  ${boot0}, ${boot1}  ($(numfmt --to=iec "${boot_size}") each)
  payload          ${payload}
  payload sha256   ${actual_sha}

This replaces the bootloader in BOTH eMMC boot partitions.  Your GPT, ESP and
root filesystem in the eMMC user area are NOT written.  If it goes wrong the
board still enters MaskROM over USB-C, which cannot be bricked from software.

EOF
if [[ ${assume_yes} != yes ]]; then
  read -r -p "Type INSTALL to continue: " reply
  [[ ${reply} == INSTALL ]] || { echo "aborted"; exit 1; }
fi

# ---- backup --------------------------------------------------------------
stamp=$(date +%Y%m%d-%H%M%S)
backup_dir=$(dirname -- "${payload}")/vim3-bootpart-backup-${stamp}
mkdir -p "${backup_dir}"
for dev in "${boot0}" "${boot1}"; do
  out=${backup_dir}/$(basename -- "${dev}").img
  dd if="${dev}" of="${out}" bs=1M iflag=fullblock status=none
  echo "  backed up ${dev} -> ${out}"
done
sha256sum "${backup_dir}"/*.img > "${backup_dir}/SHA256SUMS"
echo "  backup written to ${backup_dir}"

# ---- prove the user area is untouched ------------------------------------
user_prefix_before=$(dd if="${emmc}" bs=1M count=4 iflag=fullblock status=none | sha256sum | awk '{print $1}')

echo 0 >"/sys/block/${emmc_block}boot0/force_ro"
echo 0 >"/sys/block/${emmc_block}boot1/force_ro"
restore_force_ro() {
  echo 1 >"/sys/block/${emmc_block}boot0/force_ro" 2>/dev/null || true
  echo 1 >"/sys/block/${emmc_block}boot1/force_ro" 2>/dev/null || true
}
trap restore_force_ro EXIT

# ---- write, one partition at a time --------------------------------------
# boot0 is written AND read-back verified before boot1 is opened, so a failure
# part-way through always leaves one bootable copy.
for device in "${boot0}" "${boot1}"; do
  echo "  writing ${device}..."
  dd if="${payload}" of="${device}" bs=512 skip=1 seek=1 \
     count="${tail_blocks}" conv=notrunc,fsync status=none
  cmp -n "${tail_bytes}" \
    <(dd if="${payload}" bs=512 skip=1 count="${tail_blocks}" status=none) \
    <(dd if="${device}"  bs=512 skip=1 count="${tail_blocks}" status=none)
  echo "  verified ${device}"
done
sync

user_prefix_after=$(dd if="${emmc}" bs=1M count=4 iflag=fullblock status=none | sha256sum | awk '{print $1}')
if [[ ${user_prefix_after} != "${user_prefix_before}" ]]; then
  echo "ERROR: the eMMC user area changed - this should be impossible" >&2
  exit 1
fi

cat <<EOF

Done.  Both boot partitions carry the new firmware and read back correctly.
The eMMC user area is byte-identical (${user_prefix_after:0:16}...).

Reboot to run it.  To roll back:

  sudo bash -c 'echo 0 > /sys/block/${emmc_block}boot0/force_ro; \\
                echo 0 > /sys/block/${emmc_block}boot1/force_ro; \\
                dd if=${backup_dir}/${emmc_block}boot0.img of=${boot0} bs=1M conv=fsync; \\
                dd if=${backup_dir}/${emmc_block}boot1.img of=${boot1} bs=1M conv=fsync; \\
                sync'
EOF
