# Android reference on this S20 (ExtremeROM 3.0.6, One UI 8), recorded 2026-11-20

Recorded during the full delivery test (Jesvi iOS -> Restore -> ExtremeROM), with `tools/android-reference.sh`
(USB debugging, no root). **Raw files are private, on the Mac only:** `~/ios-port-research/device/dump/android-reference-20261120-032025/`
(`system.txt`, `etc/` = the vendor init files, `logcat-fingerprint-full.txt`, `logcat-unlock.txt`). Not in git:
they hold device identifiers. What follows is the part we use.

## 1. Fingerprint: Samsung's own command sequence (the reference for our NO MATCH)

The user's impression: enrolment "10x smoother and instant" compared with ours. `TLC_BAUTH: Call FP cmd 0xc (N)` logs every
command libbauthserver sends to the fingerprint TA (N = the opcode we drive through `kern.redwood.tee` / "fp ..." commands).

**Enrolment** (Settings, one finger):

| Time | Opcodes | Meaning (from the log around them) |
|---|---|---|
| 14.776 | `preEnroll` -> 22, 43 | session open ("set_enroll_session"), sensor control on |
| 16.163 | 16 | |
| 16.172 | 73 -> 68 -> 70 (x4, 10 ms timeout) -> 67 (x16) | **background calibration (CBGE)**: "load CBGE data again to local memory"; each 67 is one slot |
| 17.679 | 76, then 49, 75 | ready; capture armed |
| 20.5 .. 42.5 | 76 per touch (~1.2 s apart) | one enrol sample per touch; two touches rejected: `BAuth_Enroll_Do Fail BAD_QUALITY : 39` |
| 42.837 | 45, 46, 75 | template finished and stored |
| 10.595 (next min) | `postEnroll` -> 44 | session closed |

**Unlock** (lock screen, the enrolled finger), repeated 4 times with the same shape:

`authenticate` -> **108 -> 101** -> 76 -> **74 -> 49** -> (finger, ~1.5-2 s) -> **75 -> 46** = match.

**Wrong finger:** `... 74 -> 49` -> `BAuth_Identify_Do Fail BAD_QUALITY or Identify Fail : 32` -> 75, then the retry
`76 -> 108 -> 74 -> 49` without a new `authenticate`.

**The numbers are BAuth CONTROL operations** (`Call FP cmd 0xc (N)` = BAuth_Control_OP, command 12, N decimal;
fingerprint-handover.md): 76 = CONTROL 0x4c getAdditionalLog (diagnostics only), 73/68/70/67 = the CBGE chain. Samsung's
unlock in hex: **0x6c, 0x65, (0x4c), 0x4a, 0x31** before the touch, **0x4b, 0x2e** after it; the identify itself (command 5
Init / 6 Do) is not in these lines, only its failures (`BAuth_Identify_Do Fail : 32`).

Ours (xnu/files/bsd/dev/redwood_teegris.c): `fp idinit` = command 5 Identify_Init with the stored template + metadata,
then `fp run 6` = command 6 Identify_Do in a loop, answering the operations the TA asks for (check_opcode). We send
none of 0x6c / 0x65 / 0x4a / 0x31 before it nor 0x4b / 0x2e after -- the first thing to add, one at a time, against NO MATCH.

**For Jesvi iOS (redwood-fpd):** our match path is "fp idinit" + "fp run 6 1 15 0" after only "fp power" (calibrated) and
gets NO MATCH. Compare opcode by opcode with the unlock line above: 108 and 101 before the capture, 74 + 49 to arm it, 75
+ 46 after; enrolment's CBGE chain matches what we added on 2026-11-19 (73/68/70/67). Next: log our TA opcodes for one
"fp run 6" (kern.redwood.tee "last:" lines) and line them up with this table. Handoff: work/hackios-touchid/docs/devices/x1slte/touchid.md.

### Samsung's identify, from libbauthserver/libbauthtzcommon (A, 2026-11-20)

`FPBAuthService::identify` (0x50684) and `BAuth_Control_OP` (tzcommon 0x6038) read against the log above:
- **22, 49, 108 never reach the TA**: `BAuth_Control_OP` answers them client-side (22 sets the enrol-session flag,
  49 = "send HAT HMAC key" returns 0, 108 returns 0). Sending them to the TA is wrong. 0x5b/0x5c are client-side too.
- **74** carries a value in the length field (service +0x724): 0 on this phone (`iae 74 0 0` in logcat) = what we send.
- **75** returns up to 1000 bytes; **46** sends that reply back (`[sp+0x380]` -> in, buffer, length) = template update
  data; then **IDENTIFY_FINAL (cmd 7)** x3.
- **IDENTIFY_INIT** (tzcommon 0x4f9c): +0x8 chunk, +0xc op, +0x10 user id (<= 0x100), +0x110 template chunk
  (<= 0x64000, len +0x64110), **+0x64114 metadata type 3** (<= 0x400, len +0x64514), **+0x64518 params 0x45 bytes**
  (byte 0 = 0, +1 = operation id u64 (0 on the lock screen), **+0x19 = 2**), +0x6455d string <= 0x64.
- **Metadata type 3 = the reply of CONTROL 45 (SAVE_SECAUTH_ID)** at the end of `enroll` (0x4ff94..0x4ffe8, in = user
  string, out <= 0xa000), stored with `storeMetaData(3, ...)`; also refreshed by `set_active_group` and `update_sid`.
  45 needs the secure user ID from the enrolment HAT (**command 13**, `enroll(hw_auth_token_t*)`); ours refuses with
  "sid is 0". **Most likely cause of NO MATCH: no HAT -> no sid -> no type-3 metadata -> identify cannot match.**
- `send_hat_hmackey` (0x505a4) sends the HMAC key with CONTROL 49, which is client-side here: the TA gets the HAT key
  inside the secure world. A valid HAT has to come from Samsung's Gatekeeper TA (`...474154454b45`, GATEKE): enrol
  and verify our passcode with it, pass its token to command 13. Next work item.

## 2. Heat: what keeps Android cool here

Android does not set CPU governors from init on this ROM: the **kernel's EMS** (Exynos Mobility Scheduler: `/sys/kernel/ems`,
emstune modes, ontime migration) and Samsung's power HAL (`vendor.samsung.hardware.miscpower`, argos) decide at run time.
Init sets **where tasks may run** (init.exynos990.rc, init/*.rc):

| cpuset | CPUs | | schedtune (stune) | |
|---|---|---|---|---|
| background, system-background, restricted, moderate | **0-2** (little only) | | top-app | prefer_idle 1, ontime 1, util_est 1, boost 0 |
| abnormal | 0-3 | | foreground | ontime 1, util_est 1, boost 0 |
| foreground | **0-2, 4-7** (not 3) | | background | all 0 |
| top-app, foreground_window, sf, camera-daemon | 0-7 | | | |

`ontime 1`: a task moves to the big cores only after it has run long enough to need them; `boost 0` everywhere.

**cpuidle** (both states enabled on every cluster):

| Cluster | State 1 | Latency | Target residency |
|---|---|---|---|
| little (cpu0-3) | ananke-cpu-sleep | 230 us | 2000 us |
| middle (cpu4-5) | enyo-cpu-sleep | 340 us | 1000 us |
| big (cpu6-7) | lion-cpu-sleep | 455 us | 1000 us |

Not readable without root (empty in system.txt): cpufreq policies, devfreq governors (MIF/INT/DISP...), thermal trip points.
They come from the kernel: ExtremeXT's exynos990 kernel source (on the SSD: iOS-Jesvi/git/ExtremeXT__android_kernel_samsung_exynos990,
`arch/arm64/boot/dts/exynos/exynos9830*.dts*`: ems / energy model, tmu trips, devfreq tables) is the next place to read.

**For Jesvi iOS:** compare with our redwood DVFS/cpuidle (screen-off small cores only, docs/SHIPPING.md 2026-11-15):
background-class work pinned to cores 0-2, no work on core 3 for normal apps, migrate up only after sustained load,
the idle residencies above.
