# Phone calls on the S20 (VoLTE) — how they work today

Status (2026-11-14): incoming and outgoing VoLTE calls work on Jio, both ways, on iOS's own call screen and from the
Phone app, with the voice on the earpiece (Speaker button to switch). This page says plainly **what this is and what it
is not**.

## At a glance

- **What it is:** a patched-together, VoIP-style phone that speaks VoLTE. The modem only carries data. The call's voice
  never goes through the modem's voice engine or the phone's audio chip (ABOX/Calliope). Signalling, voice packets, the
  voice codec and buffering all run in software on the main processor (AP). iOS hears about calls through CallKit, the
  mechanism third-party calling apps use, plus a few hooks so it treats them as the phone's own calls.
- **Why:** the modem accepted every voice-setup command but never took the voice packets, and the route into the audio
  chip produced no sound. Doing the voice on the main processor worked within hours.
- **Portable:** nothing in the voice path depends on this modem or chipset. Porting to another device needs only two
  device-specific pieces: delivering the call's packets to the VoLTE service, and an earpiece route in the speaker driver.
- **Current limits:**
  - no echo cancellation or noise suppression;
  - narrowband sound only (AMR-NB), no HD voice;
  - no emergency calls, hold, call waiting or conference;
  - no video calls and no Wi-Fi calling;
  - no mic waveform in the Dynamic Island.
- **The native path for later:** reverse-engineer how Samsung's software makes the modem's voice engine take the voice
  stream, then the audio chip's call routes and its echo canceller. The call-screen app stays; only the voice handling
  moves to the modem.
- **Where everything lives:** the files involved and the debug logs to read are listed at the end of this page.

## The short version

**The call's voice does not go through the modem's voice engine, and not through the phone's audio DSP (ABOX/Calliope).**
The modem (Shannon S5123) is used only as a data pipe: LTE, the IMS PDN, and the dedicated voice bearer. Everything that
a phone's modem and audio DSP normally do for a call is done in software on the application processor (AP):

| Job | On a stock S20 | Here (today) |
|---|---|---|
| SIP signalling (register, INVITE, BYE…) | modem / Samsung IMS stack | `redwood-ims` on the AP |
| IPsec to the P-CSCF | Samsung IMS stack | `redwood-ims` (PF_KEY) on the AP |
| RTP / voice packets | modem's voice engine (CPVE) | `redwood-ims` on the AP (`tools/cellular/ims-media.h`) |
| AMR-NB codec | modem's voice engine | opencore-amr (Apache-2.0), on the AP |
| Jitter buffer, loss concealment | modem / DSP | `ims-media.h` on the AP |
| Echo cancellation, noise suppression | ABOX/Calliope DSP | **none yet** |
| Earpiece / speaker / mic routing | ABOX "incall" paths | the kernel's own speaker and mic drivers (`/dev/redwood-audio`, `/dev/redwood-mic`, `kern.redwood.snd_route`) |
| Call screen | CommCenter + Phone app | CallKit: RedwoodPhone.app + callservicesd, with small hooks (libredwood) |

It is, honestly, **a patched-together VoIP phone** that happens to speak VoLTE: the voice is handled the way a VoIP app
(WhatsApp, a SIP softphone) handles it, and iOS is told about calls through CallKit, the way third-party calling apps do,
with hooks so that iOS treats them as the phone's own calls.

## Why it was done this way

- The modem accepted every voice-engine command we sent (CPVE: SetPdnIp, SetChannel, SetCodec, SetRtpMedia — all
  `0x8000` OK), but never took the RTP stream: the caller's voice packets arrived at the AP as ordinary data.
- On the S20 (external S5123 modem over PCIe) the modem's voice reaches the audio DSP through ABOX's PCI path
  (`abox-pci-rmem` 0xF4200000, the modem's PCIe doorbell) and topology controls in Calliope's firmware. We mapped that
  path (`snd=3` / `cpcall=1`, kept off) and drove the stock routes (`tools/cellular/call-audio.sh`), but no audio flowed:
  the missing piece is on the modem's side (how Samsung's IMS stack tells the voice engine to use that path).
- Doing the voice on the AP worked within hours and needs nothing from the modem's undocumented internals.

## What this means

**Good:**
- **Portable.** Nothing in the voice path is specific to the S5123 or Exynos: any device where this port has a data
  connection to the IMS PDN, a speaker/earpiece output and a microphone can run the same `redwood-ims` + RedwoodPhone.
  The device-specific parts are small: how IMS packets reach the AP (here: redwood-cellulard, SBD channels 10/11 and the
  IMS utun), and the earpiece route in the kernel's speaker driver.
- Fully visible and debuggable: every call's packets, counters and timings are in `/var/tmp/redwood-ims.log`.

**Limits (until the native path exists):**
- No acoustic echo cancellation or noise suppression (the DSP does that on a stock phone). Use the earpiece; on the
  speaker the other side may hear an echo.
- AMR-NB 12.2 only (narrowband). No AMR-WB / EVS (HD voice) yet — possible on the AP too (opencore-amr has AMR-WB
  decode; an encoder is needed).
- Voice runs on the AP's CPU and the phone's own audio drivers: a little more power than the modem's voice engine.
- No emergency-call (SOS) handling, no supplementary services (hold, call waiting, conference, call forwarding), no
  video calls (ViLTE), no Wi-Fi calling (VoWiFi needs an IKEv2/EAP-AKA tunnel to Jio's ePDG).
- Dynamic Island / Control Center show no live mic level (the voice bypasses iOS's audio system).

## Known bugs and open work (2026-11-14, 05:30)

Working and stable: incoming calls on the call screen, outgoing calls from the Phone app (keypad, Recents, Contacts),
answer / decline / end / mute, the caller's hang-up, earpiece by default, the Speaker button, country code from the SIM.

Open:
1. **Call goes to the Dynamic Island instead of the full call screen.** iOS gives a third-party CallKit app's calls the
   island; the full screen comes when the provider is "telephony" (`isTelephonyProvider`) inside InCallService, but then
   InCallService waits for telephony status ("calling…" never changes) and sends End where nobody listens. Tried:
   both flags in InCallService (full screen, but stuck "calling…" and lost End), isSystemProvider alone (still island),
   none (island; stable — current). The flags can be switched live without a new image: `/var/root/callspoof 0x8N`
   (bit 0 isSystemProvider, bit 1 isTelephonyProvider; affects SpringBoard and InCallService), `callspoof 0` = defaults.
   Next idea: keep the telephony answer for the presentation only (SpringBoard / InCallService's presentation
   controller) and give InCallService the call's status and End through CallKit as now.
2. **Recents empty right after a boot** (back within ~30 s): the Phone app hides calls from a provider whose app
   LaunchServices does not know, and LaunchServices forgets /var/jb's apps at every boot; redwood-cellulard registers
   RedwoodPhone 20 s after it starts (uicache). Better: register earlier, or install RedwoodPhone as a system app.
3. **Message / video / mail buttons greyed in Recents** ("Phone Audio" calls are a third-party app's). Making them phone
   calls inside MobilePhone broke dialling and Recents — reverted. Needs a narrower hook (the contact card's actions).
4. **Bluetooth headsets / car for calls**: not yet. Apple's bluetoothd does HFP; missing: SCO audio in our Bluetooth
   kernel driver, and the call's voice through iOS's audio system (today it bypasses it) when a headset is the route.
5. **No echo cancellation / noise suppression**; AMR-NB only (no HD voice).
6. **Purple call-screen background** (GPU: the blur/tint material of InCallService).
7. **No mic waveform in the Dynamic Island** (the voice bypasses iOS's audio system).
8. Not built: video calls (ViLTE), Wi-Fi calling (VoWiFi: IKEv2/EAP-AKA to Jio's ePDG), emergency calls, hold /
   call waiting / conference, de-registration on stop.

Process notes (learnt the hard way):
- Never restart redwood-ims / redwood-cellulard while a call is up: the network keeps the call ("busy" afterwards).
- Files copied to /var just before a flash were lost (the flash reboots hard): `sync` after every copy.
- Each redwood-ims restart re-registers; fixed ports (50060/50062) make it replace the old registration.

## Review of the VoLTE implementation (2026-11-14, 05:50)

Fixed in this review (redwood-ims 30040b9, cellulard 75da9ce):
- Re-INVITE / UPDATE inside a call (session refresh, media change) was answered as if it were the first INVITE (old
  CSeq, 100/180 again) or with 200 and no SDP: long calls would have been dropped by the network. Now 200 with SDP and
  the request's own Via/CSeq (Session-Expires echoed), and the voice follows a moved media address.
- The ~110-minute re-registration restarted redwood-ims even during a call (call dropped): now deferred.
- A lost registration connection ended redwood-ims with a call still open at the network ("busy" afterwards): the
  call is closed first (BYE / CANCEL / 480) and on the call screen.
- After a long healthy run redwood-ims comes back in 2 s, not 30 (no VoLTE meanwhile).
Earlier the same day: two SIP messages in one TCP read (the second was dropped), fixed IMS ports, unique call numbers,
stale dials, cancel while dialling, late answers to ended calls (ACK + BYE), no 100rel (Jio 481'd our PRACK).

Still missing / to do (in order of importance):
1. **Emergency calls**: 112 etc. go out as ordinary numbers (tel:112 in the IMS domain) -- no emergency
   registration / urn:service:sos. Do not rely on the S20 for emergencies yet.
2. **Re-registration** is a full restart every ~110 min (a few seconds without VoLTE); should be a refresh REGISTER on
   the same connection and SAs.
3. **DTMF** (keypad tones in a call, RFC 4733 telephone-event, PT 101): not sent; received events are ignored.
4. **Codec mode requests** (CMR) from the network are ignored (always AMR-NB 12.2); no AMR-WB / EVS.
5. No RTCP (call quality reports); the network has not minded so far.
6. One call at a time: a second incoming call gets 486 (no call waiting / hold / conference).
7. No de-registration on a clean stop (fixed ports make the next registration replace the binding instead).
8. No echo cancellation (see above).

## Earpiece and speaker (user-verified 2026-11-14)

- Every call starts on the **earpiece**: the kernel's speaker driver plays the call through the top amplifier only
  (it drives the earpiece/top speaker), 18 dB quieter than the speakers' level, with the bottom speaker muted
  (`kern.redwood.snd_route` = 1; level `kern.redwood.snd_ear_below`, eighths of a dB).
- The call screen's **Speaker** button switches to both speakers and back (`snd_route` 0 / 1). iOS itself has no
  receiver port here (the audio plug-in's devices are Speaker/Codec/Hawking/Actuator; on the iPhone X the earpiece is
  part of iOS's "Speaker" device), so InCallService is given two routes of ours ("iPhone", "Speaker") by
  `userland/src/phone_dial.h`; picking one posts `com.redwood.call.speaker` / `.earpiece`, which redwood-ims applies.
- During a call redwood-ims re-applies its route every 0.5 s, and the audio plug-in does not change the route while
  `kern.redwood.snd_call` is set: system sounds on iOS's Speaker device no longer switch a call to the loudspeaker.
- After the call both are cleared (speakers again for everything else).

## The native path (later)

For a fully native phone, the modem and the audio DSP have to be reverse-engineered further so that they carry the
call the way stock Android does:
1. **Modem side:** how Samsung's IMS stack (`libsec-ims`: CallManager -> CPVEInterface -> OemIPCHandler, IPC group
   0x16) makes the voice engine take the RTP stream and hand PCM to ABOX — the commands we send are accepted but the
   stream is not taken; something (an ordering, a mode, a bearer binding) is still missing.
   See `docs/devices/x1slte/cellular/download-plan.md` (CPVE command table, payload layouts).
2. **Audio side:** ABOX/Calliope's VSS/PCI voice path and its topology controls (`abox_tplg.conf` ids, stock
   `mixer_paths.xml` "incall_*" routes, `tools/cellular/call-audio.sh`), plus Calliope's own echo canceller.
3. Then RedwoodPhone stays (it is iOS's side of the call), and `redwood-ims` keeps signalling but hands media to the
   modem instead of doing it itself.

Until then, the AP path is what makes calls work, and it is the one to keep when porting to other devices.

## Where the pieces are

- `tools/cellular/redwood-ims.c` — IMS client: registration (fixed ports 50060/50062), calls in and out, SMS.
- `tools/cellular/ims-media.h` — the voice: RTP, AMR, jitter buffer, resampling, AGC, routes.
- `tools/cellular/build-ims.sh` — builds it with opencore-amr (static).
- `tools/cellular/RedwoodPhone/` — the CallKit provider app (+ install.sh, which also puts callservicesd in place).
- `userland/src/phone_dial.h` — hooks: Phone-app dials to RedwoodPhone, our calls as phone calls, the Speaker button.
- `userland/src/audio_plugin.c` — iOS's audio plug-in (hands the mic to the call; no route changes during a call).
- `xnu-src/.../bsd/dev/redwood_exynos_sound.c` — `kern.redwood.snd_route` (earpiece = top amplifier only),
  `kern.redwood.snd_call`; also the ABOX PCI voice mapping (`cpcall`, off).
- `tools/cellular/live-modem.c` (redwood-cellulard) — hands the voice bearer's packets to the IMS interface;
  registers RedwoodPhone with LaunchServices at boot.
- Debug: `/var/tmp/redwood-ims.log` (call lines are timestamped; voice counters every 5 s),
  `/var/mobile/Library/Logs/redwood-phone.log`, `/var/tmp/redwood-cell/call-ui/` (state between the two).
