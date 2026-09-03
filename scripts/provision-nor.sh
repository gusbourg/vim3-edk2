#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

host=${VIM3_SSH_HOST:?set VIM3_SSH_HOST=user@board-ip}
backup=${1:-}
expected_hash=${2:-}
expected_jedec=ef6018
ssh_opts=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o UserKnownHostsFile=/dev/null
  -o StrictHostKeyChecking=accept-new
)

if [[ -z "${backup}" || -z "${expected_hash}" ]]; then
  echo "usage: VIM3_PROVISION_NOR=YES $0 VERIFIED_BACKUP EXPECTED_SHA256" >&2
  exit 2
fi
test "${VIM3_PROVISION_NOR:-}" = YES
test -f "${backup}"
test "$(stat -c %s "${backup}")" = "16777216"
echo "${expected_hash}  ${backup}" | sha256sum -c -

ssh "${ssh_opts[@]}" "${host}" \
  "sudo bash -s -- '${expected_jedec}' '${expected_hash}'" <<'REMOTE'
set -euo pipefail
expected_jedec=$1
expected_backup_hash=$2

test "$(tr -d '\0' </proc/device-tree/model)" = "Khadas VIM3"
case "$(findmnt -no SOURCE /)" in /dev/sd*) ;; *) exit 1 ;; esac

mtd=
for path in /sys/class/mtd/mtd*; do
  test -f "${path}/size" || continue
  test "$(cat "${path}/size")" = "16777216" || continue
  test "$(cat "${path}/erasesize")" = "4096" || continue
  mtd=${path##*/}
done
test -n "${mtd}"

found_jedec=
for path in /sys/bus/spi/devices/spi*/spi-nor/jedec_id; do
  test -f "${path}" || continue
  test "$(tr -d ' \n' <"${path}")" = "${expected_jedec}" || continue
  found_jedec=yes
done
test "${found_jedec}" = yes

device=/dev/${mtd}
current_hash=$(dd if="${device}" bs=1M iflag=fullblock status=none | sha256sum |
               awk '{print $1}')
test "${current_hash}" = "${expected_backup_hash}"
command -v flash_erase >/dev/null

# Erase only the variable FV, FTW working, and FTW spare ranges.  EDK2 writes
# the authenticated-format headers on the next boot.  The future boot area,
# scratch reservation, protected gap, and board-data region are untouched.
flash_erase "${device}" 0x00f00000 64
flash_erase "${device}" 0x00f40000 2
flash_erase "${device}" 0x00f80000 64

verify_erased() {
  offset=$1
  size=$2
  non_ff_bytes=$(
    dd if="${device}" bs=1 skip="${offset}" count="${size}" status=none |
      LC_ALL=C tr -d '\377' |
      wc -c
  )
  test "${non_ff_bytes}" -eq 0
}
verify_erased $((0x00f00000)) $((0x00040000))
verify_erased $((0x00f40000)) $((0x00002000))
verify_erased $((0x00f80000)) $((0x00040000))
echo "Variable and FTW regions erased; boot EDK2 to format the store."
REMOTE
