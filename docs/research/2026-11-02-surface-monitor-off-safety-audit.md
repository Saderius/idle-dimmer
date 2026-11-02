# Surface monitor-off safety audit — 2026-11-02

Status: SOURCE-AUDITED 2026-11-02. No screen-off, brightness write, sleep, input synthesis, or hardware experiment was performed. The Surface is not connected to this session. This is an audit of the monitor-control implementation, not acrylic appearance. App sources and settings were not changed.

## Verdict

No direct firmware-flash, voltage, embedded-controller, device-disable, power-plan rewrite, raw VCP power-off, or custom-driver control operation was found in the application source paths reviewed. The original Surface WMI brightness implementation remains byte-for-byte unchanged at function level. The added screen-off implementation requests a documented Windows display-power action. This supports a low expected risk of physical damage from ordinary intended operation; it is not a guarantee about arbitrary driver defects or proof of correct wake behavior on an untested Surface.

A complete reliability sign-off is not justified: there are concrete timeout, input-registration, and portability gaps below. Do not describe these as evidence of physical damage. Do not describe original Surface brightness dimming as a prior successful test of the later true screen-off implementation.

## Git history, not recollection

INSPECTED 2026-11-02: all locally available commits, earliest first:

- `b8adedd4601aa90d52fcc2ba94b2b497de66420a`, 2026-10-30: initial release. Internal-panel WMI brightness dimming and restoration. No `DdcBrightness.h`, `SC_MONITORPOWER`, `OffDisplays`, or `StartOffInsteadOfDim`.
- `0fbdd5d`, 2026-10-31: build/CI change.
- `3ec8c43`, 2026-11-01: checkpoint containing the added DDC brightness backend and Windows screen-off/wake path.
- Current working tree, inspected 2026-11-02: the hardware brightness and screen-off/wake functions below match that checkpoint.

No earlier Git history is available locally. Dates embedded in old documentation are historical claims, not additional commits or a live test of this build.

Function comparison (SOURCE-COMPARED 2026-11-02):

| Function group | Earliest release to current | Checkpoint to current |
|---|---|---|
| `Capture`, `SetLevels`, `SaveBrightness`, `LoadBrightness` | Identical | Identical |
| `InputEvents` | Identical | Identical |
| `CaptureDdc`, `SetDdcLevels`, `LoadDdcBrightness` | Added after initial release | Identical |
| `SetKeepAwake`, `OffDisplays`, `WakeDisplays`, `StartOffInsteadOfDim` | Added after initial release | Identical |
| `Restore`, `Dim` | Extended for DDC and screen-off | Identical |

The original `ApplyEnabled` already requested `ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED`. Keeping the PC awake is therefore not a newly discovered regression caused by adding screen-off, although retaining that claim after screen-off affects laptop power policy.

Reproducible extraction and comparison results are retained locally under ignored `build-tools/surface-power-audit-2026-11-02/prepare-audit.py` and `git-function-comparison.json`.

## What the Surface paths actually do

`IdleDimmer.cpp:277` captures active WMI instances and current brightness. `SetLevels` at line 311 writes either zero percent or the captured value through `WmiSetBrightness`. Capture, write, and WMI recovery validation are unchanged from the original release. Recovery is persisted before dimming; normal input, lock/suspend/resume, and shutdown reach restoration. This is controlled brightness, not firmware flashing and not proof that the panel is powered off.

Automatic off-instead-of-dim calls `StartOffInsteadOfDim` without calling `Dim`, so that path does not intentionally perform brightness reads/writes. `OffDisplays` at line 491 releases the display-awake claim, registers raw input, and broadcasts `WM_SYSCOMMAND / SC_MONITORPOWER / 2`. `WakeDisplays` at line 506 uses the corresponding `-1` request. There is no DDC power-state write in this path.

The footer off button at line 1475 does not share the automatic off-only entry path: it attempts `Dim()` before `OffDisplays()`. Consequently, do not tell a user that every off action avoids brightness access. Fresh-input checks may abort that dim attempt, but do not make the attempted backend queries disappear.

DDC enumeration (`DdcBrightness.h:142`) has no explicit internal-panel exclusion. Every enumerated physical monitor is queried, and only a successful brightness query with nonzero reported maximum makes it eligible for a brightness write. The code's comments describe external-monitor intent, not an enforced device-class boundary. Do not assert that the current app can never attempt DDC discovery on a Surface's internal display.

## Local follow-up tickets (open; no fixes made by this audit)

### SP-01 — Bound display-power broadcasts and avoid asserting a confirmed hardware state

Evidence: `IdleDimmer.cpp:497` and `509` use synchronous `SendMessageW(HWND_BROADCAST, ...)`. Windows documents that it waits for recipient processing. An unresponsive recipient can block the app's UI thread, including its restore/input handling. `displaysOff` is updated without observing actual display-power state. This is a source-demonstrated possibility, not a reproduced Surface failure or physical-damage finding.

Proposed acceptance: bounded dispatch, explicit handling of dispatch failures, no claim that dispatch alone verifies physical panel state, and retained recovery/input responsiveness. Validate the chosen API and reentrancy behavior before replacing it; do not simply copy an arbitrary timeout loop.

### SP-02 — Honor failed input registration before switching displays off

Evidence: `OffDisplays` ignores `InputEvents(true)` failure, unlike `Dim`, which aborts/unwinds when input registration fails. Windows can still handle wake independently, and the scheduler can notice later activity; the defect is loss of the app's intended immediate input path, not proof that a Surface becomes permanently unwakeable.

Proposed acceptance: explicit failure handling with truthful status and a bounded/recoverable outcome. Preserve existing tests and add a focused failure-path decision test if logic is extracted. Do not simulate real input on the user's desktop.

### SP-03 — Define off-only consistently and enforce backend selection

Evidence: the footer off button attempts dimming; the automatic off-only setting skips it. DDC lacks an explicit internal-device filter or a user opt-out.

Proposed acceptance: an explicit off-only operation bypasses all hardware-brightness access at every entry point. If backend overrides are added, provide a way to retain the original WMI-only Surface behavior and disable DDC discovery/writes. A read-success heuristic must not be documented as device-type identification.

### SP-04 — Bind recovery state to the originating machine/display

Evidence: startup automatically loads `brightness-recovery.bin` and `ddc-recovery.bin` and attempts restore (`IdleDimmer.cpp:3823`). Files are adjacent to the executable. WMI recovery validates a method path and percentage but not a separate originating-machine identity. DDC matching uses display description plus occurrence, with an ordinal fallback, rather than a stable physical identity. Copying an app folder with runtime recovery state, or changing identical-display topology, is not equivalent to a clean deployment. A mismatched WMI path may fail; no success on a foreign device is asserted.

Proposed acceptance: stable machine/display identity validation or safely refuse foreign/ambiguous recovery. Portable/release copies must exclude runtime recovery files. Preserve recovery for a genuine same-machine crash. Do not delete the running machine's recovery file while dimmed.

### SP-05 — State and validate the intended Surface sleep policy

Evidence: `SetKeepAwake(false)` retains `ES_SYSTEM_REQUIRED`. The original app also requested system-awake while enabled. This can affect automatic sleep and battery use; actual Modern Standby behavior must not be inferred solely from those flags. Normal user sleep/lid behavior must continue to be respected. Suspend currently calls `Restore`, which in turn wakes displays if marked off; audit this interaction on the target device instead of assuming it matches a regular mouse wake.

Proposed acceptance: explicitly retain or revise the intended keep-awake policy, then observe the Surface on AC and battery, including lock, lid/sleep/resume, without registry workarounds that force another sleep model. No new busy polling.

## Microsoft primary references checked 2026-11-02

- [Surface Brightness Control](https://learn.microsoft.com/en-us/surface/microsoft-surface-brightness-control): Microsoft supports idle brightness reduction, including a configurable percentage range of zero through full brightness, on Surface Book and other Surface models. This supports the ordinary nature of dimming; it does not establish the internals or approval of this application.
- [WmiSetBrightness](https://learn.microsoft.com/en-us/windows/win32/wmicoreprov/wmisetbrightness-method-in-class-wmimonitorbrightnessmethods): documented percentage-brightness method and return code.
- [WM_SYSCOMMAND / SC_MONITORPOWER](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-syscommand): documented display-power request for power-saving devices, including battery-powered PCs. Does not certify every driver or guarantee a specific Surface wake input.
- [SendMessageW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagew): synchronous processing and broadcast scope.
- [SetThreadExecutionState](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadexecutionstate): display/system requirements and limits; cannot be used to prevent the user explicitly putting the PC to sleep.
- [Modern Standby states](https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/modern-standby-states): screen-off and sleep are distinct parts of the Modern Standby session.
- [Surface device power states](https://support.microsoft.com/en-us/surface/battery/surface-device-power-states): Surface-specific power/wake guidance, not a guarantee for this app.
- [SetMonitorBrightness](https://learn.microsoft.com/en-us/windows/win32/api/highlevelmonitorconfigurationapi/nf-highlevelmonitorconfigurationapi-setmonitorbrightness): Microsoft cautions that incomplete MCCS implementations can cause undefined monitor behavior; arbitrary monitor compatibility needs physical validation.

## Validation boundary

SOURCE-AUDITED 2026-11-02, current working tree and all locally available Git history. Target Surface hardware tests: NEVER MEASURED for this build. No universal safety guarantee, exact driver behavior, battery effect, or wake latency is claimed. Existing self-test results are not hardware-safety certification. No application code or running-app state was changed by the audit.

## Independent external review and corrections

COMPLETED 2026-11-02: actual `agy.exe` CLI review with the user-selected
`gemini-3.8-flash-high`, conversation `ae3a518e-8af0-46d0-8699-767b38273c86`.
The prompt contained initial/current source excerpts and function comparisons, requested a
read-only review, and prohibited tools or hardware requests. This was not a renamed Codex
subagent. The two initial CLI launch failures did not produce reviews and are not counted as
additional critiques. Only the completed result is used here. Raw prompt/result and original
response are preserved under ignored `build-tools/surface-power-audit-2026-11-02/`.

The independent reviewer corroborated retained original WMI code, lack of an explicit
internal-panel exclusion in DDC enumeration, unbounded broadcasts, and the retained
system-awake claim. Its prose is not treated as authoritative. Source review rejects or
qualifies the following claims from that response:

- **"Cannot cause physical hardware damage" / "no risk"**: stronger than the source can prove.
  This audit found no direct damage mechanism; that is not an absolute guarantee about drivers.
- **"Concurrent" or "parallel" WMI and DDC execution**: the relevant functions call the backends
  sequentially. Both can participate in one operation, which is not simultaneous execution.
- **External DDC restore failure leaves the internal panel at zero**: not established. `Restore`
  calls `SetLevels(saved,true)` before checking the combined result. If WMI succeeds and DDC
  fails, the internal brightness has already been restored. Retaining state/refusing exit on
  partial failure is real; the claimed inevitable Surface blackout is false.
- **Working-directory-dependent recovery paths**: false in this application. Startup constructs
  `folder`, `stateFile`, and `ddcFile` from `GetModuleFileNameW`, not the working directory
  (`IdleDimmer.cpp:3766`). The actual portability issue is copying recovery state across devices.
- **The shutdown restore-failure guard was added in the current version**: false; the initial
  release already contains that guard. The function-comparison output shows it; the later
  shutdown difference is removal of the obsolete keyboard registration plus changed UI cleanup
  behavior through `HidePanel`.
- **Brightness zero necessarily means zero PWM/backlight power**: not established and must not
  be used to equate minimum brightness with display-off.
- **The power request necessarily prevents DRIPS / runs the Surface at full power in a bag**:
  not established by this source review. Windows documents the power requirements, but actual
  Modern Standby residency, lid behavior, heat and battery effects need target measurements.
- **A pen or detach operation necessarily strands the Surface**: not established. Raw-input
  subscriptions lack a separate pen usage, but OS wake, synthesized pointer events and ordinary
  idle activity detection must be considered. Detach invalidation is a validation scenario, not
  a reproduced failure. No device-specific raw-input or display-driver behavior was observed.
- **A blocked app broadcast prevents every OS wake route**: false as a blanket statement. It
  can block the app's own handling; it does not prove that Windows/hardware wake is disabled.

The findings and verdict above reflect the source-corrected review, not the independent
reviewer's stronger unsupported conclusions. Source audit is complete; SP-01 through SP-05
remain open local follow-up tickets. No external issue was posted and no app code was changed.

## Subsequent incident and candidate fix

UNVERIFIED 2026-11-02: after this audit the user reported monitor on/off cycling and explicitly
requested a fix without launching the app. The pre-fix findings above remain a historical
snapshot. The candidate removes synchronous broadcasts, checks input registration, separates
manual off from brightness control, and owns off as a single session with filtered/tagged
input. It is intentionally left stopped. See
[display-off incident and candidate](2026-11-02-display-off-loop.md) for the evidence limits,
source changes, hardware-free checks and pending user validation. SP-01 and SP-02 now have
candidate source changes; no live resolution is claimed. Backend classification/overrides,
foreign recovery state, and broader sleep policy remain outside this narrowly scoped fix.
