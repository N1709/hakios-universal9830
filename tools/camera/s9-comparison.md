# Working S9 port compared with S20, 2026-11-09

The relevant existing clones are under the research root:

- `git/Scion-Project__scion-starlte`, revision `76bbd6000aeea88c4da5e3e0cdc2225178d1eac0`, `CAMERA.md`.
- `git/Scion-Project__scion-kernel`, revision `cfffe8ab51efbc0dd60fe799e877bea8631805f4`, `drivers/camera/exynos/hakios.exynos_camera.c`.

The [Scion camera notes](https://github.com/Scion-Project/scion-starlte/blob/main/CAMERA.md)
report a front-camera optical stream at 30 fps in stock Camera.app. Rear
capture is still a plan there. These are the project's documented results,
not an S9 device test performed in this session. The separately cloned
`veshrao__ios-galaxy-s9` is not this working implementation.

| Layer | S9 Scion | S20 x1slte |
| --- | --- | --- |
| Front sensor | S5K3H1 | SAK3J1 / S5K3J1S, ID 0x30a1 |
| Sensor bus | Hardware HSI2C, address 0x35 | Software I2C on HSI2C1 pins, address 0x10 |
| Mode | 1632x1224 RAW10, 2 lanes, 719 Mbps/lane | 1824x1368 RAW10, 4 lanes, 1807 Mbps/lane |
| CSI implementation | v5.1, Exynos9810 | v5.3, Exynos990 |
| Receiver / PHY setup | S9 addresses, supplies, reset and timing | Must follow x1slte DT and Samsung v8.1/v5.3 sources |
| RAW delivery | WDMA ring, character device, mediaserverd capture bridge | Synthetic RAW chart already traverses our capture bridge; optical delivery unproven |
| Image processing | CPU debayer, white balance, gamma | Same general userspace route is available |

## What to reuse

The architecture is applicable: sensor -> CSI -> RAW DMA -> software image
processing -> iOS capture bridge. There is no need to emulate Apple's ISP
to follow the S9 approach. Hardware register values and sensor setfiles are
not interchangeable.

The S9 driver has a useful separate diagnostic: `pattern_start()` and
`step4()` use the **CSI DMA block's internal pattern generator**. This
bypasses the optical sensor and MIPI link and tests DMA/memory independently.
Our `cam.s20.front.stream 3` instead enables the **sensor's** test pattern;
it still depends on MIPI and is not an equivalent test. Camera.app's
synthetic chart is a third, entirely software-generated pattern.

Before implementing an S20 DMA pattern test, establish the v5.3 DMA-common
base and routing, safe SysMMU access/translation state, an exclusively owned
32-bit DMA address range, and bounded stop/abort with verified idle state.
Do not reuse the S9's hard-coded lower-RAM allocation at 0xd8000000.
Samsung v5.3 has `csi_hw_s_dma_common_pattern_enable()`; its TPG fields and
blanking differ from the v5.1 implementation. A successful DMA pattern
would establish only the downstream path, not fix or prove the sensor link.

## Concrete diagnostic corrections and live readings

Samsung `is-hw-csi-v5_3.h` defines 32 DMA frame addresses. Consequently:

| VC0 offset | v5.3 meaning |
| --- | --- |
| 0x04 | DMA0_FMT, previously mislabeled as resolution in our report |
| 0x30 / 0x40 | Frame addresses 9 / 13; **not** S9 ACT_CTRL / RESOL |
| 0x90 | DMA0_ACT_CTRL |
| 0xa4 | DMA0_RESOL |
| 0xb4 | DMA0_FCNTSEQ_STAT |

Corrected the inventory labels/offsets and added actual upstream CSIS clock
readbacks from CMU_TOP, not just CMU_CSIS's downstream user mux. Read-only
commands against the current iOS boot returned:

```text
CMU_TOP + 0x104c (CSIS bus source mux): 0x00000002
CMU_TOP + 0x1848 (CSIS bus divider):   0x00000005
CMU_TOP + 0x204c (CSIS bus gate):      0x00200000
DMA1 VC0 + 0x04 (format):             0x00000000
DMA1 VC0 + 0x90 (active control):     0x0001003c
DMA1 VC0 + 0xa4 (resolution):         0x00000000
DMA1 VC0 + 0xb4 (frame sequence):     0x00000000
```

Per Exynos990 `cmucal-node.c`/`cmucal-sfr.c`, mux 2 selects
PLL_SHARED0_DIV4 and divider 5 divides that by six. The gate's bit 21
reads set. This is a configuration readback, not a measured clock rate;
the PLL frequency and active Samsung reference values still need checking.
S9 explicitly raises its upstream camera bus rate because its boot default
caused DMA overflow. That is a reason to compare this clock, not evidence
that increasing the S20 clock will solve its zero packet count.

No clock, DMA, sensor or PHY registers were changed by these live reads.
The inventory code change is prepared for the next kernel flash. The camera
source compiled and the kernel linked; the build wrapper still fails at the
existing unrelated Private.symbolset export/import mismatch. No new kernel
was flashed for this diagnostic-only change.

Current optical status remains: the sensor's internal counter advances,
CSIS1 receives zero frames in the bounded test, and Camera.app displays a
synthetic chart. Matching selected register values is not a completed camera.
