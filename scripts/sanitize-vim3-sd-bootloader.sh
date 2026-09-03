#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

expected_model='Khadas VIM3'
expected_recovery_root_uuid='4ff4a0b3-0e47-40c1-b65e-785fe1c4d271'
expected_sd_cid='035344534832353680864d41dc019200'
expected_sd_bytes='255869321216'
expected_p1_start_lba='2048'
expected_esp_start_lba='34816'
expected_aml_boot_last_lba='32767'
expected_p1_bytes='16777216'
expected_p1_zero_sha='080acf35a507ac9849cfcba47dc2ad83e01b75663a516279c8b9d243b719643e'
expected_prepartition_zero_sha='6f1171ee37292b5d35640d81e2aea663018b51cb4cd139a61515e40454223836'
expected_p1_partuuid='ae0cd1a0-01'
expected_p2_uuid='E085-365F'
expected_p3_uuid='fcc90ce5-ad5c-4b30-a490-e7cc068e8d7d'
state_dir=${VIM3_SD_SANITIZE_STATE_DIR:-/var/lib/vim3-phase6-recovery}
p1_backup=${state_dir}/sd-mmcblk0p1-before-sanitize.bin
p1_checksum=${p1_backup}.sha256
prepartition_backup=${state_dir}/sd-prepartition-lba1-32767-before-sanitize.bin
prepartition_checksum=${prepartition_backup}.sha256
# The canonical Amlogic flash recipe writes the FIP from LBA 0 and historically
# placed the ESP at LBA 32768.  This Debian layout instead uses a sacrificial
# 16 MiB p1 at LBA 2048 and places the ESP at LBA 34816.  LBA 1..32767 is
# therefore the complete legacy Amlogic boot window; the separate p1 erase
# covers its remaining LBA 32768..34815 tail without touching the ESP.
prepartition_sectors=${expected_aml_boot_last_lba}
prepartition_bytes=$((prepartition_sectors * 512))

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

test "$(id -u)" = 0 || fail "run as root"
test "${VIM3_CONFIRM_SANITIZE_SD_BOOT:-}" = ERASE_STALE_SD_BOOTLOADER ||
  fail "set VIM3_CONFIRM_SANITIZE_SD_BOOT=ERASE_STALE_SD_BOOTLOADER"

model=$(tr -d '\0' </proc/device-tree/model)
case "${model}" in
  *"${expected_model}"*) ;;
  *) fail "unexpected board model: ${model}" ;;
esac

test "$(findmnt -no UUID /)" = "${expected_recovery_root_uuid}" ||
  fail "must run from the pinned USB recovery root"
test "$(cat /sys/block/mmcblk0/device/cid)" = "${expected_sd_cid}" ||
  fail "mmcblk0 is not the pinned 256 GB SD card"
test "$(blockdev --getsize64 /dev/mmcblk0)" = "${expected_sd_bytes}" ||
  fail "unexpected SD capacity"
test "$(blockdev --getsize64 /dev/mmcblk0p1)" = "${expected_p1_bytes}" ||
  fail "unexpected partition 1 size"
test "$(cat /sys/class/block/mmcblk0p1/start)" = "${expected_p1_start_lba}" ||
  fail "partition 1 does not start at pinned LBA ${expected_p1_start_lba}"
test "$(cat /sys/class/block/mmcblk0p2/start)" = "${expected_esp_start_lba}" ||
  fail "ESP does not start at pinned LBA ${expected_esp_start_lba}"
test "$(blkid -s PARTUUID -o value /dev/mmcblk0p1)" = "${expected_p1_partuuid}" ||
  fail "unexpected partition 1 identity"
test "$(blkid -s UUID -o value /dev/mmcblk0p2)" = "${expected_p2_uuid}" ||
  fail "unexpected SD ESP identity"
test "$(blkid -s UUID -o value /dev/mmcblk0p3)" = "${expected_p3_uuid}" ||
  fail "unexpected Debian SD root identity"
findmnt -rn -S /dev/mmcblk0p1 | grep -q . &&
  fail "partition 1 is mounted"

install -d -m 0700 "${state_dir}"
if test -e "${p1_backup}"; then
  test -f "${p1_checksum}" || fail "partition backup exists without its checksum"
  sha256sum -c "${p1_checksum}"
  test "$(stat -c %s "${p1_backup}")" = "${expected_p1_bytes}" ||
    fail "existing backup has wrong size"
  p1_backup_sha=$(sha256sum "${p1_backup}" | awk '{print $1}')
  p1_current_sha=$(sha256sum /dev/mmcblk0p1 | awk '{print $1}')
  test "${p1_current_sha}" = "${p1_backup_sha}" ||
    test "${p1_current_sha}" = "${expected_p1_zero_sha}" ||
    fail "partition 1 matches neither its backup nor the all-zero state"
  echo "Reusing the verified existing partition backup: ${p1_backup}"
else
  dd if=/dev/mmcblk0p1 of="${p1_backup}" bs=1M count=16 \
    iflag=fullblock conv=fsync status=progress
  test "$(stat -c %s "${p1_backup}")" = "${expected_p1_bytes}" ||
    fail "backup has wrong size"
  sha256sum "${p1_backup}" | tee "${p1_checksum}"
fi

if test -e "${prepartition_backup}"; then
  test -f "${prepartition_checksum}" ||
    fail "pre-partition backup exists without its checksum"
  sha256sum -c "${prepartition_checksum}"
  test "$(stat -c %s "${prepartition_backup}")" = "${prepartition_bytes}" ||
    fail "existing pre-partition backup has wrong size"
  prepartition_backup_sha=$(sha256sum "${prepartition_backup}" | awk '{print $1}')
  prepartition_current_sha=$(
    dd if=/dev/mmcblk0 bs=512 skip=1 count="${prepartition_sectors}" status=none |
      sha256sum |
      awk '{print $1}'
  )
  test "${prepartition_current_sha}" = "${prepartition_backup_sha}" ||
    test "${prepartition_current_sha}" = "${expected_prepartition_zero_sha}" ||
    fail "pre-partition region matches neither its backup nor the all-zero state"
  echo "Reusing the verified existing pre-partition backup: ${prepartition_backup}"
else
  dd if=/dev/mmcblk0 of="${prepartition_backup}" bs=512 skip=1 \
    count="${prepartition_sectors}" iflag=fullblock conv=fsync status=progress
  test "$(stat -c %s "${prepartition_backup}")" = "${prepartition_bytes}" ||
    fail "pre-partition backup has wrong size"
  sha256sum "${prepartition_backup}" | tee "${prepartition_checksum}"
fi

if test "${VIM3_SD_SANITIZE_BACKUP_ONLY:-0}" = 1; then
  echo "PASS: backup-only stage complete; the SD card was not changed."
  exit 0
fi

# Preserve sector 0 exactly: it contains the DOS partition table and disk
# signature. Erase the legacy Amlogic payload window, then the complete
# sacrificial partition 1. The ESP begins immediately after partition 1.
dd if=/dev/zero of=/dev/mmcblk0 bs=512 seek=1 \
  count="${prepartition_sectors}" conv=notrunc,fsync status=progress
dd if=/dev/zero of=/dev/mmcblk0p1 bs=1M count=16 \
  iflag=fullblock conv=fsync status=progress
sync
blockdev --flushbufs /dev/mmcblk0
cmp -n "${prepartition_bytes}" /dev/zero \
  <(dd if=/dev/mmcblk0 bs=512 skip=1 count="${prepartition_sectors}" status=none) ||
  fail "pre-partition zero read-back verification failed"
cmp -n "${expected_p1_bytes}" /dev/zero /dev/mmcblk0p1 ||
  fail "zero read-back verification failed"

echo "PASS: backed up and zeroed SD LBA1..32767 and /dev/mmcblk0p1."
echo "Preserved the SD partition table, ESP, Debian root, and swap partition."
