# CPU, GPU, thermal and idle: Samsung's tuning vs ours (A, 2026-11-20)

Sources: Samsung's device tree (ExtremeXT's exynos990 kernel, `arch/arm64/boot/dts/exynos/exynos9830.dts`, on the SSD:
`iOS-Jesvi/git/ExtremeXT__android_kernel_samsung_exynos990`), the live ExtremeROM capture
([android-reference.md](android-reference.md) section 2: cpusets, schedtune, cpuidle), and ours
(`xnu/files/bsd/dev/redwood_exynos_acpm.c`: governor, thermal steps, power profiles). Parser for the thermal zones:
the trip table below (values decoded from the decompiled DTB).

## Why it matters

The phone resets itself at 105-110 C (PMIC WTSR, "RST_STAT PINRESET") after a few minutes of heavy work (boot after
flashing, many reboots, 2026-11-19 and 11-20). Samsung's own kernel never lets it get there: it starts a continuous
power budget at 83 C and switches the big cores off at 96 C.

## Thermal: zones and trips

| Zone (sensor) | Samsung governor | Samsung control point | Samsung alerts / actions | Ours |
|---|---|---|---|---|
| BIG (M5, cpu6-7) | power_allocator (IPA), 50 ms while hot | **83 C** (passive) | 95/100/105/110 alerts; **hotplug out at 96 C, back in at 91 C**; hot 115 | steps at 78/86 C to 1586/1196 MHz, every 250 ms |
| MID (A76, cpu4-5) | IPA, 50 ms | **83 C** | 95/100/105/110 | steps 82/90 C to 1794/1482 MHz |
| LITTLE (A55, cpu0-3) | step-wise | none | 76, 81, 86, 91, 96, 101 (one cooling step each) | steps 82/90 C to 1456/1053 MHz |
| G3D (Mali) | IPA, 100 ms | **88 C** | 105/110/115; GPU's own ladder 800 -> 702 -> 572 -> 455 -> 377 -> 260, sustainable **455 MHz** | steps 82/90 C to 572/455 MHz |
| ISP, NPU | step-wise | - | 76..101 every 5 C | not read |

All Samsung trips have 5 C hysteresis (2 C on the "switch-on" points: 55 C BIG/MID, 78 C GPU, where IPA starts
tracking). IPA is not a ladder: it shares a power budget between clusters and the GPU every 50 ms so the hottest zone
converges on the control temperature, instead of dropping a rung and overshooting.

## CPU frequencies

| Cluster | Samsung min / default floor / max | Ours: rungs / touch / frame floor |
|---|---|---|
| A55 (cpu0-3) | 442 / **806** / 2002 MHz | 806 .. 2002 (touch 1456/1248, frame 1456) |
| A76 (cpu4-5) | 507 / **845** / 2600 MHz | 1066 .. 2504 (touch 1404/1222, frame 2106) |
| M5 (cpu6-7) | 546 / **962** / 2730 MHz | 1196 .. 2730 (touch 1066/858, frame 1898) |
| GPU | 156 / start 260 / 800 MHz, per-rung load thresholds; interval 5, polling 30 ms | 156 .. 800, 50 ms tick |

Our idle floors are higher than Samsung's on A76 (1066 vs 845) and M5 (1196 vs 962): the lowest rung is where idle
cores sit when they are not power-gated, so that is idle heat for nothing.

## Where work runs (the biggest difference)

Samsung's scheduler (EMS, `ems { emstune set@0000 "normal default" }`) is energy-aware:

- **weights**: little 100, mid **200**, big **20-25** (idle_weight 200 / 400 / 40-50): a task goes to a mid core first;
  the big cores are nearly a last resort.
- **ontime**: a task moves little -> mid above ~10 % of a core's capacity (105/1024) and mid -> big only above
  ~57 % (586/1024), and only after running that long.
- cpusets: background/system-background on **0-2**, foreground 0-2 + 4-7 (not 3), top-app 0-7.
- top-app gets a 5 % frequency boost, nothing else.

XNU here has one performance level for all eight cores (`hw.nperflevels 1`): it spreads work evenly, so the M5s - the
hottest, least efficient cores (weight 20 against the A76's 200) - get as much as any other. That alone explains
most of the heat in plain UI use.

## Idle and screen off

| | Samsung | Ours |
|---|---|---|
| core idle (C1) | per-core power-down: A55 230 us / 2000 us residency, A76 340/1000, M5 455/1000 | cpuidle=1 (per-core power-off, same PSCI states) |
| cluster idle (C2) | cpuidle-ucc: cluster power-down allowed on 4-7 (and 6-7 in the other configs) | not used |
| screen off | EMS "power-scenario" mode; big cores mostly idle by weight; ACPM sleep | small cores only (0-3) + 500 ms tick + low memory clock (2026-11-15) |

## What to change, in order of value

1. **Big cores off when hot**: hotplug cpu6-7 out at 96 C, back in at 91 C (Samsung's thresholds). We already have
   the mechanism (screen-off small-cores mask); this is the step that keeps the phone from ever reaching 105-110 C.
2. **Faster, earlier thermal control**: poll every 50 ms above the switch-on point (55 C), aim at 83 C for BIG/MID and
   88 C for the GPU, with 5 C hysteresis; release a step only below control - 5. Use the GPU's own ladder down to its
   sustainable 455 MHz before touching anything else.
3. **Keep work off the big cores** (EMS weights + ontime): mask cpu6-7 from the scheduler unless the A76s have been
   above ~60 % busy for several ticks (Samsung's ontime boundary), as the screen-off mask already does for 4-7.
4. **Idle floors at Samsung's defaults**: A76 845, M5 962 (now 1066 / 1196).
5. **GPU**: measure a faster governor tick (ours 50 ms; Samsung's DTS has separate 5 ms interval and 30 ms polling settings) or at least jump on a slow job (we already do).
6. Later: cluster power-down (C2) for 4-7 when both cores of a cluster are idle.

Each one goes behind its own boot argument first (project rule), tested with the overheating scenario (flash +
boot + heavy UI), then made default.

## Done so far (2026-11-20)

- **Big cores off at 96 C, back at 91 C** (`dvfs_bigoff`, Samsung's hotplug); thermal checks every 50 ms once a zone
  reaches 80 C.
- **Performance profiles**, one switch: `kern.redwood.perf_profile` (boot-arg `perf_profile=`):

  | | 0 cool | 1 balanced (default) | 2 performance |
  |---|---|---|---|
  | idle floors (A76 / M5) | 845 / 962 | 845 / 962 | 1066 / 1196 |
  | frame floor A55 / A76 / M5 / GPU (MHz) | 1456 / 1794 / 1274 / 455 | 1456 / 2106 / **1274** / 572 | 1456 / 2106 / 1898 / 572 |
  | wake boost | 0.5 s | 0.5 s | 1 s |
  | big cores parked until the A76s are busy | yes | yes | no |
  | thermal steps | 4 C earlier | as set | 4 C later |

  Every tunable is also its own sysctl (`gov_lowfloor`, `ffl_cl0..g3d`, `wake_boost_ms`, `bigpark*`,
  `perf_therm_offset`). The 96 C cutoff and Samsung's clock tables hold in every profile: no overclocking past them.
- **A/B on the phone** (same routine: Settings and Photos scrolling, home pages, lock/wake): old settings 97.6 % of
  frames under 18 ms, 0.65 % over 26 ms; the Samsung-style set with the M5 frame floor at 1196 MHz 77.3 % / 4.36 %
  -- but it ran ~6 C hotter (it came right after, no cool-down: the GPU was throttled to 455 MHz most of the time).
  Lesson: our Metal translation does its frame work on the big cores, so their frame floor matters more than on
  Android; the user picked the middle ground, M5 1274 MHz, as the balanced default. Fair tests need the same start
  temperature.
- AOD is "display off" for the power code (MiniIOMobileFramebuffer: redwood_dvfs_display(0)): screen-off profile.

## Next (parked 2026-11-20): GPU like Samsung
Samsung exposes a 5 ms DVFS interval and a separate 30 ms polling interval (the DTS alone does not establish a 5 ms governor timer), throttles along
800 -> 702 -> 572 -> 455 -> 377 -> 260 MHz, sustains 455 MHz at 85 %, controls at 88 C, and ties CPU limits to the
GPU's level (gpu_cl_pmqos_table). Ours: 50 ms tick with an instant jump on a slow job, steps 82/90 C to 572/455.


## Experimental GPU limits (2026-11-20, continuation)

The governor already contains 156, 260, 377, 455, 572, 702 and 800 MHz.
The new policy adds thermal ceilings using Samsung's existing 800 -> 702 ->
572 -> 455 -> 377 -> 260 MHz list. It does **not** reproduce Samsung's IPA
power allocator. Its thresholds and dwell times are our conservative experiment.

- `gpu_ladder=1` / `kern.redwood.gpu_ladder=1`: additional ceilings of
  702/572/455/377/260 MHz at 88/91/94/97/100 C, with 5 C hysteresis per step.
  A hot sample can immediately apply several steps; invalid temperatures keep
  the previous ceiling. Existing thermal, efficiency and power-profile limits
  still win if lower. In particular the existing 82 C / 572 MHz limit means
  the 702 MHz ceiling usually adds no restriction.
- `gpu_sustain=1` / `kern.redwood.gpu_sustain=1`: cap at 455 MHz after
  10 continuous seconds with >=85% GPU busy and >=78 C. Release after
  5 continuous seconds below 75% busy or below 73 C. Missing readings interrupt
  the dwell measurement and retain an active ceiling. Samsung supplies the
  455 MHz / 85% sustainable point; the dwell and temperatures are ours.
- `kern.redwood.gpu_policy_cap` reports the additional ceiling in kHz (0: none).
  Neither switch changes profile selection. Both default **off** until measured
  on the phone; neither raises a legacy thermal limit.

The existing thermal loop supplies samples, with no additional timer or
hardware reads. No 5 ms polling, CPU/GPU linking, or changes to CPU limits
are included. These need separate power and latency comparisons.

Host verification (from repository root):

```sh
cc -std=c11 -Wall -Wextra -Werror -iquote xnu/files/bsd/sys \
  tools/tests/gpu_policy.c -o /tmp/gpu-policy-test
/tmp/gpu-policy-test
```

Phone validation still required: cold-start baseline vs switches on, identical
scrolling/game workload, temperature, frame misses, actual clocks and cool-down.
Disable both switches to restore the existing limits. The feature is image code
and needs no manually seeded data-volume files on a clean install.


## Lower Mongoose limits (user request, 2026-11-20)

Balanced now caps M5 at **1586 MHz**, Cool at **1274 MHz**; the 1274 MHz
frame floor remains. Performance retains its explicit 2730 MHz maximum.
The cap is applied after touch/frame boosts and with fixed-clock governor
overrides, and lower thermal/power limits still win. `dvfs_m5_max=KHZ` or
`kern.redwood.m5_max` overrides it (0 removes this extra ceiling).
Balanced/Cool unpark the M5s only after A76 load >=90% for three ticks and
park them again after A76 load <60% and M5 load <20% for two seconds.
This favors the A76s and reduces M5 bursts; performance/temperature impact
needs a cold-start phone comparison. GPU and other zones can also be hot.

Screen idle now defaults to `idle_lock=2`: untouched wake sleeps after10s,
unlocked follows Settings Auto-Lock (including Never); recent audio keeps
the unlocked screen awake. AOD suppression remains in place. Runtime
`redwood-idle.conf` and boot arguments retain explicit override support.


## Lower GPU ceiling (user request, 2026-11-20)

Balanced now caps GPU bursts at **572 MHz**, Cool at **455 MHz**; explicit
Performance allows 800 MHz. Touch, frame and slow-job boosts all pass through
this ceiling. Lower thermal/efficiency/sleep limits still win. Override with
`dvfs_gpu_max=KHZ` or `kern.redwood.gpu_max` (0 removes this additional ceiling).
This clock cap is separate from the experimental GPU ladder/sustained policy,
which remains optional until measured on the phone. Actual heat and frame-rate
impact is still pending phone validation.
