#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

host=${VIM3_SSH_HOST:?set VIM3_SSH_HOST=user@board-ip}
image=${1:-}
expected_hash=${2:-}
remote_image=/tmp/vim3-spi-restore.bin
expected_jedec=ef6018
expected_emmc_cid=150100424a5444345203c7564962c600
allow_emmc_root=${VIM3_ALLOW_EMMC_ROOT:-NO}
ssh_opts=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o UserKnownHostsFile=/dev/null
  -o StrictHostKeyChecking=accept-new
)

if [[ -z "${image}" || -z "${expected_hash}" ]]; then
  echo "usage: VIM3_SPI_RESTORE=YES $0 IMAGE EXPECTED_SHA256" >&2
  exit 2
fi
test "${VIM3_SPI_RESTORE:-}" = YES
case "${allow_emmc_root}" in
  YES|NO) ;;
  *) echo "VIM3_ALLOW_EMMC_ROOT must be YES or NO" >&2; exit 2 ;;
esac
test -f "${image}"
test "$(stat -c %s "${image}")" = "16777216"
echo "${expected_hash}  ${image}" | sha256sum -c -

scp "${ssh_opts[@]}" "${image}" "${host}:${remote_image}"
ssh "${ssh_opts[@]}" "${host}" \
  "sudo bash -s -- '${remote_image}' '${expected_hash}' '${expected_jedec}' '${expected_emmc_cid}' '${allow_emmc_root}'" <<'REMOTE'
set -euo pipefail
image=$1
expected_hash=$2
expected_jedec=$3
expected_emmc_cid=$4
allow_emmc_root=$5
read_a=/tmp/vim3-spi-restore-preflight-a.bin
read_b=/tmp/vim3-spi-restore-preflight-b.bin
read_c=/tmp/vim3-spi-restore-preflight-c.bin
readback_a=/tmp/vim3-spi-restore-readback-a.bin
readback_b=/tmp/vim3-spi-restore-readback-b.bin
trap 'rm -f -- "${image}" "${read_a}" "${read_b}" "${read_c}" "${readback_a}" "${readback_b}"' EXIT

test "$(tr -d '\0' </proc/device-tree/model)" = "Khadas VIM3"
case "$(uname -r)" in
  6.12.*)
    echo "Refusing: VIM3 full-NOR reads were unstable with the 6.12 kernel." >&2
    exit 1
    ;;
esac

root_source=$(readlink -f "$(findmnt -no SOURCE /)")
case "${root_source}" in
  /dev/sd*) ;;
  /dev/mmcblk*p*)
    root_parent=$(lsblk -no PKNAME "${root_source}")
    if [[ -f "/sys/class/block/${root_parent}/device/type" ]] &&
       [[ "$(cat "/sys/class/block/${root_parent}/device/type")" = SD ]] &&
       [[ "$(blkid -s UUID -o value "${root_source}")" =
          "fcc90ce5-ad5c-4b30-a490-e7cc068e8d7d" ]]; then
      :
    elif [[ "${allow_emmc_root}" = YES ]] &&
         [[ -f "/sys/class/block/${root_parent}/device/cid" ]] &&
         [[ "$(cat "/sys/class/block/${root_parent}/device/cid")" =
            "${expected_emmc_cid}" ]]; then
      echo "WARNING: explicitly allowing the known VIM3 eMMC as root."
    else
      echo "Refusing: MMC root is neither the known SD root nor explicitly allowed eMMC." >&2
      exit 1
    fi
    ;;
  *) echo "Refusing: maintenance root is not removable media." >&2; exit 1 ;;
esac
echo "${expected_hash}  ${image}" | sha256sum -c -

mtd_matches=()
for path in /sys/class/mtd/mtd*; do
  test -f "${path}/size" || continue
  test "$(cat "${path}/size")" = "16777216" || continue
  test "$(cat "${path}/erasesize")" = "4096" || continue
  test "$(basename "$(readlink -f "${path}/device/driver")")" = "spi-nor" ||
    continue
  mtd_matches+=("${path##*/}")
done
test "${#mtd_matches[@]}" -eq 1
mtd=${mtd_matches[0]}
device=/dev/${mtd}

found_jedec=
for path in /sys/bus/spi/devices/spi*/spi-nor/jedec_id; do
  test -f "${path}" || continue
  test "$(tr -d ' \n' <"${path}")" = "${expected_jedec}" || continue
  found_jedec=yes
done
for path in /sys/kernel/debug/spi-nor/spi*/params; do
  test -f "${path}" || continue
  test "$(awk '/^id/{print $2 $3 $4}' "${path}")" = "${expected_jedec}" ||
    continue
  found_jedec=yes
done
test "${found_jedec}" = yes

emmc=
for path in /sys/block/mmcblk*; do
  test -f "${path}/device/cid" || continue
  test "$(cat "${path}/device/cid")" = "${expected_emmc_cid}" || continue
  emmc=${path##*/}
done
test -n "${emmc}"

for destination in "${read_a}" "${read_b}" "${read_c}"; do
  dd if="${device}" of="${destination}" bs=1M iflag=fullblock status=progress
done
cmp -s "${read_a}" "${read_b}"
cmp -s "${read_a}" "${read_c}"
echo "PASS: three full preflight NOR reads are identical."

flashcp=
for candidate in /usr/sbin/flashcp /sbin/flashcp; do
  test -x "${candidate}" || continue
  flashcp=${candidate}
  break
done
test -n "${flashcp}"
"${flashcp}" -v "${image}" "${device}"

dd if="${device}" of="${readback_a}" bs=1M iflag=fullblock status=progress
sync
dd if="${device}" of="${readback_b}" bs=1M iflag=fullblock status=progress
echo "${expected_hash}  ${readback_a}" | sha256sum -c -
echo "${expected_hash}  ${readback_b}" | sha256sum -c -
cmp -s "${readback_a}" "${readback_b}"
echo "PASS: two independent full-NOR read-backs match the restore image."
REMOTE
