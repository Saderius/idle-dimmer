# GPU activity and "is a game running" signals for an unprivileged, zero-idle-CPU desktop app

> **Provenance** — produced 2026-10-17 by a dispatched research agent, model `opencode/deepseek-v4.1-flash-free`, via web search. Confidence tags are the *author's*, not independently audited. The author **corrected a false premise supplied in the dispatch prompt** (see `2026-10-17-fullscreen-and-cutscene-findings.md` §3.3) — recorded because that is the report's strongest signal of diligence. Nothing here has been implemented or run.
>
> Consolidated findings and verification status: `2026-10-17-fullscreen-and-cutscene-findings.md`. Read that file first.

*Research note, 2026-10-17. Web research only — no code in this repo was changed and no
mechanism here has been verified on a running build. See `AGENTS.md` §7.1: nothing in this
document may be treated as verified until the user confirms it live.*

**Scope of the question.** A native Win32 tray app dims the screen on idle. It must cost
approximately zero CPU while its panel is closed, must not add wakeups, may not require
administrator rights and may not add third-party dependencies. The user wants the screen to
**dim** when they walk away *during gameplay*, but to **not dim** during a long non-interactive
in-game cutscene. The open question is whether GPU activity can tell those two states apart.

Confidence tags used below: **CONFIRMED** (stated by Microsoft/primary documentation),
**LIKELY** (consistent evidence, not directly documented), **UNVERIFIED** (I could not confirm).

---

## Summary

1. **GPU utilisation is readable by an unprivileged desktop app, in principle, two ways:**
   the PDH "GPU Engine"/"GPU Adapter Memory" counter sets, and the undocumented
   `D3DKMTQueryStatistics` API in `gdi32.dll`. Both require **polling** and both expose engine
   *busy time*, which only becomes a utilisation percentage once you diff two samples.
   **CONFIRMED** for existence and mechanics; the elevation status of each is **UNVERIFIED**
   (see the table).
2. **ETW (`Microsoft-Windows-DxgKrnl`) is not viable here.** Starting a trace session fails
   with `ERROR_ACCESS_DENIED` for a normal user; the documented fix is membership in the
   *Performance Log Users* group or elevation. That is an admin/configuration requirement the
   app is not allowed to impose. **CONFIRMED** (Microsoft Learn + a worked Q&A).
3. **DXGI gives you memory, not utilisation.** `IDXGIAdapter3::QueryVideoMemoryInfo` reports
   budget and *memory* usage. There is no DXGI "GPU busy" query. **CONFIRMED.**
4. **The real discriminator (cutscene vs gameplay) is not GPU load.** Both a rendered cutscene
   and active gameplay keep the 3D engine busy. GPU busy-time is therefore high in *both* the
   "should dim" state (user walked away mid-game) and the "should not dim" state (watching a
   cutscene). GPU load is, at best, a signal for the wrong half of the problem. **CONFIRMED by
   reasoning over documented engine semantics; no published study characterising the two states
   was found (see §4).**
5. **There is a cheap, documented, unprivileged signal for "a full-screen application is
   running": `SHQueryUserNotificationState`.** It returns `QUNS_RUNNING_D3D_FULL_SCREEN` (=3)
   for exclusive-fullscreen Direct3D apps and `QUNS_BUSY` (=2) for other full-screen states.
   Modern games using Fullscreen Optimizations/borderless report `QUNS_BUSY`, not the D3D value,
   so a full-screen app is detectable but "it is a *game*" is not. **CONFIRMED** (Microsoft
   Learn + Microsoft Q&A bug report).
6. **There is no supported, unprivileged, always-available "a game is running" API.** Windows'
   own game detection is the Game Bar/GameDVR presence machinery, which is Xbox/GDK-centric,
   and its presence *writer* API exists to let a game *announce itself*, not to let a third
   party *observe* games. **CONFIRMED** for the mechanism; **UNVERIFIED** for any readable
   enumeration.
7. **`GlobalSystemMediaTransportControls` (SMTC) lists media app sessions, not games.** A
   well-known compatibility list of SMTC-supporting apps contains browsers and media players
   and no games. This is consistent with this app's own earlier finding that games do not
   appear there. **LIKELY.**
8. **Comparable products do not use GPU load.** PowerToys Awake uses execution-state power
   requests (no telemetry at all); a request to make it react to CPU/GPU load was closed as not
   broadly applicable. The break-reminder app Stretchly went years without full-screen
   detection and now proposes `SHQueryUserNotificationState` plus a foreground-window-rectangle
   check. **CONFIRMED.**

**Blunt bottom line:** for *this* constraint set, GPU utilisation is measurable but not useful.
It costs a poll, it may not be usable without extra privileges for per-process attribution, and
it does not distinguish the two states the user cares about — it is high in both. The only
signal worth considering is a cheap "is a full-screen app in the foreground" query polled on
the app's *existing* idle-check cadence (no new thread, no device, no admin), and even that
does not separate a cutscene from gameplay.

---

## Can an unprivileged app read GPU utilisation?

| Mechanism | Admin? | Polling? | Power cost / side effects | Per-process attribution |
|---|---|---|---|---|
| **PDH counters** `GPU Engine`, `GPU Process Memory`, `GPU Adapter Memory` | **UNVERIFIED.** Counter sets exist on WDDM 2.0+ / Win10 FCU+; read by unelevated consumers (Rainmeter). No Microsoft statement found requiring elevation. | **Yes** — PDH counters are rates; PDH runs a sampling thread at your interval (Rainmeter: once per second, one thread per category). | Cheap but **not free**: a live sampling thread. Rainmeter calls it "very much more efficient… than PerfMon", but it is a persistent background sampler. | **Yes.** Instance names carry `pid_XXXX`; the counter is per-process by design. |
| **`D3DKMTQueryStatistics`** (`gdi32.dll`) | **UNVERIFIED.** Exported from `gdi32.dll`; no permission requirement is documented; **"Reserved for system use. Do not use."** Per-process queries take a process handle. | **Yes** — returns monotonic busy/running-time counters; utilisation is `Δ(RunningTime)/Δ(wallclock)`. | A synchronous call into `dxgkrnl`; opens no D3D device (adapters opened via `SetupDi` + `D3DKMTOpenAdapterFromDeviceName`). Whether adapter open/query wakes the GPU is **UNVERIFIED**. | **Yes**, via `D3DKMT_QUERYSTATISTICS_PROCESS_NODE` / `PROCESS_SEGMENT` with `hProcess`. |
| **ETW `Microsoft-Windows-DxgKrnl` / `-DXGI`** | **Yes / config required.** Non-elevated `StartTrace` → access denied; needs admin or *Performance Log Users*. | **No** — real-time push via a consumer thread. | Requires a cross-process ETW session (a limited system resource; 64 by default) plus a consumer thread. | **Yes** — present events carry the process. |
| **`IDXGIAdapter3::QueryVideoMemoryInfo`** | **No** (docs list no privilege requirement) | Only if you want a time series | Memory only; **no utilisation**. Requires a DXGI factory/adapter object. | Process-local only (`CurrentUsage` is *this* process's usage). |
| **`dxdiag`** | No | N/A | A whole diagnostic tool that enumerates devices and writes a text report; not a runtime signal. | No. |

Notes and citations for each row follow.

### PDH `GPU Engine` / `GPU Adapter Memory`

- Counter set names and counters, confirmed from real `Get-Counter` enumerations and the
  Rainmeter `UsageMonitor` documentation:
  - `\GPU Engine(*)\Utilization Percentage`, `\GPU Engine(*)\Running Time`
  - `\GPU Adapter Memory(*)\Dedicated Usage`, `\Shared Usage`, `\Total Committed`
  - `\GPU Process Memory(*)\Dedicated Usage`, `\Shared Usage`, etc.
  Rainmeter gives `Alias=GPU` → `Category: GPU Engine | Counter: Utilization Percentage` and
  `Alias=VRAM` → `Category: GPU Process Memory | Counter: Dedicated Usage`, each annotated
  **"Requires Windows 10 Fall Creators Update or later."**
  ([Rainmeter UsageMonitor plugin](https://docs.rainmeter.net/manual/plugins/usagemonitor/))
- Instances are **not** guessable. Microsoft's own answer on the PDH GPU counters states the
  instance names "are defined by the OS/driver and must be enumerated, not guessed", and that
  they typically look like `pid_1234_luid_0x..._phys_0_eng_5_engtype_3D`.
  ([Microsoft Q&A: how to get the special process GPU usage with the PDH API](https://learn.microsoft.com/en-us/answers/questions/5641645/how-to-get-the-special-process-gpu-usage-with-the))
- These are the same counters Task Manager builds its GPU views from, and they exist only on
  WDDM 2.0+ drivers; the underlying data comes from the kernel GPU scheduler (VidSch) and
  memory manager (VidMm), so it is API-agnostic (DirectX, OpenGL, Vulkan, CUDA).
  ([DirectX blog: GPUs in the task manager](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager))
- Rainmeter's plugin note on performance is the clearest public statement on cost: it spawns
  **one background thread per counter category**, gathering **once per second**, independent of
  the UI refresh. ([same Rainmeter page](https://docs.rainmeter.net/manual/plugins/usagemonitor/))

### `D3DKMTQueryStatistics` (undocumented)

- Microsoft documents the *function* as **"Reserved for system use. Do not use in your
  driver."** and the query-type enum **"Reserved for system use. Do not use."**
  ([D3DKMTQueryStatistics](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtquerystatistics),
  [D3DKMT_QUERYSTATISTICS_TYPE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ne-d3dkmthk-d3dkmt_querystatistics_type))
  It is nonetheless **exported from `gdi32.dll`** (also `api-ms-win-dx-d3dkmt-l1-1-0.dll`), and
  is what third-party monitors use. Process Hacker resolves it at runtime with
  `GetProcAddress(GetModuleHandle(L"gdi32.dll"), "D3DKMTQueryStatistics")`.
  ([Process Hacker `gpumon.c`](https://processhacker.sourceforge.io/doc/gpumon_8c_source.html))
- Supported query types include adapter, segment, node, and *process* variants
  (`D3DKMT_QUERYSTATISTICS_PROCESS_NODE`, `..._PROCESS_SEGMENT`, `..._PROCESS`). Per-process
  node queries take `hProcess` and return `RunningTime` (busy time); utilisation is the delta of
  `RunningTime` over the delta of wall-clock time. Process Hacker computes exactly this.
  ([same `gpumon.c`](https://processhacker.sourceforge.io/doc/gpumon_8c_source.html),
  [enum reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ne-d3dkmthk-d3dkmt_querystatistics_type))
- The struct layout is **version-dependent** — Process Hacker carries separate V1/V2 and
  Win8+ variants for the same fields — which is the concrete cost of relying on a
  "reserved" structure. ([same `gpumon.c`](https://processhacker.sourceforge.io/doc/gpumon_8c_source.html))
- A modern, maintained open-source consumer (Prometheus `windows_exporter` GPU collector) uses
  **both** mechanisms: "Data source: Perflib, D3DKMT (gdi32.dll), Configuration Manager
  (cfgmgr32.dll)", with per-process `windows_gpu_engine_time_seconds_total` derived from
  `GPU Engine` engine-time counters. It notes software adapters like the Microsoft Basic Render
  Driver are not exposed.
  ([windows_exporter GPU collector](https://github.com/prometheus-community/windows_exporter/blob/main/docs/collector.gpu.md))

### `D3DKMTQueryAdapterInfo` — sensors, not utilisation

- `D3DKMTQueryAdapterInfo` is a *supported* API (min Windows Vista, `Gdi32.lib`/`Gdi32.dll`),
  and `KMTQUERYADAPTERINFOTYPE` includes `KMTQAITYPE_ADAPTERPERFDATA`,
  `KMTQAITYPE_ADAPTERPERFDATA_CAPS`, `KMTQAITYPE_NODEPERFDATA` and
  `KMTQAITYPE_TRACKEDWORKLOAD_SUPPORT`.
  ([D3DKMTQueryAdapterInfo](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtqueryadapterinfo),
  [KMTQUERYADAPTERINFOTYPE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ne-d3dkmthk-_kmtqueryadapterinfotype))
- `D3DKMT_ADAPTER_PERFDATACAPS` is **static capability data** (max memory/PCIe bandwidth, max
  fan RPM, temperature max/warning) queried once per GPU; it is **not** live utilisation.
  ([D3DKMT_ADAPTER_PERFDATACAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_adapter_perfdatacaps))
  The live sensors exposed through the `_PERFDATA` types (temperature, fan, power, engine clock)
  require **WDDM 2.4+** and driver support; drivers report 0 for unsupported values
  ([windows_exporter](https://github.com/prometheus-community/windows_exporter/blob/main/docs/collector.gpu.md)).

### ETW graphics provider — blocked by privileges

- Microsoft is explicit on the session-controller requirement: `StartTrace` returns
  `ERROR_ACCESS_DENIED` unless the caller is an administrator, in the **Performance Log Users**
  group, or a LocalSystem/LocalService/NetworkService service.
  ([StartTraceA](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-starttracea))
- A worked case on Microsoft Q&A: a non-elevated agent gets `StartTrace` error 5; even elevated,
  `EnableTraceEx2` failed until the account was added to *Performance Log Users* and the machine
  rebooted, after which `Microsoft-Windows-DxgKrnl` / `Microsoft-Windows-DXGI` events were
  received. ([Microsoft Q&A: ETW graphics providers fail in split privilege model](https://learn.microsoft.com/en-sg/answers/questions/5951326/etw-graphics-providers-fail-in-split-privilege-mod))
- Intel's PresentMon (the reference consumer for GPU-present ETW data) states the same group
  requirement and warns that without administrator privilege it cannot report complete process
  information for processes on other accounts or short-lived processes — they appear as
  `<unknown>`. ([PresentMon README](https://github.com/GameTechDev/PresentMon))
- The StartTrace page also warns that cross-process ETW sessions are **a limited system
  resource** and should be started only when needed and stopped promptly — the opposite of a
  background tray app. ([StartTraceA](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-starttracea))

### DXGI — memory, never utilisation

- `IDXGIAdapter3::QueryVideoMemoryInfo` fills a `DXGI_QUERY_VIDEO_MEMORY_INFO` with the
  **budget and current memory usage** for a segment group; it has nothing about busy/utilisation.
  ([IDXGIAdapter3::QueryVideoMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo))
- The interface itself is described as informing "the process of the current budget and process
  usage" — memory. ([IDXGIAdapter3](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nn-dxgi1_4-idxgiadapter3))
- Consequence: `CurrentUsage` is *this process's* video memory, so it cannot attribute GPU load
  to another process at all, let alone tell you a game is running.

### `dxdiag`

`dxdiag` is a diagnostic GUI/report tool. It can emit a text report (used by support staff to
read the GPU model, driver version and **WDDM version**) but is not a runtime telemetry API; on
some systems the command-line report path is itself unreliable.
([Intel: extracting information from dxdiag](https://www.intel.com/content/www/us/en/support/articles/000022556/graphics.html),
[Microsoft Q&A: dxdiag from command line has no output](https://learn.microsoft.com/en-us/answers/questions/2470635/dxdiag-from-command-line-has-no-output))
Using it as a periodic signal would mean launching a process and enumerating devices — absurd
for a zero-idle-CPU tray app.

---

## Cost analysis for a zero-idle-CPU app

The binding constraint is not accuracy, it is the app's promise to cost ~0 CPU when its panel is
closed. Two things matter: **(a) does the signal force a poll, and (b) does it force a
persistent thread, session or device.**

- **Utilisation percentages are inherently polled.** Every mechanism above reports *cumulative
  busy time* (PDH `Running Time`, `D3DKMTQueryStatistics` `RunningTime`, ETW present events).
  To turn that into "the GPU is busy" you must sample twice and diff. That is a timer, i.e.
  wakeups. This is in direct tension with "must not cause extra machine wakeups".
- **PDH**: the cheapest correct use is a query with a collection interval. Rainmeter's
  implementation is one thread per counter category sampling once a second
  ([Rainmeter](https://docs.rainmeter.net/manual/plugins/usagemonitor/)). One-second sampling is
  cheap in *CPU* but it is a persistent background activity; you would have to start and stop it
  around the idle-check, and PDH query open/close is not free.
- **`D3DKMTQueryStatistics`**: no thread required — it is a synchronous call you can issue from
  whatever timer already exists. The unknown is whether opening an adapter handle and querying
  keeps any GPU/power state alive. That must be **measured** before trusting it; I found no
  documentation either way (**UNVERIFIED**).
- **ETW**: worst case. A cross-process session is a scarce system resource that Microsoft tells
  developers to avoid starting on customer machines, and it needs admin/Performance Log Users.
  It is also the *only* one of these that is push-based rather than polled, which would be nice
  if it were permitted. It is not permitted here.
  ([StartTraceA](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-starttracea))

**Interaction with the app's existing idle check.** The app already has to poll something like
`GetLastInputInfo` to know the user is idle; a *cheap in-process query* (`SHQueryUserNotificationState`)
can ride that same cadence at near-zero marginal cost. A *GPU telemetry* signal cannot: PDH wants
its own sampling thread, and `D3DKMTQueryStatistics` wants an adapter open per sample. So the
marginal cost of a full-screen query is ~one extra function call per idle tick, whereas the
marginal cost of GPU telemetry is structural.

---

## Per-process attribution

- **PDH**: yes, natively. `GPU Engine` instances are keyed by `pid_XXXX` and the counter is
  per-process; the same is true of `GPU Process Memory`. To use a *specific* process you must
  enumerate instances at runtime (names are OS/driver-defined).
  ([Microsoft Q&A](https://learn.microsoft.com/en-us/answers/questions/5641645/how-to-get-the-special-process-gpu-usage-with-the),
  [Rainmeter](https://docs.rainmeter.net/manual/plugins/usagemonitor/),
  [windows_exporter](https://github.com/prometheus-community/windows_exporter/blob/main/docs/collector.gpu.md))
- **`D3DKMTQueryStatistics`**: yes, by passing a process handle to the `PROCESS_NODE` /
  `PROCESS_SEGMENT` query types. Process Hacker obtains that handle from its process list.
  ([Process Hacker `gpumon.c`](https://processhacker.sourceforge.io/doc/gpumon_8c_source.html))
  The catch is obtaining a handle to the game process: a same-user desktop process can usually be
  opened for query, but the exact access rights `D3DKMTQueryStatistics` requires, and whether it
  works unelevated for a protected/anti-cheat process, is **UNVERIFIED**. This is the same class
  of limitation PresentMon documents for ETW: without elevation you lose visibility into other
  accounts' and short-lived processes.
  ([PresentMon README](https://github.com/GameTechDev/PresentMon))
- **What Task Manager does**: it aggregates straight from the kernel — VidSch (scheduler) and
  VidMm (memory manager) — and, for the process list, shows the **busiest engine across all GPUs
  for that process**, plus which GPU/engine that was. This is why it can attribute load to any
  process regardless of graphics API.
  ([DirectX blog: GPUs in the task manager](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager))
  Task Manager gets the data because it is a trusted, privileged system component; a
  third-party app reproduces it only through the per-process PDH/D3DKMT paths above.

---

## Could GPU load separate cutscene from gameplay?

**Short answer: no — and the reasoning is stronger than "it would need measuring".**

- Both states keep the 3D engine busy. A real-time-rendered cutscene renders as hard as gameplay
  (often harder, with post-processing and effects); an interactive gameplay frame and a
  cinematic frame are indistinguishable to a *busy-time* counter. The per-engine data only tells
  you *which* engine is loaded (`3D`, `Copy`, `VideoDecode`, `VideoEncode`, `Compute`), not
  whether input is driving it. ([DirectX blog](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager))
- **It is also the wrong signal direction for the stated goal.** The state where the user wants
  the screen to dim is *gameplay while they have walked away*. In that state the game is still
  rendering at ~full rate, so GPU load is high. The state where they do **not** want dimming is a
  cutscene, where GPU load is also high. GPU load is high in both. Using "GPU busy → don't dim"
  would suppress dimming precisely during the AFK-in-gameplay case the user wants to catch.
- The **one** partial exception is a pre-rendered cutscene played as a video file: that can
  light up the **`VideoDecode`** engine rather than (or in addition to) `3D`. If you could
  reliably latch "VideoDecode engine busy for > N seconds", you might flag exactly those
  cutscenes. But (a) many cutscenes are real-time rendered and use no video-decode engine, so it
  misses them; (b) loading screens, in-engine movies and menu b-roll also decode video; (c) the
  engine that a given format/GPU uses is driver-dependent — "a GPU driver may decide to decode a
  particular media clip using the video decode engine while another clip… might rely on the
  compute engine". ([DirectX blog](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager),
  [NVIDIA forum: "If it displays in-game video, the NVDEC video [engine]…"](https://forums.developer.nvidia.com/t/how-to-evaluate-gpu-utilization-usage-on-windows/245451))
  So it is a heuristic that catches a subset, with false positives, and it is **UNVERIFIED** for
  any specific game.
- **No published characterisation was found.** I could not find any study, vendor paper or
  Microsoft document that characterises GPU utilisation patterns of *cutscenes vs gameplay vs
  video playback* as distinguishable classes. The searches surfaced only generic statements
  (video decode goes to the video engine) and per-title troubleshooting. Treat "GPU load differs
  reliably between cutscene and gameplay" as an unproven hypothesis requiring first-hand
  measurement, which this app's zero-CPU/live-desktop constraints make awkward to run.

**What would actually be needed** is an *interactivity* signal, not a *load* signal: something
that observes whether input is being consumed by the foreground app, or whether the foreground
app has switched to a non-interactive mode. I found no unprivileged API that exposes that.
(An in-process app can observe its own input; a third-party tray app cannot see another
process's input without a low-level hook, and the app's own architecture already rejects
system-wide input automation — see `AGENTS.md` §2.)

---

## Detecting "a game is running"

### Windows' own detection

- **Game Bar / GameDVR presence.** Microsoft documents a *Game Bar Presence Writer* component
  "notified when a game's 'presence' state (i.e. is a game running in the foreground) changes."
  Its COM surface is `Windows.Gaming.GameBar.PresenceServer.Internal.IPresenceWriter` with
  `UpdatePresence(hWnd, event, appId, appIdType)`, and the documented purpose is to let **an app
  announce its own game presence** (and to let a developer *override* the default Xbox Live
  presence behaviour), not to let a third party *enumerate* running games. Registering a custom
  implementation also requires writing
  `HKLM\SOFTWARE\Microsoft\WindowsRuntime\Server\Windows.Gaming.GameBar.Internal.PresenceWriterServer\ExePath` —
  an HKLM write, i.e. admin.
  ([Microsoft Learn: Game Bar Presence Writer](https://learn.microsoft.com/en-us/windows/win32/devnotes/gamebar-presencewriter))
  Conclusion: this is a **push/announce** API, not an observable **"is a game running"** query.
- **There is no "Games" tab in Task Manager.** The documented Task Manager tabs are Processes,
  Performance, App history, Startup, Users, Details and Services. The game-oriented UI in Windows
  lives in *Settings → Gaming* (Game Mode, Game Bar) and the Xbox app, not in Task Manager.
  ([Wikipedia: Task Manager (Windows)](https://en.wikipedia.org/wiki/Task_Manager_(Windows)),
  [Windows 11 Task Manager overview](https://www.teqiq.com/2024/03/21/windows-11-task-manager/))
  So there is no Microsoft-documented "what Task Manager puts in its Games tab" to cite — the
  premise does not hold. The nearest real thing is the Game Bar/GameDVR presence machinery above,
  which is Xbox/GDK metadata plus a shipped known-games list (see below).
- **Known games list.** Windows ships `KnownGameList.bin` (under `Windows\GameDVR` / `BCastDVR`)
  which the Game Bar/GameDVR uses to recognise titles. It is an undocumented data file, not a
  public API; discussions treat editing/removing it as a hack.
  ([TenForums: disabling Game Bar/GameDVR](https://www.tenforums.com/gaming/83628-disabled-gamebar-gamemode-gamedvr-every-way-possible-yet.html))
  Usability from a third party is **UNVERIFIED**; relying on an undocumented binary blob is exactly
  the anti-pattern `AGENTS.md` §3 warns about.

### A documented, cheap, unprivileged full-screen signal

- `SHQueryUserNotificationState` returns a `QUERY_USER_NOTIFICATION_STATE`:
  - `QUNS_RUNNING_D3D_FULL_SCREEN` = 3 — "A full-screen (exclusive mode) Direct3D application
    is running."
  - `QUNS_BUSY` = 2 — "A full-screen application is running or Presentation Settings are
    applied."
  - `QUNS_PRESENTATION_MODE` = 4 — presentation settings active.
  ([QUERY_USER_NOTIFICATION_STATE](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ne-shellapi-query_user_notification_state))
- **Caveat, confirmed by Microsoft's own tracker:** for modern games it often returns
  `QUNS_BUSY`, *not* `QUNS_RUNNING_D3D_FULL_SCREEN`, because Fullscreen Optimizations run games
  in an optimised borderless mode rather than true exclusive fullscreen. A Microsoft Q&A thread
  records a user filing this as a bug and an MVP explaining it as the Fullscreen Optimizations
  effect. ([Microsoft Q&A: SHQueryUserNotificationState returns QUNS_BUSY…](https://learn.microsoft.com/en-us/answers/questions/1086527/shqueryusernotificationstate-returns-quns-busy-ins),
  [DirectX blog: Demystifying Fullscreen Optimizations](https://devblogs.microsoft.com/directx/demystifying-full-screen-optimizations))
- **Practical consequence:** `SHQueryUserNotificationState ∈ {2,3,4}` is a usable "a full-screen
  app is in the foreground" signal, but it is *not* a "this is a game" signal — a full-screen
  video, a slideshow or a full-screen browser all qualify. This is the widely used
  community answer for "is Game Mode / a fullscreen game active".
  ([Super User: how to check if Windows Game Mode is active](https://superuser.com/questions/1829954/how-to-check-if-windows-game-mode-is-currently-active))
- **A complementary borderless check.** Because `QUNS_BUSY` covers both games and full-screen
  video, the practical pattern is to *also* compare the foreground window's rectangle against the
  monitor rectangle — a maximised window respects the work area, a true borderless-fullscreen
  window matches the full monitor. This is exactly what the latest Stretchly proposal does (see
  §"What comparable products do"), and it is what the npm `detect-fullscreen` package does. It is
  a **window-geometry heuristic**, not a game detector.
- **Related but UWP-oriented:** `IAppVisibility` (CLSID `AppVisibility`) exposes
  `GetAppVisibilityOnMonitor` returning `MONITOR_APP_VISIBILITY`
  (`MAV_APP_VISIBLE` / `MAV_NO_APP_VISIBLE`) and an `Advise` sink for change notifications. It is
  documented as telling you "when a display is showing Universal Windows Platform apps", so it is
  a **push-capable full-screen/immersive signal but scoped to UWP**, not a general win32 game
  detector. ([IAppVisibility](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iappvisibility),
  [MONITOR_APP_VISIBILITY](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-monitor_app_visibility))

### Do games publish anything a third party can read?

- **SMTC / `GlobalSystemMediaTransportControls`: games generally do not appear.** SMTC is a
  *media* API: an app opts in by creating a `SystemMediaTransportControls` for its media session.
  A community-maintained list of which popular apps support SMTC (browsers, Spotify, VLC,
  foobar2000, MPC-HC, etc.) contains **no games**; it also notes the built-in Windows Media
  flyout only shows apps that use SMTC.
  ([ModernFlyouts: GSMTC support and popular apps](https://github.com/ModernFlyouts-Community/ModernFlyouts/blob/main/docs/GSMTC-Support-And-Popular-Apps.md))
  This is consistent with the app's own earlier finding that games do not appear there. Games
  simply do not register a media session, so absence in SMTC carries no information — it is true
  for every game and most non-media apps. **LIKELY / consistent with prior in-project evidence,
  not a positive Microsoft statement.**
- **GDK / Xbox Live / achievements:** these exist, but the only documented *game-presence* surface
  is the Game Bar Presence Writer above, which is Xbox/GDK-centric and announce-oriented. No
  supported third-party "list running games" API was found.
  ([Game Bar Presence Writer](https://learn.microsoft.com/en-us/windows/win32/devnotes/gamebar-presencewriter))
- **Process/engine name lists.** There is no authoritative, maintained, redistributable list of
  "game processes". The practical approaches are all heuristics:
  - Process-name matching against a list (e.g. a break-reminder app spawning `tasklist` every few
    seconds and scanning watched names — a design described by the "STRAFE" game-library tracker).
    Reliability is poor: launchers vs game exe, renamed exes, multi-process engines, anti-cheat
    wrappers.
    ([STRAFE game detection description](https://www.linkedin.com/posts/prathameshfuke_systemdesign-electron-react-activity-7467159171260407808-yZnC))
  - Known-games databases (Steam app IDs, PCGamingWiki, Anticheat compatibility lists) are about
    *catalogue* metadata, not "is it running now", and would be a third-party data dependency.
  - Engine detection (Unreal/Unity/id Tech) is about *identity*, not *state*. No source found
    that makes this reliable enough to gate a screen-dimming decision.
  So process-name matching is at best a **user-supplied include list** (which is what Stretchly
  ultimately chose — "pause for these executables"), not an automatic game detector.
  ([Stretchly issue #389](https://github.com/hovancik/stretchly/issues/389))

---

## What comparable products do

| Product | Signal used | Notes |
|---|---|---|
| **PowerToys Awake** | Execution-state power requests only — "spawns background threads that tell Windows that they require a specific state of the machine." No telemetry, no GPU, no idle/full-screen detection. | Microsoft-documented. ([PowerToys Awake](https://learn.microsoft.com/en-us/windows/powertoys/awake)) |
| **PowerToys Awake — GPU proposal** | *None implemented.* A feature request to keep the PC awake based on CPU/GPU/network utilisation ("set at which % … Awake would prevent sleep") was opened Nov 2023 and **closed Jan 2026** by the maintainer as "might be useful for a segment of the audience… I don't think this is broadly applicable". | Strong signal that even Microsoft's own keep-awake tool declined to build utilisation-based logic. ([PowerToys issue #29888](https://github.com/microsoft/PowerToys/issues/29888)) |
| **Stretchly** (break reminder) | For years: none. Original maintainer (2019): "AFAIK there is no way for electron/nodejs apps to check whether some other app is fullscreen" and "I know of no way for me to see if other apps are in fullscreen". The wiki-guide workaround was manual pause, or "show breaks as regular windows". | ([Stretchly issue #355](https://github.com/hovancik/stretchly/issues/355), [Stretchly about/guide](https://hovancik.net/stretchly/about/)) |
| **Stretchly — current proposal (2026)** | OR of two signals: (1) `SHQueryUserNotificationState ∈ {3,4}` via the `windows-focus-assist` dependency; (2) foreground-window rectangle equals the active **monitor** rectangle (catches borderless-windowed games and browser F11 that report `QUNS_BUSY`). Explicitly notes macOS via `AXFullScreen` and Linux X11 via `_NET_WM_STATE_FULLSCREEN`, degrading on Wayland. Still opt-in. | This is the closest published design to what this project needs, and it is **full-screen detection, not GPU or game detection**. Verified live by its author against a borderless-windowed game. ([Stretchly issue #355](https://github.com/hovancik/stretchly/issues/355)) |
| **PresentMon** (game-performance overlay) | ETW GPU present events from `Microsoft-Windows-DxgKrnl`/`-DXGI`. | Requires Performance Log Users (or admin); documents that unelevated it loses other-account/short-lived processes. Not a "game running" detector, a per-frame profiler. ([PresentMon README](https://github.com/GameTechDev/PresentMon)) |
| **Intel PresentMon 2.3.1** | Same ETW approach, re-engineered so it "can now run without Administrator privileges." | Shows the privilege bar *is* moving for ETW consumers, but it still relies on the Performance Log Users path/session resource and a persistent consumer thread. ([VideoCardz](https://videocardz.com/newz/intel-releases-presentmon-2-3-1-with-new-gpu-telemetry-features-admin-rights-no-longer-required)) |

No product was found that uses **GPU utilisation** as its "a game is running / don't dim" signal.
The real products use either a power-request toggle (Awake), a full-screen heuristic (Stretchly,
`IAppVisibility`), or user-configured executable lists — not GPU load.

---

## Bottom line for our constraint set

Stated plainly, against "~0 CPU when closed, no extra wakeups, no admin, no third-party deps":

1. **GPU utilisation is out.**
   - It requires **polling** (busy-time → rate), so it works against the "no extra wakeups"
     constraint. PDH wants a background sampling thread; `D3DKMTQueryStatistics` wants an adapter
     open per sample and is an **undocumented, "reserved for system use" API** whose struct
     layout changes across Windows versions.
   - It does not answer the user's actual question. GPU load is high both when they walk away
     mid-game (want dim) and during a cutscene (don't want dim). It is the wrong signal for the
     wrong half of the problem. The only partial exception (watching the `VideoDecode` engine to
     flag pre-rendered cutscenes) is a subset heuristic, driver-dependent, and **unverified** for
     any title.
2. **ETW is out.** It needs admin or Performance Log Users, and consumes a scarce cross-process
   session resource Microsoft explicitly warns against leaving running on customer machines.
3. **DXGI is out** for this purpose — memory only, and only this process's memory. `dxdiag` is
   out — it is a report tool, not a signal.
4. **The one usable, unprivileged, near-free signal is a full-screen query:**
   `SHQueryUserNotificationState` (returning `QUNS_BUSY` / `QUNS_RUNNING_D3D_FULL_SCREEN` /
   `QUNS_PRESENTATION_MODE`), optionally combined with a foreground-window-rectangle-equals-monitor
   check to catch borderless games. It:
   - needs no admin and no device,
   - can be called on the app's **existing** idle-check tick (one extra in-process call, no new
     thread),
   - tells you "a full-screen app is in the foreground" — **not** "a game", and **not** "a
     cutscene".
   Interpreted as "don't dim while a full-screen app owns the screen", it would suppress dimming
   during cutscenes (good) **and** during AFK gameplay (bad — exactly the case the user wants).
5. **Therefore the honest answer to the open question is: GPU activity cannot separate cutscene
   from gameplay, and neither can any other unprivileged signal I could find.** What the user is
   really asking for is an *interactivity* signal ("is the foreground app currently consuming
   input?"), and no unprivileged, third-party API for that was found. If the goal must be met,
   the realistic options are (a) a **user-controlled** "never dim while this app is in front"
   toggle/list (the Stretchly route), or (b) an explicitly **UNVERIFIED** heuristic built from
   the `VideoDecode`-engine idea, which must be measured on the user's own games before it can be
   trusted — and per `AGENTS.md` §1 and §7.1, must not be documented as a working mechanism until
   the user has confirmed it live.

---

## Sources

Microsoft Learn / primary:

- [D3DKMTQueryStatistics (d3dkmthk.h)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtquerystatistics) — "Reserved for system use"; `gdi32.dll`.
- [D3DKMT_QUERYSTATISTICS_TYPE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ne-d3dkmthk-d3dkmt_querystatistics_type) — adapter/segment/node and process variants; "Reserved for system use."
- [D3DKMTQueryAdapterInfo](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtqueryadapterinfo) — supported, `Gdi32.dll`.
- [KMTQUERYADAPTERINFOTYPE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ne-d3dkmthk-_kmtqueryadapterinfotype) — `KMTQAITYPE_ADAPTERPERFDATA`, `_CAPS`.
- [D3DKMT_ADAPTER_PERFDATACAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_adapter_perfdatacaps) — static caps, not utilisation.
- [StartTraceA (evntrace.h)](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-starttracea) — `ERROR_ACCESS_DENIED`; admins / Performance Log Users; session limits.
- [QUERY_USER_NOTIFICATION_STATE (shellapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ne-shellapi-query_user_notification_state) — `QUNS_BUSY`, `QUNS_RUNNING_D3D_FULL_SCREEN`, `QUNS_PRESENTATION_MODE`.
- [IAppVisibility](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iappvisibility) and [MONITOR_APP_VISIBILITY](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-monitor_app_visibility) — UWP-scoped full-screen signal with change notifications.
- [IDXGIAdapter3::QueryVideoMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo) and [IDXGIAdapter3](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nn-dxgi1_4-idxgiadapter3) — memory, not utilisation.
- [Game Bar Presence Writer (DevNotes)](https://learn.microsoft.com/en-us/windows/win32/devnotes/gamebar-presencewriter) — game presence is an *announce* API; HKLM registration.
- [PowerToys Awake](https://learn.microsoft.com/en-us/windows/powertoys/awake) — power-request model, no telemetry.
- [DirectX Blog: GPUs in the task manager](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager) — VidSch/VidMm, WDDM 2.0, per-engine semantics, per-process busiest engine.
- [DirectX Blog: Demystifying Fullscreen Optimizations](https://devblogs.microsoft.com/directx/demystifying-full-screen-optimizations) — borderless/optimised fullscreen.
- [Microsoft Q&A: how to get the special process GPU usage with the PDH API](https://learn.microsoft.com/en-us/answers/questions/5641645/how-to-get-the-special-process-gpu-usage-with-the) — `GPU Engine` instance naming (`pid_…_luid_…_eng_…_engtype_…`).
- [Microsoft Q&A: ETW graphics providers fail in split privilege model](https://learn.microsoft.com/en-sg/answers/questions/5951326/etw-graphics-providers-fail-in-split-privilege-mod) — StartTrace=5 non-elevated; Performance Log Users fix.
- [Microsoft Q&A: SHQueryUserNotificationState returns QUNS_BUSY…](https://learn.microsoft.com/en-us/answers/questions/1086527/shqueryusernotificationstate-returns-quns-busy-ins) — games report `QUNS_BUSY`.
- [Microsoft Q&A: dxdiag from command line has no output](https://learn.microsoft.com/en-us/answers/questions/2470635/dxdiag-from-command-line-has-no-output).

Third-party / open source:

- [Process Hacker `gpumon.c`](https://processhacker.sourceforge.io/doc/gpumon_8c_source.html) — runtime `GetProcAddress` of `D3DKMTQueryStatistics`; per-process node/segment queries; version-dependent structs.
- [Prometheus `windows_exporter` GPU collector](https://github.com/prometheus-community/windows_exporter/blob/main/docs/collector.gpu.md) — Perflib + D3DKMT + cfgmgr32; per-process `engine_time`; WDDM 2.4 sensor requirement.
- [Rainmeter `UsageMonitor` plugin](https://docs.rainmeter.net/manual/plugins/usagemonitor/) — `GPU Engine`/`GPU Process Memory` counter names; per-category background thread; Win10 FCU requirement.
- [PresentMon README](https://github.com/GameTechDev/PresentMon) — ETW; Performance Log Users; unelevated attribution limits.
- [Stretchly issue #355](https://github.com/hovancik/stretchly/issues/355) and [issue #389](https://github.com/hovancik/stretchly/issues/389) — comparable product; full-screen detection history and current proposal.
- [Stretchly about/guide](https://hovancik.net/stretchly/about/) — fullscreen workaround.
- [PowerToys issue #29888](https://github.com/microsoft/PowerToys/issues/29888) — GPU/CPU-based keep-awake declined.
- [ModernFlyouts: GSMTC support and popular apps](https://github.com/ModernFlyouts-Community/ModernFlyouts/blob/main/docs/GSMTC-Support-And-Popular-Apps.md) — no games in SMTC list.
- [Wikipedia: Task Manager (Windows)](https://en.wikipedia.org/wiki/Task_Manager_(Windows)) — tab list (no Games tab).
- [NVIDIA Developer Forums: how to evaluate GPU utilization on Windows](https://forums.developer.nvidia.com/t/how-to-evaluate-gpu-utilization-usage-on-windows/245451) — in-game video → video decode engine; engine choice is driver-dependent.
- [Super User: how to check if Windows Game Mode is currently active](https://superuser.com/questions/1829954/how-to-check-if-windows-game-mode-is-currently-active) — community use of `SHQueryUserNotificationState`.
- [TenForums: disabling Game Bar / Game Mode / GameDVR](https://www.tenforums.com/gaming/83628-disabled-gamebar-gamemode-gamedvr-every-way-possible-yet.html) — `KnownGameList.bin`.
- [VideoCardz: PresentMon 2.3.1 no longer requires admin](https://videocardz.com/newz/intel-releases-presentmon-2-3-1-with-new-gpu-telemetry-features-admin-rights-no-longer-required) — ETW privilege change.
- [Intel: extracting information from dxdiag](https://www.intel.com/content/www/us/en/support/articles/000022556/graphics.html) — dxdiag is a report tool.

---

## Open questions I could not resolve

1. **Elevation status of the PDH GPU counters and of `D3DKMTQueryStatistics`.** I found no
   Microsoft statement that either requires or does not require elevation. Evidence is indirect:
   Rainmeter (normally unelevated) reads `GPU Engine`; `windows_exporter` runs as a service. A
   direct test — call each from a plain non-elevated token and read the result/errno — is needed
   before relying on either.
2. **Does opening a GPU adapter handle / querying statistics keep the GPU or a power state
   awake?** This is the decisive cost question and I found **no** documentation either way. It
   must be measured (`powercfg /energy` or a power meter) before any D3DKMT-based signal could be
   considered compatible with the zero-cost goal.
3. **Exact access rights needed for `D3DKMTQueryStatistics` per-process queries**, and whether it
   works against anti-cheat-protected game processes. Undocumented; PresentMon documents an
   analogous limitation for ETW. **UNVERIFIED.**
4. **Whether the `VideoDecode` engine is a usable cutscene proxy in practice.** No published
   analysis; engine selection is driver/form-dependent. Would require per-title measurement on
   the user's machine. **UNVERIFIED.**
5. **Behaviour of `SHQueryUserNotificationState` under Windows 11 24H2/25H2 for modern games and
   for full-screen *videos*.** The docs are from the Vista/7 era; the `QUNS_BUSY`-for-games
   finding is from a 2022 user report. The precise mapping today, including whether a
   full-screen video reports `QUNS_BUSY` or `QUNS_PRESENTATION_MODE`, is **UNVERIFIED** and
   should be checked live.
6. **Any authoritative game-process/engine list.** None found that is maintained, documented and
   redistributable. Process-name matching is therefore a configurable include-list, not a
   detector.
7. **Whether `Game Bar` / GameDVR presence can be *observed* (rather than announced) by a third
   party.** The documented API is writer-side only; no reader/enumeration surface was found.
