# Flyout opening latency

The baseline below describes commit `7bcdf97`. A subsequent
[device-cache experiment](2026-11-03-flyout-device-cache.md) was rejected because of
increased hidden driver activity. The final WARP upload-device refinement is
documented after the baseline, with visual acceptance **UNVERIFIED**.

## Baseline

MEASURED 2026-11-03 on the user's desktop: four normal open/close cycles using
`--menu`, with temporary in-memory QPC timestamps. No mouse or keyboard input was
synthesized. This measures `ShowPanel` receipt onward, excluding shell delivery of
a physical tray click. Uncloaking and HWND position application are CPU-side
milestones, not measurements of pixels reaching the display. Windows Quick Settings
was not timed, so these results do not quantify its advantage.

| Stage (milliseconds, MEASURED 2026-11-03) | First sampled opening | Following three openings |
|---|---:|---:|
| Control overlay rasterization | 15.261 | 10.419 / 11.480 / 10.605 |
| `D3D11CreateDevice` | 133.168 | 82.331 / 84.579 / 84.027 |
| Composition commit completion wait | 13.202 | 14.831 / 6.889 / 7.584 |
| Open request to uncloaking | 191.622 | 121.982 / 121.818 / 115.367 |
| Open request to first applied animation position | 203.340 | 135.900 / 134.392 / 129.027 |

The dominant measured cost is GPU device creation, including work performed inside
that API by Windows/the driver. These timestamps do not identify its internal
suboperations. `HidePanel` calls `nativebackdrop::Stop`, and destruction of the
worker's `Scene` releases the device, compositor and surfaces. `ShowPanel` therefore
recreates them on every opening, synchronously waiting in `nativebackdrop::Start`.
Repeated openings retain a substantial initialization cost.

Preparation completes while the window is cloaked. Only after controls, the acrylic
tree, GPU fence and composition commit are ready does the animation begin and
`Reveal` uncloak the window. This prevents the previously reported empty outline
flash, but makes preparation visible to the user as a pause before motion. The
previous cadence investigation explicitly excluded opening preparation; improving
frame pacing did not remove this cost. There is no intentional tray-click delay
in this path. The timeout limits are error bounds, not unconditional sleeps.

A candidate optimization is to reuse initialized rendering resources between
openings while detaching the hidden visual tree and stopping animation/capture
activity. This was **NOT IMPLEMENTED** in this investigation. Keeping a device or
composition worker resident must be checked for actual hidden CPU/GPU work and
correct shutdown/device-loss behavior before adoption. Removing the first-frame
readiness safeguard would risk reintroducing the outline flash.

The temporary stage timestamps were buffered in memory; one append to a separate
timing file occurred after entrance completion. No per-stage/per-frame disk logging
was used. Raw evidence and profiled source snapshots remain ignored under
`build-tools/open-latency-2026-11-03/`. Temporary instrumentation was removed and
the original sources restored byte-for-byte. The clean app was rebuilt, passed
the self-test (MEASURED 2026-11-03: 2368 checks), and relaunched on the user's desktop.
Its SHA256 matches the committed animation candidate:
`9D6E1EC60A30EE52B9356DC66E58C69DFA35AE76F3E511C5FD0BE852662F9EF7`.

## Final candidate: WARP for texture uploads, full teardown on hide

After the user authorized implementation with idle CPU as the priority, a retained
hardware-device experiment was measured and rejected. It reduced opening latency
but left additional driver work while hidden. A per-thread cycle sample identified
an NVIDIA driver thread as the dominant contributor; the app's own rendering worker
had already exited. The coarse process CPU-time counter initially reporting no
increase did not establish zero CPU work. See the rejected experiment for evidence
and the differences between sampling conditions.

`Scene::CreateUploadDevice` now requests Windows' `D3D_DRIVER_TYPE_WARP` with BGRA
support and `D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS`. This is
an upload/copy device for already-rasterized GDI control pixels and noise, not a
replacement implementation of acrylic. The app executes no shaders. Windows
Composition still renders the host backdrop and the same acrylic effect graph.
WARP device-creation failure releases partial references and falls back to the
previous hardware-device creation call. Later native failures retain the existing
compatibility-renderer fallback.

The final code adds no retained device, prewarming operation, timer, or hidden
worker. The existing scene/device teardown, queue shutdown, thread join, first-frame
fence/commit checks, cloak/reveal, animation and display-power code remain in place.
Cache-specific power/display notification edits from the rejected experiment were
removed. The device request uses the threading-optimization flag to avoid WARP
creating an internal rasterization pool for this small upload-only workload; it
does not use the separate `SINGLETHREADED` flag or remove D3D thread safety.

MEASURED 2026-11-03, four instrumented WARP openings on the same desktop/protocol:

| Stage (milliseconds, MEASURED 2026-11-03) | First opening | Following three openings |
|---|---:|---:|
| Device creation | 13.687 | 2.095 / 2.107 / 2.738 |
| Open request to uncloaking | 65.717 | 38.005 / 32.754 / 36.854 |
| Open request to first applied position | 75.938 | 51.276 / 46.269 / 49.417 |

Native status showed a prepared native acrylic first frame and successful native
entrance/exit; close status showed worker absence. These timing results do not
certify displayed frame rate, absence of visual glitches, or native parity. The
normal helper follows the same WARP creation call measured here.

An additional temporary fault test forced the helper's creation result to `E_FAIL`
on its second opening, after a real WARP device had been allocated. Partial WARP
references were released, hardware fallback produced native acrylic, and entrance
completed with native/animation HRESULT success. This exercises a simulated helper
failure with partial resources; real hardware/device-loss failures remain untested.

All temporary timing/fault hooks were removed for the final build. MEASURED
2026-11-03: clean executable 288,768 bytes; self-test PASS, 2368 checks; SHA256
`31FEC9E2B1A54DEADB31F7C0C4BF5440C484156EBD31EB6625DCFE272339930F`.
Visual acceptance, high-refresh animation
regression review, Surface behavior, GPU power draw and real graphics-device loss
remain **UNVERIFIED**. No monitor power/brightness operations were invoked as tests.

## Hidden CPU comparison

MEASURED 2026-11-03 using read-only process CPU-time/cycle counters; endpoint status
requests were outside each timed interval. Same user settings: app enabled, idle
threshold one minute, media/sound/full-screen protections enabled, panel closed,
not dimmed and no display-off session. The matched-state baseline and final steady
sample were paused for media playback throughout. No settings or input were changed
to produce these samples. Samples were sequential on a live desktop, not a controlled
statistical experiment; cycle totals are not CPU-time or energy units.

| Build/state (MEASURED 2026-11-03) | Seconds | Accounted CPU increment | Process cycles |
|---|---:|---:|---:|
| Baseline after close, media already settled | 120.173 | 0 ms | 19,183,152 |
| Rejected hardware cache, includes initial state change to media protection | 120.192 | 31.25 ms | 245,577,783 |
| Final WARP after close, includes initial state change to media protection | 120.181 | 15.625 ms | 35,112,125 |
| Final WARP, settled media protection | 120.155 | 0 ms | 17,578,441 |

The final settled sample's working set was 27,377,664 bytes, private memory
4,341,760 bytes, and thread count eight at the end (MEASURED 2026-11-03). Native
presentations stayed at 48, completed animations at four, and the render worker
was absent at both ends. A separate MEASURED 2026-11-03 roughly thirty-second
thread sample contained no NVIDIA-driver-start threads, unlike the rejected cache
sample; thread-start-module attribution is not a call-stack profile.

The final matched-state result shows no observed increase in idle CPU work at this
sample's resolution; it does not establish a statistically significant reduction.
The nonzero cycle count explicitly rules out literal zero CPU use. The initial
post-launch sample is retained rather than omitted because the later steady sample
excludes that startup/state-transition activity. All native rendering resources
are torn down while hidden; GPU energy consumption and other devices were not
measured.

References read before changing upload device selection:

- Microsoft's [Win32 Composition sample device/swapchain helpers](https://github.com/microsoft/Windows.UI.Composition-Win32-Samples/blob/master/cpp/ScreenCaptureforHWND/ScreenCaptureforHWND/d3dHelpers.h)
  use WARP as a device option with composition swapchains. This repo does not add
  that sample's framework or C++/WinRT dependency.
- Microsoft's [D3D11 device creation flags](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_create_device_flag)
  document WARP's threading behavior with the optimization-prevention flag. It is
  not recommended as a general rendering-performance default; this upload-only
  utility prioritizes background overhead over parallel rasterization throughput.

Temporary source snapshots, traces, and fallback-test reports are ignored under
`build-tools/warp-upload-2026-11-03/`; CPU sampling evidence is under
`build-tools/device-cache-2026-11-03/`. Only the final upload-device helper remains
in application code. Release publication is separate from this implementation.
Parsed timing/CPU records and clean artifact identity are retained in the
[measurement artifact](assets/opening-latency-2026-11-03/measurements.json).
