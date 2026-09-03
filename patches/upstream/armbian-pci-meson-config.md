# Draft Armbian PR: enable PCI_MESON in the uefi-arm64 kernel

**Status: DRAFT — not submitted.**

Repo: `github.com/armbian/build` · File: `config/kernel/linux-uefi-arm64-edge.config`
(and the matching `-current` config if present for the family)

## The change

```diff
-# CONFIG_PCI_MESON is not set
+CONFIG_PCI_MESON=m
```

(main branch line 756 at time of writing.)

## Suggested PR title

kernel(uefi-arm64): enable PCI_MESON for Amlogic boards booting via UEFI

## Suggested PR body

The uefi-arm64 kernel family targets arm64 boards booting through generic
UEFI firmware. On Amlogic G12A/G12B boards with M.2 slots (e.g. Khadas
VIM3) running such firmware [1], the PCIe controller currently has no
driver: `CONFIG_PCI_MESON` is not set, while its companion PHY driver
`CONFIG_PHY_MESON_G12A_USB3_PCIE=m` already is.

The visible failure is worse than a missing feature: the generic
`snps,dw-pcie` fallback (`CONFIG_PCIE_DW_PLAT_HOST=y`) binds to the
controller via the DT's fallback compatible and aborts probe with

    dw-pcie fc000000.pcie: error -ENXIO: IRQ index 1 not found

leaving the device driverless and NVMe root impossible on these boards,
even though the same kernel already carries the PHY, `nvme`, and
everything else needed.

Enabling `PCI_MESON=m` matches what the meson64 family kernels already do,
costs nothing on non-Amlogic UEFI boards (module only loads on a DT
match), and makes root-on-NVMe work out of the box. Verified on a Khadas
VIM3 (A311D) running 7.0.12-edge-arm64: with the module present,
`pci-meson` binds, the host bridge and root port enumerate, and an empty
slot degrades gracefully.

[1] https://... (link to the VIM3 EDK2 firmware project/forum thread)

## Notes for the submitter

- Armbian PRs go against `main`; their bot may ask for a rebuild test —
  `./compile.sh kernel BOARD=uefi-arm64 BRANCH=edge` is the relevant build.
- Check whether `config/kernel/linux-uefi-arm64-current.config` exists and
  wants the same one-liner (current = 6.18.x also lacks it — same symptom
  verified on 6.18.32 in DT mode).
- Until this lands, build `pci-meson` as an out-of-tree module against your
  kernel headers and install it to `updates/`.
