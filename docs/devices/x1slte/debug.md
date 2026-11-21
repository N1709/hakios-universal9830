# S20 debug switches: what is off by default and how to turn it on

Everything below was found running all the time on 2026-11-14 while scrolling was being tuned (120 Hz work,
`docs/devices/x1slte/display.md`). Each costs time on a path that has to finish a frame in 8.3 ms, or wakes the cores, so the
default is **off**; each can be turned on again for an investigation. "Live" means without a reboot.

The boot line is at its limit (540 of the 550 characters the loader takes, `x1slte.cmdline`): prefer the live switches
(sysctl) over boot arguments. `sysctl` on the phone is `/var/jb/usr/sbin/sysctl` (`/redwood/bin/rwsys` reads and writes
string sysctls only).

## Off by default

| What | Cost when on | Turn on | Where |
| --- | --- | --- | --- |
| GL frame statistics every 20 frames, per-function summary every 200 | written from backboardd's render thread: every 20th frame late at 120 Hz | boot `glstats=1` | `userland/src/eagl.c` (`frame_stats`) |
| iOS logging (os_log) of backboardd copied to the console | every touch event's log lines written synchronously by the window server | build with `OSLOG=com.apple.backboardd` (or `all`); live: `ssh s20 /var/jb/usr/bin/oslog` streams without the cost | `userland/build.sh` (OSLOG default empty), `userland/mkjobs.py` |
| Wake-up accounting ("wakes core N" lines every 10 s) | runs at every timer callout and interrupt of every core | live `sysctl -w kern.redwood.wake_stats=1` (0 off) | `osfmk/arm64/psci_smp.c` (`redwood_wake_stats`), sysctl in `bsd/dev/redwood_dbgport.c` |
| Thread sampler (`SAMPL` records in `/m2m`) | an IPI to the sampled thread's core every 2 ms while it runs | live `sysctl -w kern.redwood.ksample=1` (SpringBoard main) / `2` (backboardd render) / PID; `0` off | `osfmk/arm64/user_exception_trace.c`; names: `tools/perf/objcmap.m` |
| Touch-to-screen latency trace (`tl:` lines) | a line per touch sample and per frame stage | boot `touch_lat=1` (no room on the boot line now: drop something first) | `bsd/dev/redwood_input.c`, `MiniIOMobileFramebuffer.cpp`, `inject.c`; `tools/latency.py` |
| Every GL frame a line | per frame | boot `glframes=1` | `eagl.c` |
| GL census of draws without a program | per draw | boot `glcensus=1` | `eagl.c` |
| RedwoodProbe (view-tree dumps on `notify com.redwood.probe`) | loaded into SpringBoard, PosterBoard, InCallService | `mv /var/jb/usr/lib/TweakInject.disabled/RedwoodProbe.* /var/jb/usr/lib/TweakInject/`, restart SpringBoard | `proposals/wallpaper/rwprobe.m` |

| Every DVFS clock change (`dvfs: domain N to N kHz: answered`) | a line per clock change (~200 in minutes of use) | live `sysctl -w kern.redwood.dvfs_log=1` (failures are always logged) | `bsd/dev/redwood_exynos_acpm.c` `set_rate` |
| Bluetooth vendor command lines | a line per command | first 40 a boot only | `bsd/dev/redwood_exynos_bt.c` |
| launchd's own notices on the console | the largest share of `/log` | build with `QUIET=0` (default is quiet since 2026-11-14) | `userland/build.sh`, `src/init.c` |

## Taken out

| What | Why | Undo |
| --- | --- | --- |
| ClassKit's `progressd` | dies of a null pointer 10 s after every start (pc 0x1cd79aa4c) and launchd starts it again: ~100 log lines a minute, a process start every 10 s | build with `ALLOW=progressd` (`userland/mkjobs.py` DENY) |

## Left on, and why

| What | Why it stays |
| --- | --- |
| Kernel 10 s status lines (`dvfs:`, `gpu … ms`, `fb:` frame counts, temperatures) | a few lines every 10 s; how heat and frame pacing are read |
| `apple80211: … REFUSED` | stops by itself after 800 lines a boot |
| `fb_scaler: frame N taken …` | three frames per 5 s window |
| `/m2m` channel from the Metal driver | only processes that use Metal (three wallpaper extensions); rate-limited to 400 records/s |
| `jit_flush=1` | not a debug switch: Safari/WebKit need it (instruction caches across clusters) |

## Checking the phone is quiet

- Log volume: two `curl http://10.77.0.2:8080/log` 30 s apart; about 46 lines/s before this cleanup.
- 120 Hz: `/var/root/hztest` must print `120.0 Hz` (`tools/perf/hztest.m`).
- Idle wake-ups: switch `kern.redwood.wake_stats=1` on for a minute, read the `wakes core` lines, switch it off.

## Power changes (B, 2026-11-14)

- Wi-Fi: wifid asked for a full scan every ~4 s; each polled the radio every 0.2-1 ms for up to 2 s on the big cores
  (~140 wake-ups/s idle, ~4.5 W from USB). Broad scans within `kern.redwood.wifi_scan_gap` seconds (30) of the last real
  one are answered from its results; named/channel scans always go to the radio. 0 = old behaviour.
  Measured after: big cores 85-90 % asleep with the screen off (was 19-32 %), 0.76 A from USB, 62-63 C.
- Thermal second steps softened (`bsd/dev/redwood_exynos_acpm.c` domains): GPU 455 MHz (was 377), A76 1482 MHz (was 1066)
  above 90 C -- swipes past 90 C fell off a cliff (frames over 35 ms doubled). Trip is ~108 C.
- `libredwood-metaloff` is not in the boot inject list: its IORegistry hooks were the common factor of two stack overflows
  (mediaserverd SIGBUS at its guard page, CollectionsPoster with metalon). Add it live only while `metal_props on`.
- Open: progressd still crash-loops when something starts it on demand (seen 40 min into a boot) although mkjobs denies
  its job; PosterBoard "insert nil object" crashes (gallery data) -- load `libredwood-crashlog.dylib` into PosterBoard
  via exec_inject to get the stack.
