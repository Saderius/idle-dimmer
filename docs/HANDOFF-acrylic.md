# Handoff — the acrylic background

*Written 2026-11-01, at the user's request ("no, tragic, write handoff"). It records where the material
work stands, what has been proven, what has been rejected and why, and how to measure the next attempt
instead of guessing at it.*

---

## 1. Where it stands

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
