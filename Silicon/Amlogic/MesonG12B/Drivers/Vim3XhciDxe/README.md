# Vim3XhciDxe — forked XhciDxe for the Amlogic A311D (DWC3)

Fork of `MdeModulePkg/Bus/Pci/XhciDxe` from edk2
**`b03a21a63e3bd001f52c527e5a57feddb53a690b`** (verified pristine at fork time).

## Why fork

Upstream `XhciDxe` is a polled, minimal-recovery driver. On this board's
Synopsys DWC3 it is unreliable behind hub trees: devices several tiers down
intermittently fail to enumerate, and a single error can leave an endpoint or
the command ring permanently wedged. The fixes needed are numerous enough that
maintaining them as a patch file (the previous
`patches/firmware/edk2/0006-xhci-deep-hub-address-retry.patch`, now retired) was becoming
unworkable. Keeping them as first-class source makes them reviewable and
debuggable.

`FILE_GUID` is deliberately different from upstream's
(`B7F50E91-A759-412c-ADE4-DCD03E7F7C28`) so both can never collide in an FV.
The platform builds this instead of upstream XhciDxe — see `Vim3.dsc` /
`Vim3.fdf`.

## Divergence from upstream

Every change is tagged `VIM3:` in a comment.

### Stage 1

| Area | Change | Rationale |
|---|---|---|
| `XhciSched.h` | `XHC_INIT_DEVICE_SLOT_RETRIES` 1 → 8, add `XHC_DEVICE_SLOT_RETRY_STALL` (50 ms) | Address Device races a not-yet-ready deep device. Was patch 0006. |
| `XhciSched.c` `XhcPollPortStatusChange` | settle delay between slot-init retries | ditto |
| `XhciSched.c` `XhcRingDoorBell` | `MemoryFence()` before the doorbell write | Rings are Normal-NC (the platform registers the xHC non-coherent), doorbell is Device memory. ARMv8 does not order NC vs Device without a barrier, so the controller could see a doorbell before the TRB's cycle bit. Linux has `wmb()` at every equivalent point. |
| `XhciSched.c` `XhcSyncEventRing` | `MemoryFence()` after the cycle-bit scan | Read side of the same problem; matches Linux's `rmb()` in `xhci_handle_event()`. |
| `XhciSched.h` | define completion codes 7, 8, 9, 11, 12, **19 (Context State Error)**, 36 | Upstream leaves them undefined so they hit the switch `default`. |
| `XhciSched.c` `XhcCheckUrbResult` | Context State Error / TRB Error / Split Transaction Error → `EDKII_USB_ERR_TRANSACTION` | `default` reported `EFI_USB_ERR_TIMEOUT`, which `XhcTransfer`'s recovery test explicitly ignores, so the endpoint was never reset and stayed dead. Now it runs Reset Endpoint + Set TR Dequeue. |

### Planned (not yet implemented)

- Command-ring abort state machine (wait `CRCR.CRR`, no-op aborted TRBs,
  re-sync + restart) — upstream only sets CA and returns, on one path.
- HC-death (`HCE`/`HSE`) detection with reset + re-init.
- Address Device recovery that disables/frees the slot before retrying.
- ERDP mid-drain advance; replace the event-ring-full `ASSERT (FALSE)` (a hard
  hang in RELEASE, which uses `PcdDebugPropertyMask 0x21` = assert + deadloop).
- Interrupt-driven event ring (`USBCMD.INTE` + GSIV 62 via
  `EFI_HARDWARE_INTERRUPT2_PROTOCOL`, `IMOD`), keeping the 1 ms timer as a
  fallback.

## Rebasing onto a newer upstream

```sh
cd third_party/edk2
git log --oneline b03a21a63e..HEAD -- MdeModulePkg/Bus/Pci/XhciDxe
```
Diff this directory against upstream's to see the fork's changes, keeping the
INF identity fields (`BASE_NAME`, `FILE_GUID`, `MODULE_UNI_FILE`, the `.uni`
names) and every `VIM3:`-tagged hunk.
