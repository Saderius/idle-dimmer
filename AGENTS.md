# AGENTS.md — working rules for this repository

*Created 2026-10-15*

Project-specific rules for any AI agent (or human collaborator) working on Idle Dimmer.
These override default behaviour. Read this file before touching code.

---

## 1. DEPLOY AFTER EVERY CHANGE. THIS IS NOT OPTIONAL.

**After any change to the app, rebuild it and launch the running instance so the user can audit it.**

The user is the auditor. Synthetic tests, self-tests, headless renders and harness scripts are
*not* a substitute for the real app being live on their desktop. Changes stay unverified until
the user has interacted with the running build and reported back.

```
# 1. stop whatever is running (graceful — it restores brightness on the way out)
Start-Process IdleDimmer.exe -ArgumentList '--quit' -Wait -NoNewWindow

# 2. rebuild
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1

# 3. gate: self-test must pass before deploying
Start-Process IdleDimmer.exe -ArgumentList '--self-test' -Wait -NoNewWindow
Get-Content diagnostics.txt          # must read PASS

# 4. DEPLOY — this is the step that has been skipped before
Start-Process IdleDimmer.exe -WorkingDirectory . -WindowStyle Hidden
```

Then **tell the user it is live and what to try.** Do not end a turn with "changes made,
builds clean" and no running app.

### Why this rule exists

It exists because skipping it produced a concrete failure. The click-away dismissal bug was
"fixed" three times. Every fix passed: the self-test passed (345 assertions), synthetic cursor
and click simulations closed the panel correctly every time, and the build was clean. The
panel still would not close for the user, because the synthetic input did not reproduce what
their hand actually does. Three rounds of guessing, all green, all wrong.

The only thing that would have caught it was the user clicking on the real panel.

**Corollary:** if a bug cannot be reproduced by the agent, the correct next move is to add
logging and hand the reproduction to the user — *not* to keep iterating on hypotheses. See §4.

---

## 2. Never move or synthesise the user's input

**Do not call `SetCursorPos`, `mouse_event`, `SendInput`, or anything else that moves, clicks
or drives the real mouse.** The user drives their own machine.

This was done during an earlier attempt and is forbidden. Any script that automates mouse input
must be deleted, not left in the tree.

Related: the browser/preview tooling in this environment cannot see a real desktop window, so
"take a screenshot and look at it" is not available. **Ask the user for screenshots** — they
have offered, and they are the only source of visual truth for the panel.

---

## 3. Read the reference implementation before inventing a mechanism

Before hand-rolling anything Windows-shaped — tray flyouts, click-away dismissal, icon
caching, DWM attributes — find out how a shipped app already does it and **read its source.**

EarTrumpet is open source and was the direct reference for this panel. Reading it
(`UI/Views/FlyoutWindow.xaml.cs`, `UI/Helpers/WindowAnimationLibrary.cs`,
`UI/Helpers/ShellNotifyIcon.cs`) is still worth doing — it saved a lot of guessing — **but its
mechanism was then measured failing here, so do not copy it.** EarTrumpet's flyout takes focus
on open and is dismissed by ordinary `WM_ACTIVATE`/`WA_INACTIVE`. This app's panel is
`WS_EX_NOACTIVATE` and cannot do that: a tray app loses the foreground fight to the shell, so
forcing activation made the panel open and hide itself about a second later. **What actually
works here is a `WH_MOUSE_LL` low-level mouse hook, installed only while the panel is visible**,
with `RectContainsPoint` as the outside test. See `IMPLEMENTATION.md` §9.1.

EarTrumpet also flagged a duplicate notification per click on the Windows 11 taskbar, which is
where the flicker hunt started. That diagnosis turned out to be wrong too: measured, one click
delivers `WM_LBUTTONUP` **and** `NIN_SELECT`, not `WM_LBUTTONUP` twice — two *different* codes,
which is exactly why a 300 ms timestamp de-dup could not collapse them. The gesture contract was
measured, not assumed; see `IMPLEMENTATION.md` §9.1.

---

## 4. Instrument, don't guess

When a bug does not reproduce, the fix is **evidence**, not another hypothesis.

- Add logging that captures the decision the code actually made, with the inputs that fed it
  (cursor position, window rect, flags, counters).
- Hand the reproduction to the user and read the log afterwards.
- State which candidate causes the evidence rules out.

Never respond to a failed fix attempt with a new theory and another edit. Three failed guesses
in a row means the diagnosis is wrong, not that the fourth guess will land.

### Logging discipline

- **Append-only, never truncate.** An earlier `LogEv` opened `diagnostics.txt` with `"w"`, which
  wiped the report that `--status` had just written, and raced with it.
- **Never log to a file the app also reports into.** Diagnostics go to a separate log.
- **Remove instrumentation before committing.** Debug logging left in `IdleDimmer.cpp` is
  uncommitted debris.

---

## 5. Tests

- The self-test assertion count must **never be reduced**. Fix the test or the code; do not
  delete an assertion to make a suite pass.
- Pure decision logic is extracted specifically so it can be tested (`ShouldDim`,
  `DescentBucket`, the `Flyout.h` layout/hit-test stack). Keep it pure.
- A green self-test is necessary and **not sufficient** — see §1.

---

## 6. Project constraints

| Constraint | Value | Why |
|---|---|---|
| Language | C / C++ (Win32), no framework or runtime | Background threads in a framework runtime keep ticking while the UI is closed, which defeats the idle-CPU goal |
| Idle CPU target | as close to 0 as possible when the panel is closed | The app's entire reason to exist is to cost nothing |
| Memory | generous (100 MB acceptable) | Not the bottleneck |
| UI | exactly one — the Quick Settings panel | Two front ends drift; there used to be a tray context menu and it caused exactly that |
| Installer | none needed; single folder + Run key | |
| Binary | ~246 KB (MEASURED 2026-11-01: 251,392 bytes) | 4x panel icon masks add ~70 KB and the app icon adds ~13 KB; not a hard budget, but worth not regressing |

---

## 7. Documentation is part of the change

Update `IMPLEMENTATION.md` in the same change as the code it describes — §4 for the check
cycle, §9 for the panel, §15 for measured footprint. Stale docs on this project have already
caused real confusion (a status string that advertised a wakeup period the scheduler no longer
used, a removed context menu still described in §9).

### 7.1 Doc the mechanism only after it has been verified live

A mechanism paragraph is written when **the user has confirmed it on a running build**, not
when it compiles. Until then, either leave the previous text alone or mark the new text
`UNVERIFIED`.

This is the single most important rule in this file. Three drafts of the §9.1 click-away
description were written and all three described a mechanism that was about to be replaced —
one claimed the panel took focus and used `WA_INACTIVE`, which was measured failing within a
day. A doc written from intent is worse than no doc, because it is confidently wrong.

**A green self-test is not verification.** See §1.

### 7.2 Every number carries a status tag

No bare figures. Use one of:

```
MEASURED 2026-10-15, threshold 2 min, panel closed, 300 s sample
STALE     superseded by §4.1, not re-measured
NEVER MEASURED
```

`README.md` carried `~0.02% CPU, ~4 MB, 6 threads` long after measurement contradicted it
(baseline was 0.29%, and a post-change run came back at 1.885%, still unexplained). A stale
number repeated in two documents is load-bearing and expensive to unwind.

### 7.3 Replacing a mechanism deletes its documentation outright

Do not amend, do not soften, do not leave "still accurate in spirit" prose behind. Residual
sentences are what made §9 unreadable — at one point it said "no cursor polling and no guard
flag" in one paragraph while describing both in the next.

Quoting a removed mechanism **in order to explain what went wrong** is required, not optional.
`IMPLEMENTATION.md` §9.1 and `AGENTS.md` §3 both do this deliberately.

### 7.4 The gate: `tools\check-docs.ps1`

Runs automatically as part of `build.ps1`. Fails the build on:

1. **Live terms undocumented** — a mechanism in the code with no mention in the docs.
2. **Stale claims** — literal assertions that were true once and are now false.
3. **Undocumented status strings** — any `wcscpy(status, L"…")` literal the user can see that
   the docs do not account for. This catches the next lying status string automatically,
   instead of waiting for a human to notice.
4. *(warning only)* **Dead symbols** — something in the docs that no longer exists in the code,
   reported with line numbers for human judgement, never a hard failure.

Run it directly any time:

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File tools\check-docs.ps1
```

**When you add or remove a mechanism, update the `$liveTerms` and `$staleClaims` lists in that
script in the same commit.** A gate that only knows about yesterday's mechanisms is not a gate.

### 7.5 User-visible copy is a contract

Strings shown in the tray tooltip are documented in `IMPLEMENTATION.md` §10.1. The panel status
and the docs are two descriptions of the same state; if one changes, the other changes with it.
A status string is not merely UI copy — it is a claim about behaviour, and it gets read by
people debugging, which is how the false wakeup rate survived so long.

---

## 8. Commit discipline

- Do not commit debug instrumentation or temporary harness scripts.
- `*.exe`, `build-tools/`, generated logs and `docs/superpowers/` are gitignored — leave them.

---

## 9. Persistent memory

### 9.1 Response format requested by the user (2026-10-21)

Start every substantive reply with a four-sentence TL;DR before the detailed answer.

### 9.2 Panel icons: hand-rasterized GDI → pre-rasterized SVG masks (2026-10-21, confirmed 2026-10-24)

**Before.** MEASURED 2026-10-21 on `icons/verify-panel.png` at 96 DPI: the central 30×30 px
of all four tile glyphs contained exactly **two** colours — the tile fill and the glyph colour.
There were no intermediate coverage pixels, so there was no antialiasing. The design's `IC.*` icons
are 20×20 SVGs with `stroke-width="1.5"`, round caps and round joins. The app did not render SVG:
`DrawGlyph` approximated each path with `StrokeLine` (integer-aligned `thick × thick` squares) and
`StrokeCircle` (integer annulus), and `FillBlend` wrote whole pixels. It also used `Sc(2)`
(2 logical px) rather than the design's 1.5, so the result was deliberately blocky and heavier.

**After — CONFIRMED 2026-10-24 by the user on a live 200% panel.** The design SVGs are rasterized
at 80 px by `tools\build-panel-icons.ps1` into alpha masks (`icons\panel\*.png` +
generated `PanelIconsData.h`), and `BlitPanelIcon` area-filters them to the target DPI. This keeps
the design's 1.5 px stroke and round caps instead of approximating them with integer GDI lines.
Tile glyphs are 20 px, the slider clock 18 px, and footer buttons 15 px. Do not regress to direct
integer line drawing for icons (see §10).

### 9.3 Accent colour, hover swatch, and hover wiring (2026-10-24, confirmed by the user)

**The accent is `AccentPalette` entry 1, not entry 0.** MEASURED 2026-10-24, dark theme, registry
read directly: the ramp is `[0] #ECDCAA, [1] #D4BB8B, [2] #B28249, [3] #986B3E, …`, light to dark.
Entry 1 is the resting fill of an ON Quick Settings tile; entry 0 is the lightest shade, which is
what a hovered tile shows. A pointing tool resting on a hovered tile reads the highlight, not the
fill — that measurement error produced five "fixed" commits and a self-test that pinned the wrong
swatch. Measure accent colours with nothing hovered.

**Hover paints entry 0; it is not a lift.** The ramp step from entry 1 to entry 0 is (+24,+33,+31),
so a per-channel addition can only approximate it. `SystemAccentHover()` supplies entry 0 and
`PaintPanel` paints it on a hovered ON tile; `--diagnose` prints both values.

**Two wiring truths, both found only after the colour was finally correct:**

- `PanelState::hover` and `::pressed` are populated by `CurrentPanelState()` and nowhere else. They
  were not, so the panel tracked the pointer correctly and painted `hitNone` on every frame; no tile
  could highlight. A painted-surface test cannot catch that — it sets `hover` by hand.
- Hover geometry is the tile body (`HitTestHover`); click geometry includes the caption band
  (`HitTest`). The highlight is drawn on the box, so the label must not light it, while a click on
  the label still toggles the control.

---

## 10. The Anti-Complexity Theater Rule: No Synthetic Self-Certification for Visual UI

**Unit tests, assertion counts, and zero-exit codes DO NOT prove visual fidelity.**

When a task involves visual appearance, icons, margins, typography, or UI ergonomics:
1. **Synthetic assertions are necessary, not sufficient.** Passing 600+ checks in `--self-test` proves
   only that the code did not crash and integer math was consistent. It proves NOTHING about whether
   the UI looks like the design.
2. **Never declare victory based on proxy metrics.** An agent or review pipeline must NEVER certify
   completion of a visual task based on test passes, compiler exit codes, or contact sheet file generation.
3. **No Multi-Agent Review Echo Chambers.** Do not spawn reviewer chains where reviewers merely check
   whether the implementer's self-tests pass. A review that does not inspect visual pixel output is
   complexity theater and burns tokens for zero value.
4. **The GDI Antialiasing Invariant.** GDI integer lines (`StrokeLine`/`StrokeCircle`) write binary pixels
   with ZERO edge coverage. Vector icons must never be drawn with raw integer lines directly onto the panel.
   They MUST be drawn into an oversampled mask (e.g. 4×) and box-downsampled to produce smooth alpha edges,
   or use Direct2D/icon fonts.
