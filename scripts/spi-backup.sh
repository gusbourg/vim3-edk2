#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
host=${VIM3_SSH_HOST:?set VIM3_SSH_HOST=user@board-ip}
output_dir=${1:-"${project_root}/out/spi-backup"}
remote_dir=/tmp/vim3-spi-backup
expected_jedec=ef6018
expected_emmc_cid=150100424a5444345203c7564962c600
allow_emmc_root=${VIM3_ALLOW_EMMC_ROOT:-NO}
ssh_opts=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o UserKnownHostsFile=/dev/null
  -o StrictHostKeyChecking=accept-new
)

mkdir -p "${output_dir}"
case "${allow_emmc_root}" in
  YES|NO) ;;
  *) echo "VIM3_ALLOW_EMMC_ROOT must be YES or NO" >&2; exit 2 ;;
esac

ssh "${ssh_opts[@]}" "${host}" \
  "sudo bash -s -- '${remote_dir}' '${expected_jedec}' '${expected_emmc_cid}' '${allow_emmc_root}'" <<'REMOTE'
set -euo pipefail
remote_dir=$1
expected_jedec=$2
expected_emmc_cid=$3
allow_emmc_root=$4

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

mtd_matches=()
for path in /sys/class/mtd/mtd*; do
  test -f "${path}/size" || continue
  test "$(cat "${path}/size")" = "16777216" || continue
  test "$(cat "${path}/erasesize")" = "4096" || continue
  test "$(basename "$(readlink -f "${path}/device/driver")")" = "spi-nor" ||
    continue
  mtd_matches+=("${path##*/}")
done
mtd=${mtd_matches[0]:-}
test "${#mtd_matches[@]}" -eq 1
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
emmc_of_node=$(readlink -f "/sys/block/${emmc}/device/of_node")
test -n "${emmc_of_node}"
test "$(od -An -tx1 -N4 "${emmc_of_node}/bus-width" | tr -d ' ')" = "00000004"

rm -rf -- "${remote_dir}"
mkdir -m 0700 -- "${remote_dir}"
dd if="${device}" of="${remote_dir}/vim3-spi-a.bin" bs=1M iflag=fullblock status=progress
sync
dd if="${device}" of="${remote_dir}/vim3-spi-b.bin" bs=1M iflag=fullblock status=progress
sync
dd if="${device}" of="${remote_dir}/vim3-spi-c.bin" bs=1M iflag=fullblock status=progress
cmp -s "${remote_dir}/vim3-spi-a.bin" "${remote_dir}/vim3-spi-b.bin"
cmp -s "${remote_dir}/vim3-spi-a.bin" "${remote_dir}/vim3-spi-c.bin"
test "$(stat -c %s "${remote_dir}/vim3-spi-a.bin")" = "16777216"

{
  printf 'model=%s\n' "$(tr -d '\0' </proc/device-tree/model)"
  printf 'root=%s\n' "$(findmnt -no SOURCE /)"
  printf 'mtd=%s\n' "${device}"
  printf 'mtd_name=%s\n' "$(cat "/sys/class/mtd/${mtd}/name")"
  printf 'size=%s\n' "$(cat "/sys/class/mtd/${mtd}/size")"
  printf 'erasesize=%s\n' "$(cat "/sys/class/mtd/${mtd}/erasesize")"
  printf 'jedec_id=%s\n' "${expected_jedec}"
  printf 'emmc=%s\n' "/dev/${emmc}"
  printf 'emmc_cid=%s\n' "$(cat "/sys/block/${emmc}/device/cid")"
  uname -a
  (
    cd "${remote_dir}"
    sha256sum vim3-spi-a.bin vim3-spi-b.bin vim3-spi-c.bin
  )
} >"${remote_dir}/manifest.txt"
chmod 0644 "${remote_dir}"/*
chown "${SUDO_UID}:${SUDO_GID}" "${remote_dir}" "${remote_dir}"/*
REMOTE

scp "${ssh_opts[@]}" \
  "${host}:${remote_dir}/vim3-spi-a.bin" \
  "${host}:${remote_dir}/vim3-spi-b.bin" \
  "${host}:${remote_dir}/vim3-spi-c.bin" \
  "${host}:${remote_dir}/manifest.txt" \
  "${output_dir}/"

cmp "${output_dir}/vim3-spi-a.bin" "${output_dir}/vim3-spi-b.bin"
cmp "${output_dir}/vim3-spi-a.bin" "${output_dir}/vim3-spi-c.bin"
(
  cd "${output_dir}"
  sha256sum -c <(awk '/vim3-spi-[abc]\.bin$/ {print}' manifest.txt)
)
ssh "${ssh_opts[@]}" "${host}" "sudo rm -rf -- '${remote_dir}'"
echo "Three matching off-board SPI backups: ${output_dir}"
