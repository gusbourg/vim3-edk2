# g12b-pcie-acpi — PCIe host for ACPI-mode boots

Out-of-tree module that lets an ACPI-booted OS see the VIM3's M.2 PCIe
slot. No stock path exists: the DWC root complex is not ECAM, so the
standard ACPI PCI route (MCFG — or an MCFG quirk in `pci_mcfg.c`, which is
built-in only) cannot describe it to a distro kernel.

The firmware side is a PRP0001 SSDT (`\_SB.PCIE`, compatible
`amlogic,g12b-pcie-acpi`) that the firmware installs **only** when the OS
hardware description is ACPI *and* the board's USB3/M.2 mux selects PCIe.
USB3-mux or DT boots have no such device anywhere, and this module simply
never loads.

By the time Linux runs, firmware has done the whole bring-up (PHY, clocks,
link training, bus numbers, BARs, iATU). The module only supplies config
accessors and host registration, mirroring the firmware's own accessor
rules — direct DBI for the root port, iATU-region-1 retarget through the
1 MiB window for everything downstream, phantom-alias and link-down
masking, and the fabricated 0x0604 class code (same quirk as mainline
`pci-meson.c`).

`_CRS` order is fixed ABI with `SsdtPcie.asl` (the module indexes it):

    [0] MEM 0xFC000000 4 MiB   DWC DBI (iATU unroll at +0x300000)
    [1] MEM 0xFC500000 1 MiB   downstream config window
    [2] MEM 0xFF648000 8 KiB   Amlogic glue (STATUS12 link state)
    [3] MEM 0xFC700000 25 MiB  bus MEM aperture (bus == CPU, 1:1)
    IRQ[0] GSIV 255            ALL INTx of ALL devices (hardware funnel)
    IRQ[1] GSIV 253            DWC built-in MSI demux (unused)

## Interrupts

Everything runs on legacy INTx: the SoC funnels every INTx of every
device into one GIC SPI, so `map_irq` returns platform IRQ 0 for all
pins. There is no GIC ITS on this SoC, so standard ACPI MSI does not
exist; the DWC built-in MSI demux (GSIV 253) is a possible follow-up
inside this module, not a first-cut feature. NVMe falls back to INTx
automatically.

## Build & install

    make -C /lib/modules/$(uname -r)/build M=$PWD modules
    sudo cp g12b-pcie-acpi.ko /lib/modules/$(uname -r)/updates/
    sudo depmod -a

udev autoloads it from the OF modalias (`of:N*T*Camlogic,g12b-pcie-acpi`)
that PRP0001 devices emit — same mechanism as g12b-cpufreq. (`updates/`
is fine here: the module is not needed in the initramfs; the root
filesystem mounts before PCIe matters unless you root on NVMe under ACPI,
in which case place it like the other patched modules and rerun
`update-initramfs -u`.)

## Probe-time guards

- Refuses to probe if the HHI PCIe clock gates are closed (would mean
  firmware/module version skew — a DBI access would stall the fabric).
- Refuses to probe if the root port doesn't answer with the Synopsys
  vendor ID at the DBI.
- Link-down is NOT a probe failure: the root port is still enumerable
  with an empty slot, matching DT-mode and firmware behaviour.
