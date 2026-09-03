#!/usr/bin/env bash
# Promote one firmware build to BOTH copies: the SPI NOR primary and the
# eMMC boot-partition rescue.
#
# Keeping them at the same version is the whole point.  If the SPI image is
# ever invalid the BootROM falls through to eMMC, and a stale rescue means
# that fall-through silently boots older firmware - the failure mode this
# script exists to prevent.
#
# The eMMC half runs on a normal boot.  The SPI half needs the one-shot
# maintenance boot (Linux cannot see the NOR otherwise), so this script does
# eMMC first, then tells you how to finish - it deliberately does not reboot
# the board on your behalf.
#
#   ./scripts/promote-vim3-firmware.sh out/RELEASE
#
# SPDX-License-Identifier: BSD-2-Clause-Patent

set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
target_dir=${1:-out/RELEASE}
host=${VIM3_SSH_HOST:?set VIM3_SSH_HOST=user@board-ip}
cd "${project_root}"

emmc_image="${target_dir}/fip/u-boot.bin.sd.emmc.bin"   # wrapped: FIP @0x10210
spi_image="${target_dir}/fip/u-boot.bin"                # raw:     FIP @0x10010
test -s "${emmc_image}"
test -s "${spi_image}"

emmc_sha=$(sha256sum "${emmc_image}" | cut -d' ' -f1)
spi_sha=$(sha256sum "${spi_image}" | cut -d' ' -f1)

# Both images must come from the same build, or the two copies disagree.
smbios=$(find third_party/edk2/Build -path '*RELEASE_GCC*Vim3SmbiosDxe.dll' | head -1)
version=$(strings -el "${smbios}" 2>/dev/null | grep -oE '^[0-9a-f]{7}(-dirty)?$' | head -1)
if [[ -z ${version} || ${version} == *-dirty ]]; then
  echo "refusing: firmware stamp is '${version:-unreadable}' - commit, then" >&2
  echo "re-run scripts/build.sh AND scripts/package-fip.sh together." >&2
  exit 1
fi

cat <<EOF

Promoting firmware ${version}
  eMMC rescue : ${emmc_image}
                ${emmc_sha}
  SPI primary : ${spi_image}
                ${spi_sha}

EOF

echo "=== 1/2: eMMC rescue copy ==="
VIM3_SSH_HOST="${host}" VIM3_PROMOTE_EMMC_BOOT_PARTITIONS=YES \
  ./scripts/promote-vim3-emmc-boot-partitions.sh "${emmc_sha}" "${emmc_image}"

cat <<EOF

=== 2/2: SPI primary copy - needs the maintenance boot ===

  ./scripts/stage-spi-maintenance-boot.sh
  # reboot the board, wait for it to come back, then:
  ssh ${host} 'sudo modprobe spi-nor'
  VIM3_PROMOTE_SPI=YES ./scripts/promote-vim3-spi.sh \\
      ${spi_sha} \\
      ${spi_image}

Then reboot once more to run the new firmware from SPI.  Confirm the source
on serial: 'POC:B;RCY:0;SPINOR:0' plus 'SPI NOR init' means the NOR won;
an 'EMMC:0' means it fell through to the rescue - which is safe, but tells
you the SPI image did not take.
EOF
