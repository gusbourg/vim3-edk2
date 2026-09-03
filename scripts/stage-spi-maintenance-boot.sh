#!/usr/bin/env bash
# Stage a ONE-SHOT SPI-maintenance boot on the bench board.
#
# The maintenance device tree is built from the tree the FIRMWARE ACTUALLY
# PUBLISHED on the running board (/sys/firmware/fdt), not from the static
# build artifact.  That distinction is load-bearing: firmware patches its DTB
# at runtime - it disables the amlogic simple-framebuffer and substitutes a
# uefi-framebuffer at the live GOP address, and it strips the stale U-Boot
# /memreserve entries, initrd pointers and bootargs (with a foreign root
# UUID) that the static artifact still carries.  Handing the kernel that
# static tree via GRUB's `devicetree` command replaces all of it and hangs
# the boot - observed twice, in both lane-mux states, which is what proved
# spifc was never the culprit.
#
# So: capture the live tree, strip only the per-boot properties the EFI stub
# will regenerate (linux,uefi-*, bootargs, initrd pointers), apply the
# spifc-enabling overlay, and boot that.  The entry is serial-observable
# (no splash, console + earlycon on the UART) so a failure prints instead of
# hiding behind plymouth.
#
# grub-reboot arms it for exactly one boot; every boot after returns to the
# firmware's own device tree.
#
# SPDX-License-Identifier: BSD-2-Clause-Patent

set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
host=${VIM3_SSH_HOST:?set VIM3_SSH_HOST=user@board-ip}
overlay="${project_root}/Platform/Khadas/Vim3/DeviceTree/meson-g12b-a311d-khadas-vim3-spi-maintenance.dtso"
entry_title='VIM3 SPI-NOR maintenance (one shot)'
ssh_opts=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o UserKnownHostsFile=/dev/null
  -o StrictHostKeyChecking=accept-new
)

command -v dtc >/dev/null
command -v fdtoverlay >/dev/null
command -v fdtget >/dev/null
test -s "${overlay}"

workdir=$(mktemp -d)
trap 'rm -rf -- "${workdir}"' EXIT

echo "capturing the live published device tree from ${host}..."
ssh "${ssh_opts[@]}" "${host}" 'sudo cat /sys/firmware/fdt' > "${workdir}/live.dtb"
test -s "${workdir}/live.dtb"

# Strip only what the EFI stub regenerates for the next boot.  Everything
# else - framebuffer substitution, memory, reserved regions - is firmware's
# runtime work and must survive.
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
dtc -@ -I dts -O dtb -o "${workdir}/spifc.dtbo" "${overlay}" 2>/dev/null
fdtoverlay -i "${workdir}/base.dtb" -o "${workdir}/vim3-spi-maintenance.dtb" \
  "${workdir}/spifc.dtbo"

test "$(fdtget -t s "${workdir}/vim3-spi-maintenance.dtb" \
        /soc/bus@ffd00000/spi@14000 status)" = okay
test "$(fdtget -t i "${workdir}/vim3-spi-maintenance.dtb" \
        /soc/mmc@ffe07000 bus-width)" = 4
sha256sum "${workdir}/vim3-spi-maintenance.dtb"

scp "${ssh_opts[@]}" "${workdir}/vim3-spi-maintenance.dtb" \
  "${host}:/tmp/vim3-spi-maintenance.dtb"

ssh "${ssh_opts[@]}" "${host}" "sudo bash -s -- '${entry_title}'" <<'REMOTE'
set -euo pipefail
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
entry_title=$1

test "$(tr -d '\0' </proc/device-tree/model)" = "Khadas VIM3"
command -v grub-reboot >/dev/null

# The maintenance tree inherits whatever USB3-PHY description the firmware
# published, so it is consistent with the current lane mux either way.  Warn
# if the mux is on M.2 anyway - SuperSpeed devices vanish there, and a
# USB-rooted maintenance boot should know that before it starts.
mux=$(i2cget -f -y 0 0x18 0x33 2>/dev/null || echo unknown)
if [[ ${mux} != 0x00 ]]; then
  echo "note: lane mux is ${mux} (M.2/PCIe) - USB runs at 2.0 only this boot"
fi

kernel=/boot/vmlinuz-$(uname -r)
initrd=/boot/initrd.img-$(uname -r)
test -s "${kernel}"
test -s "${initrd}"
root_uuid=$(findmnt -no UUID /)
test -n "${root_uuid}"

# Serial-observable: drop the splash, put the kernel log on the UART.
cmdline=$(sed -E 's/^BOOT_IMAGE=\S+\s*//' /proc/cmdline \
          | sed -E 's/\bsplash\b//g; s/\bquiet\b//g; s/plymouth\.[^ ]*//g' \
          | tr -s ' ')
cmdline="${cmdline} plymouth.enable=0 console=tty1 console=ttyAML0,115200"
cmdline="${cmdline} earlycon=meson,0xff803000 ignore_loglevel loglevel=8"

install -m644 /tmp/vim3-spi-maintenance.dtb /boot/vim3-spi-maintenance.dtb

cat > /boot/grub/custom.cfg <<EOF
menuentry '${entry_title}' {
    echo 'One-shot SPI-maintenance boot (spifc enabled, serial console)...'
    search --no-floppy --fs-uuid --set=root ${root_uuid}
    linux (\$root)${kernel} ${cmdline}
    initrd (\$root)${initrd}
    devicetree (\$root)/boot/vim3-spi-maintenance.dtb
}
EOF

grub-reboot "${entry_title}"
echo "staged: next boot = '${entry_title}'"
echo "  kernel:  $(uname -r)"
echo "  cmdline: ${cmdline}"
REMOTE

echo
echo "Armed.  Reboot the board; the kernel log will appear on the serial console."
