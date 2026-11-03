# Whole-window animation cadence

Status: MEASURED 2026-11-02 for application update timing. USER-CONFIRMED 2026-11-03:
the user described the final combined cadence/WARP upload-device build (`02d298c`)
as "butter smooth". This is acceptance of the reviewed live result, not an independent
review of the earlier standalone cadence artifact. No claim of exact Windows parity
or measured screen presentation rate is made.

## How this refinement was handled in the chat

This was a continuation of the flyout-animation work, not a replacement for the
earlier native acrylic or display-power work. The sequence matters because an
accepted earlier appearance must not be confused with approval of the latest timing
change.

| Stage | User observation or request | Work and outcome |
|---|---|---|
| Direction | The entrance should rise from the bottom. | Earlier work replaced the initial presentation with an upward transition. The user then supplied recordings showing remaining faults. |
| First-frame integrity | An empty skeleton or border appeared before the controls; the frame could blink. | The host was cloaked during GPU upload and composition preparation. Whole-HWND motion kept the material, content, border, and shadow together. Compile/readiness success alone was not treated as proof that the visible flash was gone. |
| Shell comparison | The native panel emerged behind the taskbar; ours overlapped it, flashed, and sometimes moved again at completion. | Recording comparisons, source inspection, and traces identified nested opening/teardown and competing positioning paths. Earlier work guarded preparation, corrected taskbar ordering, and made the UI thread the position authority. The cause of every recorded late displacement was not established. |
| Independent earlier critique | The user explicitly requested an external harness and selected Gemini 3.8 Flash High through `agy`. | That earlier phase used the actual CLI and extracted recording frames, not renamed Codex subagents. Gemini produced an initial patch; parent review corrected it. Its initial rounding explanation was retracted when it disagreed with the recording evidence. See the linked earlier reports below. |
| Earlier acceptance | The user described the running revision as "generally great!" and deferred further polish. | This accepted the general earlier result. It did not certify shell parity, every taskbar configuration, or the later cadence candidate. |
| Smoothness diagnosis | The user later said ours looked closer to a lower frame rate while Windows appeared to follow the monitor. | Earlier traces supported investigating cadence, but included logging overhead. The actual display frame rate and a fixed cap were not established. |
| Execution correction | The assistant initially explained what should change and ended the turn; the user asked whether any work was actually running. | At that point no implementation or new measurement was running. The assistant acknowledged stopping after analysis and then began the implementation. An explanation was not retroactively described as completed work. |
| Fresh measurement | A current comparison was needed before assigning the cause. | Temporary in-memory counters measured applied positions and control-paint cost on the existing implementation. A baseline executable and source snapshot were preserved locally. No per-frame log file was written. |
| Candidate | Reduce per-step work and follow composition cadence. | The compositor-clock path, one-pending-position acknowledgement, and retained GPU control texture were implemented together. The easing, travel, taskbar ordering, prepared first frame, and whole-window movement were preserved. |
| Comparison and cleanup | Establish what improved without declaring visual victory. | The same open/close protocol measured the candidate. Counters were then removed, followed by another build/self-test and launch of the clean candidate. The final handoff explicitly asked the user to judge smoothness. |

No additional external critic or subagent implemented or reviewed this latest
cadence change. References were read directly, and the implementation and measurement
were performed by the primary coding agent. The earlier Gemini participation belongs
to the preceding structural repair phase only.

Earlier evidence:
[recording comparison](2026-11-02-flyout-animation-comparison.md),
[Gemini critique and corrections](2026-11-02-gemini-flash-animation-critique.md), and
[current handoff](../HANDOFF-acrylic.md).

## Measurement protocol and interpretation

Protocol settings used for both builds: three complete open/close cycles, invoked
through the existing `--menu` command. The driver waited 700 milliseconds after
opening and 500 milliseconds after closing before requesting `--status`, so the
report was written after the motion had finished. Those waits were in the external
measurement command, not inserted into the animation implementation.

The counters used `QueryPerformanceCounter` at the UI-applied position and around
the control paint/upload submission path. Mean interval was calculated as the time
between the first and last applied position divided by the number of intervals.
It excludes preparation and time to the first position; it is not an opening-latency
measurement. Paint totals include paints while the panel's motion state was active,
which can include the initial invalidation before the first movement update.

The driver never moved or clicked the real mouse, synthesized keyboard input,
installed a hotkey, changed monitor modes, or exercised display-off/brightness
controls. Graceful rebuild/relaunch and normal panel command IPC were used. No new
desktop video was recorded during this phase. Existing recordings were context,
not a high-frame-rate measurement of the new build.

The requested design was to improve intermediate movement delivery while keeping
the existing animation. Source constants retained: `kNativeSlideTravel` is 96 logical
pixels, entrance duration is 180 milliseconds, and exit duration is 150 milliseconds.
These are app parameters approximating the earlier reference, not private Windows
constants. The clean build still uses native WAM interpolation; it does not make the
slide longer merely to produce more update samples.

## Observation and baseline

The user observed that Windows appeared to animate at display cadence while Idle
Dimmer looked closer to a lower frame rate. Earlier instrumented traces had shown
coarse movement updates, but those traces also included file logging. A fresh
baseline therefore used temporary counters kept entirely in memory. Only an
explicit status request wrote the collected result to the normal diagnostic report.

MEASURED 2026-11-02, existing implementation, three complete open/close cycles,
same machine and geometry:

| Per-cycle result | Entry | Exit |
|---|---|---|
| UI-applied movement updates | 7, 7, 8 | 6, 7, 6 |
| Mean interval between applied positions, milliseconds | 26.148, 24.265, 21.555 | 28.557, 22.182, 31.134 |
| Total control paint time during motion, milliseconds | 107.500, 103.349, 106.595 | 87.466, 85.165, 73.140 |

The moving panel regenerated the transparent controls by painting over black and
white, recovering alpha, copying the pixels, uploading a new texture, and presenting
after every position. Source inspection and these paint timings establish substantial
avoidable per-step work. The measurements do not assign all delay to that work or
establish a fixed frame-rate cap.

## Candidate mechanism

- Windows Animation Manager retains the existing interpolation and duration.
- `DCompositionWaitForCompositorClock` drives explicit manager updates only while
  a transition is active. There is no permanent frame timer or compositor subscription.
- The UI thread remains the sole HWND positioning authority. A `FrameApplied` event
  acknowledges each applied position before another position request is posted.
- A GPU `controlsTexture` retains the prepared content during the slide. Each
  presentation copies it into the current flip buffer, avoiding an uninitialized
  alternate buffer. Moving the HWND does not reconstruct the controls on the CPU.
- Entrance completion refreshes the content once; ordinary visible-state changes
  still repaint normally after the slide.
- The previous WAM automatic timer remains a compatibility path when the compositor
  API is absent. A failed or occluded clock finishes the finite transition rather
  than immediately re-entering a failing wait in a busy loop.
- Immediate hide signals the stop event, joins the renderer, and releases cached
  texture/event/clock resources. No input or display-power implementation was changed.

The existing first-frame GPU preparation, cloak, taskbar ordering, session/direction
validation, complete HWND motion, and single final-position authority are retained.
The animation does not request a refresh-rate boost or change display modes.

## Candidate measurement

MEASURED 2026-11-02, profiled candidate, same three-cycle procedure:

| Per-cycle result | Entry | Exit |
|---|---|---|
| UI-applied movement updates | 12, 12, 12 | 10, 10, 10 |
| Mean interval between applied positions, milliseconds | 16.581, 16.478, 16.597 | 16.683, 16.760, 16.677 |
| Maximum interval between applied positions, milliseconds | 17.000, 16.874, 16.828 | 17.351, 17.456, 16.806 |
| Control repaints during motion | 0, 0, 0 | 0, 0, 0 |

This is approximately display-rate application movement on the tested setup, around
sixty applied steps per second. It is not a direct measurement of displayed frames,
nor a test on a high-refresh display. Both baseline and candidate passed the full
app self-test (MEASURED 2026-11-02: 2,368 checks). All measured closes reported the native
worker absent, and two rapid interruption sequences also ended hidden with the
worker absent and no animation error. Those rapid sequences interrupted preparation
or early startup, so they do not substitute for a user closing midway through a
visibly moving panel.

Temporary counters were removed after measurement. The profiled executables, source
snapshots and before/after reports remain local under the ignored
`build-tools/animation-cadence-2026-11-02/` directory. Synthetic checks and these timings
do not verify perceived smoothness, frame/border integrity, or click-away behavior.
The rebuilt clean candidate must be judged by the user on the desktop.

MEASURED 2026-11-02, counters removed, clean build: 288,256 bytes; full self-test
PASS with 2,368 checks; launched on the interactive desktop as PID 55400. Its
SHA-256 is `9D6E1EC60A30EE52B9356DC66E58C69DFA35AE76F3E511C5FD0BE852662F9EF7`.
A subsequent normal open/close reached the prepared visible state and completed
both transitions without an animation error. During a 20.006-second hidden sample,
the process CPU-time counter increased by 0 ms, presentations remained at 25, and
the native worker was absent at both ends. That sample was enabled and waiting for
idle, with the idle-check counter unchanged at 1; it does not measure steady-state
media checks, literal zero CPU cycles, or long-term energy use.

## Files changed and evidence retained

| File | Role in the change |
|---|---|
| `NativeBackdrop.cpp` | Loads the optional compositor clock, explicitly advances WAM during motion, waits for UI acknowledgement, retains GPU control pixels, and releases the added resources on teardown. |
| `NativeBackdrop.h` | Exposes `FrameApplied` so the UI can acknowledge an applied position. Temporary profiling APIs were removed. |
| `IdleDimmer.cpp` | Acknowledges the whole-window move and skips redundant control reconstruction while entering/exiting. Completion still performs no separate reposition. |
| `IMPLEMENTATION.md` | Records the candidate mechanism, current measurement limits, binary footprint, and verification status. |
| `tools/check-docs.ps1` | Adds the new clock, acknowledgement, pending-position, and retained-texture mechanisms to the documentation gate. |

[Structured measurements](assets/animation-cadence-2026-11-02/measurements.json)
preserve the baseline and profiled-candidate reports in the repository. Local
snapshots, profiled executables, raw endpoint reports, and the clean build's hidden
sample are under ignored `build-tools/animation-cadence-2026-11-02/`. The clean
candidate contains no `MotionMetrics`, `MotionApplied`, `MotionPaint`, or per-frame
audit output. Old display-power and presentation logs were not truncated or reused.

## Review still required

At the original handoff, the request to document the work was not visual acceptance.
The later 2026-11-03 "butter smooth" feedback confirms the final combined live build's
perceived smoothness. The following targeted cases still require separate evidence:

- Matched native-Windows comparisons at higher refresh rates or across monitors
  with different timing, beyond the accepted smoothness on the reviewed desktop.
- Frame/shadow/content integrity, behind-taskbar appearance, and absence of a final
  visible position correction in a new matched recording.
- A real user dismissing the flyout midway through its visible movement.
- The legacy timer path on a system without the compositor-clock API and the
  failure/occlusion branch on a disconnected or sleeping display; these were not
  exercised by forcing hardware power transitions.

If the user reports remaining stepping or flashing, obtain fresh evidence of that
specific behavior and instrument the producer, UI queue, and presentation stages
as needed. Do not treat the applied-position counters as actual displayed frames,
randomly retune the duration, or fall back to a client-only slide inside a stationary
border. Keep all cadence work finite and remove temporary instrumentation before
committing.

## References read

- [Microsoft compositor clock overview and wait-loop example](https://learn.microsoft.com/en-us/windows/win32/directcomp/compositor-clock/compositor-clock).
- [Compositor wait API](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-dcompositionwaitforcompositorclock).
- [Zed's Windows vsync implementation](https://github.com/zed-industries/zed/blob/main/crates/gpui_windows/src/vsync.rs), including early-return behavior on an occluded display.
- The prior [Windows Animation Manager timer sample](https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/Win7Samples/multimedia/WindowsAnimation/TimerDriven), retained as the compatibility model.
