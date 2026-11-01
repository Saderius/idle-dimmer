# Case file: the acrylic background colour (2026-11-01)

**Status: OPEN.** The panel body colour has been "fixed" four times and still reads wrong to the user.
This file freezes what is *verified* so the next attempt starts from evidence instead of a guess.

Trigger, verbatim: *"how come we completely missed the colors of the background again? how is that even
possible? read and research what colors or like how windows acrylic makes that is looks as it looks
meaning, what is behind it, what colors from theme it gets and so on."*

---

## 1. What Windows actually composites

Source read for this section: `microsoft/microsoft-ui-xaml` @ `main`,
`controls/dev/Materials/Acrylic/AcrylicBrush.cpp` and `AcrylicBrush_themeresources.xaml`
(reached from `learn.microsoft.com/windows/apps/develop/ui/in-app-acrylic`, which links the file).
Line numbers are from that revision.

### 1.1 The effect graph, in order

From the recipe comment at `AcrylicBrush.cpp:417-471` and the builder at `:472-560`:

```
Backdrop ──► composite over an OPAQUE FallbackColor (SourceOver, :508-518)
         ──► GaussianBlur(BlurAmount = sc_blurRadius = 30, BorderMode = Hard)      (:520-526)
         ──► Blend (Luminosity)  fg = LuminosityColor                             (:396-402)
         ──► Blend (Color)       fg = TintColor                                   (:406-412)
         ──► composite noise ON TOP (SourceOver)  fg = noise tex * 0.02           (:530-549)
         ──► CrossFade between the acrylic result and the opaque FallbackColor    (:551-575)
```

**Trap: the two blend modes are swapped in the code.** `D2D1_BLEND_MODE`'s `Color` and `Luminosity`
names are flipped in the effect wrapper, so the source writes
`luminosityBlendEffect->Mode(BlendEffectMode::Color)` when it means *Luminosity*, and
`colorBlendEffect->Mode(BlendEffectMode::Luminosity)` when it means *Color*
(`:398-400` and `:408-410`). Read literally, the graph is upside down. It is not.

Effects of each step, stated plainly:

| Step | What it takes | What it keeps |
|---|---|---|
| Luminosity blend | lightness from `LuminosityColor` | hue + saturation from the blurred desktop |
| Color blend | hue + saturation from `TintColor` | lightness from the previous step |
| Noise | 2% of a tiled 256×256 noise texture over everything | - |

Consequence: with a **neutral** tint (Windows' dark tint is `#202020`, saturation 0) the Color blend
*desaturates the wallpaper* by `TintOpacity`. That is why Windows' acrylic over a red wallpaper is a
muted warm grey rather than a red panel.

### 1.2 The two colours, and how they are derived

`AcrylicBrush.cpp:189-314`:

```cpp
// TintColor as used by the Color blend
if (TintLuminosityOpacity != null)            // the theme brushes always set it
    tint.A = round(255 * TintOpacity);        // dark Base: 0.5  -> 128
else
    tint.A = round(255 * TintOpacity * GetTintOpacityModifier(tint));

// LuminosityColor as used by the Luminosity blend
if (TintLuminosityOpacity != null)
    return { tint.R, tint.G, tint.B, clamp(TintLuminosityOpacity) };   // dark Base: #202020 @ 0.96
else
    return { HSV(tint.H, tint.S, clamp(tint.V, 0.125, 0.965)),
             alpha = tint.A/255 * (1.03 - 0.15) + 0.15 };               // clamped to 1.0
```

Two facts worth keeping:

* **`LuminosityColor` is not the tint colour with a different opacity setting** - it is the tint's *RGB*
  with `TintLuminosityOpacity` as alpha. For `#202020` that means "force lightness to ~0.126, keep the
  wallpaper's hue".
* **`GetTintOpacityModifier` (`:209-263`) exists to stop an app tinting itself blind**: 100% tint
  opacity is remapped to 45% for pure white, 90% for mid grey, 85% for pure black, damped as
  saturation rises. It is skipped entirely when `TintLuminosityOpacity` is set, which is why the
  theme brushes below use their literal opacities.

### 1.3 `sc_saturation` and `sc_exclusionColor` are declared but never used

`AcrylicBrush.h:64-67`:

```cpp
static constexpr float sc_blurRadius    = 30.0f;
static constexpr float sc_noiseOpacity  = 0.02f;
static constexpr winrt::Color sc_exclusionColor{ 26, 255, 255, 255 };
static constexpr float sc_saturation    = 1.25f;
```

`AcrylicBrush.cpp` references `sc_blurRadius` and `sc_noiseOpacity` only. **`sc_saturation` and
`sc_exclusionColor` appear nowhere in that file** - they are leftovers of the older (RS3-era) recipe.
The public docs still describe acrylic as "background → blur → exclusion blend → tint → noise"
(`learn.microsoft.com/windows/apps/design/style/acrylic`, "How we designed acrylic"), so the exclusion
layer is real in *some* build of the material, but it is **not** in the WinUI 3 brush this app copies.

### 1.4 The theme brushes (dark)

`AcrylicBrush_themeresources.xaml`, `Default` theme dictionary:

| Resource | TintColor | TintOpacity | TintLuminosityOpacity | FallbackColor |
|---|---|---|---|---|
| `AcrylicBackgroundFillColorDefaultBrush` | `#2C2C2C` | 0.15 | 0.96 | `#2C2C2C` |
| `AcrylicBackgroundFillColorBaseBrush` | `#202020` | **0.50** | 0.96 | `#1C1C1C` |
| `AcrylicBackgroundFillColorDefaultInverseBrush` | `#FCFCFC` | 0.00 | 0.85 | `#F9F9F9` |

Light theme: Default = `#FCFCFC` @ 0.00 / 0.85; **Base = `#F3F3F3` @ 0.00 / 0.90**.

Quick Settings is a system surface and reads as the **Base** variant. This app uses Base for dark
since 2026-11-01 (previously Default, which measured ~10 luma too light over the same wallpaper).
The light path is still wrong: `IdleDimmer.cpp` passes tint `#FCFCFC` with luminosity 0.85, i.e. the
*Default* variant, where Base wants `#F3F3F3` at 0.90.

### 1.5 The sub-surfaces: Fluent tokens, not panel colours

A flyout is not one surface. Everything drawn *on* the material in Windows is a translucent white or
black wash, so it inherits the material's colour instead of covering it
(`controls/dev/CommonStyles/Common_themeresources_any.xaml`, lines as noted):

| Token | Dark | Light |
|---|---|---|
| `ControlFillColorDefault` (`:15` / `:219`) | `#0FFFFFFF` (white 6%) | `#B3FFFFFF` (white 70%) |
| `ControlFillColorSecondary` (`:16` / `:220`) | `#15FFFFFF` (white 8%) | `#80F9F9F9` |
| `ControlFillColorTertiary` (`:17` / `:221`) | `#08FFFFFF` (white 3%) | `#4DF9F9F9` |
| `SubtleFillColorSecondary` (`:26` / `:230`) | `#0FFFFFFF` | `#09000000` |
| `CardBackgroundFillColorDefault` (`:56` / `:260`) | `#0DFFFFFF` (white 5%) | `#B3FFFFFF` |
| `CardStrokeColorDefault` (`:46` / `:250`) | `#19000000` (black 10%) | `#0F000000` (black 6%) |
| `SolidBackgroundFillColorBase` (`:68` / `:272`) | `#202020` | `#F3F3F3` |

What this app paints today, for comparison:

| Surface | This app | Windows token | Verdict |
|---|---|---|---|
| OFF tile | white 13/255 dark, black 10/255 light (`Flyout.cpp:317,337`) | `ControlFillColorDefault` | dark ≈ right; **light is inverted** (a dark wash where Windows uses white 70%) |
| Tile hover | white 21/255 dark, black 15/255 light (`:318,338`) | `ControlFillColorSecondary` | dark right; light wrong |
| Tile border | white 18/255 dark (`:319`) | `ControlStrokeColorDefault` | plausible |
| Power card | **accent at 8%** (`Flyout.cpp:862`) | `CardBackgroundFillColorDefault` (neutral white 5%) | **invented** - tints a third of the panel with the accent |
| Footer / dock | white **14/255** veil over the material (`Flyout.cpp:919`) | none - the footer is the material plus a divider | **invented** - this is the "ribbon too light" complaint |


---

## 2. What this app does, and where it deviates

`Acrylic.cpp::CaptureIntoScratch` (lines 250-320): capture → downscale ¼ → three box passes (radius 26)
→ saturation ×1.25 → exclusion with white 26/255 → Luminosity blend (`#202020` @ 0.96) → Color blend
(`#202020` @ 0.50) → per-pixel noise at 2% → publish as `PaintPanel`'s base layer.

| # | Deviation | Status |
|---|---|---|
| D1 | **Saturation ×1.25** — not in the WinUI 3 recipe | [VERIFIED: absent from `AcrylicBrush.cpp`] |
| D2 | **Exclusion with white at 26/255** before the tint - not in the WinUI 3 recipe | [VERIFIED: absent from `AcrylicBrush.cpp`] |
| D3 | **Noise amplitude and placement.** MEASURED 2026-11-01: Microsoft's asset is 256×256, 8bpp greyscale, opaque, **full range** (R/G/B min 0, max 255, mean 128.6, sd 52.8) - it is *not* a soft texture, so amplitude is not the difference. The two real differences are (a) **+1.9 luma brightening bias**: both recipes lerp toward a mean of ~128, but Windows composites onto a 33-level body, so `0.02 × (128 − 33)` is a one-way lift, and (b) **placement**: this app applies the noise to the half-scale scratch buffer, so a 2× HALFTONE upscale smears each sample into a 2×2 blot - coarser and blurrier than Windows' 1:1 grain | [VERIFIED in code; asset measured] |
| D4 | **Sub-surfaces are opaque absolute colours.** Tiles paint `p.tileOff`, the dock paints `p.dock`; Windows paints `ControlFillColor*` / `CardBackgroundFillColor*` - translucent washes *over* the same material, so they inherit its colour. The material therefore only shows through the body gaps | [VERIFIED in `Flyout.cpp:790, 923`] |
| D5 | No opaque fallback composite before the blur | [VERIFIED, harmless over the desktop] |

D1 and D2 cancel out in the average: simulated side by side on the same blur they differ by ≤1 luma.
They are not the visible problem; D3 and D4 plausibly are.

---

## 3. Measurements

### 3.1 Formula vs formula, same backdrop

`probe.cs` (kept out of the repo, see §6) captures the rect behind the closed panel, blurs it, then
applies **Windows' WinUI 3 recipe** and **this app's recipe** to identical pixels and compares the mean
of a strip of plain panel body:

```
backdrop (raw)     #635A46   luma 91
backdrop (blur)    #615945   luma 89
Windows recipe     #262218   luma 34
our recipe (sim)   #272219   luma 35
```

**The body formula matches Windows' to within 1 luma.** Whatever is wrong, it is not the body maths.

### 3.2 Live panel against the user's Windows capture

| Sample | This app (live) | Windows (user's crop) |
|---|---|---|
| Body | `#2A2824` / `#2B2924`, luma 41-42 | `#1C2A33`, luma ≈ 41 |
| Ribbon / footer | `#2C2F32`, luma 45 | `#22333D`, luma ≈ 48 |

Equal luma, different hue - but the two samples are over *different* backdrops, so only luma is
comparable. This comparison cannot settle the question and should stop being used as if it did.

### 3.3 What has not been measured

* The user's own screenshots have never been probed numerically; they exist only in the chat. Every
  "Windows says X" figure in this file and in `IMPLEMENTATION.md` is therefore an **eyeball estimate
  from an image**, not a measurement.
* Windows' own Quick Settings panel cannot be opened from here without synthesising input
  (`AGENTS.md` §2 forbids it), so **no same-backdrop side-by-side has ever been taken**. That is the
  measurement this whole case lacks.

---

## 4. Open questions, in the order they matter

1. **Is the visible mismatch the body tone, the sub-surface tones, or the grain?** Everything measured
   says the body tone is right, which points at D3/D4. Needs a screenshot of our panel next to
   Windows' over the *same* wallpaper, both cropped to the same region.
2. **Does the shell use the DWM recipe (with saturation + exclusion) or the WinUI 3 one?** The shell's
   flyouts are composited by DWM (`DWMSBT_TRANSIENTWINDOW`), not by `AcrylicBrush`; the unused
   constants in `AcrylicBrush.h` are evidence that the DWM variant may still contain those steps.
   If it does, D1/D2 should stay.
3. **What is the real noise asset's amplitude?** `Assets/NoiseAsset_256X256_PNG.png` is in the repo and
   can be fetched and measured instead of guessed at. That would settle D3 with a number.

---

## 5. What the next attempt must not do

* Do not re-derive the tint colour or its opacities: §1.4 is the source, and the Base variant was
  already corrected once.
* Do not compare luma across two different backdrops and call it agreement (§3.2).
* Do not add a mechanism paragraph to `IMPLEMENTATION.md` before the user confirms it on a live build
  (`AGENTS.md` §7.1).

---

## 6. Reproducing §3.1

```
# probe lives outside the repo (not committed): %TEMP%\opencode\acrylicprobe
csc /unsafe /out:probe.exe /r:System.Drawing.dll probe.cs

IdleDimmer.exe --menu              # close the panel
IdleDimmer.exe --status            # read "Panel rect: x,y wxh" from diagnostics.txt
probe.exe cap <x> <y> <w> <h> bg.png
IdleDimmer.exe --menu              # open it
probe.exe cap <x> <y> <w> <h> panel.png
probe.exe analyze bg.png panel.png
```

Flaky while the user is at the keyboard: any click dismisses the panel, and a capture taken in the
wrong state silently measures the desktop instead. Verify the image, not the status line.

---

## 7. Independent audit (agy, `gemini-3.8-flash-high`, 2026-11-01)

Run: `docs/superpowers/packets/ACR-1_acrylic-colour-audit.md`, log
`docs/superpowers/packets/ACR-1_agy.log` (SUCCESS, 164,597 tokens, no denied actions, no edits made).
It was given the recipe and the code and asked six questions; this is the orchestrator's audit of its
answer, not a transcript.

### 7.1 Accepted

| # | Finding | Lands in |
|---|---|---|
| A1 | The **power card is tinted with the accent at 8%** - an invented surface. Windows uses the neutral `CardBackgroundFillColorDefault` (white 5% dark). | `Flyout.cpp:862-863` |
| A2 | The **footer carries a white veil** (14/255) that Windows does not have; the divider alone separates it. This is the "ribbon too light" report. | `Flyout.cpp:919-921` |
| A3 | The **saturation ×1.25 and exclusion-26 steps are not in WinUI's acrylic** and should go rather than be tuned. | `Acrylic.cpp:266-281` |
| A4 | The **noise is a net brightening**: lerping toward a mean of 128 lifts a 33-level body by ~1.9 luma. Windows pays the same lift, but also gets 1:1 grain for it; applied at half scale here it costs the lift and delivers blurry blotches. Drop it, or make it zero-mean at 1:1. | `Acrylic.cpp:27-38, 302-309` |
| A5 | The **light theme is using the Default acrylic variant** (`#FCFCFC` @ 0.85) where Base wants `#F3F3F3` @ 0.90, and its tile washes are black where Windows uses white 70%. | `IdleDimmer.cpp:1882`, `Flyout.cpp:337-339` |
| A6 | Snapshot mode is the right architecture; the minimum for "never changes after it appears" is capture → process → upscale **before** `ShowWindow`, with no post-open timer. That is already what the code does. | - |

### 7.2 Rejected, with the reason

| Claim | Why it is wrong |
|---|---|
| "Rec.601 vs Rec.709 luma coefficients are a bug (1-3 luma)" | The W3C Compositing and Blending spec - which is what Direct2D's `BlendEffect` implements - defines `Lum(C) = 0.3R + 0.59G + 0.11B`. `Acrylic.cpp:40` is *correct* and matches Windows. |
| "The exclusion blend lifts the final image by +19.5 counts / +2.6 luma" | It is applied **before** the Luminosity blend, which resets luminance to the tint's; only ~4% of the lift survives. Measured side by side (§3.1) the two recipes differ by ≤1 luma. |
| "OFF tiles use arbitrary opacities" | They are already `ControlFillColorDefault` (white 13/255) and `ControlFillColorSecondary` (21/255). Only the *light* values are wrong. |
| "Dark `CardStrokeColorDefault` is white 18/255" | It is `#19000000` - **black** at 10% (`Common_themeresources_any.xaml:46`). |

### 7.3 Where this leaves the case

Four of the five accepted items are *removals* of things Windows does not do (an accent-tinted card, a
veiled footer, an exclusion step, a saturation step, a noise layer that brightens). They were the
cheapest possible experiment: each one moves the panel toward Windows' arithmetic rather than toward a
taste.

**LANDED 2026-11-01** (same day, after the audit):

| Change | File |
|---|---|
| Card: accent 8 % → `CardBackgroundFillColorDefault` + `CardStrokeColorDefault` (both themes) | `Flyout.cpp` palette (`cardFill`, `cardEdge`) |
| Footer: white veil removed; bare material above the divider | `Flyout.cpp` §8 |
| Saturation ×1.25 and exclusion-26 deleted | `Acrylic.cpp` |
| Noise layer and its +1.9 luma lift deleted | `Acrylic.cpp` |
| Light theme → Base `#F3F3F3` @ 0.90 | `IdleDimmer.cpp` (both call sites), `Acrylic.cpp` luminosity default |

Self-test PASS 2120 (unchanged), docs gate PASS, binary 250,880 bytes. **Not yet audited by the user** —
per `AGENTS.md` §1 and §7.1 nothing above may be described as verified until they say so on a live build.

### 7.4 Follow-up the same evening: the removals were wrong, and the real variable is `TintOpacity`

The user's verdict on the stripped build was *"kilometers apart, almost like we lost the blur coloring"* —
i.e. removing the saturation / exclusion / noise steps made the panel **dead grey**. They were restored.

That forced a proper instrument instead of another guess (§3.3 said no same-backdrop measurement had ever
been taken, and that was the actual gap). `probe.cs` was extended to reproduce the app's own pipeline
exactly — same 4× downscale, same three box passes at radius 26, same W3C `SetLum`/`ClipColor` — and the
live panel was then compared against it on a **static** backdrop, with the panel's own rect re-measured
each round:

| Round | backdrop (blur) | app formula, same pixels | LIVE panel |
|---|---|---|---|
| 1 | `#49855D` luma 117 chroma 59 | `#162E1E` luma 40 chroma 24.6 | `#162E20` luma 40 chroma 23.6 |
| 2 | identical | identical | identical |
| 3 | identical | identical | identical |

**The live panel matches the code's own arithmetic to within 1 luma.** There was no processing bug, and
the earlier "13 luma gap" was my first simulation using a non-W3C `SetLum`. `panel-shot.bmp` (what
`PaintPanel` renders) matched the live capture too, so capture, processing and painting all agree.

So the colour loss is a **parameter**, and the lever is the Colour blend - the one step that takes hue
away, since the Luminosity layer only fixes brightness. Measured live at four opacities, same backdrop:

| `TintOpacity` | luma | chroma |
|---|---|---|
| 0.50 (what shipped) | 40 | 23.6 |
| 0.30 | 41 | 33.2 |
| 0.20 | 41 | 38.0 |
| **0.15 (Windows' own brush value)** | **41** | **40.4** |

Luma barely moves (the luminosity layer holds it at Windows' measured 41); chroma nearly doubles. That is
the whole of "we are losing colours": the neutral Colour blend was set to 0.50, the *Base* surface value,
while **both** acrylic variants' flyout brush - `AcrylicBackgroundFillColorDefaultBrush` - carries
`TintOpacity 0.15`. Base's 0.50 desaturates the wallpaper twice as hard as any Windows flyout does.

**LANDED 2026-11-01:** dark tint stays `#202020` (Base's luminosity colour, which is what puts the body at
Windows' luma) with `TintOpacity 0.15` (Default's value, which is what keeps the wallpaper's colour).
Live re-measure of the shipped build, no environment overrides: `#0B341D` **luma 41, chroma 40.4**.
Light theme went back to the Default values (`#FCFCFC` @ 0.00 / 0.85) for the same reason.

Stated plainly so the next reader does not repeat it: picking the luminosity colour and the colour
opacity from *different* variants is deliberate, because each is measured against a different target
(brightness vs hue retention). It is not a transcription of one brush.

**VERDICT 2026-11-01: rejected.** The user's response to the 0.15 build was *"no, tragic"*. They then
asked to keep that build as it stands and to write a handoff instead of another round
(`docs/HANDOFF-acrylic.md`). So the table above is an arithmetic fact - luma is flat across those
opacities and chroma roughly doubles - but **it is not evidence about what the user wants to see**:
the build that they called "really, really close to native" had 0.50 and the extra steps. Do not treat
"more chroma" as progress; it was tried and rejected within the hour.




