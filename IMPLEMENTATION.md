# Idle Dimmer — Implementation Reference

*Created 2026-10-14*

Technical documentation of the current native implementation in `IdleDimmer.cpp` (build 2026-11-01, 251,392 bytes). Companion to [README.md](README.md), which covers usage. Every statement here reflects the code as it exists today; deviations are marked.

## 1. Purpose and design goals

A resident tray utility for Windows that polices focus: after a configurable idle period (default 2 minutes) it takes every display it can control to 0% brightness and restores the previous level on the first user input. Two mechanisms cover different hardware: **WMI `WmiSetBrightness`** for a laptop's internal panel, and **DDC/CI** (`dxva2.dll`) for external monitors. Optionally (`OffMinutes`, or the footer button) the displays are then switched off altogether — a DPMS standby request the OS wakes on the next input — which is the only state in which the backlight is genuinely off (§5.1). The app sleeps until a dim could actually be due rather than waking on a fixed period — see §4.1.

Priority order for all decisions:

1. **Efficiency** — near-zero CPU, minimal memory, minimal wakeups, no polling of input data.
2. **Native Windows behavior** — Win32/WMI/WinRT only; no framework, no runtime, no installer, no network.
3. **Safety** — brightness is always restored (on input, lock, sleep, shutdown, quit, failure); no key content is ever read.

## 2. Build and binary

| Property | Value |
|---|---|
| Compiler | Bundled Zig `cc` (`build-tools\zig-x86_64-windows-0.15.2`), invoked by `build.ps1` |
| Flags | `-Os -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--subsystem,windows -s -municode` |
| Linked | ole32, oleaut32, wbemuuid, winrt (l1-1-0, string), shell32, user32, gdi32, advapi32, wtsapi32 |
| Output | `IdleDimmer.exe`, single file, ~246 KB (MEASURED 2026-11-01: 251,392 bytes; includes the embedded 4x panel icon masks, the multi-resolution app icon, the DDC/CI path, the WASAPI audio fallback, both displays-off modes, and two extra panel glyphs) |
| Runtime deps | OS DLLs only (no VC runtime, no .NET, no Electron). `dxva2.dll` is resolved by name at first use, never linked |

The build also runs `tools\build-panel-icons.ps1` before compiling: it rasterizes
`icons\panel\*.svg` to 80 px PNG alpha masks and regenerates `PanelIconsData.h`,
which `Flyout.cpp` includes. `tools\check-docs.ps1` runs afterwards as the documentation
gate. The source has no exceptions/RTTI and creates no threads.

**The build quits a running instance before linking.** Windows maps `IdleDimmer.exe` while it
runs, so `lld-link` cannot replace the file and fails with `failed to write output ... Permission
denied` — an error that names the file but not the cause. `build.ps1` therefore detects a running
instance, sends it `--quit` (which restores brightness first), waits for the process to exit, and
relaunches it after a successful build. A machine with nothing running, such as CI, skips all of
this. `MEASURED 2026-10-31`: without the guard the build fails in under a second with that message.

## 3. Process model

- **Single instance**: named mutex `Local\IdleDimmerNative`. A second invocation forwards CLI commands (`--enable`, `--quit`, `--status`, `--menu`) to the running instance and exits.
- **Hidden window**: a `WS_EX_TOOLWINDOW | WS_POPUP` 0×0 window (class `IdleDimmerNativeWindow`) owns the tray icon, timers, raw input, and all messages. It is never shown.
- **Single message loop**: `GetMessageW/DispatchMessageW` — the thread sleeps in the kernel while idle. No worker threads, no background polling loop.
- **COM**: `RoInitialize(RO_INIT_SINGLETHREADED)` + `CoInitializeSecurity` at startup (needed for WMI and WinRT media).
- **Desktop attachment**: `OpenDesktopW(L"Default", ...)` + `SetThreadDesktop(desk)` at `wWinMain` entry binds the thread to Explorer's interactive desktop (`WinSta0\Default`), ensuring `Shell_NotifyIconW` can register with the taskbar and CLI IPC forwarding (`FindWindowW`) works even when spawned from an isolated runner desktop (`WinSta0\exebox-...`).

## 4. The check cycle (core loop)

### 4.1 Adaptive wake scheduling (`ScheduleNext`)

The loop used to wake on a **fixed 30 s period** — `SetCoalescableTimer(window, kCheckTimer, 30000, nullptr, 1000)` — and, on every wake, ask two expensive questions *before* looking at the clock:

- `MediaPlaying()` — a WinRT round-trip
- `Fullscreen()` — window enumeration

Both answers only matter at the instant the app is about to dim. Everywhere else they cannot change a decision. So the app now **sleeps until the decision point**:

```
ScheduleNext():                      // thin wrapper; the arithmetic lives in ScheduleWaitMs
  if disabled or locked            → KillTimer; nothing to decide, sleep forever
  if last input unreadable         → arm kCheckMaxMs, return
  SetCoalescableTimer(window, kCheckTimer,
                      ScheduleWaitMs(idle, idleMs, graceOutstanding, graceAgeMs, dimmed),
                      nullptr, 1000)

ScheduleWaitMs(idle, threshold, graceOutstanding, graceAgeMs, dimmed):   // pure, self-tested
  if dimmed                       → kCheckDimmedMs (2 min)
  wait = kCheckMaxMs (15 min backstop, so a coalesced/missed timer cannot strand us)
  if idle < threshold             → wait = min(wait, threshold − idle)   // wake when it could dim
  if graceOutstanding             → wait = min(wait, threshold − graceAgeMs)
  else if idle >= threshold       → wait = kCheckReadyMs (30 s)           // ← the fix, see below
  clamp to [250 ms, 15 min]
```

`ScheduleWaitMs` is pure precisely so the sleep can be asserted. It and its three cadences
(`kCheckReadyMs`, `kCheckDimmedMs`, `kCheckMinMs`) are pinned by `ScheduleChecks()` in `--self-test`.

Wakeups drop from a fixed **2 per minute** to roughly **1 per idle period** — 0.5/min at the default 2-minute threshold, and far fewer at longer thresholds.

**Media detection does not regress.** If a video starts after a check, the next wake is still a decision point: it observes the video and declines to dim. What goes stale *between* checks is the status line, and only while nothing is reading it — with the panel open the 1 s `kPanelTimer` tick re-asks.

### 4.2 Check cycle

On `WM_TIMER` → `Check()`, which **always** ends in `ScheduleNext()`:

```
Check():
  ++checks
  if a saved brightness exists but we are not dimmed   → Restore() (failure-retry path)
  if disabled or session locked                        → return
  ── cheap path ──
  if not dimmed AND idle < idleMs AND now < graceUntil → ScheduleNext(); return
      (no dim can be due, so neither probe can change a decision — this is the common case
       and covers the whole countdown)
  ── decision point ──
  if ProtectMedia      → MediaPlaying()  ∈ {playing, unknown}
  if ProtectFullscreen → Fullscreen()
  if either blocked    → Restore(); grace = now + idleMs; update status
  idle = GetTickCount() − lastInput.dwTime             (wrap-safe DWORD math)
  if dimmed  and idle <  idleMs → Restore(); grace = now + idleMs
  if not dimmed and ShouldDim(...) → Dim()
  else if not dimmed             → refresh status/tooltip
```

`graceUntil` must be armed **before** the first `ScheduleNext()`. `ScheduleNext` reads it, and with `graceUntil == 0` the cheap-path guard `now < graceUntil` fails — the app would dim on its very first wake instead of waiting out a full idle period. `ApplyEnabled()` therefore sets grace, then schedules.

`ShouldDim(on, isLocked, idle, now, grace, threshold, mediaBlocked, fullscreenBlocked)` is a pure function — `on && !isLocked && idle >= threshold && now >= grace && !blocked` — extracted so `--self-test` can exercise it (11 assertions, including threshold and tick-wrap cases).

### Why raw input, not polling

Idle comes from `GetLastInputInfo` — Windows' own last-input timestamp (the same counter the power plan uses). The app never enumerates or records input content. Raw-input **sinks** (`RIDEV_INPUTSINK` for keyboard, mouse, touch, pen) are registered **only while dimmed**, so:

- steady state: zero input-processing overhead;
- while dimmed: the first touch/key/mouse event wakes an immediate restore (sub-second), regardless of focus;
- on restore: `RIDEV_REMOVE` unregisters everything again.

`WM_INPUT` handler deliberately does not parse the payload — "the existence of input is enough" (see code comment). Restore is posted as `kRestore` to keep WMI work off the input path and avoid re-entrancy (`restorePending` flag).

## 5. Dimming and restore pipeline

Two independent mechanisms, captured and restored together, because the hardware they cover is
different:

| Mechanism | Covers | Entry points |
|---|---|---|
| WMI `WmiSetBrightness` (`ROOT\WMI`) | A laptop's **internal** panel | `Capture()`, `SetLevels()` |
| DDC/CI via `dxva2.dll` | **External** monitors | `CaptureDdc()`, `SetDdcLevels()`, `ddc::*` in `DdcBrightness.h` |

**Why DDC/CI exists (MEASURED 2026-10-31, this desktop).** WMI drives the internal panel only. On
a machine with three external monitors and no internal panel, `--diagnose` reported
`Brightness HRESULT: 0x8004100C` (`WBEM_E_NOT_SUPPORTED`), `Internal panels: 0`, so `Dim()`
aborted with "Brightness unavailable" and the screen never changed — while a standalone dxva2
probe read all three monitors at 97%, 100% and 78% and accepted a no-op write on each. The app
worked on the Surface Book 3 because that machine *has* an internal panel. External monitors are
reached with the same API Twinkle Tray uses:
`EnumDisplayMonitors` → `GetPhysicalMonitorsFromHMONITOR` → `GetMonitorBrightness` /
`SetMonitorBrightness`.

`dxva2.dll` is loaded **by name** (`ddc::Library()`), never linked, for the same reason the DWM
entry points are: the project forbids new hard runtime dependencies and a machine without dxva2
must still start. `GetMonitorCapabilities` is deliberately *not* a gate — the three measured
monitors answered `caps=no, flags=0x00000000` while `GetMonitorBrightness` worked on all three,
so trusting the caps call would reproduce the bug it was meant to prevent.

A monitor is only used when `GetMonitorBrightness` succeeds **and** reports `max > 0`; that
filter is what keeps port/dock entries that answer 0..0 out of the saved list.

**Dim** (`Dim()`):

1. `Capture()` — WMI: `WmiMonitorBrightness` (instance name + current %) and
   `WmiMonitorBrightnessMethods` (the `__PATH` used for `WmiSetBrightness`). Up to 4 panels.
2. `CaptureDdc()` — one display walk, saving description + current level per brightness-capable
   physical monitor. Identity is `(description, occurrence)`, not the index: all three measured
   monitors report "Generic PnP Monitor", so the index alone would restore every level to the
   first panel. `ddc::FindMonitor` / `ddc::Occurrence` are pure and pinned by `--self-test`.
3. Either capture succeeding is enough. Both failing is `Brightness unavailable`, with the WMI
   HRESULT reported (the DDC path reports no HRESULT of its own).
4. `SaveBrightness()` / `SaveDdcBrightness()` — write the two recovery structs to
   `brightness-recovery.bin` and `ddc-recovery.bin` (crash/kill safety).
5. `InputEvents(true)` — register raw-input sinks.
6. **Re-check idle** — `GetLastInputInfo` again; if the user returned during the brightness
   queries, undo and bail (never dim over fresh input; `testing` mode skips this).
7. `SetLevels(0)` and `SetDdcLevels(0)` — WMI `WmiSetBrightness(Brightness=0, Timeout=0)` per
   panel, and `SetMonitorBrightness` at the monitor's minimum per DDC monitor. Failing **both**
   mechanisms unwinds; one working mechanism is a dim.
8. On success: `dimmed=true`, log, tray state/icon/tooltip update. If `OffMinutes` is non-zero,
   arm the one-shot `kOffTimer` for the displays-off stage below.

### 5.1 Displays-off stage: `OffMinutes` and the footer button

Two ways to reach it, one implementation (`OffDisplays()` / `WakeDisplays()`):

| Trigger | Behaviour |
|---|---|
| `OffMinutes=N` (N > 0) | `N` minutes **after a dim succeeds**, the one-shot `kOffTimer` fires and the displays are switched off. `Restore()` kills the timer, so a screen that woke early is never switched off afterwards. |
| `OffInsteadOfDim=1` | At the **idle threshold itself**, `Check()` calls `StartOffInsteadOfDim()` instead of `Dim()`: the displays go to standby and the brightness is never touched. `OffMinutes` is then irrelevant — there is no dim to delay after. |
| Footer button (`hitDisplaysOff`, GDI mask `display.svg`) | Manual: dims first when nothing is dimmed, then switches the displays off immediately. Works even if no brightness control answers (`Dim()` fails, the standby request still goes out). |

**Why "off instead of dim" sets `dimmed` without capturing anything.** `ScheduleWaitMs` treats a
non-dimmed machine whose idle time is past the threshold as "a dim is due right now" and returns
`kCheckReadyMs` (250 ms) — the comment there records that this once produced ~144 checks in two
minutes. Switching the displays off is the settled state, so it sets `dimmed=true`, which puts the
scheduler on the two-minute dimmed cadence. `Restore()` therefore had to learn that a dimmed state
with no captured level is legitimate: it skips the brightness write, still accumulates the dark
interval, clears the flag and unregisters raw input. The tray tooltip distinguishes the two
aftermaths — `Displays off; input restores brightness` only when a level was actually captured,
`Displays off; input wakes them` when it was not.

**Mechanism: `SendMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2)`**, not a DDC/CI
power write. Twinkle Tray's button writes VCP `0xD6` over DDC/CI and that works, but it hands
recovery to the monitor: while the panel is in that state only a successful DDC wake write brings
it back, and if the panel ignores it or the link drops, the screen stays dark with no OS-level way
out. The broadcast is the request the power plan itself makes — VESA **DPMS** standby, from which
Windows wakes the display on the next input by itself (`SC_MONITORPOWER` value `-1` is sent as
well, belt and braces) — and the app then only has to restore the brightness it captured.

Two details that are not optional:

- **The keep-awake claim is dropped first.** The app holds
  `ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED` to keep the screen lit while it is armed,
  which fights an explicit standby request. `SetKeepAwake(false)` releases the display claim,
  `WakeDisplays()` re-takes it. The system-awake part is kept.
- **Raw input is registered even when nothing was dimmed.** The footer button can switch off an
  already-lit desktop; `RIDEV_INPUTSINK` is what turns the next mouse movement into a wake, and it
  is unregistered again on wake when `!dimmed`.

**Health reasoning behind offering it** (engineering consensus, *not* measured here; no credible
sensor-level data was available in this environment — `websearch` returned nothing, so the two
sources consulted are VESA DPMS and Wikipedia's LED-backlit LCD article):

- Backlight LEDs age with **junction temperature and on-time**; a mode that stops driving them
  ages nothing. Standby is the only state in which the light source is genuinely off.
- **Dimming is not uniformly gentler.** Panels that dim by PWM keep the LED at full drive current
  and switch it on and off; only DC-dimmed ("flicker-free", most units made since ~2025) panels
  reduce actual LED current. So "0% brightness" can still mean full-current pulses at a low duty
  cycle, which is not the same as "off".
- The cost of standby is **power cycling**, and the stress that carries is inrush current into the
  PSU's bulk capacitors and thermal cycling. Monitors are built to be switched daily for years,
  so a handful of cycles per day is negligible; pathological cycling (every minute) is where the
  wear concentrates — hence a minimum delay rather than switching off immediately.
- Standby also stops the LCD from being driven and drops draw from ~8–20 W to the DPMS "off" band
  (< 8 W in VESA's own table; modern units are far below that).
- One visible side effect, stated rather than discovered later: waking from standby takes about a
  second (DPMS recovery), so it is a **second stage after dimming**, never the first response to
  idleness. Nothing is lost if a monitor never sleeps — `OffMinutes=0` keeps the old behaviour.

**Restore** (`Restore()`): both mechanisms are restored whenever *either* has saved state, then
state is cleared, both recovery files deleted, raw input unregistered, status/tray updated.
Returns `false` if either mechanism reports failure; `Check()` retries on the next tick because
the saved counts are still non-zero. With a laptop panel plus external monitors one dim may
legitimately have touched both, and skipping the empty side costs nothing. Every restore path also
kills `kOffTimer` and wakes the displays, so quitting or locking with the screens asleep cannot
leave them dark.

Brightness is captured **once per dim** (never queried on the scheduler tick) — steady-state
traffic is zero for both mechanisms.

## 6. Idle threshold and grace periods

- `idleMs` = `IdleMinutes × 60000`, read from `settings.ini` at startup (before CLI dispatch so `--diagnose` reports it), clamped to **1–60 minutes** (default 2).
- Tray submenu offers 1/2/5/10/15/30 minutes (`kIdlePresets`); any other ini value shows as "N minutes (custom)". Preset selection writes the ini immediately and re-runs `Check()` when enabled (the grace anchor is *not* reset by a threshold change).
- `graceUntil` (`GetTickCount64` absolute time) prevents dimming until a full idle period has passed after any "system just changed" event. It is set to `now + idleMs` at: enable/startup, unlock, resume from sleep, every media/fullscreen-paused tick, every restore (input return, test end).

## 7. Protections (when *not* to dim)

| Protection | Mechanism | Notes |
|---|---|---|
| Media playing | WinRT `GlobalSystemMediaTransportControlsSessionManager` | Accessed through hand-written read-only COM vtables (prefix-only interface structs — no MIDL, no winmd). `MediaPlaying()` returns `1` playing, `0` none, `-1` unknown; **unknown also blocks dimming** (fail toward not disturbing the user). States 4 (Playing) and 5 (Paused) both block — conservative: an active media session counts as "in use" (the inline comment's "Changing" label for 5 is stale; enum value 5 is Paused). Initialisation is lazy and the async operation is never left outstanding — §7.1. **Panel tile: Media.** |
| Audio rendering ("Sound") | WASAPI (`IAudioSessionManager2`) | `audio::AnyActive()` in `AudioPlayback.h`: any non-system session in `AudioSessionStateActive` on the default render endpoint. Panel tile: **Sound**, and it is a switch of its own because it answers a question SMTC cannot — a player that never publishes a session. `MediaInUse()` asks Media first and Sound second, and one positive answer is enough; unknown still pauses dimming. See §7.2 for the measurement that forced it. |
| Full-screen app | `SHQueryUserNotificationState` (`QUNS_BUSY`, `QUNS_RUNNING_D3D_FULL_SCREEN`, `QUNS_PRESENTATION_MODE`) **or** foreground window rect ≥ monitor rect | Covers games/presentations and borderless-fullscreen windows |
| Session locked | `WTSRegisterSessionNotification` → `WM_WTSSESSION_CHANGE` | Lock restores immediately and drops the keep-awake request; unlock re-arms with a fresh grace |
| Sleep/shutdown | `WM_POWERBROADCAST` (`PBT_APMSUSPEND`/`RESUME`), `WM_QUERYENDSESSION`, `WM_ENDSESSION` | Suspend restores brightness and resets the media objects; resume starts a fresh grace |

### 7.1 Media detection lifetime: lazy init, bounded wait, cancel

**`UNVERIFIED`** — written from measured crash evidence and the code; not yet confirmed across a real suspend/resume on the running build (`AGENTS.md` §7.1).

`MediaPlaying()` is the only code that starts a media request, and it never returns while one is still outstanding. There is **no `InitMedia()` call at startup**.

#### What the crash evidence said

> `media-power-trace.log` (temporary `TMPTRACE` instrumentation, `AGENTS.md` §4) recorded the same sequence twice: `PBT_APMSUSPEND` → hours of sleep → `PBT_APMPOWERSTATUSCHANGE` ×4 → `PBT_APMRESUMESUSPEND` → resume handler resets media → `PBT_APMRESUMEAUTOMATIC` → reset again → ~0.2 s later `unhandled_exception 0xC0000005` in `C:\Windows\System32\Windows.Media.MediaControl.dll`. At **every** power event the trace reads `media=0 operation=1`: a request had been created and never collected.

The old code called `InitMedia()` unconditionally at startup. `InitMedia()` starts an async operation (`ManagerFactory::Request`) that only `MediaPlaying()` ever collects — and `MediaPlaying()` runs only when media protection is on (`Check()`). With media protection off the request was created at launch, never collected, and first released by the resume handler; releasing a never-completed `MediaControl.dll` operation across a power transition is what faulted.

#### The two halves of the fix

- **Lazy init.** No startup media call at all. `MediaPlaying()` calls `InitMedia()` itself, so the process neither costs nor owns anything on the media side until media protection actually asks a question. Verified: launching the normal app with `ProtectMedia=0` appends no `media.init` line to the trace.
- **Bounded wait, then cancel.** While `media` is still null the operation's `IAsyncInfo::Status` is polled for at most ~300 ms (15 × 20 ms, the `--diagnose` retry pattern). *Completed* → the result is used as before. *Canceled/Error* → reset, return `-1`, as before. **Still `Started` after the bound → `IAsyncInfo::Cancel()`, then `mediaOperation.reset()`, return `-1`** — cancel-before-release is the order that is safe. If `Status` itself fails, the same cancel-then-release runs, because a request that can no longer be collected must not be left behind either. The cancel path reports `HRESULT_FROM_WIN32(ERROR_TIMEOUT)` in `mediaError` rather than `S_OK`, which is what happened and which stops `--diagnose` from rebuilding and re-timing-out the request 99 more times.

`-1` still means "unknown", and unknown still blocks dimming (§7). The net effect is that no operation can be outstanding when the machine suspends.

### 7.2 Why there is an audio fallback (the signal SMTC cannot give)

**MEASURED 2026-11-01, this desktop, live build.** The user reported that dimming still happened while a YouTube video was playing. Measured, in this order:

| Step | Measurement |
|---|---|
| Audio really rendering | Vivaldi's `audio.mojom.AudioService` utility process alive (PID 24236), +1.906 s CPU over a 2 s sample |
| SMTC session list | **empty** — `GetSessions()` returned 0, `GetCurrentSession()` was `null`, `--diagnose` printed `Media: not playing` with `Media HRESULT: 0x00000000` (the call *succeeded*) |
| Browser configuration | `%LOCALAPPDATA%\Vivaldi\User Data\Local State` → `browser.enabled_labs_experiments = ["hardware-media-key-handling@2"]` (2 = Disabled) |

The two are the same feature. In Chromium, `MediaSessionService` both handles hardware media keys and publishes the Windows media session, and it is created only when `kHardwareMediaKeyHandling` is enabled. With that flag off the browser renders audio and tells Windows nothing, so the app's protection had no signal to read and dimmed over the video. This is not an app bug: no SMTC consumer can see such a player.

**The fallback.** When SMTC reports nothing — and only then — `MediaInUse()` asks WASAPI instead: `CoCreateInstance(MMDeviceEnumerator)` → `GetDefaultAudioEndpoint(eRender, eConsole)` → `Activate(IAudioSessionManager2)` → `GetSessionEnumerator()`, and counts sessions where `GetState() == AudioSessionStateActive`. A session rendering audio is not opt-in; every player that makes sound owns one. `IsSystemSoundsSession()` and this process's own PID are filtered out, so a notification chime cannot hold the display awake, and a session whose PID query failed still counts (the browser's audio service is the common case). Since 2026-11-01 the two signals are separate panel switches, **Media** and **Sound**, because either can be the only one that works: a browser with media keys off publishes no session, while a player on a non-default endpoint renders audio the walk does not see.

Latitude and cost, stated plainly:

- It **cannot tell YouTube from Discord**, a game or a call. Anything rendering audio suppresses dimming for as long as it renders. That is the accepted trade-off of not depending on a browser flag.
- A **paused** video stops rendering → the session goes `Inactive` → dimming resumes. A paused tab is not "in use".
- It is only consulted when SMTC reports nothing, so a machine whose players publish sessions pays nothing for it.
- It runs at most once per scheduler wake, never on a fixed poll (§4.1).
- Unknown must never dim: `audio::Verdict` returns `-1` when the audio stack cannot be queried at all, and `MediaInUse()` propagates that, so the fail-safe is unchanged.

`--diagnose` reports the two signals separately (`Media session (SMTC):`, `Audio playback (WASAPI):`, `Audio sessions: N active of M`) precisely because the interesting case is one saying "not playing" while the other says "playing".

## 8. Power policy ownership

While enabled: `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)`.

`ES_DISPLAY_REQUIRED` tells Windows the *app* owns display-off duty — the panel stays "on" so that 0%-brightness dimming is what the user sees instead of the OS turning the display off. It is cleared on disable, lock, quit, and sleep. This is why the legacy installer used to set the power plan's display timers to Never; the native app achieves the same per-thread instead (no power-plan mutation at all).

## 9. Tray icon and menu

- `Shell_NotifyIconW` with `NOTIFYICON_VERSION_4`; the app re-adds itself on the `TaskbarCreated` broadcast (explorer restarts).
- **Artwork**: five user SVG designs in `icons\` (20×20 viewBox, hollow white strokes). Designs 1–4 are static (`tray-dimmer-bulbCollarChevron.svg`, `tray-dimmer-horizonSunDoubleChevron.svg`, `tray-dimmer-horizonSunOriginal.svg`, `tray-dimmer-pick4SingleChevron.svg` (default — bulb and single chevron, the same identity as the executable's own icon)), each carrying the three shared states. Design 5 is the **sunset timer**: eight SVGs (`tray-dimmer-sunset-off/-dim/-d0…-d5.svg`) whose sun descends toward a pinned horizon rule as idle time accrues, so the tray shows how close the next dim is without being opened. It deliberately has **no `on` frame** — with design 5 selected, one could never be displayed. `icons\build-icons.ps1` (invoked by `build.ps1` when an SVG is newer than the ICOs) rasterizes each via headless Edge on a magenta background (alpha recovered as green channel — fringe-free), then derives 40 ICOs across 2 theme variants, each with 16/20/24/28/32/36/40/48 px PNG-compressed entries. `verify.png` is the contact sheet; `verify-tray.png` is the true-size tray strip (each row on the taskbar colour its tint actually sits against).
- **Why design 5 is baked, not drawn at runtime**: a runtime tray-icon redraw measures **578 µs**, of which **~490 µs is `CreateDIBSection` alone** (`tools\bench-icon.cpp`, staged with `QueryPerformanceCounter`). Hoisting the DIB out of the redraw path drops it to **87 µs**, but the whole-process allowance at the 0.02% CPU goal is ~12 ms/minute — so even the optimised path adds +0.011% CPU at a 4 s cadence and +0.043% at 1 s. Baked frames cost nothing and add no wakeups. The descent therefore advances only on the existing 30-second `Check()` tick, which keeps §15 intact.
- **Descent plumbing**: `DescentBucket(idle, threshold)` is a pure function — the same reasoning as `ShouldDim`, extracted so `--self-test` can prove it — mapping elapsed idle time onto `d0`…`d5`, clamping at both ends and guarding a zero threshold. `UpdateTray()` reads Windows' own last-input counter (the same source the dim decision uses, so the icon cannot disagree with what `Check()` is about to do) and calls `EnsureDescentIcon()`, which caches the loaded frame and only reloads when the bucket actually changes. `LoadIcons()` drops the cache on a theme change, because a frame is baked for one theme variant.
- **Theme variants** follow Microsoft's app-icon guidance (separate light/dark assets for theme-sensitive areas; learn.microsoft.com/windows/apps/design/iconography/app-icon-design): the active variant is chosen from the documented `HKCU\...\Themes\Personalize\SystemUsesLightTheme` value (1 = light Windows/taskbar → `*-onlight-*.ico` dark glyphs; 0 → `*-ondark-*.ico` light glyphs). Live updates via `WM_SETTINGCHANGE` filtering `ImmersiveColorSet` + `WM_THEMECHANGED`, reloading only when the value actually changed (same guard Windows Terminal ships for its own taskbar icon — the broadcast also fires on lock/UAC).
- **States (UNVERIFIED 2026-10-27 for design 4's separated bulb)**: design 4's enabled and dimmed bulb now has an outer outline, a transparent inset band, and an inner fill. Its `build-icons.ps1` mapping is `on  = 'tray-dimmer-pick4SingleChevron-separated.svg'`; `dim` uses that same SVG with its existing blue tint. Enabled retains the normal `on` tint (white on dark / `#202020` on light), and disabled keeps the original outline in the `off` tint. The new SVG reuses the bulb path for the outline and scaled inner body, leaving actual alpha transparency between them; it preserves the chevron. The previous solid-filled bulb had no transparent separation. For this update Sharp renders the SVG at the pipeline's source resolution, then `Refit-Mask`, `Scale-Mask`, `Set-Tint` and `Write-Ico` generate the existing native sizes. The other tray designs retain their artwork. Design 5 still uses descent frames with `off`/`dim`. Loaded with `LoadImageW(LR_LOADFROMFILE)`; `MakeIcon()` remains the missing-file fallback. The panel bulb now uses the same separated outline and inset fill; see the panel artwork notes below.
- **Sizing**: the process declares per-monitor-v2 DPI awareness at startup (`SetProcessDpiAwarenessContext` via GetProcAddress, `SetProcessDPIAware` fallback), so all metrics are true. `TrayIconSize()` requests exactly `SM_CXSMICON` scaled by the DPI of the monitor that actually hosts the taskbar (`GetDpiForWindow(GetShellWindow())`) = 32 px on this 200 %-scaled machine. Handing the shell a *larger* icon is not ignored, just resampled: 48 px produced a glyph identical to the 32 px frame scaled by 32/48, so the extra pixels only cost sharpness. `Shell_NotifyIconGetRect` — the shell API for the exact cell rect — returns E_FAIL on this Windows build, so positioning falls back to the measured constant.
- **Design picker**: the panel's "Tray icon" tile opens a native popup showing the five designs as icon-only preview bitmaps side by side (`MF_MENUBARBREAK` columns), the active one with a drawn box outline (`COLOR_HIGHLIGHT`). Previews composite the icon's straight-alpha pixels (from `GetIconInfo`+`GetDIBits` — `DrawIconEx` misrenders alpha icons) over `GetSysColor(COLOR_MENU)`, picking the variant that matches that background. Design 5 previews its mid-descent frame `d2`, since it has no `on`. The selection is saved as `Icon=1..5` and applied immediately (`kIconFirst` popup ids 120–124; `kIconDesignCount` in `IdleDimmer.cpp` must match `$designs` in `build-icons.ps1`).
- **Executable application icon**: `tools\set-app-icon.ps1` runs as part of `build.ps1`, embedding `icons\tray4-app-ondark.ico` — design 4 (bulb and single chevron), 8 sizes from 16 to 48 px — into `IdleDimmer.exe` using `kernel32` resource update APIs (`BeginUpdateResourceW`/`UpdateResourceW`/`EndUpdateResourceW`). This provides the native icon in Windows Explorer, Task Manager, and the window class without external resource compiler dependencies. It used to embed design 3 (`tray3-ondark-on.ico`, horizon sun and arrow) until 2026-10-26; the executable and the tray therefore showed different identities.

  `tray4-app-ondark.ico` is generated by `Build-AppIcon` in `icons\build-icons.ps1` from the **plain** design-4 SVG, tinted with each theme's `on` colour (`tray4-app-onlight.ico` is the dark-glyph twin). It is deliberately not the tray's `on` artwork: that state carries an inset fill (outer outline, transparent band, inner body) which **merges into a single white blob at 16–32 px**, and those are the sizes the shell actually draws. MEASURED 2026-10-31 by extracting the embedded 16 px frame from `IdleDimmer.exe`: one solid shape, whereas the plain outline reads as a ring above a chevron at the same size. `tools\build-icons.ps1 -AppOnly` regenerates just this pair; the full run does too. The app icon is fixed — changing the tray design does not change the embedded file icon.
- **One UI**: the classic context menu has been removed. `Menu()`, `kEnabled`, `kMedia`, `kFullscreen`, `kAutostart`, `kFolder`, `kExit` and `kIdleFirst` are gone; every one of those actions is now a panel control. `TogglePanel()` is the single entry point, reached from left-click, right-click (`WM_CONTEXTMENU`), `NIN_SELECT` and `NIN_KEYSELECT`, and it toggles — so there is one answer to "what does clicking the tray icon do". `kDimTest` survives for `--test-brightness` via `StartDimTest()`; the panel's "Dim now" button uses `DimManual()` with a 2-second grace period. The icon picker popup is the one surviving native menu and is kept deliberately: it is a rare one-shot choice, `IconPreview()` already renders it correctly, and a panel-native version would be a second preview implementation free to disagree with the first. First launch shows a balloon (suppressed for autostart starts).

### 9.1 Panel window, focus and click-away dismissal

The panel is a second window in the same process: `WS_POPUP | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST`, class `IdleDimmerPanel`, painted entirely by `PaintPanel(HDC, …)` in `Flyout.cpp`. `WS_EX_TOOLWINDOW` keeps it out of the taskbar and Alt+Tab.

**UNVERIFIED 2026-10-26, pending user audit:** `ShowPanel()` selects the monitor from the tray
click and uses `ReserveTaskbarSpace` to exclude the expanded auto-hidden taskbar bounds from
`SHAppBarMessage(ABM_GETTASKBARPOS)`. `AnchorPanel` then places the panel at the usable area's
bottom-right corner with `kPanelEdgeGap` = 12 logical pixels on both edges, following
[EarTrumpet's positioning source](https://github.com/File-New-Project/EarTrumpet/blob/master/EarTrumpet/UI/Views/FlyoutWindow.xaml.cs).
The previous cursor-centered placement (`cursor.x - width / 2`) made the resting position
depend on which tray cell had been clicked; that mechanism has been removed.

**UNVERIFIED 2026-10-26, pending live motion audit:** the entrance slides upward by
`kPanelEntranceTravel` = 25 logical pixels over `kPanelEntranceMs` = 130 milliseconds using
`PanelEntranceOffset`'s quintic ease-out, based on
[EarTrumpet's animation source](https://github.com/File-New-Project/EarTrumpet/blob/master/EarTrumpet/UI/Helpers/WindowAnimationLibrary.cs).
**UNVERIFIED 2026-10-26, pending user audit of open/close:** `DismissPanel()` reverses the motion
for tray-toggle, click-away and Escape dismissal. `PanelExitOffset` moves down from the current
displayed position by the same logical travel over `kPanelExitMs` = 150 milliseconds with a
quintic ease-in. An interrupted entrance therefore closes without jumping back to its resting
position. The entrance-only implementation previously asserted "Dismissal remains immediate";
that omitted the closing half of the requested interaction and has been replaced.

**UNVERIFIED 2026-10-26:** both directions share `kPanelMotionTimer` at a requested
16-millisecond cadence, with `GetTickCount64` determining elapsed time.
`SPI_GETCLIENTAREAANIMATION` disables both animations when Windows animations are off.
Each step uses `SWP_NOACTIVATE` and updates `panelBounds`. Closing immediately removes the
click-away hook, stops the panel refresh timer, and disables panel controls; the motion timer
stops when the final hide completes. Extra dismissals during the exit are idempotent, and
opening waits for that exit to finish. `HidePanel()` remains immediate for app shutdown,
Explorer restart and "Dim now". Timer failure completes the requested transition immediately.
No animation thread or closed-panel animation timer remains. This is a native approximation,
not a claim to use the shell's private Quick Settings animation; visual fidelity awaits the user.

`MEASURED 2026-10-26, desktop geometry probe, taskbar auto-hide enabled`: monitor and work area
both span `(0,0)-(3240,2160)`, while the expanded bottom taskbar occupies `(0,2064)-(3240,2160)`.
The previous work-area-only clamp allowed the footer to overlap that taskbar. The panel remains
topmost above ordinary windows; this does not promise priority over other topmost or shell windows.

#### The panel never takes focus, and never tries to

`WS_EX_NOACTIVATE` is deliberate and required. `ShowPanel()` **does not call `SetForegroundWindow`**, and `WM_ACTIVATE` is an empty case. Dismissal is therefore *not* the ordinary Windows path, and cannot be:

> **Measured.** A tray app loses the foreground fight to the shell. Forcing activation made the panel open and then hide itself within about a second — `--menu` opened it, `--status` two seconds later reported it hidden, with no input in between.

So focus-based dismissal (`WA_INACTIVE`) is not available here: a window that never activates never receives `WA_INACTIVE`. This is the opposite of what the EarTrumpet source does, and it is why three earlier fixes on this project failed.

#### Click-away: a `WH_MOUSE_LL` low-level mouse hook

`PanelMouseHook` is installed in `ShowPanel()` **only while the panel is visible** and unhooked in `HidePanel()`. On a left/right/middle button-down it evaluates `RectContainsPoint(panelBounds, pt)` — the predicate is in `Flyout.h` and is pinned by 4 self-test assertions, with the right and bottom edges exclusive so a click exactly on the border belongs to whatever is underneath — and posts `kPanelDismiss`. The teardown itself runs on our own message loop, and the click is **not** swallowed.

**The hook callback does no file I/O.** Windows silently removes a low-level hook whose callback exceeds `LowLevelHooksTimeout`; a hook callback that formats a string and opens/appends/closes a file is exactly that latency, and the symptom it produces is "click-away quietly stopped working" with no error anywhere. The callback is a pure predicate plus at most one `PostMessage`.

`Escape` also closes the panel (`WM_KEYDOWN` / `VK_ESCAPE`).

#### The tray gesture contract (measured 2026-10-15, on the live app)

Under `NOTIFYICON_VERSION_4`, `LOWORD(lParam)` is the notification code, `HIWORD(lParam)` is the icon id, and `GET_X_LPARAM(wParam)` / `GET_Y_LPARAM(wParam)` carry the cursor anchor.

| Physical gesture | Codes delivered, in order |
|---|---|
| **Left click** (1×) | `WM_LBUTTONDOWN` (513), `WM_LBUTTONUP` (514), `NIN_SELECT` (1024) — 514 and 1024 arrive ~12 ms apart with an identical anchor |
| **Right click** (1×) | `WM_RBUTTONDOWN` (516), `WM_RBUTTONUP` (517), `WM_CONTEXTMENU` (123) — only 123 counts |
| Welcome balloon | `NIN_BALLOONSHOW` (1026) ~10 s after launch, `NIN_BALLOONTIMEOUT` (1028) |

So **one left click is two toggle-worthy codes**, and the old code classified both as toggles: the panel opened and closed inside a frame. That is the flicker. A right click was always a single toggle, which is why right-clicking "worked" while left-clicking never did.

A 300 ms timestamp de-dup sat here before and **could not have caught it**: the two codes of one click carry different `lParam` values, so each reset the other's window. It was a guess about a race, and it demonstrably failed. It has been removed.

#### `TrayGesture.h` — pure gesture decisions

- `ClassifyTrayMessage(UINT code)` → `trayToggle` for `WM_LBUTTONUP`, `WM_CONTEXTMENU`, `NIN_SELECT`, `NIN_KEYSELECT`; `trayIgnore` for **everything else**, including every balloon and popup lifecycle code (they are measured arriving ~10 s after launch and must never open the panel).
- `IsTrayGesturePair(code, previousCode)` → true only for the `WM_LBUTTONUP` ↔ `NIN_SELECT` pair, in either order.
- `ResolveTrayGesture(code, previousCode, msSincePrevious, openState)` → collapses a paired code arriving within `kTrayPairWindowMs` (**400 ms**) to `trayIgnore`; otherwise the classification wins.
- **UNVERIFIED 2026-10-26:** `TrayPressClosesPanel(code, pressStartedOpen)` keeps the release of a tray press begun while visible as a close request, including if click-away has already completed the hide. `trayPressStartedOpen` is captured on tray button-down or double-click notification and consumed by its accepted release; keyboard activation still toggles normally.

**Why 400 ms.** The half-to-half gap of one click measured **12 ms typical, 47 ms worst case** across a burst of real clicks; two genuinely distinct clicks in that same burst were 410–630 ms apart. 400 ms sits with an order of magnitude of margin on both sides. Only *distinct* codes pair, so two `WM_LBUTTONUP`s are two clicks however fast.

**UNVERIFIED 2026-10-26:** `previousCode` advances only for an accepted tray open/close request,
after processing it; ignored balloon and hover deliveries cannot disturb the pair.
`DismissPanel()` clears earlier pairing state via `ForgetLastToggle()` when a close begins.
An accepted tray close then records its gesture, and the animated final hide uses
`HidePanel(false)` to preserve it: the paired `NIN_SELECT` must not reopen the panel after it
has become hidden. Unrelated force-hides still clear the pairing state.

`WM_LBUTTONDBLCLK` is deliberately **not** a toggle: a double-click is two physical clicks, each of which is one gesture (see §16 for what still goes wrong there).

### 9.2 Verification honesty

This section records a process failure worth repeating.

The click-away bug was "fixed" three times. Every attempt passed the then-345-assertion self-test, and synthetic `SetCursorPos`/`mouse_event` simulations closed the panel correctly every time. **It was still broken for the user**, because synthetic input did not reproduce their actual interaction. Three rounds of green tests and zero progress.

The rules that came out of it now live in `AGENTS.md`: deploy after every change and let the user audit, never synthesise the user's mouse, read the reference implementation before inventing a Windows mechanism, and instrument rather than guess when a bug does not reproduce. Instrumenting is what produced the table above — the gesture contract in §9.1 is measured, not assumed, and the trace was removed once the measurement had been turned into pure, tested functions (`AGENTS.md` §4).

**A green self-test is necessary and not sufficient.** The current **2029** assertions all pass
(MEASURED 2026-10-25, `--self-test` on the live build; this figure is re-measured after every
change), the click-away path is confirmed working by the user on the live app,
and the double-click path is not (§16).

### 9.3 Panel content and layout

> **PARTIALLY VERIFIED.** The raster icon pipeline below was confirmed by the user on a live
> 200% panel on 2026-10-24. The remaining layout and behaviour claims were written with the code;
> unless a separate note says otherwise, treat them as description of intent pending user
> confirmation (`AGENTS.md` §7.1).

The panel is a direct transcription of `docs\design-max-power-win11.html`. `Flyout.h` holds the
design's CSS values as logical pixels, in the design's own order, and derives **every** Y
coordinate from one content stack, so the painter and `HitTest()` cannot disagree about where a
control is. Both take their rects from the same functions (`TileRect`, `SliderTrackRect`,
`PowerCardRect`, `DimNowRect`, `FooterExitRect`) — restating a rect in the painter is how the
divider once ended up painted through the tile captions.

Contents, top to bottom, and the CSS each is measured from:

| Element | Design source | Notes |
|---|---|---|
| Tiles: 6 across 2 rows (3+3; a shorter count is a prefix of the same list), 46 px boxes, 10 px gutters | `.tilegrid` | Dimming / Media / Sound / Full screen / Off at idle / Start at login, with targets `hitTileDimming`, `hitTileMedia`, `hitTileSound`, `hitTileFullscreen`, `hitTileOffMode`, `hitTileAutostart`. One table (`TileTargetForIndex`) feeds both the painter and the hit test, so a tile cannot be drawn as one control and hit-tested as another. The switch from one row to two happens at 5 buttons and is driven by `kRowPitch` in `DividerY`/`PanelHeight`, so nothing else in the stack moves |
| Half-tone rule, full bleed | `.sep` | `.sep` spans the flyout; it is not inset by the content padding |
| WinUI-style slider (UNVERIFIED 2026-10-27): leading clock, track, trailing value | `.slider-row`, `.track-wrap`, `.stime` | thin antialiased track, fixed outer thumb ring and resizing inner accent disc; linear mapping and invisible magnets retained |
| Half-tone rule, full bleed | `.sep` | `.sep` divider before power card |
| Power dashboard card | `.pwrcard` | symmetric 12 px top and bottom margins (`kCardMarginTop`, `kCardMarginBottom`) separating card from dividers |
| Distinct bottom dock bar: app identity + action buttons (UNVERIFIED 2026-10-27) | `.foot` | app name on left with a smaller version hint below, action buttons on right. The ribbon is `#222222` on a dark theme — the value Windows' own Quick Settings ribbon reports (reported 2026-11-01); the light plate is `#F9F9F9` from the design sheet, UNVERIFIED on a light taskbar. When the acrylic base is present it is a **light veil over the material** (white at 18/255 ≈ 7%): measured against the same wallpaper, Windows' ribbon is a few percent *lighter* than the panel body, not darker — painting `#222222` over the material produced a black band that visibly did not match; with no acrylic it stays the exact opaque value |

**Icons are pre-rasterized alpha masks, not integer GDI line art.** `icons\panel\*.svg`
holds the design's `IC.*` artwork at its native 20×20 viewBox. `tools\build-panel-icons.ps1`
uses headless Edge on a magenta background to recover coverage (the same fringe-free trick as
the tray pipeline), writes 80 px white+alpha PNGs, and embeds the alpha channel in the generated
`PanelIconsData.h`. `BlitPanelIcon` area-filters that 4× source to the exact DPI and tints
it with `p.onAccent` or `p.text`, so the dark theme, light theme and active tile all share one
asset.

The old implementation walked integer endpoints with `StrokeLine`/`StrokeCircle`. It reproduced
the SVG coordinates but not its rendering: a 30×30 crop of each tile glyph measured exactly two
colours, i.e. no edge coverage at all. The replacement is measured with the same crop and the
self-test now asserts intermediate edge pixels. Tile glyphs are 20 px (the design's icon size),
the slider clock is 18 px (`.sico`) and footer buttons are 15 px (`.iconbtn svg`). The Dimming
tile uses the design-4 bulb-and-chevron shape from the tray artwork instead of the design's
`IC.sun`; the other tiles keep their `IC.*` glyphs. No runtime SVG
parser, COM imaging component or GDI+ startup is involved. The result was confirmed by the user on
the live 200% panel on 2026-10-24.

**UNVERIFIED 2026-10-27, pending user audit:** the enabled Dimming tile uses `gDimFilled` and
`panel\dimmingFilled.svg`; the disabled tile keeps the outline in `panel\dimming.svg`.
The panel bulb now follows `tray-dimmer-pick4SingleChevron-separated.svg`: its outer outline
and inset fill leave a genuinely transparent band between them, at the panel's existing scale,
stroke and alignment. This replaces the previous solid-filled body. The chevron remains
outlined. `build-panel-icons.ps1` processes the variant through the usual SVG-to-alpha-mask
pipeline and `BlitPanelIcon` applies the existing DPI filtering and foreground tint. `gDimFilled`
still selects this enabled variant; disabled Dimming keeps the existing outline asset.
**Build note 2026-10-27:** Edge's headless rasterizer stalled or exited without a PNG during
this change. Its experimental launch changes were removed. The filled SVG's existing-format
alpha PNG is generated with the already-installed Sharp SVG rasterizer, at the same source
size, then consumed by the normal cached mask/header generation. The app has no new dependency.

**Capsule slider with continuous minutes and one-way magnetic presets:** **UNVERIFIED on a live
panel** (built 2026-10-25). The geometry below is pinned by self-test at 96 and 192 DPI; the feel
still needs the user's audit (`AGENTS.md` §7.1). The slider selects every whole minute from **1 to
60**, including 39. The scale is **linear over 59 equal intervals**, so 1→2 takes the same distance
as 30→31 and 59→60. An earlier logarithmic scale made the first step dozens of pixels and the last
one or two, which the user rejected as asymmetric. `MinutesToTrackPx` converts minutes to a pixel
offset in a single rounding step: 4 or 5 px per minute on the 242 px track at 96 DPI, 8 or 9 px on
484 px at 200%. The pointer inverse is `SliderPermilleAtX` → `MinutesFromPermille`, and the value is
stored in **per-mille** (`kSliderPermilleMax = 1000`) rather than percent: at 0–100 adjacent values
collapsed (38 and 39 both landed on 89%), so “every minute” was impossible even before the drag
logic. **No preset dots are painted.** The `.ticks` row height is retained as spacing only; a
seven-dot row implied seven stops that no longer exist.

**UNVERIFIED 2026-10-27, pending user audit against supplied Quick Settings screenshots:**
the slider drawing follows [Microsoft's WinUI slider template](https://github.com/microsoft/microsoft-ui-xaml/blob/main/controls/dev/CommonStyles/Slider_themeresources.xaml).
`kSliderTrackHeight` is 4 logical pixels; `kSliderThumbDiameter` keeps the outer ring at 20.
The inner accent disc uses `kSliderInnerDiameter` = 12 at rest, `kSliderHoverDiameter` = 14
when hovered, and `kSliderPressedDiameter` = 10 while pressed. The dark ring uses a neutral
fill, and the light ring a white fill. The previous solid accent knob and enlarging accent
halo were removed. `FillSliderCapsule` uses a 4x4 coverage mask per output pixel and box
downsampling for antialiased track caps and thumb discs, replacing binary integer circle
edges. It adds no animation timer; state changes repaint through the existing pointer path.
The footer no longer shows uptime. Preview the actual painter at the user's DPI with
`render-panel.exe <output.png> 192 rest`, `hover`, `pressed`, `light`, or `light-hover` as the
final argument. These are visual fixtures, not verification of the live app.

Presets are **one-way magnets**, not hard stops, and they are invisible. `SliderMagnet` is pure drag
state: a click is a plain continuous selection and never snaps, so values inside a pull-in zone stay
directly clickable. During a drag, entering within `kSliderSnapEnterPermille = 20` (2% of the track,
about 1.2 minutes on the linear scale) while moving **toward** a preset snaps to it; continuing
toward it holds; moving away releases after `SliderReleasePermille(track width)` — about one
physical pixel — measured from the closest point reached, so a wide pull-in zone never becomes a
wide sticky zone. A fast drag that jumps past a preset is not captured. 39 minutes is outside the
30- and 60-minute pull-in zones and remains selectable by dragging through or clicking it.

**Dynamic layout reflow:** The tile grid displays 4 buttons in a single row by default. When 5 or
more buttons are present, `TileRect` dynamically reflows them into 2 balanced rows so no orphan
button is left on an incomplete row.

**Distinct bottom dock bar (UNVERIFIED 2026-10-27, pending user audit):** the app name occupies
the main line on the left, with a smaller version hint beneath it, action buttons on the right
and a subtle surface tint.

**The dim-now control lives in the footer**, beside quit, not at the trailing end of the slider
row. It renders with a distinct crescent moon icon (`dimnow.svg` via the rasterized panel icon
pipeline) and triggers `DimManual()`: the panel hides, the display dims to 0% immediately, and a
2-second grace deadline (`manualDimIgnoreUntil = GetTickCount64() + 2000ULL`) ensures the physical
click-release does not accidentally un-dim. The screen remains dimmed until user input arrives via
the raw-input sink (`WM_INPUT`). The 3-second self-timed test (`StartDimTest`) is reserved for `--test-brightness`.

**A second footer button switches the displays off** (`display.svg` → `hitDisplaysOff` →
`OffDisplays()`, §5.1). The dock now reads, from the right: `[displays off][dim now][exit]`, so Exit
stays at the right edge. The label and version block is measured against the leftmost button
(`statW = DisplaysOffRect().x - padX - 8`), which is why it shrank rather than the dock growing.
`UNVERIFIED 2026-11-01: painted and hit-tested, not yet confirmed on the running panel` — the
geometry is pinned by `--self-test` (its centre must resolve to its own target, so an overlap with
dim-now or exit fails the suite), but only the user can judge how it looks.

**The power dashboard card shows real counters, collected by `sessionstats` (`SessionStats.h`
/ `.cpp`).** The module is deliberately pure — no window, no DC, no thread, and no `GetTickCount`
read inside it. The scheduler and the dim/restore paths own the clock and pass ticks in, which is
what lets the counters be driven from `--self-test` the same way `Flyout.h`'s geometry is. `NoteWake`
is called from `Check()`, `NoteDim`/`NoteRestore` from the success paths of `Dim()`/`Restore()`
(never after their `return`, and never on a failed attempt), and `AccumulateDimmed` from `Check()`
only while dimmed. That last point is the one that matters for the project's primary constraint:
the statistics ride the wake that already happens, and add no timer, no wakeup and no work while
the panel is closed and the screen is lit.

**The card's bar is a real ratio, not a decoration.** It fills with the share of *this process's*
uptime that was spent dimmed: `sessionDimmedMs / sessionMs`, where `sessionMs` is process uptime
and `sessionDimmedMs` is `dimmedMs` minus the lifetime total loaded from
`stats.bin` at startup. That subtraction is required: `dimmedMs` is persisted, so pairing the
lifetime figure with a per-process denominator could paint a bar past 100% after a restart.
`ProgressFillWidth` is pure and clamps to `[0, width]`; `whole == 0` paints nothing, because no
uptime yet is not 0% and must not render as a full bar. **UNVERIFIED on a live panel** (built
2026-10-26); the sheet's `stats` cell is the fixture to compare against.

> ## RESOLVED 2026-10-24 — the panel's tile accent matches Windows
>
> **The channel order was one bug; the palette index was another.** The earlier banner on this
> section declared the accent fixed because the tiles stopped painting the byte-reverse of what
> `--diagnose` reported. That reversal was real and is gone: `BlendPixel` writes `d[0]=s.b` and
> `d[2]=s.r`, and `AccentChecks()` samples a painted tile pixel. But the colour being resolved was
> wrong, so a correct pipeline reproduced the wrong swatch exactly.
>
> The accent is `AccentPalette` **entry 1**, not entry 0. `SystemAccent()` now reads entry 1, and
> `--diagnose` names `AccentPalette[1]` as the source. Entry 0 has two jobs of its own: it is the
> fill fallback when the palette is shorter than seven bytes (and the reported source says so), and
> it is the highlight painted on a hovered tile — see below.
>
> **MEASURED 2026-10-24**, registry read directly on this machine (dark theme):
>
> | Entry | Value | Role |
> |---|---|---|
> | 0 | `#ECDCAA` | lightest shade — the fill a tile shows while the pointer is over it |
> | 1 | `#D4BB8B` | resting fill of an ON Quick Settings tile — matches the user's screenshot |
> | 2 | `#B28249` | next shade down |
> | 3–6 | `#986B3E`, `#7F5330`, `#60361F`, `#3C130B` | progressively darker shades |
>
> **Why the wrong index survived five fixes.** No channel test can see it: entries 0 and 1 differ
> by (+24,+33,+31) per channel, which is a shade, not a swap, and both are plausible colours for a
> tan accent. The value the app pinned was measured with a pointing tool resting on a hovered tile,
> so the tool read back the highlight the app was already painting. Every later "verification"
> compared the app against that number and confirmed itself.
>
> **MEASURED 2026-10-24.** `--diagnose` reports `Accent resolved: #D4BB8B (source: AccentPalette[1])`,
> and `AccentChecks()` asserts the painted tile pixel is `#D4BB8B` and not its byte-reverse
> `#8BBBD4`. The swatch comparison is the user's, from a screenshot of Windows' own Quick Settings
> next to the app's panel; the self-test proves the pipeline from value to pixel, not the swatch.
>
> **NOT MEASURED 2026-10-24: light theme.** The same entry is assumed there, because the accent is a
> system colour rather than a theme colour. The user's screenshot — and therefore the only visual
> verification of either index — is the dark theme.
>
> **Tile hover paints the highlight swatch.** An ON tile rests at entry 1 and highlights to entry 0
> (`SystemAccentHover()`, printed by `--diagnose` as `Accent highlight:`), so a hovered tile shows
> `#ECDCAA` — the colour Windows itself uses — instead of the flat +16 lift that was there before,
> which could only ever approximate it: the two entries differ by (+24,+33,+31). A held tile steps
> one further (+16 on the highlight) so a press is distinguishable from a hover; that extra step is
> a UI choice, not a measured Windows value. If entry 0 is unavailable or equal to the fill,
> `PaintPanel` derives the lift instead, so hover never goes dead. Pinned by `AccentChecks()`
> sampling a hovered tile surface.
>
> **Hover geometry is the tile body; click geometry includes the caption.** `HitTest()` owns the
> tile box *and* the caption band beneath it, so clicking a label toggles the control the box does —
> the same contract as Windows' own flyout. `HitTestHover()` is the same function with the caption
> band excluded, and that is what `WM_MOUSEMOVE` uses, because the highlight is painted on the box
> and must not appear while the pointer is on the text. Both share one implementation so the two
> geometries cannot drift.
>
> **The hover state only reaches the painter through `CurrentPanelState()`**, which copies
> `panelHover` and `panelPressed` into `PanelState`. Until 2026-10-24 it copied neither, so every
> frame was painted with `hitNone` and no tile could highlight — the shading was computed and never
> drawn. The painted-surface test cannot see that class of bug because it supplies `hover` by hand;
> a separate assertion now drives `CurrentPanelState()` and checks the copy.

### Three wrong sources, then the right source at the wrong index

| Source | Why it was wrong |
|---|---|
| `Themes\Personalize\AccentColor` | **Does not exist on Windows 11.** MEASURED: absent here. Could never fire. |
| `GetSysColor(COLOR_HIGHLIGHT)` | A Win16-era metric Windows 11 no longer maintains. Returned `0x000000` on one call and a plausible **orange** on another in the same session — and because the orange is non-zero it **suppressed the safe fallback**, reaching the pixels. |
| `DwmGetColorizationColor` | A real, documented DWM API — and the wrong question. It is the **Aero colourisation** tint for window *borders*, not the tile accent. Chosen because "it is the API Windows calls", which was reasoning from familiarity rather than measurement. |
| `AccentPalette[0]` (the right key, the wrong entry) | The final wrong answer. `HKCU\…\Explorer\Accent` was the correct key, but entry 0 is the lightest shade in the ramp, so the panel painted one step too light. A wrong index is harder to spot than a wrong source, because the neighbouring value is always a plausible colour. |

A value that is *sometimes* wrong is worse than no value: every guard here keys on "non-zero", so
unreliability passes straight through. Two of the three sources were only catchable by comparing the
rendered pixel against Windows' own.

### What is actually established

`AccentPalette` is a 32-byte `REG_BINARY` at `HKCU\…\Explorer\Accent`: eight 4-byte entries, light to
dark, each `R,G,B,A` in memory order. **MEASURED 2026-10-24:** entry 1 is `D4 BB 8B 00` = `#D4BB8B`,
the resting tile fill; entry 0 is `EC DC AA 00` = `#ECDCAA`, a lighter shade. The earlier record in
this section wrongly claimed entry 0 was `#EADFCD` and that the palette's first entry matched Windows' tiles
exactly — that claim was wrong in both halves: no entry of the live palette contains those bytes, and
entry 0 is the hover shade rather than the fill.

The palette is deliberately not read through `GetRValue` — `COLORREF` packs red in the *low* byte, so
the same bytes read as a `COLORREF` give `#EA0000` and drop green and blue. `AccentChecks()` pins the
index, the byte order, short palettes, out-of-range indices and the painted pixel.

`--diagnose` now prints `Accent resolved: #RRGGBB (source: …)`, added specifically so a colour
mismatch is a number to compare rather than a swatch to argue about.

### The process failures, which are the more useful record

* **Five implementations were declared done; all five are wrong.** Each was committed with a message
  describing it as fixed. `--self-test` passed every time. It is green because it tests the pure
  conversion, not the path from registry to pixel.
* **"Deployed" was claimed twice without checking the artefact.** Once the shipped binary genuinely
  lacked the change; once the check itself was wrong — an ASCII scan for `AccentPalette` cannot find
  it, because `RegGetValueW` takes a wide string and the literal is stored UTF-16. Both times the
  claim rested on a successful build log rather than on the built file.
* **Two assertions failed by expecting a colour copied from a neighbouring value** rather than the
  bytes observed. This is how two of the four wrong sources survived a green suite.
* **The measurement itself was wrong, and nobody re-took it.** The colour the suite pinned came from
  a pointing tool resting on a hovered tile, so it read the highlight. Five fixes then aimed at
  matching that number. The user's screenshot of the real panel is what broke the loop — the same
  lesson as `AGENTS.md` §1, in a cheaper form: the person looking at the screen sees the swatch.
* **Every diagnosis was offered as fact before the evidence existed**, and twice had to be retracted:
  an acrylic "one-line fix", a Windhawk theory, and a byte-order claim about a key that does not
  exist.

`AGENTS.md` §1 and §7.1 both anticipate this and both were not followed. The rule that would have
caught most of it: **do not report a colour as fixed until the painted pixel has been read**, and
read it from the window, not from a screenshot.

**Counters persist across restarts, via `stats.bin` (`PersistedStats`, section 10).** `SaveStats()` runs
on every dim and restore; `LoadStats()` runs once at startup and seeds the module through
`sessionstats::Seed()`. `lastDimWallMs` is stored as a **wall-clock** stamp
(`GetSystemTimeAsFileTime`), not a `GetTickCount64` value, because a tick count is meaningless
across a reboot: it resets at boot. The card now reads that one stamp for every "Last dim" value:

```
msSinceDim = WallDeltaMs(WallNowMs(), persistedLastDimWallMs)
```

An earlier build used `GetTickCount64` when the dim happened in this session and the wall clock
otherwise, so the same event was described by two different clocks depending on when the panel was
opened. That split is gone; a tick cannot survive the reboot the row is meant to outlive.

`WallDeltaMs` **saturates at zero** instead of subtracting blind. The wall clock is not monotonic:
an NTP correction, a manual change, or a DST boundary can move it backwards, and subtracting two
`unsigned long long` in that case underflows to near 2^64. That is not "a very old dim", it is an
unknown one, and it would print as something like `17050329600000 min ago`, large enough to look
like a measurement and meaningless in fact. Pinned by `PersistedStatsChecks()`.

**"Wake count" is the number of restores from dimmed, not scheduler wakeups.**
`Restore()` increments `restoreCount` only after a dimmed screen has actually returned to full
brightness, so every recorded dim pairs with a wake. The scheduler's own `wakeCount` is still
recorded and persisted for diagnostics, but the card does not display it. Before 2026-10-26 the row
printed that scheduler counter - 12,774 on this install - a number that had nothing to do with the
screen waking.

**"Last dim" is a plain minute count** (`FormatWhen`), rendered as `3 min` / `1581 min`.
Minutes never roll up into hours or days: that is the requested format, so a dim from yesterday
reads as a large minute count rather than a clock time. The count comes from the wall stamp, not a
process tick, so it survives a reboot; `lastDimKnown` supplies the em dash when an old `stats.bin`
has no stamp at all.

`hasDimHistory` is the never-vs-zero discriminator for "Display hours saved". `lastDimKnown` is a
separate flag because a `stats.bin` written before `lastDimWallMs` existed can know dimmed time
without knowing when the last dim happened. Each row falls back to an em dash on its own rather
than letting one missing field blank the others.

**The bar is a real ratio: session dimmed time over session uptime.** It fills with
`sessionDimmedMs / sessionMs`, where `sessionMs` is this process's uptime
and `sessionDimmedMs` is `dimmedMs` minus the lifetime total loaded from `stats.bin` at
startup. That subtraction is required: `dimmedMs` is persisted, so pairing the lifetime figure with
a per-process denominator could paint a bar past 100% after a restart. `ProgressFillWidth` is pure
and clamps to `[0, width]`; `whole == 0` paints nothing, because no uptime yet is not 0% and must
not render as a full bar.

**The final dimmed segment is counted before the restore.** While dimmed the scheduler wakes every
two minutes (`kCheckDimmedMs`), so an ordinary input restore arrives before the next check. The
first version only accumulated time in `Check()`, which meant a dim that began and ended between
two checks contributed zero: the common case at a one-minute threshold. Measured 2026-10-26, 79
recorded dims had accumulated only 7.8 h of dimmed time, and the bar stayed empty after a dim.
`Restore()` now adds `now - lastStatsTick` on its success path, and `Dim()` sets `lastStatsTick` at
the actual dim, so a dim started from the footer button cannot charge awake time to the dim.

*Why the card was empty for so long, and what it used to claim:* it printed `142`,
`09:14 restored 09:17` and `0.6 h` — values the app previously printed as though they were measured,
which is fabricated data wearing the clothes of a measurement (`AGENTS.md` §7.2). Those numbers were
also **internally inconsistent**, which is how the error was finally caught: 09:14 → 09:17 is
3 minutes, and 3 minutes is 0.05 h, not `0.6 h` (0.6 h is 36 minutes). The 10-minute case is now
pinned by assertion at `0.2 h`.

**CONFIRMED 2026-10-31 on the running panel:** the footer's main line is `Idle Dimmer`.
The smaller second line is `v1.0.0`, using the former uptime hint's placement and subdued style.
The earlier combined name/version line has been replaced. The previous `up <n> min` line
described process uptime and was removed at the user's request. `processStartTick` and
`sessionMs` remain internal inputs to the power
card's ratio; they are no longer displayed in the footer. An earlier baked-in build date was
also removed because it went stale when the source moved. The battery percentage and charging
label were also replaced; the panel no longer queries `GetSystemPowerStatus` or carries battery
fields in `PanelState`.

**The full-screen guard rejects maximised windows, and did not used to.** `LooksFullScreen`
(`ScreenGuard.h`) is the pure decision, called by `Fullscreen()` with values gathered from
Win32. `MEASURED 2026-10-17, this machine, foreground Chrome_WidgetWin_1 style 0x15C70000 rect
-7,-7 - 1626,1086 against a 0,0 - 1620,1080 monitor`: the previous geometry-only test returned
**true** for a merely *maximised* browser. Two things made that possible — Windows inflates a
maximised window past the monitor edges by its invisible resize border, and an auto-hiding taskbar
removes the bottom gap. With the work area equal to the monitor, "covers the monitor" stopped
distinguishing maximised from full-screen, so the guard reported a full-screen app was permanently
present and **the app never dimmed at all**. The display still slept on Windows' own timeout, which
is what made it look as though Windows were overriding the app.

The discriminator is the window style, which the geometry test never consulted: a maximised window
carries `WS_MAXIMIZE` and the `WS_OVERLAPPEDWINDOW` bits, while a full-screen title has had them
stripped. Both are now rejected before the cover test, which is Raymond Chen's documented recipe.
`--self-test` pins the exact measured values so the regression cannot return silently.

`UNVERIFIED 2026-10-17`: a window **cloaked** by the shell (another virtual desktop, a suspended
store app) keeps a full rect while not being on screen, so the cover test can still false-positive
on it. It is not handled — fixing it needs `DWMWA_CLOAKED`, resolved by name from the dynamically
loaded `dwmapi.dll` as every other DWM entry point here. No second virtual desktop was available to
measure against. Also `UNVERIFIED`: whether a borderless title that retains `WS_CAPTION` bits will
be missed, which would dim during an in-game cutscene. Both are listed in
`docs\research\2026-10-17-fullscreen-and-cutscene-findings.md` §7 and §8.

`SHQueryUserNotificationState` is still consulted first and all three of its full-screen values
(`QUNS_BUSY`, `QUNS_RUNNING_D3D_FULL_SCREEN`, `QUNS_PRESENTATION_MODE`) are still honoured; that
branch did **not** cause the bug above. `MEASURED 2026-10-17`: with nothing full-screen on screen
it returns `5` = `QUNS_ACCEPTS_NOTIFICATIONS`, the normal state.

**An in-game cutscene cannot be detected from outside the game.** `NEVER MEASURED, and the research
concludes it is not measurable`: input idle time, foreground window identity, audio session state,
media-session presence and GPU 3D load are all identical between "watching a cutscene" and "walked
away mid-gameplay". GPU load is specifically wrong-signed — busy in both — so "GPU busy ⇒ don't dim"
would suppress dimming exactly when it is wanted. Full evidence, with sources and the three
retracted claims recorded alongside it, is in
`docs\research\2026-10-17-fullscreen-and-cutscene-findings.md`.

**The panel's own background is opaque, and the acrylic backdrop it requests is therefore invisible.**
This is deliberate and load-bearing, not an oversight. `IdleDimmer.cpp` asks DWM for
`DWMWA_SYSTEM_BACKDROP_TYPE = 3` (acrylic) and `check-dwm` reports that this machine honours it,
but `PaintPanel` fills the entire client with an opaque base first. Removing that fill does **not**
reveal the backdrop: it produces a solid black panel, because the end-of-paint alpha repair at
`Flyout.cpp` depends on the invariant that step 1 already made the surface opaque. The window is
also non-layered and presented with `SRCCOPY`, so per-pixel alpha cannot reach the screen at all.
`--status` therefore reports `Backdrop: acrylic`
from the attribute call's return code alone — that means the request was accepted, not that any
blur is visible. `UNVERIFIED 2026-10-17: judged from code, not confirmed against a screenshot`.

**MEASURED 2026-11-01 — the legacy accent API does not rescue it either.** A throwaway probe
(temp dir, not this repo) built a layered panel-style window with per-pixel alpha over the most
detailed 320×240 region of this desktop and called `user32!SetWindowCompositionAttribute`
(`WCA_ACCENT_POLICY`, the exact call Twinkle Tray's 60-line `acrylic` addon makes) once per state.
Detail was measured as mean |Δluma| at 1/8/32/64 px and compared against a control window carrying
the same semi-transparent plate with the policy **off** — the control scales every lag by the
plate's alpha, which is what makes the comparison meaningful:

| State | 64 px structure vs control | Reading |
|---|---|---|
| policy off (control) | 100% | the plate over the real desktop |
| `ACCENT_ENABLE_ACRYLICBLURBEHIND`, no tint | **13%** | flat fill — no backdrop at all |
| `ACCENT_ENABLE_ACRYLICBLURBEHIND`, `#222222` tint | 13% | identical, tint ignored |
| `ACCENT_ENABLE_BLURBEHIND` | 15% | flat fill |
| `ACCENT_ENABLE_HOSTBACKDROP` (state 5) | 100% | inert on this window |
| colour-key (`LWA_COLORKEY`) holes | mean luma **3.8** vs 33.6 desktop | holes reveal black, not the desktop |

So on this build the accent policy *substitutes* a flat surface for the window's backdrop instead of
blurring what is behind it. That is why Twinkle Tray's addon has nothing to copy: its effect is a
property of Chromium's composited swapchain (a window with no redirection bitmap), not of the API
call, and a redirection-surface (GDI) window does not participate.

**MEASURED 2026-11-01 — the supported route gives a flat tint too.** A second probe presented a
plate through a **DirectComposition composition swapchain** (`D3D11CreateDevice` +
`CreateSwapChainForComposition` with `DXGI_ALPHA_MODE_PREMULTIPLIED`) on a
`WS_EX_NOREDIRECTIONBITMAP` window, and switched `DWMWA_SYSTEM_BACKDROP_TYPE`:

| What was presented | Composite luma | Detail at 1/8/32/64 px |
|---|---|---|
| plate at alpha 255 (opaque) | **44.0** = exactly `#2C2C2C` | 0 / 0 / 0 / 0 |
| plate at alpha 160, backdrop NONE | 41.9 (desktop showing through) | 9.27 / 17.73 / 15.24 / 16.19 |
| plate at alpha 160, acrylic, light | 106.0 | 0 / 0 / 0 / 0 |
| plate at alpha 160, acrylic, dark | 58.0 | 0 / 0 / 0 / 0 |
| plate at alpha 160, mica, dark | 39.0 | 0 / 0 / 0 / 0 |

The opaque row is the control: it proves the swapchain composites and that the material sits
*behind* it. The interior is then **perfectly flat** — zero variation at every scale — and the same
luma (81.6 after undoing the plate) at three different desktop positions *and* on a framed
`WS_CAPTION|WS_THICKFRAME` window. A backdrop that sampled or blurred what is behind it could not
be position-independent and variance-free. So the system backdrop on this build is a constant tint,
not a material: the DComp route would reproduce the flat ribbon this app already paints, at the cost
of replacing the entire GDI presentation path. That trade was measured and rejected.

**MEASURED 2026-11-01 — `Windows.UI.Composition` cannot even be started unpackaged here.** The one
API that does have a live backdrop is `Compositor.CreateHostBackdropBrush` (what XAML's
`AcrylicBrush` wraps), reached from Win32 through `ICompositorDesktopInterop::CreateDesktopWindowTarget`.
A probe built that path with interfaces hand-declared from the SDK's own MIDL attributes
(`ICompositor b403ca50-…`, `ICompositor3 c9dd8ef0-…` — which is where `CreateHostBackdropBrush`
lives — `ICompositorDesktopInterop 29e691fa-…`), and:

| Call | Result |
|---|---|
| `RoInitialize(RO_INIT_MULTITHREADED)` | `S_OK` |
| `RoActivateInstance("Windows.UI.ViewManagement.UISettings")` (control) | **`S_OK`** — WinRT works here |
| `RoGetActivationFactory("Windows.UI.Composition.Compositor")` | `S_OK` |
| `ActivateInstance` / `RoActivateInstance` on the Compositor | **`0x80070005` `E_ACCESSDENIED`** |

Context checked: window station `WinSta0`, session 1 (the console session), `DwmIsCompositionEnabled`
= 1, medium integrity, same user. OS reports build **26300.9818 (26H2)**. So the denial is specific
to the composition runtime, not to the environment — it wants package identity (MSIX) or the
Windows App SDK's `Microsoft.UI.Composition`, which is not installed, and either one is a framework
dependency this project forbids. Live material is therefore **unavailable to an unpackaged Win32
app on this machine**, by three independent measurements (accent policy, DWM backdrop, composition
activation).

What is left for "live" is our own capture loop: `WDA_EXCLUDEFROMCAPTURE` lets the panel read what
is behind itself (measured: capture 255 → 71.5 with the window excluded, clean on reset), at
1.56 ms of CPU per frame and one 16.7 ms composition-tick wait per capture — cheap enough at 4–10 fps
while the panel is open, and zero when it is closed.

### 9.4 Live acrylic: capture, blur, tint (`Acrylic.h` / `Acrylic.cpp`)

**What it is — the real recipe, from WinUI's source.** `AcrylicBrush.cpp` and
`AcrylicBrush_themeresources.xaml` in `microsoft/microsoft-ui-xaml` give the whole thing:

```cpp
sc_blurRadius = 30.0f;
// dark  AcrylicBackgroundFillColorBaseBrush: TintColor #202020, TintOpacity 0.50,
//                                             TintLuminosityOpacity 0.96
// dark  AcrylicBackgroundFillColorDefaultBrush: TintColor #2C2C2C, TintOpacity 0.15,
//                                             TintLuminosityOpacity 0.96
// light AcrylicBackgroundFillColorDefaultBrush: TintColor #FCFCFC, TintOpacity 0.00,
//                                             TintLuminosityOpacity 0.85
// effect graph: blur -> Luminosity blend (tint colour @ TintLuminosityOpacity)
//                    -> Color  blend      (tint colour @ TintOpacity)
```

Two things are taken from the two variants, on purpose, because they are separate measurements: the
**luminosity colour** (`#202020` @ 0.96, Base) is what sets the body's brightness to Windows' measured
luma ≈ 41, and the **colour opacity** (0.15, Default - the brush flyouts actually use) is what decides how
much of the wallpaper's hue survives. Measured on a static backdrop, live, at four opacities: luma stays
40-41 while chroma goes 23.6 → 33.2 → 38.0 → **40.4** at 0.50 / 0.30 / 0.20 / 0.15.

**That 0.15 build was rejected by the user in the same session ("no, tragic").** The measurement is sound
(luma is flat across the range, chroma roughly doubles) but it is not evidence of what looks right: the
build they called "really, really close to native" had 0.50. Nothing in this section is verified until the
user says so on a running build; `docs/HANDOFF-acrylic.md` records the whole sequence, the rejections, and
the one measurement still missing.

This app implements that pipeline in software (`Acrylic.cpp`): blur (three box passes, radius 26 at
½ scale ≈ 30 logical px at this DPI), Luminosity blend at 0.96 with `#202020`, Color blend at 0.15,
published as the panel's base layer — **and nothing else**. Three steps that used to be here were
removed on 2026-11-01 because they are not in WinUI's recipe: a saturation ×1.25 and an exclusion
blend against white at 26/255 (both constants exist in `AcrylicBrush.h` but `AcrylicBrush.cpp` never
uses them — an RS3-era recipe applied to a 19H1+ graph), and the 2% noise layer. The noise was
measured rather than guessed: Microsoft's 256×256 asset is opaque and full range (mean 128.6,
sd 52.8), so at 2% it is as much a **+1.9 luma lift** on a ~33-level body as it is texture, and this
pipeline applied it on the half-scale buffer, so it paid the lift and delivered a 2×2 blot instead of
grain.

**Surfaces are Fluent tokens, not plate colours** (`Flyout.cpp`, palette at `MakePalette`). Everything
drawn on the material is a translucent white/black film so it inherits the material's colour —
`ControlFillColorDefault` (white 13/255) for a resting tile, `ControlFillColorSecondary` (21/255) for
hover, `CardBackgroundFillColorDefault` (white 13/255 dark, white 179/255 light) plus
`CardStrokeColorDefault` (black 25/255 dark, 15/255 light) for the dashboard card. The card used to be
**the accent at 8 %**, which stained a third of the panel with the user's accent colour, and the footer
used to carry a white veil (14/255, then 9, then 18 before that) that always read as a lighter band.
The footer is now bare material above its divider, which is what Windows shows.

**Where the colour question stands is written down in `docs/research/2026-11-01-acrylic-colour-case-file.md`**,
not here: the §1 recipe with line numbers, the Fluent surface-token table, the measurements, and the
independent audit (what was accepted and what was rejected with evidence). A simulation of this app's
formula against Windows' formula on the same backdrop agreed to within 1 luma, which is why the body
maths was left alone and the surfaces were changed instead.

**Where the shape comes from.** The public recipe page describes acrylic as blur → exclusion blend →
tint → noise (`learn.microsoft.com/windows/apps/design/style/acrylic`), but the shipped WinUI 3 brush
has no exclusion step (case file §1.3), so the code follows the brush, not the picture. The public
pages do not state a blur radius, so the value was chosen by rendering the same captured region at radius 3 / 6 / 8 / 12
and comparing them: the first version downscaled by 12, which is a ~30 px *box* and flattened every
detail below 30 px — measured 9% of structure retained against 43% for the current setting, and in
use it read as a smear rather than as glass. `IDLE_DIMMER_BLUR=<radius>` overrides the radius for
tuning runs.

**Lifetime is the panel's lifetime.** `ShowPanel()` starts the session before the window is shown
(the exclusion has to exist before the first frame can be grabbed), `HidePanel()` stops it, which
joins the thread, frees the bitmaps and clears the capture exclusion. Measured with the panel
closed: 0.0 ms of CPU over 4 s and 5 threads; with it open: 5 threads → **6**, ~4.2% of one core at
11 fps (the capture itself is 1.56 ms; the repaint dominates). A frame is published only when the
blur actually changed, so a static desktop costs the idle panel rate.

**Concurrency, stated because it is easy to get wrong.** The capture runs *outside* the lock — it
blocks ~16.7 ms on DWM's composition tick and would stall the UI — while the lock covers only the
upscale and the buffer swap. The painter holds the same lock for the duration of a paint
(`LockFrame`/`UnlockFrame`), so the worker can never write the buffer being blitted, and the capture
rect is copied under the lock because the UI thread rewrites it on every animation tick. `Stop()`
frees the surfaces **only if the join succeeded**; on a timeout the surfaces are leaked rather than
freed under a possibly-running thread.

**The panel is excluded from capture while a session runs** (`SetWindowDisplayAffinity`,
`WDA_EXCLUDEFROMCAPTURE`). Without it the worker captures its own previous frame and the image
smears toward black — measured: the panel rendered as a uniform `#000000`. Consequence, stated
plainly: while the panel is open it is also missing from screenshots and screen recordings, and
capture tools that fill undefined regions show **garbage there** (the "red" artefact the user
reported is that hole, not the blur).

Two modes, chosen by `AcrylicLive`:

- **Snapshot (default, `AcrylicLive=0`)** — one capture taken *before* the panel appears, and **no
  refresh afterwards**. A refresh existed for a while, to replace a backdrop that had caught a window
  mid-animation; it repaints only when the capture differs, and when it does the panel visibly changes
  a moment after appearing — reported as the blur "loading". One frame is what a still material can
  promise: no capture exclusion is used at all, so Snipping Tool, Game Bar and recordings work with
  nothing to press, and the picture never changes after the panel is up. The cost is that a window
  caught mid-animation stays in the backdrop until the panel is reopened.
- **Live (`AcrylicLive=1`)** — a worker refreshes at ~11 fps while the panel is open. This needs
  `WDA_EXCLUDEFROMCAPTURE`, which makes Windows report "a protected app is preventing desktop
  capture"; **Ctrl+Alt+S** (or `--acrylic-pause`) freezes the material and drops the exclusion so a
  screenshot works. The panel also watches for a capture tool taking the foreground and freezes
  itself (`IsCaptureTool`).

Both publish the same `HBITMAP` base layer, so the painter is identical. In snapshot mode the
background is a still: something moving behind the panel does not move inside it. `--panel-shot`
renders the panel into `panel-shot.bmp` in either mode.

**Three defects that only a rendered frame could show** (all were invisible in a passing self-test
and in the frame counters):

1. **The worker captured an empty desktop.** A new thread inherits the *process's* window
   station/desktop, not the main thread's, and this app moves its main thread to `WinSta0\Default`
   at startup. Every frame came back uniform black. Fixed by binding the worker with
   `OpenDesktopW(L"Default")` + `SetThreadDesktop`, and closing that handle when the thread exits.
2. **A bitmap cannot be selected into two DCs.** The worker kept each frame selected in its own DC,
   so the painter's `SelectObject` failed, its source DC stayed on the default 1×1 bitmap, and the
   panel painted `#000000` while the frame file next to it was correct. Fixed by selecting surfaces
   only around the GDI calls that draw into them.
3. **A per-capture capture-exclusion left holes.** Applying `WDA_EXCLUDEFROMCAPTURE` only around
   each `BitBlt` made the panel invisible for ~20 ms out of every 90 — and a screenshot taken in
   that window showed the *sharp* desktop where the panel should be, which the user read as a blur
   glitch. The exclusion is now held for the whole session: no holes, at the cost of the panel being
   absent from screenshots (hence `--panel-shot`).

None is detectable from the counters — `frames N` and `painted N` were healthy while the panel was
black. That is why `--panel-shot` exists: it runs the *real* painter into a bitmap and writes
`panel-shot.bmp`, so the panel's appearance is an artifact rather than a claim.

## 10. Persistence and files

All state lives next to the exe (`folder\…`):

| File | Content | Lifecycle |
|---|---|---|
| `settings.ini` | `[Settings] Enabled, ProtectMedia, ProtectSound, ProtectFullscreen, IdleMinutes, Icon, OffMinutes, OffInsteadOfDim, Acrylic` — exactly one section, no BOM, CRLF, fixed key order | Rewritten whole by `SaveSettings()` (temp file + rename) on any toggle/preset/design change; the startup heal adds missing keys (`ProtectSound=1`, `OffMinutes=0`, `OffInsteadOfDim=0`, `Acrylic=1`) to a file written before they existed |
| `brightness-recovery.bin` | `Saved { magic 0x53444932 ('SDI2'), count, panels[4]{ path[1024], brightness } }` | Created before dimming, deleted on restore; validated on load (magic, count 1–4, path must contain `WmiMonitorBrightnessMethods.`, brightness ≤ 100) |
| `ddc-recovery.bin` | `DdcSaved { magic 0x31434444 ('DDC1'), count, monitors[8]{ desc[128], brightness, min, max } }` | Same lifecycle as `brightness-recovery.bin`, for the DDC/CI external-monitor path; validated on load (magic, count 1–8, terminated description, brightness ≤ 100). Written only when at least one monitor answered |
| `stats.bin` | `PersistedStats { magic 0x53545453 ('STTS'), version 1, wakeCount, dimCount, restoreCount, dimmedMs, lastDimWallMs }` | Written on every dim/restore and on every dimmed scheduler check; read once at startup into `sessionstats::Seed()`. Rejected on magic or version mismatch, which silently leaves the counters at zero |
| `activity.log` | Timestamped events: start, dim, restore, errors | Deleted when > 64 KB (fresh log), no key/screen data |
| `diagnostics.txt` | Last report: `--self-test`, `--diagnose`, or `--status` | Overwritten by each report |

### `settings.ini` format, tolerance and healing

The file is parsed by `ParseSettings` (raw text in, settings out) and written by `FormatSettings`
(settings in, canonical text out). Both are pure and pinned by `--self-test`; `WriteSettingsFile`
is the only I/O around them.

**Writing is canonical.** Exactly one `[Settings]` section, no UTF-8 BOM, CRLF line endings, and
the keys in the fixed order `Enabled`, `ProtectMedia`, `ProtectFullscreen`, `IdleMinutes`, `Icon`.
The bytes are written to `settings.ini.tmp` and renamed over the target
(`MoveFileExW` + `MOVEFILE_REPLACE_EXISTING`), so a crash mid-write cannot leave a truncated
settings file.

**Reading is tolerant, and the last `[Settings]` section wins.** A leading UTF-8 BOM is skipped;
keys are only read inside a `[Settings]` section; if the file carries several `[Settings]`
sections the **last** one is used, because that is the section the profile API used to read, so
no value the user chose is reinterpreted. A key that is missing or unparseable keeps its default
(a typo must not silently turn dimming off), and `IdleMinutes`/`Icon` are clamped to 1–60 and
1–5 as before.

**A non-canonical file is healed on startup.** On the normal app path, after the settings are
read, `SettingsTextIsCanonical` compares the file byte-for-byte with what `FormatSettings` would
write for the values it parsed; if they differ, the file is rewritten from those values.
`--self-test` and `--diagnose` never write — they return before this point.

**Status**: the pure parser/formatter and the canonical check are pinned by `--self-test`, and the
read → heal → write round trip was proven on a byte-for-byte copy of the damaged file with a
throwaway harness (169 bytes in, 83 canonical bytes out, no `.tmp` left behind).
**MEASURED 2026-10-16, live launch, this machine**: the startup heal was observed on a real
deploy — `settings.ini` went from 169 bytes (UTF-8 BOM, two `[Settings]` sections, values
`Enabled=1/ProtectMedia=1/…` then `Enabled=0/ProtectMedia=0/…`) to **83 bytes, no BOM, one
section, `Enabled=0 ProtectMedia=0 ProtectFullscreen=1 IdleMinutes=1 Icon=4`** — the last
section's values, i.e. no setting was lost. The file is now byte-identical to `SaveSettings()`
output, so the heal cannot loop.

This heals the concrete damage found on this machine: `settings.ini` had a UTF-8 BOM (written by a PowerShell one-liner, which emits one by default) **and two `[Settings]` sections**. The cause was the Windows profile API: it cannot see a section behind a BOM, so it skipped the real header and appended a clean second one beside it, leaving two sections with different values and an effective state that depended on which API was asked. `GetPrivateProfileIntW`/`WritePrivateProfileStringW` are no longer used anywhere in this app — they can neither read this format nor be made to write it.

**Startup recovery**: if a valid `brightness-recovery.bin` **or** `ddc-recovery.bin` exists at launch (previous instance killed while dimmed), the app restores brightness immediately; if that restore fails it keeps the timer running in a disabled state and retries on the next scheduled wake (§4.1). Either file is enough — `Restore()` reapplies whichever mechanism was saved.

### 10.1 User-visible status strings

These literals reach the user through the tray tooltip and the `--status` report. They are a
contract with the documentation, and `tools\check-docs.ps1` fails the build if any of them
is absent from this file. A status string that advertises behaviour the code no longer has is a
documentation bug in its own right — an earlier one read `Enabled; checking every 30 seconds`
and survived the change to the adaptive scheduler, which made it false.

| String | Meaning |
|---|---|
| `Enabled; waiting for idle` | Armed, not yet dimmed. The app is sleeping until a dim could be due (§4.1) — it is *not* polling. |
| `Dimmed; input restores brightness` | Dimmed; the first input will restore (§4 "raw input, not polling"). |
| `Disabled; normal Windows power settings` | Master switch off; the power plan owns display-off again (§8). |

| `Displays off; input restores brightness` | The displays-off stage ran while dimmed (§5.1); any input wakes them and restores the captured level. |
| `Displays off; input wakes them` | The footer button switched the displays off without dimming; there is no level to restore. |

Error and pause states append their own text at the point of failure — for example
`Paused for media playback` (an SMTC session is Playing/Paused), `Paused for audio playback`
(no session, but WASAPI shows an active stream — §7.2), `Paused for full-screen app`, and
`Media detection unavailable; dimming paused`. Those are set inline rather than from the
`status` buffer and are not part of the gate.

## 11. Autostart

`HKCU\Software\Microsoft\Windows\CurrentVersion\Run` value `Idle Dimmer` = `"…\IdleDimmer.exe" --autostart`. `Autostart()` compares the value for an exact match with the current exe path (so the menu check reflects a moved exe), `ToggleAutostart()` writes/deletes it. `--autostart` only suppresses the welcome balloon. No service, no scheduled task, no admin rights.

## 12. Command line

| Flag | Behavior |
|---|---|
| *(none)* | Normal tray run (with welcome balloon) |
| `--autostart` | Silent run (Run-key style) |
| `--enable` | First instance: force `Enabled=1`; second instance: forward to running one |
| `--quit` | Graceful shutdown of the running instance (restores brightness) |
| `--status` | Make the running instance write `diagnostics.txt` (status/enabled/dimmed/threshold/checks/input-sink state); exit 1 if no instance |
| `--menu` | Open (or close) the Quick Settings panel of the running instance. Flag name kept for compatibility; there is no separate menu any more |
| `--self-test` | Run 2098 decision-function assertions (dim decisions, clamps, wrap, tray descent buckets, tray gesture classification/pairing, click-away rect, `settings.ini` parse/format/heal including `OffMinutes`, flyout layout/hit-test including the displays-off button, linear slider/magnet spacing, panel icon edge coverage, DDC/CI monitor matching and level clamping, WASAPI session classification), write report, exit 0/1 (no COM/tray/mutex/hardware) |
| `--diagnose` | Report media API health (**SMTC sessions and the WASAPI audio fallback separately**, because they fail apart — §7.2), WMI brightness, panel levels, DDC/CI availability and every monitor's current level/range, the dim path that will be used, the displays-off delay, the live acrylic setting, fullscreen detection, autostart, threshold, icon design/theme variant/file presence; exit 1 only if media is unavailable or **both** brightness mechanisms are unavailable (can run alongside a live instance — dispatch happens before the mutex) |
| `--test-brightness` | Full hardware dim→verify minimum→restore cycle per mechanism, report PASS/FAIL with per-monitor before/dimmed/restored levels, then quit |
| `--acrylic-test` | Run the real capture→blur→tint pipeline once over the busiest desktop region, write `acrylic-sample.bmp`, report fine/coarse detail before and after; exit 0 only when fine detail drops by more than half while coarse structure survives (a blur, not a flat fill) |
| `--panel-shot` | Render the panel through the real painter into `panel-shot.bmp` and exit. Works with the panel closed — it starts a capture session for the shot if none is running. The only way to see the panel as an image, because a live session excludes it from ordinary screen capture (§9.4) |
| `--acrylic-pause` | Freeze the material where it is and clear the screenshot exclusion, so the panel can be captured by ordinary tools; the panel keeps its last frame. The same thing is on **Ctrl+Alt+S**. The next panel open starts a fresh session |

## 13. Failure handling matrix

| Failure | Behavior |
|---|---|
| WMI capture fails at dim | Try DDC/CI; abort with status+log only if that fails too |
| No brightness control on any display | `Brightness unavailable` with the WMI HRESULT; no state change |
| Recovery file write fails | Abort dim, unwind |
| Raw-input registration fails | Immediately restore, report |
| Fresh input detected during dim preparation | Undo, no dim |
| `WmiSetBrightness` fails | Unwind to previous state, report |
| `SetMonitorBrightness` fails (one monitor) | That monitor keeps its level; the dim still counts and the failure is logged |
| Restore fails (any moment) | Status "Restore failed; retrying"; `Check()` retries next tick; shutdown path keeps the app alive retrying every 30 s with a balloon instead of exiting dark |
| `SetThreadExecutionState` fails | Auto-disable with status |
| Explorer restarts | Tray icon re-added via `TaskbarCreated` |
| Media API unavailable | Dimming **paused** (fail-safe), status says so |

## 14. Privacy and security posture

- No network calls, no telemetry, no admin requirement; runs as the signed-in user.
- Raw input is consumed as *existence only*; key/mouse payloads are never read or stored (code comment enforces this).
- **Live acrylic reads screen pixels, in memory only.** While the panel is open the worker captures the panel's screen rectangle (not the whole desktop), blurs and tints it, and hands it to the painter. Nothing is written to disk and nothing leaves the process. The only screen pixels that ever reach a file are the two explicit audit commands — `--acrylic-test` (a 360×520 region) and `--panel-shot` (the rendered panel) — and `Acrylic=0` disables the capture entirely. Stated here because an earlier version of this list said the app never touches screen content, which stopped being true the moment the material was implemented.
- Logs contain brightness transitions and error codes only.
- The only registry writes are its own Run key value; the only brightness writes are WMI sets on its own panels and DDC/CI `SetMonitorBrightness` calls to the monitors it restores.

## 15. Measured footprint (this machine, Surface Book 3)

> **These figures predate the adaptive scheduler in §4.1 and are STALE — re-measurement
> outstanding.** They describe the fixed-30 s-timer build. Do not quote them as the current
> cost. See the note below on what has and has not been established.

| Metric | Value (old build) | Status |
|---|---|---|
| Steady-state CPU | 0.0156 s per 90 s ≈ **0.02 %** (original claim) | **Disputed** — see below |
| Private working set | ~4 MB (2.2 MB before first WMI/media use) | superseded — panel adds GDI/DIB state |
| Threads / handles | 6 / ~370 | now 8 threads, ~18.4 MB working set |
| Wakeups | 2 per minute | ~0.5/min at a 2 min threshold — **guaranteed by `ScheduleNext`, not measured** (§4.1) |
| Steady-state WMI traffic | none (only at dim/restore transitions) | unchanged |
| Self-test assertions | 345 | **2029** (MEASURED 2026-10-26; includes entrance/exit geometry and tray-close press/release checks) |

### What the CPU measurements actually established

Three separate attempts to measure this produced three different numbers, which is itself the finding:

1. **0.02 %** — the original figure in `README.md`/§15. The basis is not reproducible from the
   current harness and it predates the panel entirely.
2. **0.29 %** — a 30 s-timer baseline measured against the pre-flyout binary at commit
   `3d614fa`, in a worktree at `%TEMP%\idlemeter-base`.
3. **1.885 %** — a measurement taken *after* §4.1 was applied. This is the opposite of the
   expected result and is **not yet explained**. Candidate causes, none yet eliminated:
   the measurement window included startup and first-time WMI/media initialisation;
   the panel was open (1 s tick) during the sample; or `Fullscreen()`/`MediaPlaying()` is far
   more expensive per call than assumed and is still being hit on the cheap path.

Until that is resolved, the honest statement is: **the adaptive scheduler is implemented and
its logic is reasoned through, but its CPU benefit is not yet demonstrated on this machine.**
`tools\measure-cpu.ps1` must be re-run against an idle machine with the panel closed,
with the threshold set *before* launch (setting it while running measures a dim/restore cycle
rather than steady state — an earlier harness bug).

**No re-measurement has been taken since**, including after the §9.1 tray-gesture and
click-away work. What that work does change structurally (not by measurement): the
`WH_MOUSE_LL` hook is installed only while the panel is visible and unhooked in `HidePanel()`,
and the pairing window holds two scalars — so neither adds any cost to the closed-panel steady
state, which is the state §15 is about. Treat every CPU figure above as **outstanding**, and do
not restate the superseded 0.02 %.

## 16. Known limitations

> **UNVERIFIED 2026-10-26 — candidate tray-close fix awaits user reproduction.** Previously,
> clicking the tray icon while open triggered click-away on press and reopened the panel on
> release. The candidate fix records whether that tray press began with a visible panel,
> treats its release as a close request, and preserves the close gesture across the animated
> final hide. It does not depend on the unavailable `Shell_NotifyIconGetRect` bounds. Audit the
> actual tray sequence: click to open, click the same icon to close, then click again to reopen.

1. **Internal panel only** — WMI `WmiMonitorBrightness` covers the built-in display; external monitors keep their own brightness (by design of the API).
2. **Dim to 0%, not off** — the screen stays logically on (see §8).
3. Media/full-screen detection is best-effort; "unknown" conservatively blocks dimming.
4. Threshold granularity: minutes via menu/ini only (no seconds), clamp 1–60.
5. Icon design files ship in `icons\`; if deleted, the app falls back to procedural 32 px glyphs without light/dark variants.

## 17. Related documents

- **`docs\power-efficiency-references.md`** — the citations and measurement backing behind §15 and
  the near-zero-idle-CPU goal: language/runtime energy, timer coalescing and resolution,
  event-driven vs polling, global-hook cost, DWM materials, and measurement tooling. Every claim is
  tagged `MEASURED` / `STALE` / `NEVER MEASURED` / `INFERRED` per `AGENTS.md` §7.2.
- **`docs\research\`** — the Win32 research that decided the protection design: full-screen
  detection, GPU activity signals, and non-interactive session detection. Each report states what it
  ruled out as well as what it found.

## Appendix A — Keyboard backlight: investigated, not controllable

Probed on this Surface Book 3 (2026-10-14) to determine whether the dimmer could couple to the keyboard backlight:

- Enumerated all 33 attached HID interfaces via SetupAPI + `HidD_GetPreparsedData`/`HidP_GetCaps`/`HidP_GetUsages`: **no** Keyboard Backlight collection (`Usage Page 0x0C, Usage 0x07`), **no** Set-Level output (`0x7B`), **no** LampArray (`0x59:0x01`).
- `root\wmi` (1,591 classes): no keyboard-illumination class. `Enum\HID`: no backlight strings.
- `HKLM\SOFTWARE\Microsoft\Lighting\Backlight` exists (Windows build 26300 ≥ 26200.7922 has the new host-side protocol) but is **empty** — no compatible device attached. The single `HKCU\...\Lighting\Devices` entry belongs to a stale virtual HID device (Logitech G HUB), not the Surface.
- The keyboard/trackpad talk through Microsoft vendor collections (`TARGET_KIP`, pages `ff05`/`ff0b`/`ff07`, incl. undocumented "Surface HID Color") — undocumented, out-of-servicing firmware (Book 3 driver end: 2025-04-01); not written to.

**Conclusion**: backlight levels, the F5/F7 key, and the idle fade timer live entirely in the keyboard base's firmware — Windows has no entry point, and none is needed (the keyboard dims and re-illuminates itself).
