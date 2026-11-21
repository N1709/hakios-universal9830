# Exynos audio recovery and microphone bring-up

The speaker path uses RDMA3, UAIF2 and two CS35L41 amplifiers. Bluetooth's UART fix and unfragmented ACL default are documented in x1slte-bluetooth.md.

## Speaker recovery

A failed amplifier power-up latches playback off. Previously `snd.prep` reset the chips directly from the debug command but left the worker's failure flag set, so successful preparation did not restore automatic playback. It could also race the worker's I2C operations.

`snd.prep` now queues recovery on the audio worker and wakes it from silence. The worker mutes the guard, powers down, resets and prepares both amplifiers, then requires both to respond with no amplifier fault bits before clearing the latch. A reset invalidates the cached volume and presence state. An answered, fault-free power-up timeout now queues this same recovery automatically, at most twice per boot. Missing status reads and reported amplifier faults keep playback latched off. Recovery failure leaves playback muted.

Run through the existing `rwsys` helper:

```
rwsys kern.redwood.poke 'snd.prep'
```

The command reports that recovery was queued. Check the kernel log for `snd: amplifier recovery ready` or `FAILED (muted)`; `snd.state` reporting "ready" alone does not establish that playback is active. Do not issue raw `snd.amps` or amp-register writes concurrently with playback.

Validation on 2026-11-07: kernel built and installed; explicit recovery completed with both amps responding; the three-second AudioQueue tone delivered all frames and the worker powered both amps on and then off without reported faults. The user confirmed audible speaker output after recovery. A subsequent cold boot reproduced the power-up timeout, motivating the bounded automatic retry. The 14:14 kernel cold boot and six-second iOS tone completed with both amps on; the automatic timeout branch was not needed on that boot.

## Built-in microphone

Native iOS recording now works through `/dev/redwood-mic`: DMIC1 -> VTS serial LIF -> UAIF6 -> NSRC4 -> WDMA4. The shared serial link selects three converter inputs, so **all three DMIC_AUD converters must be initialized and enabled**, even with only the main microphone powered. Enabling only converter 0 produced zeros. This was verified first in VTS's direct AHB capture and then in the normal ABOX DMA ring. Production capture does not need the VTS firmware core.

The device provides nonblocking 48 kHz signed 16-bit mono samples from channel 0 of the hardware's eight-channel, right-aligned signed 24-bit stream. The worker follows ABOX 3.0.1's period-index/byte-count pointer format. It starts hardware on open, buffers samples for the existing Codec input in the audio HAL, and stops capture on close. Capture-register snapshots are restored afterward; the mic-bias GPIO update touches only pin 1, preserving the adjacent amplifier I2C pins. Mutating `mic.*` diagnostics are rejected while iOS owns capture; `mic.peek` remains available.

The node is enabled by default with the Exynos speaker driver; `mic_ios=0` disables it for diagnosis. The existing userland HAL needs no change.

Validation on the cold-booted 14:43 kernel: the existing iOS recorder returned complete 5-second and 6-second mono files with nonzero audio; 6 seconds contained 288000 samples. Both captures completed with zero ring overruns. The user confirmed the built-in mic and Voice Memos work. Initial capture latency was 100–120 ms, including the staged helper's diagnostic delay; power/latency optimization follows this baseline.

Preserve the watchdog disable at boot: it addresses the separate bootloader watchdog restart.

## Reference findings and diagnostic guard

The v7 reference enabled regmap cache bypass during a successful six-second Samsung capture (48 kHz stereo, peak 4934; 573536 of 576000 samples nonzero). The real WDMA4 CTRL was `00201721`, rather than cached `00200000`; SPUM CTRL1 at `0304` read zero rather than cached `948`. Native `mic.cap` uses the hardware format (24-bit, eight channels, buffer update); `mic.peek` reports the existing ring without restarting capture. Those corrections alone left native samples at zero; enabling all three converters subsequently fixed capture.

The v8 reference read forty specific hardware registers with Samsung's existing `exynos-debug-test` SFR command. Its parser requires two trailing spaces after the address to unambiguously select a read. It showed DMIC0's parent divider 3, DMIC_AUD divider 23, SERIAL_LIF divider 31; the earlier native 15/5/7 ratios produce the same final frequencies. Matching the exact ratios live also left samples zero. Microphone clock and data pins both toggle. VTS pad retention already read `860` (released), so its absence from the setup helper did not explain this run's zeros.

A subsequent native bulk read `r 0x15500600 16` aborted at reserved physical address `15500608`. The saved current panic identifies `rwsys` and ESR `96000210`. Bulk reads overlapping CMU_VTS are now rejected before mapping/access; use the guarded, individual-register `mic.cmu` helper. This rejection was verified on the installed 14:14 kernel. Appended loader logs also contain older console panics; match kernel version and boot session before attributing one to a new run.
