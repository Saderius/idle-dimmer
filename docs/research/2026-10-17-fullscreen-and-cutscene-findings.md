# Full-screen guard and cutscene detection — research findings

**Date:** 2026-10-17
**Subject:** Why the full-screen guard suppresses dimming permanently, and whether "cutscene vs gameplay" is detectable from outside the game.
**Status:** Investigation complete. Guard bug **root-caused and measured** (§2, §4). Cutscene question **answered — no**, on directional grounds (§6). Guard fix **specified but not built** (§7).

---

## 1. Provenance

Every artifact in `docs\research\` was produced by a dispatched agent, not by the orchestrator. The models are named because a finding's weight depends on who made it and whether anyone checked it.

| Artifact | Model | Dispatched | Verified by orchestrator |
|---|---|---|---|
| `fullscreen-detection.md` | `opencode/beam-free` | 2026-10-17 | Partially — see §5.1 |
| `noninteractive-detection.md` | `opencode/beam-free` | 2026-10-17 | No — citations read, not re-fetched |
| `gpu-activity-signals.md` | `opencode/deepseek-v4.1-flash-free` | 2026-10-17 | No — read, not re-fetched; see §6 |

Earlier in the same session, unrelated to this investigation:

| Artifact | Model | Outcome |
|---|---|---|
| Panel design-gap evidence review | `opencode/deepseek-v4.1-flash-free` | Complete; refuted the orchestrator's translucency hypothesis |
| `native\SessionStats.h` / `.cpp` | `opencode/beam-free` | Complete; rejected an arithmetic error in the spec it was given |
| `docs\design-feasibility-2026-10-17.md` | `opencode/space-bunny-free` | Complete |

`deep-research` (the Gemini skill) was **attempted and unusable**: no Python interpreter, no `httpx`, and `GEMINI_API_KEY` unset. Substituted web-research agents. No research result in this directory came from that skill.

**Verification policy applied here:** a claim is only marked CONFIRMED if the orchestrator independently reproduced it or fetched the primary source. Everything else is carried at the confidence its author assigned. No claim was upgraded on the strength of a citation alone.

---

## 2. The bug

**Symptom.** The app never dims. `activity.log` records zero `Dimmed` events across an entire session. The display still turns off after a timeout, which is Windows' own behaviour and has nothing to do with this app.

**MEASURED 2026-10-17, this machine, `--diagnose` while idle with a normal desktop foreground:**
```
Full-screen protection active now: yes
```

**MEASURED 2026-10-17, this machine, `activity.log`, session started 15:58:14:**
zero dim events. Last successful dim of any kind: 15:01:55, before this session.

**Mechanism.** `Fullscreen()` in `native\IdleDimmer.cpp` has two branches. The first, `SHQueryUserNotificationState`, returns **false** (see §3). The second — a foreground-window-rect-covers-monitor test — returns **true** for an ordinary maximised window, because the taskbar auto-hides, so the work area equals the monitor and a maximised window covers it.

---

## 3. Three corrections the orchestrator got wrong

Recorded because all three were asserted with confidence before being checked, and all three were wrong. Two were mine alone; the third was a false premise planted in a research prompt.

### 3.0 A false premise in the GPU research prompt

The dispatch brief for `gpu-activity-signals.md` asked the agent to report "how Windows' own Task Manager decides what to put in its **Games** tab". **Task Manager has no Games tab.** The premise was invented by the orchestrator and shipped to the agent as a fact to confirm.

The agent did not comply — it reported that the premise does not hold and explained what Windows actually uses (the Game Bar / GameDVR presence machinery, which is an *announce* API: its writer exists so a game can declare itself, not so a third party can observe games).

**Lesson:** a leading question in a research brief is an assertion wearing a question mark. Briefs must state the question, never the presumed answer.

### 3.1 Windhawk interference — RETRACTED

The orchestrator proposed that Windhawk shell mods were poisoning `SHQueryUserNotificationState` and that this wedged the guard off. **This is false.**

The probe returned the value `5`. The orchestrator labelled it `QUNS_PRESENTATION_MODE` from a hand-written enum table in the probe script. The SDK header, fetched from Microsoft Learn and independently confirmed, gives:

```
QUNS_NOT_PRESENT             = 1
QUNS_BUSY                    = 2
QUNS_RUNNING_D3D_FULL_SCREEN = 3
QUNS_PRESENTATION_MODE       = 4
QUNS_ACCEPTS_NOTIFICATIONS   = 5   ← the value actually returned
QUNS_QUIET_TIME              = 6
QUNS_APP                     = 7
```

`QUNS_ACCEPTS_NOTIFICATIONS` is documented as *"None of the other states are found, notifications can be freely sent."* That is the normal, benign state. The shell is healthy. The app's own compiled constants were always correct; only the ad-hoc diagnostic was wrong.

**Lesson:** the diagnostic guessed at an enum it should have read. A plausible-looking probe that mislabels its own output is worse than no probe.

### 3.2 "Translucent base is a one-line fix" — RETRACTED

An earlier draft claimed removing the opaque fill in `PaintPanel` would reveal the DWM acrylic backdrop, and rated it high value per effort. A dispatched review refuted this: removing the fill yields a **solid black panel**, because the end-of-paint alpha repair depends on the invariant that the surface was already made opaque. Real translucency needs a different presentation route (`AlphaBlend`/msimg32 — forbidden by project constraint; `WS_EX_LAYERED` — no OS blur; DirectComposition — new dependency). The panel stays opaque. See `IMPLEMENTATION.md` §9.

---

## 4. Measured evidence

All rows MEASURED 2026-10-17 on this machine.

| Observation | Value |
|---|---|
| `SHQueryUserNotificationState` | `5` = `QUNS_ACCEPTS_NOTIFICATIONS` (normal) |
| Foreground window class | `Chrome_WidgetWin_1`, not minimised |
| Foreground window style | `0x15C70000` — `WS_VISIBLE｜WS_MAXIMIZE｜WS_CAPTION｜WS_SYSMENU｜WS_MINIMIZEBOX｜WS_MAXIMIZEBOX` |
| Foreground extended style | `0x00200100` — `WS_EX_WINDOWEDGE｜WS_EX_NOREDIRECTIONBITMAP`; **no** `WS_EX_TOPMOST` |
| Foreground rect | `-7,-7 - 1626,1086` (1633×1093) |
| Monitor rect | `0,0 - 1620,1080` (1620×1080) |
| Existing rect test result | **true** — i.e. "full-screen" |
| Projection mode | `PC screen only` (confirmed by user screenshot) |
| `--status` backdrop | `acrylic`, rounded corners `yes` — accepted by DWM, invisible behind an opaque fill |

The decisive row is the last-but-two: a maximised browser satisfies the app's full-screen test. That is the bug, and it needs no third-party explanation.

**Style discrimination, derived from the measured values:**

| Window | `style & WS_OVERLAPPEDWINDOW` | `WS_MAXIMIZE` |
|---|---|---|
| Measured Chrome (maximised) | `0x00C70000` — present | set |
| Full-screen game, styles stripped | absent | clear |

---

## 5. Findings by report

### 5.1 `fullscreen-detection.md` — partially verified

The orchestrator independently confirmed its single most load-bearing claim: the enum values in §3.1, against the primary source. The rest is carried at the author's confidence.

- The rect-vs-`rcMonitor` test is the de facto standard — PowerToys (`WindowsInteropHelper.IsWindowFullscreen`) and ModernFlyouts both use it, comparing against the **full monitor bounds**, not the work area. CONFIRMED by the author's citation.
- **A maximised window false-positives when the taskbar auto-hides**, because the work area then equals the monitor. This is the app's bug, documented in the report's failure-mode table. CONFIRMED by direct measurement in §4.
- `SHQueryUserNotificationState` is a **notification-propriety** API, not a full-screen detector. Its own doc comment says so. CONFIRMED by the orchestrator's own fetch in §3.1.
- With fullscreen optimizations enabled, full-screen games commonly report `QUNS_BUSY` rather than `QUNS_RUNNING_D3D_FULL_SCREEN`. Author's citation; not independently verified.
- DXGI swapchain fullscreen state is **not cross-process observable**; there is no API to enumerate another process's swap chains. Author marks this CONFIRMED-by-absence — i.e. inferred from the absence of any enumeration API in the DXGI reference, which is weaker than a documented prohibition.
- `DWMWA_CLOAKED` filters windows on other virtual desktops, which keep their rects and are otherwise false positives. Author CONFIRMED.
- Raymond Chen's documented recipe is the rect test **plus** absence of `WS_OVERLAPPEDWINDOW`. Author CONFIRMED.

### 5.2 `noninteractive-detection.md` — read, not re-verified

- **GSMTC is opt-in.** `GlobalSystemMediaTransportControlsSessionManager` enumerates only apps that explicitly integrated `SystemMediaTransportControls`. The OS does not instrument playback. This confirms the app's existing media guard is structurally blind to games. Author HIGH confidence.
- **"A video is playing" is not a queryable OS state.** Media Foundation builds pipelines in the calling process from a caller-supplied URL; DirectShow graphs are visible cross-process only via opt-in Running Object Table registration. Author HIGH confidence.
- **Audio sessions are observable cross-process** — `IAudioSessionManager2::GetSessionEnumerator` → `IAudioSessionControl2::GetProcessId` → `IAudioSessionControl::GetState`. Author HIGH confidence on the API, and HIGH that it does **not** distinguish cutscene from gameplay: a game is Active during both.
- **Input cannot separate the two states.** `GetLastInputInfo` is identical when watching a cutscene and when away.
- **No cutscene concept exists for third parties.** GameBar exposes overlay visibility and input redirection only.
- No commercial product found that distinguishes cutscene from gameplay. Author marks this MEDIUM / partly UNVERIFIED — an absence-of-evidence finding.

---

## 6. The cutscene question — ANSWERED: no, and not for the reason expected

The user's hypothesis was that GPU load or DXGI diagnostics could separate a cutscene from gameplay. **It cannot, and the reason is directional rather than technical.**

### 6.1 The hypothesis fails on the signal's direction

GPU busy-time is **high in both states that matter**:

| State | User wants | GPU 3D engine |
|---|---|---|
| Walked away mid-gameplay | **dim** | busy |
| Watching a cutscene | **do not dim** | busy |

Any rule of the form "GPU busy → don't dim" therefore suppresses dimming **exactly when it is wanted**. GPU load is not merely an imprecise discriminator here; it is a discriminator pointing the wrong way. Author's framing, CONFIRMED by reasoning over documented engine semantics; no published study characterising the two states was found.

The one partial exception identified — watching the `VideoDecode` engine, which pre-rendered cutscenes may use — is a driver-dependent subset heuristic and is UNVERIFIED.

### 6.2 The mechanisms, for the record

| Mechanism | Admin? | Polling? | Attribution | Viable here |
|---|---|---|---|---|
| PDH `GPU Engine` / `GPU Process Memory` | UNVERIFIED | Yes — persistent sampling thread | Yes (`pid_XXXX`) | Cost: a live background sampler |
| `D3DKMTQueryStatistics` (`gdi32.dll`) | UNVERIFIED | Yes | Yes | **Marked "Reserved for system use. Do not use."** |
| ETW `Microsoft-Windows-DxgKrnl` | **Yes** — `StartTrace` denied; needs *Performance Log Users* | No | Yes | **Blocked** |
| `IDXGIAdapter3::QueryVideoMemoryInfo` | No | — | Process-local | Memory only, no utilisation |
| `dxdiag` | — | — | — | A report tool, not a signal |

None of these would be worth their cost even if 6.1 had gone the other way.

### 6.3 Interim position, now corroborated

Every signal examined is identical in the two states that matter:

| Signal | Watching a cutscene | Walked away |
|---|---|---|
| `GetLastInputInfo` idle time | increasing | increasing |
| Foreground window identity | same | same |
| Audio session state | Active | Active |
| GSMTC session | absent | absent |
| GPU 3D load | busy | busy |

The distinguishing information exists only inside the game process.

### 6.4 Independent corroboration

The author's finding is not isolated. PowerToys Awake uses execution-state power requests only, and a request to make it react to CPU/GPU load was **closed as not broadly applicable**. Stretchly, a long-standing break-reminder, spent years without full-screen detection and now proposes `SHQueryUserNotificationState` plus a foreground-window-rect check — converging on §7 from an independent direction.

### 6.5 A factor that lowers the stakes

The app registers raw-input sinks **only while dimmed** (`InputEvents(true)` in `Dim()`), so any mouse movement or keypress restores brightness immediately. A wrong dim therefore costs: the screen is dark while the user is away, and one input fixes it on return. The genuinely bad case is narrower than it first appears — *watching* a cutscene without touching anything for longer than the threshold. At a 1-minute threshold that is likely over a 20-minute cutscene; at 10 minutes it is not. This is a threshold trade-off, not only a detection problem.

---

## 7. Proposed guard predicate — NOT YET BUILT

Recorded so the reasoning survives. **UNVERIFIED 2026-10-17: reasoned from the sources in §5.1 and the measurements in §4; never compiled, never run, and per `AGENTS.md` §7.1 it may not be documented as working until confirmed on a running build.**

```
fullscreen == shellStateIsFullScreen( BUSY | RUNNING_D3D_FULL_SCREEN | PRESENTATION_MODE )
              OR ( foregroundRectCoversMonitor
                   AND !(style & WS_OVERLAPPEDWINDOW)
                   AND !(style & WS_MAXIMIZE)
                   AND !cloaked )
```

Plus the special cases PowerToys applies: `ConsoleWindowClass`, `Progman`/`WorkerW`, `Flip3D`.

**Known risk, stated before implementation:** this can produce a *false negative*. A title running borderless-windowed that retains `WS_CAPTION` bits would not be recognised as full-screen, and the screen would dim during a cutscene. The research marks that specific question UNVERIFIED. The fix restores dimming for ordinary desktop use — the actual defect — but it is **not** a guarantee about cutscenes.

---

## 8. Open questions

1. Do any shipping games register an SMTC session? An Unreal plugin proves it is possible; absence of evidence is not evidence of absence. UNVERIFIED.
2. Does `ABN_FULLSCREENAPP` fire for borderless-windowed titles, or only mode-changing exclusive full-screen? UNVERIFIED.
3. Are ETW `Microsoft-Windows-Win32k` `ForegroundWindowFullScreen` events stable enough to consume? UNVERIFIED, and likely too costly for a ~0-idle-CPU app regardless.
4. Exact window-style combinations used by modern DX12 titles in borderless mode. UNVERIFIED — no authoritative source found. **This one gates the false-negative risk in §7.**
5. Elevation status of the PDH `GPU Engine` and `D3DKMTQueryStatistics` paths, and whether an adapter open/query wakes the GPU. These need a live test, not more reading — and per §6.1 the answer no longer changes the decision, so they are recorded rather than pursued.
6. Would watching the `VideoDecode` engine isolate pre-rendered cutscenes on this machine? Driver-dependent, UNVERIFIED, and per §6.1 it discriminates the wrong way.

**Nothing above blocks the §7 guard fix**, which addresses the measured defect in §2 and is independent of the cutscene question.

---

## 9. Reproducing the measurements in §4

```powershell
# shell state
Add-Type -TypeDefinition '[DllImport("shell32.dll")] public static extern int SHQueryUserNotificationState(out int s);'
$s=0; [void][SomeType]::SHQueryUserNotificationState([ref]$s); $s

# foreground geometry + style, and the app's own rect test
GetWindowLong($fg, -16)          # GWL_STYLE
GetWindowRect($fg, out $r)
GetMonitorInfo(MonitorFromWindow($fg,2), out $mi)
# app's predicate:
$r.L -le $mi.mon.L -and $r.T -le $mi.mon.T -and $r.R -ge $mi.mon.R -and $r.B -ge $mi.mon.B
```

Note the enum trap in §3.1: do not hand-write the `QUNS_*` table in a probe. Read it from the SDK, or print the raw integer and resolve it against Microsoft Learn.
