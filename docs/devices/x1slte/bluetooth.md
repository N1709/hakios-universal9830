# Galaxy S20 Bluetooth audio

A2DP playback was verified on the SM-G980F (Exynos 990) with Galaxy Buds2 Pro on 2026-11-07.

## UART transmit fix

The UART has a 256-byte transmit FIFO. A full FIFO reports a zero byte count plus a full flag. Reading the count and full flag separately lets a FIFO that drains between the reads appear completely empty. The driver then writes too many bytes, corrupting HCI traffic. Calculate available space from one UFSTAT snapshot.

Before the fix, starting audio repeatedly disconnected the headphones and produced controller firmware dump events. With the fixed kernel, the user confirmed audible, stable playback.

## Packet sizing

Send whole HCI ACL packets by default. The earlier 339-byte fragmentation workaround consumes extra controller credits. During live playback with fragmentation disabled, the transmit count increased by more than 1.2 MB without new credit waits, recounts, receive overruns, or dropped bytes. The user confirmed stable playback.

Diagnostic commands through `rwsys kern.redwood.poke`:

- `bt.acl`: controller credits, waits, recounts, and trace status.
- `bt.baud`: UART rate, byte counters, overruns, and drops.
- `bt.frag 0`: whole packets; `bt.frag 339` restores the old diagnostic workaround.
- `bt.trace`: recent HCI headers.

The FIFO fix was built and flashed with loader read-back verification. The whole-packet default was then tested live and built into the next image. That image was installed during the phone-microphone reference test restore; after reboot, `bt.frag` reports 0 without a live override.

## Microphone status

The S20 built-in microphone is not implemented by `redwood_exynos_sound.c`. iOS advertises an input, but the phone has no `/dev/redwood-mic`; a four-second AVAudioRecorder check returned digital silence. This is separate from working A2DP output. The microphone path requires VTS/DMIC capture through ABOX UAIF6 and WDMA4.
