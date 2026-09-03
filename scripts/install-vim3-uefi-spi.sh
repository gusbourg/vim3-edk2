#!/usr/bin/env bash
# Move the VIM3 UEFI firmware into the SPI NOR (SPI primary, eMMC rescue).
#
# Run this ON THE BOARD, as root, from a DeviceTree-mode Linux boot.  It is
# a TWO-STEP flow driven by the same script:
#
#   step 1 (normal boot):      stages a one-shot "SPI maintenance" reboot
#                              that exposes the NOR to Linux (the firmware's
#                              published device tree keeps spifc disabled -
#                              firmware owns that controller normally).
#   step 2 (maintenance boot): backs up the NOR boot region (THIS IS YOUR
#                              OOWOW BACKUP), flashes the raw u-boot.bin at
#                              offset 0, verifies it byte-for-byte, and only
#                              then flips the MCU to SPI-first boot.
#
# The script detects which step applies by whether the NOR is visible.
#
# WHAT YOU GAIN: firmware that survives ANY eMMC reflash (stock images,
# Krescue, dd) - OS storage becomes just storage.  Capsule updates write
# BOTH copies (SPI primary + eMMC rescue) from then on.
# WHAT YOU LOSE: oowow, which lives in the NOR boot region.  The backup this
# script takes restores it.
#
# SAFETY LADDER if anything goes wrong:
#   1. A bad/interrupted SPI image -> the BootROM falls through to the eMMC
#      rescue BY ITSELF.  The board still boots the same firmware.
#   2. Want eMMC-first back      -> sudo i2cset -f -y 0 0x18 0x20 1, then a
#      true power cycle (pull DC).
#   3. Floor                     -> MaskROM over USB-C; the board cannot be
#      bricked from software.
#
# SPDX-License-Identifier: BSD-2-Clause-Patent

set -euo pipefail
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

boot_region_size=$((0x00C00000))   # 12 MiB; firmware scratch/vars live above
entry_title='VIM3 SPI-NOR maintenance (one shot)'

usage() {
  cat >&2 <<'EOF'
usage: sudo install-vim3-uefi-spi.sh [--yes] PAYLOAD [EXPECTED_SHA256]

  PAYLOAD           the RAW u-boot.bin from the release package (firmware/
                    u-boot.bin).  NOT u-boot.bin.sd.emmc.bin - the wrapped
                    media image is for the eMMC installer only.
  EXPECTED_SHA256   optional; if given, the payload must match it (see
                    SHA256SUMS in the package).
  --yes             skip the interactive confirmations.

Run once from a normal DeviceTree-mode boot (stages a one-shot maintenance
reboot), reboot, then run it again with the same arguments to flash.
EOF
  exit 2
}

assume_yes=no
if [[ ${1:-} == --yes ]]; then assume_yes=yes; shift; fi
payload=${1:-}; expected_sha=${2:-}
[[ -n ${payload} ]] || usage
[[ $(id -u) -eq 0 ]] || { echo "must run as root" >&2; exit 1; }

# ---- board identity ------------------------------------------------------
# Both steps need a DeviceTree-mode boot: the maintenance tree is derived
# from the live published DTB, and the flash step runs inside that tree.
if [[ ! -r /proc/device-tree/model ]]; then
  cat >&2 <<'EOF'
refusing: no /proc/device-tree - this is an ACPI-mode boot.

The SPI install needs DeviceTree mode (both steps).  In the firmware setup
menu set  Device Manager -> VIM3 Platform Configuration -> OS Hardware
Description = Device Tree,  reboot into Linux, and run this again.  You can
switch back to ACPI after the install.
EOF
  exit 1
fi
model=$(tr -d '\0' </proc/device-tree/model)
case "${model}" in
  *"VIM3"*) ;;
  *) echo "refusing: board model '${model:-unknown}' is not a Khadas VIM3" >&2; exit 1 ;;
esac

command -v i2cget >/dev/null || { echo "need i2c-tools installed" >&2; exit 1; }
mcu_id=$(i2cget -f -y 0 0x18 0x15 2>/dev/null || echo none)
if [[ ${mcu_id} != 0x03 ]]; then
  echo "refusing: MCU DEVICE_NO reads '${mcu_id}', expected 0x03 (VIM3)" >&2
  echo "  (wrong board, or the MCU is not answering on i2c bus 0 @0x18)" >&2
  exit 1
fi

# ---- payload -------------------------------------------------------------
payload=$(realpath -- "${payload}")
[[ -s ${payload} ]] || { echo "payload is empty or missing" >&2; exit 1; }
payload_size=$(stat -c %s "${payload}")
(( payload_size <= boot_region_size )) || {
  echo "refusing: payload (${payload_size}) exceeds the 12 MiB NOR boot region" >&2
  exit 1; }
actual_sha=$(sha256sum "${payload}" | awk '{print $1}')
if [[ -n ${expected_sha} && ${expected_sha} != "${actual_sha}" ]]; then
  echo "refusing: payload sha256 mismatch" >&2
  echo "  expected ${expected_sha}" >&2
  echo "  actual   ${actual_sha}" >&2
  exit 1
fi

# A raw bootmk payload carries the FIP table signature at 0x10010; the
# wrapped media images carry it at 0x10210.  Testers get no experiments:
# refuse anything that is not the raw form.
sig_raw=$(od -An -tx1 -j $((0x10010)) -N 8 "${payload}" | tr -d ' \n')
if [[ ${sig_raw} != "010064aa78563412" ]]; then
  sig_wrapped=$(od -An -tx1 -j $((0x10210)) -N 8 "${payload}" | tr -d ' \n')
  if [[ ${sig_wrapped} == "010064aa78563412" ]]; then
    echo "refusing: this is the WRAPPED media image (u-boot.bin.sd.emmc.bin)." >&2
    echo "  The SPI NOR takes the raw u-boot.bin from the package." >&2
  else
    echo "refusing: no FIP signature found - not a VIM3 firmware image" >&2
  fi
  exit 1
fi

# ---- step detection ------------------------------------------------------
find_nor_mtd() {
  local path found= count=0
  for path in /sys/class/mtd/mtd*; do
    [[ ${path} == *ro ]] && continue
    [[ -f ${path}/size ]] || continue
    [[ $(cat "${path}/size") == 16777216 ]] || continue
    [[ $(cat "${path}/erasesize") == 4096 ]] || continue
    found=${path##*/}
    count=$((count + 1))
  done
  [[ ${count} -eq 1 && -n ${found} ]] && echo "${found}"
}
nor_mtd=$(find_nor_mtd || true)

stage_marker=/var/lib/vim3-spi-install.staged

# Why the staging can look like it "did nothing": three distinct failures all
# leave the NOR invisible and used to be indistinguishable, so the script just
# asked for STAGE again and the tester had no way to tell it had failed.
# Report which one it is.
diagnose_missing_nor() {
  local dt_status= mtd_report= grubenv=
  dt_status=$(tr -d '\0' \
    </proc/device-tree/soc/bus@ffd00000/spi@14000/status 2>/dev/null || echo "<absent>")
  for p in /sys/class/mtd/mtd*; do
    [[ ${p} == *ro || ! -f ${p}/size ]] && continue
    mtd_report+="      ${p##*/}: size=$(cat "${p}/size") erasesize=$(cat "${p}/erasesize")"$'\n'
  done
  [[ -n ${mtd_report} ]] || mtd_report=$'      (no MTD devices at all)\n'
  grubenv=$(grub-editenv list 2>/dev/null | grep -E '^(next_entry|prev_entry)=' || true)

  cat >&2 <<EOF

The SPI NOR is not visible, so the maintenance boot did not take effect.
Diagnostics:

  spifc status in the RUNNING device tree : ${dt_status}
  MTD devices seen by this kernel:
${mtd_report}  grubenv one-shot state : ${grubenv:-<none set>}
  grub.cfg sources custom.cfg : $(grep -qc 'custom\.cfg' /boot/grub/grub.cfg 2>/dev/null \
      && echo yes || echo NO)

Read it like this:

  * spifc says "okay" but no 16 MiB MTD appears  -> the tree was applied and
    the driver did not attach, or this kernel reports a different erase size.
    Send the MTD list above; do NOT retry.
  * spifc says "disabled" or <absent>            -> the maintenance device
    tree was never applied.  GRUB either did not boot the one-shot entry, or
    booted it without honouring 'devicetree' (the command needs GRUB's fdt
    module; some signed/monolithic GRUB builds omit it).
  * next_entry= is still set                     -> GRUB never consumed the
    one-shot entry at all.
  * custom.cfg sourced = NO                      -> your grub.cfg predates
    /etc/grub.d/41_custom; run 'update-grub' once and stage again.

EOF
}

if [[ -z ${nor_mtd} && -f ${stage_marker} ]]; then
  staged_boot=$(cat "${stage_marker}" 2>/dev/null || echo unknown)
  this_boot=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo unknown)
  if [[ ${staged_boot} != "${this_boot}" ]]; then
    echo "refusing: a maintenance boot was staged and has already been spent." >&2
    diagnose_missing_nor
    echo "Once the cause is known, remove ${stage_marker} and stage again." >&2
    exit 1
  fi
fi

if [[ -z ${nor_mtd} ]]; then
  # ======================= STEP 1: stage ==================================
  for tool in dtc fdtoverlay fdtget grub-reboot python3; do
    command -v "${tool}" >/dev/null || {
      echo "need ${tool} (apt install device-tree-compiler grub2-common)" >&2
      exit 1; }
  done
  [[ -r /sys/firmware/fdt ]] || { echo "no /sys/firmware/fdt" >&2; exit 1; }
  [[ -d /boot/grub ]] || { echo "no /boot/grub - GRUB required" >&2; exit 1; }

  workdir=$(mktemp -d)
  trap 'rm -rf -- "${workdir}"' EXIT

  # The maintenance tree is built from the tree the firmware ACTUALLY
  # published (it patches the framebuffer and strips stale bootloader
  # leftovers at runtime); strip only the per-boot properties the EFI stub
  # regenerates, then enable spifc via overlay.  A static build-artifact
  # DTB here hangs the boot - measured, twice.
  cat "/sys/firmware/fdt" > "${workdir}/live.dtb"
  dtc -I dtb -O dts -o "${workdir}/live.dts" "${workdir}/live.dtb" 2>/dev/null
  python3 - "${workdir}/live.dts" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
s = re.sub(
    r'^\s*(linux,uefi-[a-z-]+|bootargs|linux,initrd-(start|end))\s*=.*?;\s*$\n',
    '', s, flags=re.M)
open(p, 'w').write(s)
PY
  dtc -@ -I dts -O dtb -o "${workdir}/base.dtb" "${workdir}/live.dts" 2>/dev/null

  cat > "${workdir}/spifc.dtso" <<'DTSO'
/dts-v1/;
/plugin/;

/* Linux-only SPI maintenance policy: spifc on, eMMC narrowed to 4-bit
 * (the two share pins on this SoC).  One boot only. */

&sd_emmc_c {
	pinctrl-0 = <&emmc_ctrl_pins>, <&emmc_data_4b_pins>,
		    <&emmc_ds_pins>;
	bus-width = <4>;
};

&spifc {
	status = "okay";
};
DTSO
  dtc -@ -I dts -O dtb -o "${workdir}/spifc.dtbo" "${workdir}/spifc.dtso" 2>/dev/null
  fdtoverlay -i "${workdir}/base.dtb" -o "${workdir}/maint.dtb" "${workdir}/spifc.dtbo"

  [[ $(fdtget -t s "${workdir}/maint.dtb" /soc/bus@ffd00000/spi@14000 status) == okay ]]
  [[ $(fdtget -t i "${workdir}/maint.dtb" /soc/mmc@ffe07000 bus-width) == 4 ]]

  kernel=/boot/vmlinuz-$(uname -r)
  initrd=/boot/initrd.img-$(uname -r)
  [[ -s ${kernel} && -s ${initrd} ]] || {
    echo "refusing: ${kernel} / ${initrd} not found" >&2; exit 1; }
  root_uuid=$(findmnt -no UUID /)
  [[ -n ${root_uuid} ]]

  cmdline=$(sed -E 's/^BOOT_IMAGE=\S+\s*//' /proc/cmdline \
            | sed -E 's/\bsplash\b//g; s/\bquiet\b//g; s/plymouth\.[^ ]*//g' \
            | tr -s ' ')
  cmdline="${cmdline} plymouth.enable=0"

  cat <<EOF

  board    ${model}
  payload  ${payload}  (sha256 ${actual_sha:0:16}...)

Step 1 of 2: arm a ONE-SHOT maintenance reboot that exposes the SPI NOR.
Nothing is flashed yet.  The next boot uses a modified device tree (eMMC
temporarily at 4-bit width); every boot after that is back to normal.

EOF
  if [[ ${assume_yes} != yes ]]; then
    read -r -p "Type STAGE to continue: " reply
    [[ ${reply} == STAGE ]] || { echo "aborted"; exit 1; }
  fi

  install -m644 "${workdir}/maint.dtb" /boot/vim3-spi-maintenance.dtb
  cat > /boot/grub/custom.cfg <<EOF
menuentry '${entry_title}' {
    echo 'One-shot SPI-maintenance boot...'
    search --no-floppy --fs-uuid --set=root ${root_uuid}
    linux (\$root)${kernel} ${cmdline}
    initrd (\$root)${initrd}
    devicetree (\$root)/boot/vim3-spi-maintenance.dtb
}
EOF
  grub-reboot "${entry_title}"

  # Verify the staging actually took.  Each of these has been seen to fail
  # silently, leaving a tester in a loop with no signal that anything is wrong.
  staging_ok=yes
  if ! grep -q 'custom\.cfg' /boot/grub/grub.cfg 2>/dev/null; then
    echo "WARNING: /boot/grub/grub.cfg does not source custom.cfg -" >&2
    echo "  the one-shot entry will be invisible to GRUB.  Run 'update-grub'" >&2
    echo "  once, then run this script again." >&2
    staging_ok=no
  fi
  if ! grub-editenv list 2>/dev/null | grep -q "^next_entry=${entry_title}$"; then
    echo "WARNING: grub-reboot did not record the one-shot entry in grubenv." >&2
    echo "  Current: $(grub-editenv list 2>/dev/null | grep '^next_entry=' || echo '<unset>')" >&2
    staging_ok=no
  fi
  if [[ ${staging_ok} != yes ]]; then
    echo >&2
    echo "refusing to promise a maintenance boot that will not happen." >&2
    exit 1
  fi

  # Record which boot staged this, so a later run can tell "not staged yet"
  # from "staged, booted, and the tree did not take".
  install -d -m755 /var/lib
  cat /proc/sys/kernel/random/boot_id > "${stage_marker}" 2>/dev/null || true

  cat <<EOF

Staged.  Now:

  sudo reboot
  # after the board is back up:
  sudo ./install-vim3-uefi-spi.sh ${payload}

(If the maintenance boot fails to come up, power-cycle: the one-shot entry
is spent and the board boots normally.)
EOF
  exit 0
fi

# ========================= STEP 2: flash ==================================
device=/dev/${nor_mtd}
command -v flash_erase >/dev/null || {
  echo "need flash_erase (apt install mtd-utils)" >&2; exit 1; }
jedec=$(cat /sys/bus/spi/devices/spi*/spi-nor/jedec_id 2>/dev/null | head -1 || echo unknown)

sectors=$(( (payload_size + 4095) / 4096 ))
extent=$(( sectors * 4096 ))

cat <<EOF

  board       ${model}
  SPI NOR     ${device}  (16 MiB, 4 KiB sectors, JEDEC ${jedec})
  payload     ${payload}
  sha256      ${actual_sha}
  write       ${payload_size} bytes (${sectors} sectors) at offset 0

Step 2 of 2: this ERASES OOWOW from the SPI NOR and replaces it with the
UEFI firmware, then sets the MCU to boot SPI-first.  A full backup of the
NOR boot region is taken first - keep it, it IS your oowow restore.

While the flash runs, do not touch UEFI variables (no efibootmgr, no
/sys/firmware/efi writes): firmware and Linux would be two masters on the
same flash controller.

EOF
if [[ ${assume_yes} != yes ]]; then
  read -r -p "Type ERASE-OOWOW to continue: " reply
  [[ ${reply} == ERASE-OOWOW ]] || { echo "aborted"; exit 1; }
fi

# ---- backup the boot region (oowow) --------------------------------------
# Only the boot region: the firmware owns 0xF00000+ (its variable store) and
# raw reads of that range are unreliable while runtime services are live.
stamp=$(date +%Y%m%d-%H%M%S)
backup=$(dirname -- "${payload}")/vim3-nor-bootregion-backup-${stamp}.img
echo "  backing up NOR boot region (12 MiB) -> ${backup}"
dd if="/dev/${nor_mtd}ro" of="${backup}" bs=4096 \
   count=$((boot_region_size / 4096)) iflag=fullblock status=none
sha256sum "${backup}" > "${backup}.sha256"
echo "  backup sha256: $(awk '{print $1}' "${backup}.sha256")"

# ---- erase + write + verify ----------------------------------------------
echo "  erasing ${sectors} sectors..."
flash_erase --quiet "${device}" 0 "${sectors}"
echo "  writing..."
# No conv=fsync: MTD character devices reject it (EINVAL); writes are
# synchronous and the readback compare below is the real proof.
dd if="${payload}" of="${device}" bs=4096 conv=sync status=none
cmp -n "${payload_size}" "${payload}" "${device}"
echo "  readback verified (${payload_size} bytes)"

tail_byte=$(dd if="${device}" bs=1 skip="${extent}" count=8 status=none |
            od -An -tx1 | tr -d ' \n')
[[ ${tail_byte} == "ffffffffffffffff" ]] || {
  echo "ERROR: erase extent tail not 0xFF - aborting before BOOT_MODE flip" >&2
  exit 1; }
echo "  erase confined to the image extent"

# ---- flip the MCU to SPI-first -------------------------------------------
# Only after the image is verified: worst case from here is booting the
# firmware we just proved is in the NOR, with the eMMC rescue behind it.
i2cset -f -y 0 0x18 0x20 0
echo "  MCU BOOT_MODE set to SPI-first"

# The staging marker has served its purpose; a stale one would make a future
# run report a spent maintenance boot that never happened.
rm -f "${stage_marker}"

cat <<EOF

Done.  The SPI NOR carries the UEFI firmware, verified byte-for-byte, and
the MCU is set to boot from it.  Now do a TRUE POWER CYCLE (pull the DC
plug for a few seconds) - a warm reboot keeps the old boot-source latch.

Keep the backup:  ${backup}

To restore oowow later:
  run this script's step 1 again to get a maintenance boot, then:
    sudo flash_erase --quiet ${device} 0 $((boot_region_size / 4096))
    sudo dd if=${backup} of=${device} bs=4096 conv=sync
    sudo i2cset -f -y 0 0x18 0x20 1     # eMMC-first again
  and power-cycle.

To just boot from eMMC again (firmware there still works):
    sudo i2cset -f -y 0 0x18 0x20 1
  and power-cycle.
EOF
