# Glyph stroke-weight measurement — 2026-11-02

Status: MEASURED 2026-11-02 from the three user-supplied PNGs; app code unchanged. This measures rendered screenshot coverage, not Windows' private vector definitions or an exact font identity.

## Inputs

1. Windows full panel: `codex-clipboard-6e991b7d-e9f8-4bb1-88b0-079ad2b3c2d4.png`, 673 x 668 pixels.
2. Idle Dimmer tile crop: `codex-clipboard-20a39161-cb7a-4a67-909b-6b612b0bd967.png`, 658 x 339 pixels.
3. Windows tile crop: `codex-clipboard-03ba4020-e1d3-46dd-bf48-a21725ce8237.png`, 647 x 386 pixels.

Dimensions and results below: MEASURED 2026-11-02. Coordinates are zero-based in the original PNGs, without resizing.

## Method

Extract RGB pixels losslessly. Across an isolated straight stroke, estimate fractional foreground coverage as

`alpha = clamp(dot(pixel - background, foreground - background) / dot(foreground - background, foreground - background), 0, 1)`.

Sum alpha across the stroke, then average several parallel cross-sections away from corners and intersections. This gives effective stroke width in screenshot pixels, including antialiased edge contributions. Simply counting non-background pixels would overestimate width and depend on alignment.

Both black-on-accent samples have the same flat background RGB (221,172,173), with black foreground. Windows Project uses white foreground and estimated local background RGB (90,45,51); Windows speaker uses white and (80,32,38). Acrylic noise makes white-stroke measurements approximate. Report to two decimals, not the extra calculation digits. This assumes ordinary RGB coverage blending; it is not a metrological calibration of either renderer.

## Results

All values: MEASURED 2026-11-02, supplied screenshots, straight sections only.

| Sample | Effective width, screenshot px | Measurement coordinates |
|---|---:|---|
| App pause, left bar | 2.63 | Image 2: x345..350, y101..115 |
| App pause, right bar | 2.63 | Image 2: x356..361, y101..115 |
| App speaker, upright | 2.63 | Image 2: x548..553, y106..112 |
| Windows Bluetooth, upright | 1.31 | Image 3: x297..300, y94..98 |
| Windows Project, left upright | 1.75 | Image 3: x313..317, y269..273 |
| Windows Project, right upright | 1.74 | Image 3: x329..333, y268..272 |
| Windows Project, upper horizontal | 1.75 | Image 3: x322..327, y264..268 |
| Windows speaker, upright | 1.75 | Image 1: x92..96, y503..507 |

The app speaker upright is approximately 50% thicker than the Windows speaker upright. The app pause upright is approximately twice as thick as the Windows Bluetooth upright. These comparisons describe these sampled strokes; different icon artwork/control sizes mean they are not one universal native-to-app ratio.

## Source explanation and limits

Source inspected 2026-11-02: `icons/panel/media.svg` and `sound.svg` specify stroke-width="1.5" in a 20-unit viewBox. `Flyout.cpp` draws tile artwork at 20 logical pixels. `tools/build-panel-icons.ps1` rasterizes to an 80-pixel alpha mask, and `BlitPanelIcon` area-downsamples it. Those source constants are design settings, not screenshot measurements.

The measured app width agrees with the expected 2.625 physical pixels at 175% scale (1.5 x 1.75). The pause-bar centre spacing also agrees with that scale. These are inferred scaling checks, not a separate OS DPI query. Native sampled widths correspond approximately to 1.0 and 0.75 logical pixels at the same assumed scale. Do not claim all Windows glyphs use either width or that this identifies the native font/API.

The screenshots and source agree: current app artwork is heavier. Antialiasing is present and preserves the specified weight; these samples do not show the old binary-pixel GDI defect. Identical black-on-accent colors in the app/Bluetooth samples also rule out color contrast as the entire explanation.

A candidate visual adjustment is reducing ordinary app strokes from 1.5 to about 1.0 logical units while preserving icon size and antialiasing. That would be a proposed starting point requiring live user review, not a verified fix. Exact native matching needs per-glyph artwork/weight decisions, rather than assuming one global Windows stroke width.

Calculation script, full JSON results and inspected 8x nearest-neighbour crops are retained locally in the ignored `build-tools/glyph-weight-2026-11-02/` directory. No app change or deployment was made for this analysis.

## Implementation follow-up — 2026-11-02

UNVERIFIED visual result, awaiting live user audit: following the user's request to get closer
to native, the six tile glyphs now specify stroke-width="1.0", including both Dimming variants.
SVG paths, icon dimensions/placement, colors, alpha filtering, and runtime lifecycle are
preserved. Footer, slider, and tray glyph weights are unchanged. PNG masks and the embedded
header were regenerated through the existing Edge rasterization pipeline.

MEASURED 2026-11-02, regenerated Media mask: an isolated straight cross-section integrates to
4.0 source pixels, compared with 6.0 before, in the existing 80-pixel source mask. The expected
width at the supplied screenshot's inferred scale is 1.75 physical pixels, compared with
2.625 before. This verifies asset weight only; the new running panel has not yet been measured
in a user screenshot or approved visually.

MEASURED 2026-11-02, tile-weight build: 284,160-byte executable, build/doc/icon gates PASS,
self-test PASS (2,128 checks). The build was launched for user review. Reproducible before/
after alpha-area calculations are in the local ignored `asset-weight-after.json` and
`measure-assets.py` alongside the baseline calculations.

UNVERIFIED clock follow-up, 2026-11-02: the user requested the same lighter weight for the
slider clock, which was excluded from the initial tile-only change. `clock.svg` now uses
stroke-width="1.0" with its existing paths and logical size. The alpha PNG and embedded header
were regenerated through the existing build pipeline. MEASURED 2026-11-02, clock follow-up
build: 284,160 bytes; build gates PASS; self-test PASS (2,128 checks). Awaiting live user review.
