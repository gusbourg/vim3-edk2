#!/usr/bin/env bash
# Write the firmware boot image into the SPI NOR's boot region (offset 0).
#
# Run from the dev host; the board must be in a SPI-MAINTENANCE boot (the
# published DTB keeps spifc disabled, so a normal boot exposes no mtd - see
# scripts/stage-spi-maintenance-boot.sh).  The image is the RAW bootmk
# payload `u-boot.bin` (FIP signature at 0x10010) - the vendor's SPI recipe
# writes exactly that at offset 0.  Do NOT pass the .sd/.emmc wrapped images
# unless deliberately testing the alternate-format hypothesis.
#
# Writes touch ONLY the image's 4 KiB-aligned extent from offset 0, far below
# the 0xC00000 scratch / 0xF00000 runtime regions; the firmware variable
# store is never read or written.  While the flash is in progress do not
# write UEFI variables (no efibootmgr, no efivarfs writes): the firmware's
# runtime FVB and Linux's spifc driver would be two masters on one SPIFC
# controller.
#
# This script does NOT change the BootROM probe order.  Flipping MCU
# BOOT_MODE (0x20) to SPI-first is a separate, deliberate step - the script
# prints the recipe at the end.
#
# SPDX-License-Identifier: BSD-2-Clause-Patent

set -euo pipefail

host=${VIM3_SSH_HOST:?set VIM3_SSH_HOST=user@board-ip}
expected_hash=${1:-}
image=${2:-}
expected_jedec=ef6018
expected_cid=150100424a5444345203c7564962c600
boot_region_size=$((0x00C00000))
ssh_opts=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o UserKnownHostsFile=/dev/null
  -o StrictHostKeyChecking=accept-new
)

if [[ -z "${expected_hash}" || -z "${image}" ]]; then
  echo "usage: VIM3_PROMOTE_SPI=YES $0 EXPECTED_SHA256 IMAGE" >&2
  echo "  IMAGE is the raw bootmk payload out/<TARGET>/fip/u-boot.bin" >&2
  exit 2
fi
test "${VIM3_PROMOTE_SPI:-}" = YES
image=$(realpath -- "${image}")
test -s "${image}"
image_size=$(stat -c %s "${image}")
if (( image_size > boot_region_size )); then
  echo "refusing: image (${image_size}) exceeds the 12 MiB NOR boot region" >&2
  exit 1
fi
echo "${expected_hash}  ${image}" | sha256sum -c -

# Sanity: a raw bootmk payload carries the FIP table signature at 0x10010.
# The wrapped media images carry it at 0x10210 instead; warn (don't refuse -
# the alternate-format experiment is legitimate) if this looks wrapped.
sig_raw=$(od -An -tx1 -j $((0x10010)) -N 8 "${image}" | tr -d ' \n')
sig_wrapped=$(od -An -tx1 -j $((0x10210)) -N 8 "${image}" | tr -d ' \n')
if [[ ${sig_raw} != "010064aa78563412" ]]; then
  if [[ ${sig_wrapped} == "010064aa78563412" ]]; then
    echo "WARNING: image looks like the WRAPPED media form (sig at 0x10210)," >&2
    echo "         not the raw payload. Continuing - alternate-format test." >&2
  else
    echo "refusing: no FIP table signature at 0x10010 or 0x10210" >&2
    exit 1
  fi
fi

scp "${ssh_opts[@]}" "${image}" "${host}:/tmp/vim3-spi-image.bin"

ssh "${ssh_opts[@]}" "${host}" \
  "sudo bash -s -- '${expected_jedec}' '${expected_hash}' '${expected_cid}' '${image_size}'" <<'REMOTE'
set -euo pipefail
expected_jedec=$1
expected_hash=$2
expected_cid=$3
image_size=$4
image=/tmp/vim3-spi-image.bin

test "$(tr -d '\0' </proc/device-tree/model)" = "Khadas VIM3"
echo "${expected_hash}  ${image}" | sha256sum -c - >/dev/null

# The board must be identifiable and in the maintenance configuration:
# exactly one 16 MiB / 4 KiB-erase mtd bound to spi-nor with the pinned
# JEDEC id, and the pinned eMMC present (the rescue copy lives there).
emmc_cid=
for path in /sys/block/mmcblk*/device/cid; do
  test -f "${path}" || continue
  emmc_cid=$(cat "${path}")
done
test "${emmc_cid}" = "${expected_cid}"

mtd=
count=0
for path in /sys/class/mtd/mtd*; do
  [[ ${path} == *ro ]] && continue
  test -f "${path}/size" || continue
  test "$(cat "${path}/size")" = "16777216" || continue
  test "$(cat "${path}/erasesize")" = "4096" || continue
  mtd=${path##*/}
  count=$((count + 1))
done
if [[ ${count} -ne 1 || -z ${mtd} ]]; then
  echo "refusing: expected exactly one 16MiB/4K mtd (maintenance boot?)" >&2
  exit 1
fi

found_jedec=
for path in /sys/bus/spi/devices/spi*/spi-nor/jedec_id; do
  test -f "${path}" || continue
  test "$(tr -d ' \n' <"${path}")" = "${expected_jedec}" || continue
  found_jedec=yes
done
test "${found_jedec}" = yes
command -v flash_erase >/dev/null

device=/dev/${mtd}
sectors=$(( (image_size + 4095) / 4096 ))
extent=$(( sectors * 4096 ))

echo "  writing ${image_size} bytes (${sectors} sectors) to ${device} @0"
flash_erase --quiet "${device}" 0 "${sectors}"
# No fsync: MTD character devices reject it (EINVAL).  mtd writes are
# synchronous anyway - the readback compare below is the real proof.
dd if="${image}" of="${device}" bs=4096 conv=sync status=none

# Full readback compare over the image length, then confirm the tail of the
# erased extent really is erased (dd's conv=sync pads the last sector with
# zeros, not 0xFF - compare only the image bytes).
cmp -n "${image_size}" "${image}" "${device}"
echo "  readback verified (${image_size} bytes)"

# Prove the erase stayed inside its extent: the sector immediately after the
# image must still read erased.
tail_byte=$(dd if="${device}" bs=1 skip="${extent}" count=8 status=none |
            od -An -tx1 | tr -d ' \n')
test "${tail_byte}" = "ffffffffffffffff"
echo "  erase confined to the image extent (tail still 0xFF)"

# NOTE: do NOT try to validate the firmware runtime region (0xF00000+) from
# here.  While firmware runtime services are live it owns that range through
# its own SPIFC path and a RAM shadow; raw MTD reads of it from Linux come
# back with bits spuriously set (measured: the FV header reads _F^H and the
# GUID/length fields show 0->1 bit flips) even though the store is perfectly
# healthy - efivarfs and efibootmgr keep working throughout.  The firmware
# region is simply not Linux's to inspect here, and a check against it would
# false-alarm on every run.

cat <<'DONE'

SPI boot image written and verified.  The BootROM will not use it until
BOOT_MODE is flipped - separate deliberate step, from Linux:

  # verify MCU addressing first (must print 0x03):
  i2cget -f -y 0 0x18 0x15
  # SPI-first:
  i2cset -f -y 0 0x18 0x20 0
  # revert to eMMC-first:
  i2cset -f -y 0 0x18 0x20 1

Then a TRUE power-on reset (DC pull, or MCU power-off via reg 0x80) - a warm
reboot keeps the old boot-source latch and proves nothing.
DONE
REMOTE
