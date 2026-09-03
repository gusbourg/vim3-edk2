# vim3-hdmi-snd — ACPI HDMI sound card for the VIM3

Out-of-tree machine driver for ACPI-mode boots, where the in-tree
`axg-card` cannot run (it is structurally OF-only: child-node walking,
phandle parsing, of_-only card properties).  Binds via PRP0001 against
the SSDT `CARD` device (`khadas,vim3-acpi-hdmi-sound`, SsdtAudio.asl)
and replicates the playback subset of the VIM3 DT card:

    FE  frddr_a "FRDDR"            DPCM front end, playback only
    BE  tdmif_a "TDM Pad"          i2s, cpu clock provider, mclk-fs 256,
                                   2 slots on all four lanes (DT <1 1>)
        -> codec tohdmitx "I2S IN A"
    C2C tohdmitx "I2S OUT" -> hdmi-codec "i2s-hifi"   (dw-hdmi child)

Every component is resolved at probe by scanning the platform bus for
its compatible (PRP0001 device names are enumeration-order dependent —
never hardcoded); the dw-hdmi audio child is found by the
`hdmi-audio-codec.` name prefix.  Missing components defer the card, so
module load order is irrelevant.

Name prefixes are applied through `codec_conf` (FRDDR_A / TDM_A /
TDMOUT_A / TOHDMITX), reproducing the DT control names exactly.  The
routing muxes therefore need the same one-time amixer configuration as
DT mode (no UCM profile exists for this card):

    amixer -c0 cset name='FRDDR_A SINK 1 SEL' 0    # OUT 0
    amixer -c0 cset name='FRDDR_A SRC 1 EN Switch' on
    amixer -c0 cset name='TDMOUT_A SRC SEL' 0      # IN 0
    amixer -c0 cset name='TOHDMITX I2S SRC' 0      # I2S A
    amixer -c0 cset name='TOHDMITX Switch' on
    alsactl store

Build (needs the private axg-tdm.h header, hence the Kbuild ccflags):

    make -C <kernel-tree-or-headers> M=$PWD

Install with `depmod`; udev autoloads it from the OF modalias that
PRP0001 devices emit.  Depends at runtime on patches 0014-0017 (clk
island + ASoC ACPI probes) and the MesonAudioDxe/SsdtAudio firmware.

## Bitstream passthrough (AC3 / DTS / DD+)

`KHADAS-VIM3.conf` installs to `/usr/share/alsa/cards/` (named after the
card's *driver*; the `CARD=` argument uses the *id*, `KHADASVIM3`).  It
provides the `iec958` plugin the card otherwise lacks:

    install -D -m644 KHADAS-VIM3.conf /usr/share/alsa/cards/KHADAS-VIM3.conf
    aplay -D iec958:CARD=KHADASVIM3,AES0=0x06 stream.spdif

It sets the IEC958 channel-status non-audio bit before the stream opens,
which kernel patch 0042 turns into `HDMI_AUD_CONF2[1]` (NLPCM) on the DWC
HDMI TX.  Two details are load-bearing and were each found by failure:

* `iface PCM` — hdmi-codec registers the control on the PCM interface,
  not the `ctl_elems` hook's default of MIXER.
* `device 2` — the control lives on the codec-to-codec rtd, while the
  playback node is `hw:0,0`.  `HDA-Intel.conf` solves the identical
  mismatch with `index 16`.

Verify on the register, not on faith — `AUD_CONF2` is directly
addressable at `0xFF603103`:

    sudo busybox devmem 0xFF603103 8     # 0x02 passthrough, 0x00 PCM

`amixer cset` cannot set an IEC958-type control: it silently no-ops and
prints back the unchanged value.
