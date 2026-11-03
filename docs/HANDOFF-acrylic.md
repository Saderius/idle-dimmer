# Handoff — the acrylic background

> **HISTORICAL CHECKPOINT — 2026-11-01, commit `3ec8c43`.** This handoff records the rejected
> software material. The subsequent native-composition revision is described in IMPLEMENTATION.md
> section 9.4; current review status is below. The earlier Compositor-access diagnosis was
> disproved by successful activation with a DispatcherQueue on the interactive desktop. This file
> remains as the record of the failed experiments, not the current implementation instructions.

## Current handoff — 2026-11-03

**Opening-latency refinement — UNVERIFIED visual behavior:** hardware-device
creation was the dominant measured cost before reveal. The final candidate uses
Windows' WARP device for uploading already-rasterized controls/noise, with hardware
creation as a fallback. Windows Composition still owns the acrylic effect. All
rendering resources and the worker are released on hide; no cache or prewarming
worker remains. A device-retention experiment was rejected after per-thread cycle
sampling found extra hidden work in the NVIDIA driver. See IMPLEMENTATION.md
section 9.4 and the
[opening-latency investigation](research/2026-11-03-flyout-opening-latency.md).
MEASURED 2026-11-03, four instrumented openings: request-to-uncloak was approximately
66 ms initially and 33–38 ms on repeat opens, compared with 115–192 ms on the previous
hardware path. These are API milestones, not screen-presentation measurements.
MEASURED 2026-11-03, clean candidate, 120.155 seconds hidden with media protection
settled: accounted CPU increment 0 ms, process cycles 17,578,441, no render worker,
presentations unchanged at 48. This was comparable to the preceding hardware
build's settled 120.173-second sample of 19,183,152 cycles / 0 ms; nonzero cycles
mean this is not literal zero CPU use. The clean build passed 2,368 self-test checks.
Visual acceptance, GPU power behavior, and hardware lifecycle coverage remain pending.

**Preceding cadence refinement — measured candidate, visual review UNVERIFIED:**
after the user noted that the flyout looked lower-frame-rate than Windows, a fresh
in-memory timing comparison found substantial per-move control repaint work. The
current candidate uses `DCompositionWaitForCompositorClock` to drive WAM interpolation
only during a transition. The UI still moves the whole HWND; `FrameApplied` permits
one outstanding move, and retained `controlsTexture` pixels are copied into each
flip buffer for presentation. Completion refreshes the controls once and does not
reposition the window again. There is no compositor-clock subscription at rest,
and hiding joins the worker and releases the entire composition scene/device.

MEASURED 2026-11-02, three-cycle comparison: baseline mean applied-position intervals
were approximately 22–31 milliseconds; candidate means were approximately
16.5–16.8 milliseconds, with no control repaints during motion. These are application
timings, not measured displayed FPS. Temporary counters were removed, then the clean
candidate was rebuilt, passed all 2,368 self-test checks, and was launched for user
review. Clean artifact: 288,256 bytes; SHA-256
`9D6E1EC60A30EE52B9356DC66E58C69DFA35AE76F3E511C5FD0BE852662F9EF7`.
No visual confirmation of this new cadence revision has been received.

The assistant initially stopped after explaining the diagnosis. The user challenged
that lack of execution, after which the agent measured, implemented, compared,
removed instrumentation, rebuilt, and deployed the candidate. No additional Gemini
or other agent was used for this phase. The complete chronology, comparison protocol,
files changed, and outstanding review are in the
[animation refinement report](research/2026-11-02-animation-cadence-fix.md).

**USER-CONFIRMED display-off behavior, 2026-11-02:** after returning and testing the running
candidate, the user reported "behavior generally seems correct now" and requested a commit.
The trace records two manual off/wake sessions with one request per transition. This is a
general desktop confirmation; Surface and suspend/resume behavior remain unverified. The
manual path uses a five-second pre-off settle, one OS request on the app's own window, filtered/
timestamped input, and tagged deferred work. It bypasses brightness traffic and pauses ordinary
idle/media checks while active. Temporary display-power instrumentation was removed before
commit without changing those decisions. Historical no-run phases and the incident analysis
are preserved in `docs/research/2026-11-02-display-off-loop.md`.

**UNVERIFIED glyph-weight adjustment, 2026-11-02:** reduced the slider clock and six tile glyphs' SVG stroke
setting from `1.5` to `1.0`, including both Dimming variants, after screenshot coverage
measurements showed heavier app strokes. Size, position, antialiasing, and runtime lifecycle
are preserved; the footer moon, exit, and tray artwork were unchanged, while the footer monitor
shares the updated Display mask. The clock was added in a follow-up after
the user pointed out that the tile-only adjustment left it heavier. Awaiting live user comparison;
see `docs/research/2026-11-02-glyph-stroke-weight.md` for the measured baseline.

**Version update, 2026-11-02:** the app footer now displays `v1.0.1`; README and the documentation
gate use the same version. This is a label-only change with no animation or lifecycle changes.

**Cleanup, 2026-11-02:** removed the unused `Mmss` formatter and `CaptureRegionToBmp` declaration/
implementation, plus the temporary presentation logger, its calls, audit deadline, and debug-only
factory query. The reviewed animation's position, repaint, and completion behavior is preserved;
this is not a new smoothness implementation. The software fallback, CLI audit commands, COM ABI
slots, and all self-test assertions are retained. Stale tray-submenu documentation is corrected.
Existing trace evidence is preserved unchanged, with a snapshot under ignored
`build-tools/cleanup-baseline-2026-11-02/`; the current build does not append presentation traces.
MEASURED 2026-11-02, cleanup build: 284,160 bytes; build gates PASS; self-test PASS with 2,128
checks; all application source files compile with unused-function/unused-variable warnings as
errors. Global keyboard registration remains absent. Earlier binary and resource figures below
describe their named historical revisions, not the cleanup build.

**Global shortcut removal, 2026-11-02:** removed the old Ctrl+Alt+S registration and keyboard
handler after the user identified the unexpected hotkey. It was present in checkpoint `3ec8c43`
as a screenshot workaround for the software renderer and was still reserved globally at startup.
Native acrylic does not need it. No global keyboard shortcuts are now registered; the
software-only `--acrylic-pause` CLI command remains. Keyboard shortcuts mentioned in historical
sections below describe the removed implementation.
MEASURED 2026-11-02, shortcut-removal build: 288,768 bytes; build gates PASS; self-test PASS with
2,128 checks; registration API references absent from source and executable. The app was restarted
to release the prior process's shortcut registration. No animation code changed in this removal.

**Live review outcome — USER-REVIEWED 2026-11-02:** after the reviewed implementation was
deployed, the user responded "okay, generally great!" and asked for documentation to be updated.
Treat this revision as the positively reviewed baseline. The feedback covers the overall result;
it does not prove exact shell parity or individually verify every former artifact. No additional
animation changes were made for this documentation update.

**Earlier accepted baseline, before the cadence refinement:** following user instruction and critique reconciliation, implementation of
the flyout entrance fixes completed. Three key structural defects identified in the trace and
frame comparisons have been addressed in code. Gemini 3.8 Flash High produced the initial patch
through the actual `agy` CLI; parent review corrected preparation-guard lifetime, removed the
remaining completion-position write, and added direction validation and immediate content updates:

1. **Re-entrant preparation and teardown:** protected the open cycle with a `panelOpening` lifecycle
   guard in `IdleDimmer.cpp` and moved tray gesture time/code recording before `TogglePanel` invocation,
   dropping duplicate `WM_LBUTTONUP`/`NIN_SELECT` cascades that previously aborted and restarted active
   composition.
2. **Taskbar occlusion:** added `TaskbarForMonitor` to locate the monitor's taskbar HWND (`Shell_TrayWnd`
   or `Shell_SecondaryTrayWnd`) and inserted the flyout immediately beneath it via `hWndInsertAfter = taskbarWnd`,
   allowing the panel to emerge from behind the taskbar during its upward entrance.
3. **Single UI-thread positioning authority:** removed cross-thread `SWP_ASYNCWINDOWPOS` calls from the WAM
   worker thread in `NativeBackdrop.cpp`; position updates are now posted via `kAnimationUpdateMessage`
   with session/direction tokens. Obsolete updates are discarded; the final update uses the same
   handler as other positions, and completion performs no separate snap. Every applied update
   was coupled to a fresh control repaint rather than waiting for the visible-only seconds tick.
   That repaint coupling is the costly step replaced by cached presentation in the current
   cadence candidate described above.

MEASURED 2026-11-02, reviewed revision: build/icon/documentation gates PASS; self-test PASS with
2,128 checks; executable 289,280 bytes. Those gates establish build health, not visual fidelity.
That instrumented revision included session-tagged requested/applied positions; the later cleanup
above removed the logger without altering that baseline's native update path. The later cadence
candidate is a separate change, with its own measurement and pending visual review.

**Visual status of the earlier baseline:** overall improvement accepted in the live review above. Exact motion matching,
all monitor/auto-hide taskbar configurations, and separate frame-by-frame confirmation of each
former artifact remain **UNVERIFIED**. Do not replace the reviewed mechanism based only on a
synthetic test or an unmeasured theory.

The preferred material remains Windows host-backdrop composition with transparent controls.
The entrance moves the **whole HWND** using Windows Animation Manager, carrying its DWM
border/shadow with the material and controls. During open, `DWMWA_CLOAK` hides the frame while the complete
control texture is uploaded and the composition tree is attached. `PrepareFirstFrame` waits for a GPU fence
and composition commit before the window is uncloaked behind the taskbar and scheduled for ascent.
Windows' animation preference is respected. The timer is disabled at rest; hide releases the animation
manager and native renderer. MEASURED 2026-11-02, current revision PID 19620, one-minute threshold,
20.040-second closed-panel sample: process CPU increment 0 ms; working set 25.71 MiB; private
bytes 9.27 MiB; 6 threads; native presentation count unchanged at 25 and no native worker at
either end. This is a bounded CPU sample, not a long-term power guarantee. Native open/close
completed with HRESULT `0x00000000`; the user subsequently gave the positive live verdict above.

Historical presentation traces established ordering, not complete on-screen readiness or the
precise cause of every artifact. The logger is removed in the cleanup above. Any future motion
investigation should use fresh, scoped profiling against a user reproduction rather than treat
the historical timing or a successful composition commit as proof of current visual fidelity.

MEASURED 2026-11-02, earlier pre-fix trace build: self-test PASS with 2,128 checks; build gates PASS;
executable 287,744 bytes. These checks establish build health, not visual fidelity. Current code
and documentation remain uncommitted after checkpoint `3ec8c43`; generated audit BMPs and logs
must not be included in a commit.

The detailed mechanism and open issue are in [IMPLEMENTATION.md section 9.1](../IMPLEMENTATION.md#91-panel-window-focus-and-click-away-dismissal).
Native material and ribbon details remain in [section 9.4](../IMPLEMENTATION.md#94-native-desktop-acrylic-nativebackdropcpp).

## Earlier native material and ribbon follow-up — 2026-11-01

The preferred renderer is now `NativeBackdrop.cpp`: Windows host-backdrop composition beneath
transparent GDI controls, with the original Microsoft grain texture embedded in the executable.
The rejected software material below remains only as a compatibility fallback. The user reviewed
the native build as "kinda good", then supplied footer screenshots showing that Windows has a
brighter upper surface and dark seam while the app had a continuous surface and light seam.

The requested correction is implemented and deployed: dark `acrylicBodyWash` is white at 9/255
above `FooterDividerY`, `footerDivider` is black at 26/255, and the footer keeps the original
acrylic. **UNVERIFIED visually:** there is no verdict or screenshot of that adjustment yet.
MEASURED 2026-11-01: build gates PASS, self-test PASS with 2,128 checks, executable 277,504 bytes;
native composition initialized with HRESULT `0x00000000` and the panel was visible at deployment.
No native worker is retained on hide; the earlier bounded hidden CPU samples and their limitations
are in IMPLEMENTATION.md section 15, not evidence of the latest build's exact power consumption.

Continue from [IMPLEMENTATION.md section 9.4](../IMPLEMENTATION.md#94-native-desktop-acrylic-nativebackdropcpp)
and the [case-file follow-up](research/2026-11-01-acrylic-colour-case-file.md#8-native-composition-and-footer-follow-up-2026-11-01),
which includes the preserved screenshot evidence. Commit `3ec8c43` remains the pre-native checkpoint;
the native and ribbon changes are uncommitted. The remaining sections describe
the historical software checkpoint and must not be read as current implementation instructions.


*Written 2026-11-01, at the user's request ("no, tragic, write handoff"). It records where the material
work stands, what has been proven, what has been rejected and why, and how to measure the next attempt
instead of guessing at it.*

---

## 1. Where it stood at the software checkpoint

The panel's background is a software copy of Windows 11 acrylic: one screen capture at panel open, blurred,
tinted with the WinUI recipe, painted as the panel's base layer.

**It is not accepted.** The user's last three verdicts, in order:

| Build | Verdict |
|---|---|
| Base variant, `TintOpacity 0.50`, saturation + exclusion + noise, accent card, 14/255 footer veil | *"really, really close to native"* (the only positive verdict so far) |
| Same minus saturation / exclusion / noise, neutral card, no footer veil | *"kilometers apart, almost like we lost the blur coloring"* |
| Same plus `TintOpacity 0.15` | *"no, tragic"* — then: keep this build, write the handoff |

The flip-flop is the point: **the same "fix" direction (darker + greyer) produced both rejections, and the
only build the user liked had the steps an independent audit called wrong.** Nothing in this file should be
read as "this is correct". The current tree is the third row.

Shipped right now:

* `Acrylic.cpp`: tint `#202020`, `TintOpacity 0.15` (`g.tintAlpha = 38`), `TintLuminosityOpacity 0.96`,
  saturation ×1.25, exclusion with white 26/255, 2% noise, blur = 3 box passes of radius 26 at ¼ scale.
* `Flyout.cpp`: card = Fluent `CardBackgroundFillColorDefault` token; footer = bare material above the
  divider; tiles = `ControlFillColorDefault` / `Secondary` washes (dark), black washes (light).
* Self-test PASS 2120 (never reduced), docs gate PASS, binary 250,880 bytes.

---

## 2. The moving parts

| File | What lives there |
|---|---|
| `Acrylic.cpp` / `Acrylic.h` | capture → blur → tint pipeline, snapshot vs live mode, capture exclusion, `--acrylic-test` probe, tuning env vars |
| `Flyout.cpp` | `PaintPanel`: base layer, tiles, slider, card, footer, text. Palette in `MakePalette` |
| `Flyout.h` | geometry constants (`k*`), `PanelState`, hit-testing |
| `IdleDimmer.cpp` | panel lifetime (`ShowPanel`/`HidePanel`), settings (`Acrylic`, `AcrylicLive`), `--menu`, `--status`, `--panel-shot`, `--acrylic-pause`, hotkey |
| `docs/research/2026-11-01-acrylic-colour-case-file.md` | the source study: WinUI's recipe with line numbers, the Fluent token table, every measurement, the audit |

Two modes, chosen by `AcrylicLive` in `settings.ini`:

* **snapshot (default)** — one capture before the window is shown, never refreshed. Screenshots work, no
  capture exclusion, nothing changes after the panel appears. A window caught mid-animation stays baked in
  until the panel is reopened.
* **live** — ~11 fps worker, needs `WDA_EXCLUDEFROMCAPTURE`, so Windows reports a protected app until
  **Ctrl+Alt+S** freezes it. ~4.2% of a core while open.

Env knobs (no rebuild): `IDLE_DIMMER_BLUR`, `IDLE_DIMMER_TINT` (0-255 = TintOpacity), `IDLE_DIMMER_LUMINOSITY`,
`IDLE_DIMMER_NO_EXCLUSION=1`.

---

## 3. What is actually proven

**3.1 The live panel matches its own code.** Measured three times on a static backdrop, with the panel
verified open/closed by pixels:

```
backdrop (blur)       #49855D   luma 117   chroma 59.3
app formula (sim)     #162E1E   luma  40   chroma 24.6
LIVE panel            #162E20   luma  40   chroma 23.6
panel-shot.bmp        #162D20   luma  40   chroma 23.4
```

So capture, arithmetic and painting agree. **There is no processing bug to find.** An earlier "13 luma gap"
was my own simulation using a non-W3C `SetLum`; it vanished the moment the sim used the same clip algorithm
as the code.

**3.2 The two layers do different jobs, and they were being confused.** The Luminosity blend fixes
brightness (it set the body at 40-41, matching the ~41 measured off the user's Windows capture); the Colour
blend decides how much of the wallpaper's hue survives. Measured live at four opacities, same backdrop:

| `TintOpacity` | luma | chroma |
|---|---|---|
| 0.50 | 40 | 23.6 |
| 0.30 | 41 | 33.2 |
| 0.20 | 41 | 38.0 |
| 0.15 | 41 | 40.4 |

Luma is unchanged across the whole range: **changing `TintOpacity` buys colour for free.** That is the one
lever that is neatly separated from darkness.

**3.3 The noise is not decoration.** Microsoft's own asset is 256×256, opaque, full range (mean 128.6,
sd 52.8). At 2% it is worth about **+1.9 luma** on a ~33-level body — removing it is what pushed the panel
into "kilometers apart" darkness. Keep it, or replace it with an equivalent lift.

**3.4 Source facts worth not re-researching** (all in the case file with line numbers):

* WinUI 3's acrylic graph is blur(30) → **Luminosity** blend → **Color** blend → noise 2% on top.
* The two blend-mode names are swapped in `AcrylicBrush.cpp` (`Mode(Color)` means Luminosity).
* `LuminosityColor` = the tint's RGB with `TintLuminosityOpacity` as alpha.
* `sc_saturation 1.25` and `sc_exclusionColor 26/255` exist in `AcrylicBrush.h` and are **never used** by
  `AcrylicBrush.cpp`. They are in the pipeline here anyway, because the build without them was rejected.
* Dark brushes: Base `#202020` @ 0.50 / 0.96; **Default `#2C2C2C` @ 0.15 / 0.96** (the flyout brush).
  Light Default `#FCFCFC` @ 0.00 / 0.85.
* Fluent surface tokens (dark): `ControlFillColorDefault #0FFFFFFF`, `ControlFillColorSecondary #15FFFFFF`,
  `CardBackgroundFillColorDefault #0DFFFFFF`, `CardStrokeColorDefault #19000000`.

---

## 4. What has NOT been measured — and is the real blocker

**No same-backdrop side-by-side with Windows' own Quick Settings has ever been taken.** Everything said
about "what Windows looks like" comes from two screenshots the user pasted into chat, judged by eye. That is
the single reason this work keeps oscillating.

Why it has not happened: opening Quick Settings needs a synthesised `Win+A`, which `AGENTS.md` §2 forbids,
and the user's screenshots have so far existed only in the chat (an attempt to recover them from the local
session database found no embedded image data).

**Until that comparison exists, every colour change is a guess with a number attached.** If the next person
can obtain it — the user saving their two screenshots to disk is enough — start there.

---

## 5. The measurement harness (already built, outside the repo)

`%TEMP%\opencode\acrylicprobe\` (~/… on this machine, not committed because it is a throwaway instrument):

| Piece | Purpose |
|---|---|
| `probe.cs` / `probe.exe` | `cap x y w h out.png`, `isopen file.png`, `stat file.png x0 x1 y0 y1`, `sim bg.png`, `analyze bg.png panel.png` |
| `measure.ps1 [-TintAlpha n]` | end-to-end: restart the app, close/open the panel with verification, capture backdrop + panel, print the table |

`analyze` prints, for the same pixels: the blurred backdrop, the WinUI brush graph, this app's formula, the
stripped variant, Base and Default variants, and the live panel — each with luma and chroma.

**Two traps that cost hours:**

1. **`--menu` is a toggle.** Ending up in the wrong state silently measures the desktop instead of the panel.
   Always verify with `isopen` (it looks for accent-coloured tile pixels), never trust `--status`.
2. **The panel's `--status` rect changes between sessions** (`3189,1297` vs `3189,1341`). Re-read it after
   opening, and capture backdrop and panel at the *same* rect.
3. A simulation is only evidence if it uses the **same clip algorithm** as the code (W3C `SetLum`/`ClipColor`),
   the same blur (4× downscale, three passes of radius 26), and the same noise lift.

---

## 6. What to try next, in order

1. **Get the same-backdrop pair.** One Windows Quick Settings screenshot and one of ours, same wallpaper,
   same region. Probe both with `probe.exe stat`. Everything below becomes checkable.
2. **If our body is too grey and Windows' is not** (the current hypothesis): the Colour blend is the lever,
   and 3.2 shows luma is unaffected. The user rejected 0.15 as "tragic", so the useful range is probably
   0.25-0.35 — but pick it from the pair, not from taste.
3. **If our body is too light/dark**: the lever is `TintLuminosityOpacity` (0.96 → 1.0 pins the body to the
   tint's own luminance). The tint *colour* (`#202020` vs `#2C2C2C`) moves it by ~7 luma.
4. **If the composite looks wrong rather than the body**: the surfaces. Windows paints tiles/cards/footer as
   translucent washes over the material; this build uses tokens for tiles and the card but keeps a 14/255
   footer veil and an accent-tinted card was rejected. Measure a card pixel against a body pixel before
   touching it.
5. **Blur strength** is a separate axis (`IDLE_DIMMER_BLUR`, box radius at ¼ scale). The current 26 ≈ 30
   logical px matches `sc_blurRadius`. A weaker blur keeps more structure and reads more "glassy"; the user
   has never judged a value other than this one.

---

## 7. Rules that must survive the handoff

* **The user is the auditor.** No change is verified until they look at the running build. A green self-test
  is necessary and never sufficient (`AGENTS.md` §1, §7.1, §10).
* **Never synthesise their input** — no `SetCursorPos`, `mouse_event`, `SendInput`. This is why Windows'
  own panel has not been captured yet (§4).
* **Deploy after every change**: quit, build, `--self-test` (must be PASS, never fewer assertions), launch
  hidden, tell the user what to look at.
* **Every number carries a status tag** — MEASURED (with date and backdrop), STALE, or UNVERIFIED.
* **`tools\check-docs.ps1` runs in the build** and fails on undocumented mechanisms, stale claims and
  undocumented status strings. Update its `$liveTerms` / `$staleClaims` lists with the same change.
* Instrument, don't guess (`AGENTS.md` §4): three failed guesses in a row means the diagnosis is wrong. This
  session reached that point twice.

---

## 8. Uncommitted

Everything here is uncommitted: `Acrylic.cpp`, `Acrylic.h`, `Flyout.cpp`, `Flyout.h`, `IdleDimmer.cpp`,
`IMPLEMENTATION.md`, `AGENTS.md`, `README.md`, `build.ps1`, `tools/check-docs.ps1`,
`docs/research/2026-11-01-acrylic-colour-case-file.md`, plus the icon pipeline from the previous session.
`panel-shot.bmp`, `acrylic-frame.bmp`, `acrylic-sample.bmp` are self-test artifacts and should not be
committed.
