# Rejected flyout device cache: experiment and audit record

Status: **REJECTED 2026-11-03** because the retained hardware device increased hidden
CPU cycles. The cache implementation described below was removed. The current
[opening refinement](2026-11-03-flyout-opening-latency.md) uses a WARP upload device
and retains full resource teardown on hide. The rejected cache code is excluded
from the final implementation; this document preserves its audit evidence.

## UX problem and cause

Clicking the tray icon previously caused a period with no visible response while
the app prepared its renderer. GPU device creation was the dominant measured cost,
not the animation's frame cadence. The panel intentionally stays cloaked until its
complete first frame is ready, preventing the previously reported empty-frame flash.
The preceding [baseline investigation](2026-11-03-flyout-opening-latency.md) measured
the preparation path on the committed compositor-clock build.

## Rejected change and ownership (historical)

`NativeBackdrop.cpp` retains one `ID3D11Device` reference in `cachedDevice` between
normal openings. A new scene's `AcquireDevice` checks `GetDeviceRemovedReason`,
reuses a healthy device or creates a replacement, then acquires its immediate
context. `Stop` still joins the previous worker before another can start. Cache
access occurs on that one worker or on the UI thread after joining it, so immediate
context and DXGI operations do not overlap across rendering workers.

Normal hide still closes the composition target/compositor, releases textures and
swapchains, cancels animation, drains/shuts down the dispatcher queue, and joins the
application rendering worker. The immediate context is cleared and flushed during
scene destruction. Retaining the device does not add an application timer, polling
loop, compositor-clock subscription, render worker, or startup prewarming operation.
Driver-internal work remains a measurement question, not something established by
the absence of an application worker.

`ReleaseDevice` stops and joins the worker before discarding the retained device.
It is called on normal shutdown, suspend, display-topology notification, and
settings/power notifications that disable native acrylic. Suspend and topology
notification also hide the panel. Native scene startup/presentation failure clears
the cache after join, allowing a later opening to create a new device. A join
timeout retains the reference rather than racing the worker.

The first opening after process startup or eviction is still cold. There is no
claim that the initial startup delay has disappeared. Control rasterization,
composition initialization and first-frame readiness still occur on every open.
The visibility safeguards, animation, click handling, and display-power operations
were not replaced by this optimization.

## Opening measurements

MEASURED 2026-11-03 on the user's desktop. Temporary QPC stage timestamps were
buffered in memory and appended to a separate file after entrance completion.
Existing `--menu` requests opened/closed the real flyout; no mouse/keyboard input
was synthesized. An explicit cache eviction was inserted only into the timing
build before its fifth opening, then removed from the clean candidate.

| Opening (milliseconds, MEASURED 2026-11-03) | Device acquisition | Request to uncloak | Request to first applied position |
|---|---:|---:|---:|
| First, cold | 126.153 | 205.074 | 216.053 |
| Second, reused | 0.004 | 22.482 | 35.804 |
| Third, reused | 0.004 | 23.023 | 37.513 |
| Fourth, reused | 0.004 | 34.041 | 47.558 |
| Fifth, explicitly evicted | 82.410 | 146.849 | 160.462 |
| Sixth, reused again | 0.004 | 29.285 | 43.148 |

The repeated-opening reduction is consistent with removing device creation from
the critical path. The explicit eviction restores that cost. The acquisition
figures include the helper's device-removal check/context acquisition; rounding
does not imply nanosecond precision. Uncloak and position application are CPU-side
milestones, not actual screen presentation timestamps. Shell tray-click delivery
and Windows Quick Settings latency were not measured. No claim of native parity
or visual acceptance follows from these numbers.

## Clean build and pending checks

Temporary instrumentation, the explicit fifth-opening eviction, and probe includes
were removed before rebuilding the clean candidate. MEASURED 2026-11-03: self-test
PASS, 2368 checks; executable 288,768 bytes; SHA256
`6F8916440B8FE999E61ABFFAC83463079F1003BFA27DBCCEF874B74ECEE96C04`.
The clean candidate was launched on the interactive desktop. Ordinary CLI-driven
open/close inspection reached native acrylic without native/animation error and
confirmed that the render worker exits after hiding. The user may have interacted
during clean deployment, so its animation count is not an isolated benchmark.

MEASURED 2026-11-03, hidden after close: the baseline used 19,183,152 process cycles
over 120.173 seconds, while the cache candidate used 245,577,783 over 120.192 seconds.
Process CPU-time deltas were 0 ms and 31.25 ms respectively. Those counters are
coarse: their earlier zero readings did not mean no CPU work. The baseline was
paused for media throughout; the candidate began waiting for idle and ended paused
for media. These are observations under similar configuration, not a controlled
causal estimate from otherwise identical application state.

A separate MEASURED 2026-11-03 per-thread sample of the warmed cache, lasting about
60 seconds, attributed 90,446,459 cycles to a thread whose start address belonged
to `nvwgf2umx.dll`; that was the largest contributor. The flyout presentation and
animation counters stayed unchanged during the process sample, and its worker was
absent at both ends. This supports rejecting retention for the idle-CPU priority;
it does not explain the driver's internal work or quantify GPU power draw.

GPU power draw, long-duration driver behavior,
real driver removal, suspend/resume, display hotplug, and all policy transitions
remain **UNVERIFIED**; those hardware/power events were not triggered as tests.
The rejected cache changed no monitor brightness or screen-off API.

## Sources read before implementation

- Microsoft's [D3D11 threading contract](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro):
  device methods are thread safe; immediate-context/DXGI use must be serialized.
- Microsoft's [GetDeviceRemovedReason](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-getdeviceremovedreason)
  reference for checking retained-device validity on demand.
- [SDL's shipped D3D11 renderer source](https://github.com/libsdl-org/SDL/blob/main/src/render/direct3d11/SDL_render_d3d11.c),
  specifically separation of device resources from window-size resources and full
  device reconstruction in `D3D11_HandleDeviceLost`. No SDL dependency or code was copied.

Raw timing traces, profiled sources/executable, clean source snapshots, and CPU
sampling evidence remain ignored under `build-tools/device-cache-2026-11-03/`.
