#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Wrap a VIM3 eMMC boot-partition image in an FMP capsule.

The payload is the sector-padded ``u-boot.bin.sd.emmc.bin`` wrapper from a
pinned candidate directory - the exact bytes the SSH promotion script would
write.  Vim3FmpDxe receives it unchanged and performs the write-boot0,
verify, write-boot1 sequence itself.

Deliberate differences from BaseTools' GenerateCapsule.py:

* ``Flags = 0`` - never CAPSULE_FLAGS_PERSIST_ACROSS_RESET.  On this SoC
  every reset retrains DDR, so a capsule parked in RAM for "the next boot"
  is simply gone.  A flagless capsule is processed synchronously inside
  UpdateCapsule() during the boot that discovers it, which is the only
  delivery that can work here (see Vim3BootManagerLib::HandleCapsules).

* No FMP_PAYLOAD_HEADER.  That 16-byte header is a FmpDevicePkg/FmpDxe
  convention; Vim3FmpDxe would faithfully write it to the boot partition,
  corrupting the image.  Its absence is also load-bearing for the size
  check: the payload is a whole number of sectors, and Vim3FmpDxe rejects
  anything that is not.

Usage: make-vim3-capsule.py CANDIDATE_DIR_OR_PAYLOAD OUTPUT.cap
"""

import pathlib
import struct
import sys
import uuid

# gEfiFmpCapsuleGuid - EFI_FIRMWARE_MANAGEMENT_CAPSULE_ID_GUID.
FMP_CAPSULE_GUID = uuid.UUID("6dcbd5ed-e82d-4c44-bda1-7194199ad92a")

# PcdVim3SystemFirmwareImageTypeIdGuid: byte-for-byte the DEC default
# {0x6d,0x1a,0xf4,0xc8, 0x39,0x5b, 0x42,0x7e, 0xb0,0x93,...}.
VIM3_IMAGE_TYPE_ID = uuid.UUID("c8f41a6d-5b39-7e42-b093-2c8471e50da6")

VIM3_UPDATE_IMAGE_INDEX = 1
SECTOR = 512
BOOT_PARTITION_BYTES = 8192 * SECTOR


def build_capsule(payload: bytes) -> bytes:
    if len(payload) == 0 or len(payload) % SECTOR:
        raise SystemExit(
            f"payload is {len(payload)} bytes; must be a non-empty "
            f"multiple of {SECTOR} (is this really u-boot.bin.sd.emmc.bin?)"
        )
    if len(payload) > BOOT_PARTITION_BYTES:
        raise SystemExit(
            f"payload is {len(payload)} bytes; the eMMC boot partitions "
            f"hold {BOOT_PARTITION_BYTES}"
        )

    # EFI_FIRMWARE_MANAGEMENT_CAPSULE_IMAGE_HEADER, Version 3 (pack(1)):
    # Version, UpdateImageTypeId, UpdateImageIndex, reserved[3],
    # UpdateImageSize, UpdateVendorCodeSize, UpdateHardwareInstance,
    # ImageCapsuleSupport.
    image_header = struct.pack(
        "<I16sB3xIIQQ",
        3,
        VIM3_IMAGE_TYPE_ID.bytes_le,
        VIM3_UPDATE_IMAGE_INDEX,
        len(payload),
        0,  # UpdateVendorCodeSize
        0,  # UpdateHardwareInstance
        0,  # ImageCapsuleSupport: no authentication, no dependency
    )

    # EFI_FIRMWARE_MANAGEMENT_CAPSULE_HEADER with one payload item.  The
    # item offset is relative to the start of this header.
    fmp_header = struct.pack("<IHH", 1, 0, 1)
    item_offset = len(fmp_header) + 8  # one UINT64 ItemOffsetList entry
    fmp_capsule = fmp_header + struct.pack("<Q", item_offset)
    assert len(fmp_capsule) == item_offset
    fmp_capsule += image_header + payload

    # EFI_CAPSULE_HEADER.  Flags = 0: see module docstring.
    header_size = 28
    capsule_image_size = header_size + len(fmp_capsule)
    capsule_header = struct.pack(
        "<16sIII",
        FMP_CAPSULE_GUID.bytes_le,
        header_size,
        0,  # Flags
        capsule_image_size,
    )
    return capsule_header + fmp_capsule


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)

    source = pathlib.Path(sys.argv[1])
    if source.is_dir():
        source = source / "u-boot.bin.sd.emmc.bin"

    payload = source.read_bytes()
    capsule = build_capsule(payload)

    out = pathlib.Path(sys.argv[2])
    out.write_bytes(capsule)
    print(f"payload  {len(payload):8d} bytes  {source}")
    print(f"capsule  {len(capsule):8d} bytes  {out}")


if __name__ == "__main__":
    main()
