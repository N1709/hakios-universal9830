# S20 panel ALPM transport prototype

`alpm.h` is a portable, unconnected protocol core for the observed hubble1 panel
ID81 01 03, ALPM2-nit mode. It is **not hardware support in the running kernel**.
It does not promise an actual2-nit output without measurement/calibration.
No boot argument, setting or user-space action currently sends these commands.

Primary reference: locally available Samsung GPL kernel commit69515fbb7,
`drivers/video/fbdev/exynos/panel/s6e3hab/s6e3hab_hubble1_a3_s0_panel.h`
(lpm_nit_table, lpm_mode_table, lpm_off_table, packet offsets and ALPM sequences)
and `drivers/video/fbdev/exynos/panel/panel_drv.c` (minimum panel-on age, restore
brightness,34ms exit settle, regulator/subdevice handling). Gamma update0x0f is
the normal command-log-disabled form;0x8f is for DDI command logging and unused.

The core requires explicit experimental opt-in, exact panel ID, active display,
60Hz normal mode, no pending transition, and saved normal settings. It has
normal/active/fault states; partial entry attempts full exit and restoration.
Exit attempts every cleanup write, reports a fault on any failed write or restore,
and blocks re-entry until recovery. The caller owns serialization, hardware
identity/readback, frame validity, normal-mode and brightness restoration.
Callbacks receive parameter offsets separately: the real transport must issue
the appropriate B0 global-parameter command before the register payload.

Host validation (no device access):

```
xcrun clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/aod/panel/test_alpm.c -o out/aod/test_alpm
out/aod/test_alpm
```

Tests inject failure into every entry/exit transfer, verify restoration and fault
latching, and check profile refusal, register offsets, minimum124ms panel-on age,
16.7ms intermediate delay,34ms exit settle and repeat requests. This validates
transport logic only. It does not validate panel behavior or power consumption.

Driver integration still required before a flash candidate is reviewable:

- serialize with presenter, mode changes, brightness worker and pending off fade;
- transition120Hz to supported60Hz normal mode before entry, saving the exact mode;
- retain valid content and plan update/TE/DECON/clock ownership in panel low power;
- preserve/reapply panel gamma/AOR/brightness and full mode before wake frames;
- account for Samsung regulator/short-detection and panel AOD/self-move policy;
- feed completed native AOD transitions, cancel stale work on wake/disable/crash;
- verify readback and repeated lock/wake/disable behavior, measure actual power.

Keep experimental hardware work disabled and separate from the working native
AOD service until those contracts are implemented and tested. Never replace the
framebuffer's binary off operation with raw ALPM writes: its existing off path
masks frames and lowers SoC clocks for a completely dark display.

Additional stock-path findings: Samsung distinguishes `DECON_STATE_DOZE` (panel
can receive updates) from `DECON_STATE_DOZE_SUSPEND` (deeper host suspend), in
`dpu30/decon_core.c`. Entry flushes the frame worker before switching state.
The port's binary display-off is not equivalent to either retained-content path.
The stock self-display engine separately programs porch and register0x76; it is
not enabled by ALPM brightness commands alone. Its internal clock images are
separate from the native iOS clock, so no Samsung clock bitmap is used here.

The x1slte revision00 device tree has an actual rail difference: panel VDDR goes
from1,800,000uV normal to1,500,000uV low power. VCI remains3,000,000uV and VDD3
remains1,800,000uV. VDDR phandle0x55 maps to S2DOS05 buck1 (`vdd1`); short-detect
phandle0x56 maps to S2DOS05 ELVSS. This is a reference-board finding, not a verified
live-board setting. The prototype makes **no regulator writes**. Restoring that
rail before exit is part of stock sequencing; do not guess PMIC registers or
lower a rail based only on this table.
