# Tray icon artwork and generated ICOs

Artwork status: **present** — five SVG designs live here and are the source of every tray icon the app shows:

| Id | SVG | Design | States |
|---|---|---|---|
| 1 | `tray-dimmer-bulbCollarChevron.svg` | Bulb and chevrons | on / off / dim |
| 2 | `tray-dimmer-horizonSunDoubleChevron.svg` | Horizon sun, double chevrons | on / off / dim |
| 3 | `tray-dimmer-horizonSunOriginal.svg` | Horizon sun and arrow (default) | on / off / dim |
| 4 | `tray-dimmer-pick4SingleChevron.svg` | Bulb and single chevron | on / off / dim |
| 5 | `tray-dimmer-sunset-*.svg` (8 files) | **Sunset timer (live)** | off / dim / d0…d5 |

All use a 20×20 viewBox and transparent background, with outline and filled variants.

**UNVERIFIED 2026-10-27, pending live tray audit:** design 4's enabled `on` and dimmed `dim`
states use `tray-dimmer-pick4SingleChevron-separated.svg`. The bulb has its existing outer
outline and a smaller inset fill with a genuinely transparent band between them. The inset
reuses the bulb path scaled around its center, rather than painting a background-colored
line, so the separator works on either taskbar theme. The chevron is unchanged. Disabled
`off` keeps the existing outline artwork; enabled and dimmed retain their existing tints.
The previous solid-filled bulb had no separation between its border and body.
For this update Sharp renders the SVG into a transparent alpha source, which then runs
through the pipeline's `Refit-Mask`, `Scale-Mask`, `Set-Tint` and `Write-Ico` functions.

## Design 5 is the only animated one

Its sun descends toward the horizon rule as idle time accrues, so the tray reports how close the next dim is
without being opened. Six descent frames (`d0`…`d5`) carry the enabled-and-undimmed look; `off` is disabled
and `dim` is dimmed. There is deliberately **no `on` frame** — with design 5 selected it could never display.

Two constraints make this design behave differently, and both are enforced rather than eyeballed:

- **`Raw` in `$designs`.** `Refit-Mask` trims each frame to its own content box and re-centres on both axes.
  That is fatal here: the frames differ only in the sun's height, so their content boxes differ, and
  re-centring each one independently makes the horizon rule *jump* between steps. Design 5 is authored in a
  fixed frame and skips the refit.
- **Registration is gated in `tools\measure-icons.ps1`.** Because the sun is *supposed* to sit off-centre and
  the ink height changes every frame, the centring and fill gates do not describe it. Instead the gate
  locates the horizon rule in each frame (it is the widest run of ink by a wide margin), requires them to
  agree within 1 px, and forbids clipping at any frame edge.

Geometry that satisfies both: rule at `y=12`, disc `r=2.8` travelling `4.6 → 15.4`. That leaves 1.8 viewBox
units of margin at each extreme — 1.44 px at 16 px, which is what stops the sun clipping at the small end.
The travel is bounded by that margin: `2·r + 2·margin ≤ 20`, so a 2.16-unit step is about as fine as the
16 px frame allows.

## Regenerating

`build-icons.ps1` rasterizes each SVG via headless Edge (magenta `#FF00FF` backdrop; alpha is recovered as the green channel, which keeps edges fringe-free), derives an alpha mask, and recolors it into every variant:

- **Theme variant** (`ondark` = light glyph, `onlight` = dark glyph), per Microsoft's app-icon guidance to ship separate light/dark assets for theme-sensitive areas.
- **State tint**: `on` (white / `#202020`), `off` (gray `#969CA5`), `dim` (blue `#70B6FF` / `#1F7AE0`). Descent steps `d0`…`d5` take the `on` tint; only `off` and `dim` have their own colour.

A design carries either one SVG shared across the three default states, or an explicit `state → svg` map
(design 5), because for the sunset the artwork *is* the state.

Output: 40 `tray<design>-<variant>-<state>.ico` files with 16/20/24/28/32/36/40/48 px PNG-compressed entries,
plus two sheets — `verify.png` (every design × variant × state at 48 px) and `verify-tray.png` (the sunset
descent at true tray size, each row on the taskbar colour its tint actually sits against).

One frame per `SM_CXSMICON` value Windows can report from 100% to 300% scaling, so `LoadImageW` always finds an exact match instead of resampling.

Run standalone:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native\icons\build-icons.ps1
```

or just `native\build.ps1`, which regenerates automatically when any SVG is newer than the ICOs (requires Microsoft Edge; a failure keeps the existing ICOs and warns).

## Measuring

Appearance regressions in this pipeline are invisible in `verify.png` — it shows the art at 48 px on flat backgrounds, not what the tray actually draws. Measure instead:

```powershell
# Ink geometry of every generated frame. -Check gates the build.
powershell -NoProfile -ExecutionPolicy Bypass -File native\tools\measure-icons.ps1 -Check

# The rendered result: screenshots the notification area and reports each glyph's
# ink box, so the app's glyph can be compared against the shell glyphs beside it.
powershell -NoProfile -ExecutionPolicy Bypass -File native\tools\measure-tray.ps1
```

`measure-icons.ps1 -Check` fails when any frame is off-centre (`|padT-padB| > 1`) or clipped
(longest side below 85% of the canvas). Ink width is reported but **not** gated: designs 1 and 4
are genuinely narrow because their artwork is, and widening them is an artwork change.

Design 5 is gated differently, because the gates above do not describe an animated glyph. Its sun is
*meant* to be off-centre and its ink height legitimately changes every frame, so instead the check
verifies that the horizon rule lands on the same row in all six descent frames (within 1 px) and that
no frame clips at the top or bottom edge. That is the defect `Refit-Mask` would reintroduce.

At 200% scaling the expected rendered result for the app's glyph is ~28 px tall with its vertical
centre matching its neighbours. A build that sits several px low means a centring regression.

## Contract with the app

- The `.ico` files are the only runtime artifact; SVG is a build-time input only (no in-process SVG renderer — that would fight the size/efficiency goal).
- Design ids must stay in sync between `build-icons.ps1` (`$designs`), the file naming (`tray1-…` … `tray5-…`), `kIconDesignCount`/`kIconNames[]` in `IdleDimmer.cpp`, and this table. A design whose `$states` map is used must also have its `kDescentNames[]` entries match.
- The app picks the theme variant from the documented `HKCU\...\Themes\Personalize\SystemUsesLightTheme` value and reloads on `WM_SETTINGCHANGE` ("ImmersiveColorSet") / `WM_THEMECHANGED`. If the ICOs are missing it falls back to procedural 32 px glyphs.
- To add a static design: drop the SVG here, add it to `$designs`, extend `kIconNames[]` (and the clamp in `ClampIconDesign`), rebuild. Adding an *animated* one additionally needs `kDescentDesign`, `kDescentSteps`, `kDescentNames[]` and `kDescentPreview` in `IdleDimmer.cpp`, plus the registration gate if it is a `Raw` design.
