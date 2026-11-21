# Independent automatic cooling

The new kernel automatically limits each CPU cluster and the GPU from its own
thermal zone. No boot flag is needed (`dvfs=2` remains required). A hot M5 alone
does not cap the A76, A55, GPU, or memory.

Installed on 2026-11-14 after the user's instruction to apply it. Only the
recovery partition's kernel slot at 0x2000000 was updated; installed loader,
command line and userland were preserved. The phone rebooted into the new kernel.
USB readback matched the staged 16 MiB slot byte for byte.

## Native automatic policy

| Domain | Normal maximum | Automatic ceiling | Engage | Recover at or below |
| --- | ---: | ---: | ---: | ---: |
| M5 super cores | 2730 MHz | 1794 MHz | 76 C | 68 C |
| A76 | 2504 MHz | 2106 MHz | 80 C | 72 C |
| A55 efficiency cores | 2002 MHz | 1456 MHz | 82 C | 74 C |
| GPU | 800 MHz | 572 MHz | 80 C | 72 C |
| Memory | 2028 MHz | No automatic cap | No dedicated telemetry | — |

Each domain releases its own cap after 30 seconds below its own recovery mark.
A warmer zone cannot prevent another domain's recovery. An invalid sensor caps
only that zone until it recovers; it does not infer a memory cap. All CPU cores
remain available. Lower explicit targets, stricter thermal steps, Settings Low
Power Mode and screen-off limits take precedence. Zero targets remain unmanaged.

Memory has no exposed dedicated temperature or bandwidth measurement here. Its
clock is not automatically inferred from CPU/GPU heat. Explicit manual efficiency
mode, Settings LPM and screen-off retain their existing memory policy (1352 MHz,
1352 MHz and 421 MHz respectively). Normal memory remains at 2028 MHz.

These are provisional die-temperature settings, not battery/case limits or a
measured optimum. Battery savings, sustained temperatures and UI/call latency
still need an on-device comparison. CPU utilization alone does not trigger caps.

## Monitoring cost and request handling

The native policy reuses the four temperature samples the ACPM thermal thread
already reads every 250 ms. It adds four small state machines: no extra sensor
reads, threads, timers, allocations, log scans or polling daemon. Logs are written
only when the active cap mask changes. Clocks still follow the existing 50 ms
governor; an automatic decision normally applies on the following tick plus ACPM
request time. This is small additional work, not a measured zero-cost claim.

`lpmd` uses only event-driven Settings notifications. The temporary log-polling
implementation has been removed entirely, including on older kernels. It never
writes the manual efficiency switch or overrides native automatic control.

Unchanged requested clocks do not send another ACPM request. Failed requests are
not cached as applied and are retried. Missing utilization data preserves the
requested rung but still applies ceilings. Touch/frame/wake/slow-GPU boosts and
fixed-clock boot configurations honor the same per-domain caps. Capped demand
estimation uses the requested capped clock, rather than an unreachable boost rung.

The policy starts with the existing ACPM thermal loop, after initialization and
its default 10-second wait; it does not control S-Boot clocks. `dvfs=1` only
monitors. The developer command `acpm.rate` can bypass policy until a later
update and should not be used as a sustained clock test.

## Control and status (new kernel)

Root can inspect or disable native automatic control:

```sh
sysctl kern.redwood.efficiency_auto kern.redwood.efficiency_mask
sysctl -w kern.redwood.efficiency_auto=0
sysctl -w kern.redwood.efficiency_auto=1
```

Automatic is on by default. Mask bits: A55=1, A76=2, M5=4, GPU=8; no memory bit.
For example, mask 4 means only M5's automatic ceiling is active. Disabling clears
state at the next thermal check; re-enabling reevaluates current temperatures.

Manual whole-device efficiency remains separate:

```sh
sysctl -w kern.redwood.efficiency=1
sysctl -w kern.redwood.efficiency=0
```

The manual switch caps all listed domains, including memory at 1352 MHz. Turning
it off does not disable independent automatic protection. `dvfs_efficiency=1`
is an optional initial manual setting, not a requirement for native automatic
control. The existing per-zone thermal steps stay enabled regardless.

## Harder per-zone thermal steps

| Policy | Existing kernel | New kernel |
| --- | --- | --- |
| M5 steps | 85 / 95 C | 78 / 86 C |
| A55/A76 steps | 85 / 95 C | 82 / 90 C |
| GPU steps | 87 / 97 C | 82 / 90 C |
| Release hysteresis | 10 C | 8 C |
| Brightness reductions | 90 / 96 C | 82 / 88 C |

M5 step 1 caps at 1586 MHz and step 2 at 1196 MHz. A76 uses 1794/1066 MHz,
A55 1456/1053 MHz, GPU 572/377 MHz. These use each domain's own sensor and remain
checked every 250 ms. `dvfs_hot=C` overrides the first step with second step C+8;
brightness keeps its separate `fb_hot` override and 4 C release hysteresis.

## Current live phone and legacy fallback

The earlier live experiment used log polling and the old whole-device LPM
switch. It successfully engaged and recovered, but that switch also capped
memory. It has now been stopped to avoid that coupling and monitoring overhead.
The original `/redwood/bin/lpmd` was restored in `user/501/com.redwood.lpmd`;
Settings LPM works, and no temporary userspace automatic controller is active.
Native independent automatic cooling is now enabled in the installed kernel.

Saved experiment artifacts remain inert in `/var/root/lpmd-auto` and
`/var/root/com.redwood.lpmd-auto.plist`. Original backups are
`/var/root/lpmd-before-auto-20261114` and
`/var/root/lpmd-job-before-auto-20261114.plist`. Historical transitions remain in
`/var/tmp/redwood-power.log`. The new `prebuilt/userland/lpmd` mirrors Settings
without polling. The current phone's original helper is already purely
event-driven and can coexist with the new kernel.

There is no whole-device automatic fallback in the new helper. On an old kernel,
it only mirrors Settings; independent automatic control requires the new kernel.

## Validation

```sh
python3 tools/power/test_efficiency.py
cc -std=c11 -Wall -Wextra -Werror tools/power/test_clusters.c -o /tmp/test-redwood-clusters
/tmp/test-redwood-clusters
```

Tests compile actual cap/request code and exercise independent masks, unchanged
unselected domains, memory isolation, manual/LPM/screen-off precedence, lower
targets, failed clock retry, unchanged-clock suppression, independent recovery,
interrupted cool dwell and sensor failure. The legacy parser and polling tests
were removed together with that implementation.
lpmd cross-compiles with warnings as errors. Exynos GICv2 DEVELOPMENT builds.
Live verification: `efficiency_auto=1`, manual `efficiency=0`, `lowpower=0`.
Observed native mask transitions 0 -> 5 -> 7 -> 15 during boot, confirming
independent activation rather than a global LPM toggle. SpringBoard, backboardd,
cellulard and redwood-ims were running, and IMS registration was present. Native
recovery behavior, sustained responsiveness and incremental overhead still need
real-use testing. Installation files and the previous kernel slot are saved in
`out/deploy-20261114-efficiency/`; `installed.json` records hashes. The previous
slot can be restored through fastboot Log access by staging `kernel-before.bin`
and issuing `fastboot oem write recovery 2000000`, then rebooting.

All source changes are committed on local `main`. Remote push to
`upstream/x1slte-efficiency` was attempted but failed because GitHub credentials
were unavailable; the unrelated remote `main` was not overwritten.


## Passcode-session responsiveness observations (14 November 2026)

Automatic cooling remained enabled (`efficiency_auto=1`, manual efficiency and
LPM both 0), with mask 15. Earlier active samples showed temperatures around
82–85 C; heavier rendering reached big/mid/little/GPU 93/90/94/91 C. One heavy
10-second window averaged A55/A76/M5/GPU at 1053/1066/1196/377 MHz. These are
applied clocks under existing thermal protection, not evidence of a CPU-bound
credential daemon. `keybagd` and `lpmd` sampled at 0% CPU; backboardd reached 99%
in a busy sample. GPU telemetry showed 5614 fragment jobs over 5 seconds (2265 ms
busy), plus 5613 vertex/tiler/compute jobs (404 ms). This port's rendering can
produce substantial work even when individual jobs are short.

Frame diagnostics in several windows recorded maxima of 65–89 ms; these samples
cover mixed UI activity and do not isolate a passcode transition. There were
198553 free 16-KiB pages (about 3.0 GiB), and the native UFS counters showed zero
errors. Wallpaper diagnostics reported parallax off. No thermal ceiling was
raised, memory cap coupled to CPU temperature, diagnostic poller installed, or
UI effect forcibly disabled during this check.

The user's final clarification concerned the early swipe threshold into the
passcode view, and they accepted the transition. No gesture/animation change or
frame-rate improvement is claimed. The separate software-passcode empty-probe
optimization removes unnecessary KDF work but is staged for a future kernel
boot, not measured as a fix for renderer stutters. See `x1slte-passcode.md` for
implementation, validation boundaries and the persistent repeat-lock bridge.

## Screen off / AOD (15 November 2026, A)

With the display off (also ~2-6 s into AOD, when backboardd powers the panel down) the kernel:

| What | Switch (boot arg / live) | Default |
| --- | --- | --- |
| Scheduler on the A55s only (recommended cores 0-3); all 8 back at display on, and while sound plays + 10 s | `scroff_little=0` / `sysctl -w kern.redwood.scroff_little=0`; state `kern.redwood.scroff_little_now` | on |
| Governor + thermal tick 500 ms (clocks are at the screen-off caps anyway) | `dvfs_scroff_tick=50` (old) / 250 / 1000 | 500 |
| Wi-Fi poll 2 ms → 10 ms → 100 ms when quiet | `net_scroff_poll=0` | on |
| USB controller tick 250 ms with no host | — | on |
| Wi-Fi on the battery: PM_MAX + listen every 3rd DTIM; PM_FAST + every beacon at display on | `wifi_scroff=0` | on |
| Flashlight thread sleeps until iOS asks (any screen state) | — | on |

Measured in AOD (cable attached): fast cores 4/5/7 had zero wake-ups in 10 s (were ~250 a second each), SoC
60-61 C (was 68-70 C). Wake-up hunt: `sysctl -w kern.redwood.wake_stats=1` for 30 s, read `wakes core` in /m2m;
thread calls are now named by function (`fn 0x...`: `atos -o out/mach.x1slte ADDR`). Not done yet: cluster
power-down (CPD) and SoC idle (SICD) from the dtb's exynos-cpupm, touch low-power mode in AOD.
