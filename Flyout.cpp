// Pure layout geometry for the flyout panel. See Flyout.h for why this is separated out
// and free of window state.
#include "Flyout.h"
#include "AppVersion.h"
#include <windows.h>

namespace {

// The magnetic preset points. The slider selects every minute 1..60 and paints no dots; these
// seven values exist only so SliderMagnet can make the useful landmarks (1/2/5/10/15/30/60)
// feel slightly sticky on a drag. kIdlePresets in IdleDimmer.cpp is 1/2/5/10/15/30; the panel
// adds 60 because the threshold clamp allows it and a slider that cannot reach the maximum of
// its own range is a trap.
const int kDetents[kSliderDetentCount] = { 1, 2, 5, 10, 15, 30, 60 };

// Scales a logical metric to physical pixels. Guarded against a 0 DPI, which GetDpiForWindow
// can legitimately return for a window that has not been shown yet; a zero here would
// collapse every rect to zero width and silently draw nothing.
inline int S(int logical, int dpi) {
    return MulDiv(logical, dpi > 0 ? dpi : 96, 96);
}

// Minute -> pixel offset on a track, in one rounding step over 59 equal intervals. The
// painter and SliderThumbX both go through this. Deriving the pixel from the per-mille value
// instead added a second rounding, which made some adjacent minutes 3 px apart and others
// 5 px at 96 DPI - exactly the asymmetry this control is supposed to not have.
inline int MinutesToTrackPx(int trackWidthPx, int minutes) {
    if (minutes < 1) minutes = 1;
    if (minutes > 60) minutes = 60;
    const int intervals = 59;
    return (int)(((long long)trackWidthPx * (minutes - 1) + intervals / 2) / intervals);
}

}  // namespace

int DetentCount() {
    return (int)(sizeof(kDetents) / sizeof(kDetents[0]));
}

int DetentAt(int index) {
    if (index < 0 || index >= DetentCount()) return kDetents[0];
    return kDetents[index];
}

int DividerY(int buttonCount) {
    const int extra = (buttonCount >= 5) ? kRowPitch : 0;
    return kDividerY + extra;
}
int SliderCenterY(int buttonCount) {
    const int extra = (buttonCount >= 5) ? kRowPitch : 0;
    return kSliderY + extra;
}
int TickY(int buttonCount) {
    const int extra = (buttonCount >= 5) ? kRowPitch : 0;
    return kTickY + extra;
}
int TicksDividerY(int buttonCount) {
    const int extra = (buttonCount >= 5) ? kRowPitch : 0;
    return kTicksSepY + extra;
}
int FooterDividerY(int buttonCount) {
    const int extra = (buttonCount >= 5) ? kRowPitch : 0;
    return kFooterDividerY + extra;
}
int FooterY(int buttonCount) {
    const int extra = (buttonCount >= 5) ? kRowPitch : 0;
    return kFooterY + extra;
}
int PanelHeight(int buttonCount) {
    return FooterY(buttonCount) + kFooterHeight;
}

Rect TileRect(int index, int dpi, int totalButtons) {
    if (totalButtons <= 0) totalButtons = 4;
    if (totalButtons > 6) totalButtons = 6;
    if (index < 0 || index >= totalButtons) return Rect{ 0, 0, 0, 0 };
    if (totalButtons <= 4) {
        const int cols = 4;
        const int tileW = (kPanelWidth - (kPanelPadX * 2) - (kTileGap * (cols - 1))) / cols;
        const int col = index % cols;
        const int row = index / cols;
        const int x = kPanelPadX + col * (tileW + kTileGap);
        const int y = kGridTop + row * kRowPitch;
        return Rect{ S(x, dpi), S(y, dpi), S(tileW, dpi), S(kTileHeight, dpi) };
    }
    // Dynamic reflow for 5 or more buttons: 2 balanced rows so no orphan button is left
    const int row0Count = (totalButtons + 1) / 2;
    const int row1Count = totalButtons - row0Count;
    const int maxCols = row0Count;
    const int tileW = (kPanelWidth - (kPanelPadX * 2) - (kTileGap * (maxCols - 1))) / maxCols;
    const int availW = kPanelWidth - (kPanelPadX * 2);
    if (index < row0Count) {
        const int col = index;
        const int rowW = row0Count * tileW + (row0Count - 1) * kTileGap;
        const int startX = kPanelPadX + (availW - rowW) / 2;
        const int x = startX + col * (tileW + kTileGap);
        const int y = kGridTop;
        return Rect{ S(x, dpi), S(y, dpi), S(tileW, dpi), S(kTileHeight, dpi) };
    } else {
        const int col = index - row0Count;
        const int rowW = row1Count * tileW + (row1Count - 1) * kTileGap;
        const int startX = kPanelPadX + (availW - rowW) / 2;
        const int x = startX + col * (tileW + kTileGap);
        const int y = kGridTop + kRowPitch;
        return Rect{ S(x, dpi), S(y, dpi), S(tileW, dpi), S(kTileHeight, dpi) };
    }
}

Rect PowerCardRect(int dpi, int buttonCount) {
    return Rect{ S(kPanelPadX, dpi), S(TicksDividerY(buttonCount) + kSepH + kCardMarginTop, dpi),
                 S(kPanelWidth - kPanelPadX * 2, dpi), S(kCardH, dpi) };
}

Rect SliderTrackRect(int dpi, int buttonCount) {
    // Mirrors .slider-row / .track-wrap: 16 px side padding, an 18 px leading clock plus the
    // row's 14 px gap, and the trailing ".stime" label (40 px minimum) plus its 14 px gap.
    const int lead = 18 + 14;
    const int trail = 40 + 14;
    const int x = kPanelPadX + lead;
    const int w = kPanelWidth - kPanelPadX * 2 - lead - trail;
    const int y = SliderCenterY(buttonCount);
    return Rect{ S(x, dpi), S(y, dpi), S(w, dpi), S(1, dpi) };
}

int PermilleFromMinutes(int minutes) {
    if (minutes < 1) minutes = 1;
    if (minutes > 60) minutes = 60;
    // Linear, not logarithmic: 59 equal intervals, so 1->2 is exactly the same width as
    // 30->31 and 59->60. The previous log scale compressed the top of the range until a
    // single minute was a pixel or two, which made the slider feel asymmetric.
    //
    // Per-mille rather than percent: 1000 steps leave every one of the 59 minute widths
    // distinct after rounding (about 16.95 per-mille per minute).
    const int intervals = 59;
    return ((minutes - 1) * kSliderPermilleMax + intervals / 2) / intervals;
}

int MinutesFromPermille(int permille) {
    if (permille < 0) permille = 0;
    if (permille > kSliderPermilleMax) permille = kSliderPermilleMax;
    // Nearest whole minute on the same 59 equal intervals. Integer rounding, no scan and no
    // series: the painter and the drag handler use one linear mapping.
    const int intervals = 59;
    int minute = 1 + (permille * intervals + kSliderPermilleMax / 2) / kSliderPermilleMax;
    if (minute < 1) minute = 1;
    if (minute > 60) minute = 60;
    return minute;
}

int SliderReleasePermille(int trackWidthPx) {
    // One physical pixel expressed in per-mille. At 200% this is ~2 per-mille; at 100% ~4.
    // A zero-width track cannot be dragged, so make the release immediate rather than divide.
    if (trackWidthPx <= 0) return 1;
    const int p = kSliderPermilleMax / trackWidthPx;
    return p < 1 ? 1 : p;
}

int ProgressFillWidth(int widthPx, unsigned long long part, unsigned long long whole) {
    // The card's bar used to be a bare track because "Display hours saved" had no denominator.
    // It has one now: this process's uptime. The numerator is deliberately the session-only
    // dimmed time, not the persisted lifetime total - mixing a lifetime counter with a
    // per-process denominator would allow a bar past 100% after a restart.
    if (widthPx <= 0 || whole == 0 || part == 0) return 0;
    if (part >= whole) return widthPx;
    return (int)((part * (unsigned long long)widthPx + whole / 2) / whole);
}

int SliderMagnetBegin(SliderMagnet* state, int permille) {
    if (permille < 0) permille = 0;
    if (permille > kSliderPermilleMax) permille = kSliderPermilleMax;
    // A click is not a drag: it selects the exact minute under the pointer and never snaps.
    // That is what keeps 31, 39, 59 and every other value directly clickable even when it
    // sits close to a preset.
    state->preset = 0;
    state->lastPermille = permille;
    state->apexPermille = permille;
    return MinutesFromPermille(permille);
}

int SliderMagnetUpdate(SliderMagnet* state, int permille, int releasePermille) {
    if (permille < 0) permille = 0;
    if (permille > kSliderPermilleMax) permille = kSliderPermilleMax;
    if (releasePermille < 1) releasePermille = 1;

    const int last = state->lastPermille;
    const int delta = permille - last;

    if (state->preset != 0) {
        const int presetP = PermilleFromMinutes(state->preset);
        // Moving toward the preset keeps the hold and tightens the release apex. Moving
        // away releases after about one pixel - the range is measured from the closest
        // point reached, not from the preset, so a wide pull-in zone does not become a
        // wide sticky zone.
        const bool toward = (last < presetP && permille > last) ||
                            (last > presetP && permille < last);
        if (toward) {
            if (last < presetP)
                state->apexPermille = permille < presetP ? permille : presetP;
            else if (last > presetP)
                state->apexPermille = permille > presetP ? permille : presetP;
        } else {
            const int away = permille > state->apexPermille
                           ? permille - state->apexPermille
                           : state->apexPermille - permille;
            if (away >= releasePermille) state->preset = 0;
        }
    }

    if (state->preset == 0 && delta != 0) {
        int best = 0;
        int bestDist = 0;
        for (int i = 0; i < DetentCount(); i++) {
            const int m = DetentAt(i);
            const int presetP = PermilleFromMinutes(m);
            // Enter only while moving toward the preset and still on its near side. A fast
            // drag that jumps past the preset must not be captured by it.
            const bool toward = (last < presetP && permille > last && permille <= presetP) ||
                                (last > presetP && permille < last && permille >= presetP);
            if (!toward) continue;
            const int dist = presetP > permille ? presetP - permille : permille - presetP;
            if (dist <= kSliderSnapEnterPermille && (best == 0 || dist < bestDist)) {
                best = m;
                bestDist = dist;
            }
        }
        if (best != 0) {
            state->preset = best;
            state->apexPermille = permille;
        }
    }

    state->lastPermille = permille;
    return state->preset != 0 ? state->preset : MinutesFromPermille(permille);
}

Rect ReserveTaskbarSpace(Rect work, Rect taskbar, unsigned int edge) {
    const int right = work.x + work.w, bottom = work.y + work.h;
    if (taskbar.w <= 0 || taskbar.h <= 0 || taskbar.x >= right || taskbar.y >= bottom ||
        taskbar.x + taskbar.w <= work.x || taskbar.y + taskbar.h <= work.y) return work;
    Rect adjusted = work;
    switch (edge) {
    case 0: adjusted.x = taskbar.x + taskbar.w; adjusted.w = right - adjusted.x; break;
    case 1: adjusted.y = taskbar.y + taskbar.h; adjusted.h = bottom - adjusted.y; break;
    case 2: adjusted.w = taskbar.x - work.x; break;
    case 3: adjusted.h = taskbar.y - work.y; break;
    }
    return adjusted.w > 0 && adjusted.h > 0 ? adjusted : work;
}

int PanelEntranceOffset(unsigned long long elapsedMs, int dpi) {
    if (elapsedMs >= kPanelEntranceMs) return 0;
    const double remaining = 1.0 - (double)elapsedMs / kPanelEntranceMs;
    return (int)(S(kPanelEntranceTravel, dpi) * remaining * remaining * remaining * remaining * remaining + 0.5);
}

int PanelExitOffset(unsigned long long elapsedMs, int fromOffset, int dpi) {
    const int travel = S(kPanelEntranceTravel, dpi);
    const int end = fromOffset + travel;
    if (elapsedMs >= kPanelExitMs) return end;
    const double progress = (double)elapsedMs / kPanelExitMs;
    return fromOffset + (int)(travel * progress * progress * progress * progress * progress + 0.5);
}

Rect AnchorPanel(int dpi, int workX, int workY, int workW, int workH,
                 int buttonCount) {
    const int w = S(kPanelWidth, dpi);
    const int h = S(PanelHeight(buttonCount), dpi);
    const int gap = S(kPanelEdgeGap, dpi);
    int x = workX + workW - w - gap;
    int y = workY + workH - h - gap;

    // Clamp each axis independently. Doing them independently (rather than as a combined
    // bounds test) means a work area narrower than the panel still pins to the left/top
    // instead of producing a rect with negative extent.
    if (x + w > workX + workW) x = workX + workW - w;
    if (y + h > workY + workH) y = workY + workH - h;
    if (x < workX) x = workX;
    if (y < workY) y = workY;

    return Rect{ x, y, w, h };
}// Painting for the flyout panel.
//
// Every primitive here takes a plain HDC so that tools\render-panel.cpp can drive the exact
// same code with no window. That is the whole point of the split: a contact sheet rendered
// by a copy of this routine would drift from what ships, and there is no interactive preview
// in this environment to catch the drift.
//
// GDI only. Alpha is composited by hand into a 32-bpp destination rather than with
// AlphaBlend, because that would pull in msimg32 and the project forbids new dependencies.
#include "Flyout.h"
#include "SessionStats.h"
#include "PanelIconsData.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

namespace {

inline int Sc(int logical, int dpi) {
    return MulDiv(logical, dpi > 0 ? dpi : 96, 96);
}

// ---------------------------------------------------------------- palette
// Values match docs\tray-ui-proposal.html exactly. Every color carries an alpha, and the
// panel paints its own opaque base beneath the translucent layers, so it stays legible when
// the DWM backdrop silently degrades (Transparency effects off, or an OS without attr 38).
struct Palette {
    Color base,      tileOff,  tileHover, tileBorder, divider, text, textDim,
           thumb,    track,     tick,      pillBg,    pillText, onAccent, dock,
           cardFill, cardEdge, acrylicBodyWash, footerDivider;
};

Palette MakePalette(bool dark, Color accent) {
    const int lum = (accent.r * 299 + accent.g * 587 + accent.b * 114) / 1000;
    const Color onAcc = (lum > 140) ? RGBc(0x00, 0x00, 0x00) : RGBc(0xFF, 0xFF, 0xFF);
    if (dark) {
        return Palette{
            RGBc(0x2C, 0x2C, 0x2C),   // base
            RGBc(0xFF, 0xFF, 0xFF, 15),  // ControlFillColorDefault #0FFFFFFF
            RGBc(0xFF, 0xFF, 0xFF, 21),  // tileHover ~8%
            RGBc(0xFF, 0xFF, 0xFF, 18),  // tileBorder
            RGBc(0xFF, 0xFF, 0xFF, 21),  // divider   ~8%
            RGBc(0xFF, 0xFF, 0xFF),      // text
            RGBc(0xFF, 0xFF, 0xFF, 200), // textDim
            accent,                      // thumb
            RGBc(0xFF, 0xFF, 0xFF, 71),  // track     ~28%
            RGBc(0xFF, 0xFF, 0xFF, 128), // tick
            Color{ 41, accent.r, accent.g, accent.b },  // pillBg
            accent,                      // pillText
            onAcc,                       // onAccent
            // The bottom ribbon. #222222 is what Windows' own Quick Settings ribbon reports on a
            // dark theme (reported by the user 2026-11-01, not derived from the base colour): the
            // dock is a darker plate than the #2C2C2C panel, not a translucent tint over it.
            RGBc(0x22, 0x22, 0x22),       // dock
            // Power dashboard card. Fluent's CardBackgroundFillColorDefault / CardStrokeColorDefault,
            // not an accent wash: the card is a neutral white film over whatever is behind it, which
            // is what makes it read as "lighter than the panel" over acrylic.
            RGBc(0xFF, 0xFF, 0xFF, 13),   // cardFill  #0DFFFFFF (white 5%)
            RGBc(0x00, 0x00, 0x00, 25),   // cardEdge  #19000000 (black 10%)
            // Fitted to the user's Windows footer crop (2026-11-01); live match UNVERIFIED.
            RGBc(0xFF, 0xFF, 0xFF, 9),    // acrylicBodyWash: lift the main surface, keep footer bare
            RGBc(0x00, 0x00, 0x00, 26),   // footerDivider: dark seam, separate from other rules
        };
    }
    return Palette{
        RGBc(0xF3, 0xF3, 0xF3),      // base
        RGBc(0xFF, 0xFF, 0xFF, 179), // ControlFillColorDefault #B3FFFFFF
        RGBc(0xF9, 0xF9, 0xF9, 128), // ControlFillColorSecondary #80F9F9F9
        RGBc(0x00, 0x00, 0x00, 15),  // tileBorder
        RGBc(0x00, 0x00, 0x00, 21),  // divider   ~8%
        RGBc(0x00, 0x00, 0x00),      // text
        RGBc(0x00, 0x00, 0x00, 154), // textDim
        accent,                      // thumb
        RGBc(0x00, 0x00, 0x00, 57),  // track     ~22%
        RGBc(0x00, 0x00, 0x00, 128), // tick
        Color{ 26, accent.r, accent.g, accent.b },  // pillBg
        accent,                      // pillText
        onAcc,                       // onAccent
        // Light-theme ribbon. The dark value is the one Windows reports (#222222); this lighter
        // plate comes from the design sheet's light acrylic token (rgba(249,249,249)), which is the
        // closest thing to a measured source available here. UNVERIFIED on a light taskbar.
        RGBc(0xF9, 0xF9, 0xF9),      // dock
        // Light theme uses the opposite film: a near-opaque white card with a faint dark edge.
        RGBc(0xFF, 0xFF, 0xFF, 179), // cardFill  #B3FFFFFF (white 70%)
        RGBc(0x00, 0x00, 0x00, 15),  // cardEdge  #0F000000 (black 6%)
        RGBc(0x00, 0x00, 0x00, 0),   // acrylicBodyWash: no light-theme reference measured
        RGBc(0x00, 0x00, 0x00, 21),  // footerDivider: existing light-theme rule
    };
}

// ---------------------------------------------------------------- primitives
// Each primitive blends src over dst using src's alpha, so translucent layers compose
// correctly without a separate blend pass.

void BlendPixel(BYTE* d, Color s) {
    // 32-bpp DIB memory order is BGRA (d[0]=Blue, d[1]=Green, d[2]=Red, d[3]=Alpha).
    if (s.a == 255) { d[0] = s.b; d[1] = s.g; d[2] = s.r; d[3] = 255; return; }
    if (s.a == 0) return;
    const int a = s.a;
    const int ia = 255 - a;
    d[0] = (BYTE)((s.b * a + d[0] * ia) / 255);
    d[1] = (BYTE)((s.g * a + d[1] * ia) / 255);
    d[2] = (BYTE)((s.r * a + d[2] * ia) / 255);
    d[3] = 255;
}

// Fills an axis-aligned rect of the destination with a color, blended.
void FillBlend(HDC dc, BYTE* base, int w, int h, int x, int y, int rw, int rh, Color c) {
    if (rw <= 0 || rh <= 0) return;
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + rw > w) rw = w - x;
    if (y + rh > h) rh = h - y;
    if (rw <= 0 || rh <= 0) return;
    for (int yy = y; yy < y + rh; yy++)
        for (int xx = x; xx < x + rw; xx++)
            BlendPixel(base + ((size_t)yy * w + xx) * 4, c);
    (void)dc;
}

// Rounded-rect fill. For radius <= 1 this is just a rect, which is the common case for the
// accent fills and keeps the inner loop cheap.
void FillRound(HDC dc, BYTE* base, int w, int h, Rect r, int radius, Color c) {
    if (r.w <= 0 || r.h <= 0) return;
    if (radius > r.w / 2) radius = r.w / 2;
    if (radius > r.h / 2) radius = r.h / 2;
    if (radius <= 1) { FillBlend(dc, base, w, h, r.x, r.y, r.w, r.h, c); return; }
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        for (int xx = r.x; xx < r.x + r.w; xx++) {
            if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
            // Inside test against the rounded corners only.
            int cx = 0, cy = 0;
            if (xx < r.x + radius) cx = r.x + radius;
            else if (xx >= r.x + r.w - radius) cx = r.x + r.w - radius - 1;
            if (yy < r.y + radius) cy = r.y + radius;
            else if (yy >= r.y + r.h - radius) cy = r.y + r.h - radius - 1;
            if (cx == 0 || cy == 0) { BlendPixel(base + ((size_t)yy * w + xx) * 4, c); continue; }
            const int dx = xx - cx, dy = yy - cy;
            if (dx * dx + dy * dy <= radius * radius)
                BlendPixel(base + ((size_t)yy * w + xx) * 4, c);
        }
    }
}

// Rounded-rect stroke of 1px, used for tile borders and the selection outline.
void StrokeRound(HDC dc, BYTE* base, int w, int h, Rect r, int radius, Color c) {
    if (r.w <= 0 || r.h <= 0) return;
    if (radius > r.w / 2) radius = r.w / 2;
    if (radius > r.h / 2) radius = r.h / 2;
    if (radius <= 1) {
        FillBlend(dc, base, w, h, r.x, r.y, r.w, 1, c);
        FillBlend(dc, base, w, h, r.x, r.y + r.h - 1, r.w, 1, c);
        FillBlend(dc, base, w, h, r.x, r.y + 1, 1, r.h - 2, c);
        FillBlend(dc, base, w, h, r.x + r.w - 1, r.y + 1, 1, r.h - 2, c);
        return;
    }
    const int inRad = radius > 1 ? radius - 1 : 0;
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        for (int xx = r.x; xx < r.x + r.w; xx++) {
            if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
            // 1. Must be inside outer rounded rect
            int cx = 0, cy = 0;
            if (xx < r.x + radius) cx = r.x + radius;
            else if (xx >= r.x + r.w - radius) cx = r.x + r.w - radius - 1;
            if (yy < r.y + radius) cy = r.y + radius;
            else if (yy >= r.y + r.h - radius) cy = r.y + r.h - radius - 1;
            if (cx != 0 && cy != 0) {
                const int dx = xx - cx, dy = yy - cy;
                if (dx * dx + dy * dy > radius * radius) continue;
            }

            // 2. Must not be inside inner rounded rect (inset by 1)
            if (xx >= r.x + 1 && xx < r.x + r.w - 1 && yy >= r.y + 1 && yy < r.y + r.h - 1) {
                int icx = 0, icy = 0;
                if (inRad > 0) {
                    if (xx < r.x + 1 + inRad) icx = r.x + 1 + inRad;
                    else if (xx >= r.x + r.w - 1 - inRad) icx = r.x + r.w - 1 - inRad - 1;
                    if (yy < r.y + 1 + inRad) icy = r.y + 1 + inRad;
                    else if (yy >= r.y + r.h - 1 - inRad) icy = r.y + r.h - 1 - inRad - 1;
                }
                if (icx != 0 && icy != 0) {
                    const int idx = xx - icx, idy = yy - icy;
                    if (idx * idx + idy * idy <= inRad * inRad) continue;
                } else {
                    continue;
                }
            }

            BlendPixel(base + ((size_t)yy * w + xx) * 4, c);
        }
    }
    (void)dc;
}

// ---------------------------------------------------------------- text
// Draws a label. GDI cannot alpha-blend text into a DIB, so the caller passes the FINAL
// colour to draw (already mixed against the background it sits on) rather than an alpha.
//
// An earlier version tried to draw at full colour and then post-blend toward the background
// by measuring which pixels had changed. That is unsound: ClearType antialiasing spreads
// coverage well outside the glyph core, so the "changed" test caught background pixels too
// and washed the labels out to near-invisible on the dark theme. Mixing the colour up front
// needs no coverage estimate and cannot misfire.
void DrawTextA(HDC dc, BYTE* base, int w, int h, const wchar_t* text, Rect box,
               Color color, Color blendBg, int pxWeight, int fontPx, UINT align) {
    HFONT font = CreateFontW(-fontPx, 0, 0, 0, pxWeight, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    if (!font) {
        font = CreateFontW(-fontPx, 0, 0, 0, pxWeight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    if (!font) return;
    // ANTIALIASED_QUALITY, not CLEARTYPE_QUALITY: ClearType assumes a light subpixel
    // backdrop and boosts contrast against it, so over the dark panel's near-black base the
    // labels rendered at a measured max luminance of 44/255 - effectively invisible. Grayscale
    // AA is correct on both themes. Measured on verify-panel.png, not guessed.
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(color.r, color.g, color.b));

    RECT rc = { box.x, box.y, box.x + box.w, box.y + box.h };
    DrawTextW(dc, text, -1, &rc, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);

    SelectObject(dc, oldFont);
    DeleteObject(font);
    (void)base; (void)w; (void)h; (void)blendBg; (void)align;
}

// Mixes a translucent colour against a known opaque background. Used for text that sits on
// the panel base rather than on a tile.
Color Mix(Color fg, Color bg) {
    const int a = fg.a;
    return RGBc((BYTE)((fg.r * a + bg.r * (255 - a)) / 255),
                (BYTE)((fg.g * a + bg.g * (255 - a)) / 255),
                (BYTE)((fg.b * a + bg.b * (255 - a)) / 255));
}

// A filled circle, for the thumb and the status pill's LED.
// A 4x4 coverage mask per output pixel, box-downsampled before alpha compositing.
// Used for both slider capsules and discs: integer binary circle edges are visibly jagged.
void FillSliderCapsule(BYTE* base, int w, int h, Rect r, Color c) {
    if (r.w <= 0 || r.h <= 0) return;
    const int radius = (r.w < r.h ? r.w : r.h) * 4;
    const int farX = r.w * 8 - radius, farY = r.h * 8 - radius;
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        if (yy < 0 || yy >= h) continue;
        for (int xx = r.x; xx < r.x + r.w; xx++) {
            if (xx < 0 || xx >= w) continue;
            int coverage = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    const int px = (xx - r.x) * 8 + sx * 2 + 1;
                    const int py = (yy - r.y) * 8 + sy * 2 + 1;
                    const int dx = px < radius ? radius - px : (px > farX ? px - farX : 0);
                    const int dy = py < radius ? radius - py : (py > farY ? py - farY : 0);
                    if (dx * dx + dy * dy <= radius * radius) coverage++;
                }
            }
            if (coverage) {
                Color sample = c;
                sample.a = (BYTE)((c.a * coverage + 8) / 16);
                BlendPixel(base + ((size_t)yy * w + xx) * 4, sample);
            }
        }
    }
}

// ---------------------------------------------------------------- glyphs
// The design's icons are SVG strokes; this painter has no SVG renderer and GDI's integer
// lines have no edge coverage. The build pipeline therefore rasterizes every design glyph
// once at 4x (native\tools\build-panel-icons.ps1 -> PanelIconsData.h), and this routine
// area-filters that alpha mask down to the exact DPI. Tinting happens here, so the same
// mask serves the dark theme, the light theme and the on-accent state.
//
// The previous implementation walked integer endpoints with StrokeLine/StrokeCircle and
// wrote whole pixels. It reproduced the path coordinates but not the SVG's rendering:
// measured on verify-panel.png, a 30x30 crop of each tile glyph contained exactly two
// colours, i.e. no antialiasing at all. Do not reintroduce direct integer line drawing
// for icons (AGENTS.md section 10).
enum Glyph { gDim, gDimFilled, gMedia, gFullscreen, gIcon, gPower, gFolder, gSun, gClock, gDimNow, gGear, gExit, gDisplay, gSound };

enum MaskIndex { miSun, miMedia, miFull, miFolder, miPower, miClock, miGear, miExit, miApps, miDimNow, miDimming, miDimmingFilled, miDisplay, miSound, miCount };
static_assert(sizeof(panelicons::kMasks) / sizeof(panelicons::kMasks[0]) == miCount,
              "PanelIconsData.h and MaskIndex are out of sync");

const panelicons::Mask& MaskFor(Glyph g) {
    switch (g) {
    case gDim:       return panelicons::kMasks[miDimming];
    case gDimFilled: return panelicons::kMasks[miDimmingFilled];
    case gSun:       return panelicons::kMasks[miSun];
    case gDimNow:    return panelicons::kMasks[miDimNow];
    case gMedia:     return panelicons::kMasks[miMedia];
    case gFullscreen:return panelicons::kMasks[miFull];
    case gFolder:    return panelicons::kMasks[miFolder];
    case gPower:     return panelicons::kMasks[miPower];
    case gClock:     return panelicons::kMasks[miClock];
    case gGear:      return panelicons::kMasks[miGear];
    case gExit:      return panelicons::kMasks[miExit];
    case gDisplay:   return panelicons::kMasks[miDisplay];
    case gSound:     return panelicons::kMasks[miSound];
    case gIcon:      return panelicons::kMasks[miApps];
    }
    return panelicons::kMasks[miSun];
}

// Area-weighted downsample of the 4x alpha mask. Every destination pixel integrates the
// source pixels that fall inside its footprint, so fractional DPI scales (125%, 150%)
// stay smooth instead of snapping to integer geometry. The source mask is already
// antialiased by the browser rasterizer; this pass preserves that coverage.
void BlitPanelIcon(BYTE* base, int w, int h, const panelicons::Mask& mask,
                   int cx, int cy, int targetPx, Color c) {
    if (!mask.alpha || mask.width <= 0 || mask.height <= 0 || targetPx <= 0) return;
    const int left = cx - targetPx / 2;
    const int top = cy - targetPx / 2;
    const double sx = (double)mask.width / (double)targetPx;
    const double sy = (double)mask.height / (double)targetPx;

    for (int dy = 0; dy < targetPx; dy++) {
        const int y = top + dy;
        if (y < 0 || y >= h) continue;
        const double y0 = dy * sy;
        const double y1 = y0 + sy;
        int iy0 = (int)floor(y0);
        int iy1 = (int)ceil(y1);
        if (iy0 < 0) iy0 = 0;
        if (iy1 > mask.height) iy1 = mask.height;

        for (int dx = 0; dx < targetPx; dx++) {
            const int x = left + dx;
            if (x < 0 || x >= w) continue;
            const double x0 = dx * sx;
            const double x1 = x0 + sx;
            int ix0 = (int)floor(x0);
            int ix1 = (int)ceil(x1);
            if (ix0 < 0) ix0 = 0;
            if (ix1 > mask.width) ix1 = mask.width;

            double sum = 0.0, area = 0.0;
            for (int yy = iy0; yy < iy1; yy++) {
                const double oy = (y1 < yy + 1.0 ? y1 : yy + 1.0) -
                                  (y0 > yy ? y0 : yy);
                if (oy <= 0.0) continue;
                const unsigned char* row = mask.alpha + (size_t)yy * mask.width;
                for (int xx = ix0; xx < ix1; xx++) {
                    const double ox = (x1 < xx + 1.0 ? x1 : xx + 1.0) -
                                      (x0 > xx ? x0 : xx);
                    if (ox <= 0.0) continue;
                    sum += (double)row[xx] * ox * oy;
                    area += ox * oy;
                }
            }
            if (area <= 0.0) continue;
            const int coverage = (int)(sum / area + 0.5);
            if (coverage <= 0) continue;
            const int alpha = coverage * c.a / 255;
            if (alpha <= 0) continue;
            BlendPixel(base + ((size_t)y * w + x) * 4, Color{ (BYTE)alpha, c.r, c.g, c.b });
        }
    }
}

void DrawGlyph(HDC dc, BYTE* base, int w, int h, Glyph g, int cx, int cy,
               Color c, int dpi, int logicalSize) {
    const int safeDpi = dpi > 0 ? dpi : 96;
    BlitPanelIcon(base, w, h, MaskFor(g), cx, cy, Sc(logicalSize, safeDpi), c);
    (void)dc;
}


// ---------------------------------------------------------------- layout helpers
struct Row {
    int count;
    int tile[6];
    bool tileOn[6];
    const wchar_t* tileCaption[6];
    Glyph tileGlyph[6];
    int target[6];
};

// One table for "which control is tile N", used by both the painter's model and the hit test.
// Two copies of this mapping is exactly the drift that makes a click toggle its neighbour.
static int TileTargetForIndex(int index) {
    switch (index) {
    case 0: return hitTileDimming;
    case 1: return hitTileMedia;
    case 2: return hitTileSound;
    case 3: return hitTileFullscreen;
    case 4: return hitTileOffMode;
    case 5: return hitTileAutostart;
    default: return hitNone;
    }
}

Row BuildRows(const PanelState& s) {
    Row r = {};
    int count = s.buttonCount > 0 ? s.buttonCount : 6;
    if (count > 6) count = 6;
    r.count = count;

    // Prefix order, so a shorter count is just the first N of the same list. The four protections
    // come first because they answer the same question ("when must we not go dark"), then the
    // displays-off policy, then the login toggle.
    r.tileOn[0] = s.enabled;          r.tileCaption[0] = L"Dimming";        r.tileGlyph[0] = s.enabled ? gDimFilled : gDim;
    r.tileOn[1] = s.mediaGuard;       r.tileCaption[1] = L"Media";          r.tileGlyph[1] = gMedia;
    r.tileOn[2] = s.soundGuard;       r.tileCaption[2] = L"Sound";          r.tileGlyph[2] = gSound;
    r.tileOn[3] = s.fullscreenGuard;  r.tileCaption[3] = L"Full screen";    r.tileGlyph[3] = gFullscreen;
    r.tileOn[4] = s.offMode;          r.tileCaption[4] = L"Off at idle";    r.tileGlyph[4] = gDisplay;
    r.tileOn[5] = s.autostart;        r.tileCaption[5] = L"Start at login"; r.tileGlyph[5] = gPower;
    for (int i = 0; i < count; i++) r.target[i] = TileTargetForIndex(i);
    return r;
}

}  // namespace

// Live acrylic frames arrive as an HBITMAP from Acrylic.cpp; the painter needs its size to stretch
// it onto the surface. A failed read returns 0, which makes StretchBlt fail and the painter fall
// back to the flat base colour rather than drawing nothing.
static int BackdropWidth(HBITMAP bmp) { BITMAP bm = {}; return bmp && GetObject(bmp, sizeof(bm), &bm) ? bm.bmWidth : 0; }
static int BackdropHeight(HBITMAP bmp) { BITMAP bm = {}; return bmp && GetObject(bmp, sizeof(bm), &bm) ? bm.bmHeight : 0; }

void PaintPanel(HDC dc, int widthPx, int heightPx, const PanelState& state, int dpi, HBITMAP backdrop) {    if (dpi <= 0) dpi = 96;
    Color acc = RGBc((BYTE)((state.accent>>16)&0xFF),(BYTE)((state.accent>>8)&0xFF),(BYTE)(state.accent&0xFF));
    // The highlight swatch (AccentPalette[0]) is a separate colour, not a per-channel lift: on this
    // machine the ramp steps from #D4BB8B to #ECDCAA, a (+24,+33,+31) difference that a flat lift
    // cannot reproduce. When it is unavailable, or resolves to the same colour as the fill, derive
    // a lift instead so a hovered tile still responds rather than looking dead.
    Color accHover;
    if (state.accentHover && state.accentHover != state.accent) {
        accHover = RGBc((BYTE)((state.accentHover>>16)&0xFF),
                        (BYTE)((state.accentHover>>8)&0xFF),
                        (BYTE)(state.accentHover&0xFF));
    } else {
        accHover = Color{ 255, (BYTE)(acc.r > 239 ? 255 : acc.r + 16),
                               (BYTE)(acc.g > 239 ? 255 : acc.g + 16),
                               (BYTE)(acc.b > 239 ? 255 : acc.b + 16) };
    }
    const Palette p = MakePalette(state.dark, acc);

    // Composite into a 32-bpp DIB we own, because every primitive blends per-pixel and GDI
    // has no alpha for that. The caller's DC is only used at the very end to blit the
    // result, so a DC with no bitmap selected still works.
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = widthPx;
    bi.bmiHeader.biHeight = -heightPx;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP tmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!tmp || !pixels) {
        // No DIB available: fill the base colour through GDI so the panel still draws.
        RECT full = { 0, 0, widthPx, heightPx };
        HBRUSH br = CreateSolidBrush(RGB(p.base.r, p.base.g, p.base.b));
        FillRect(dc, &full, br);
        DeleteObject(br);
        return;
    }
    BYTE* base = (BYTE*)pixels;
    memset(base, 0, (size_t)widthPx * heightPx * 4);

    // GDI draws (all text, and anything else going through the DC) must land in the SAME
    // buffer the manual pixel writes go to. `dc` still has the CALLER's bitmap selected at
    // this point, so drawing into it writes to a buffer that is never blitted. An earlier
    // version did exactly that, and every label came out blank while the shapes - written
    // through `base` directly - rendered perfectly. This is precisely why the headless
    // renderer exists: the defect is obvious the moment you look at the sheet and invisible
    // in code review.
    HDC paint = CreateCompatibleDC(dc);
    if (!paint) { DeleteObject(tmp); return; }
    HGDIOBJ prevBmp = SelectObject(paint, tmp);
    // ---- 1. opaque base. Painted FIRST and fully, so the panel is legible even when the
    //         DWM backdrop is unavailable (attr 38 absent, or Transparency effects off).
    //         When a live acrylic frame is supplied it becomes the base instead: the blur module
    //         has already blended the panel tint into it, so the rest of the painter (tiles,
    //         dividers, text) composes exactly as before and nothing here needs alpha.
    bool paintedBackdrop = false;
    if (backdrop) {
        HDC src = CreateCompatibleDC(paint);
        HGDIOBJ prevSrc = SelectObject(src, backdrop);
        SetStretchBltMode(paint, HALFTONE);
        SetBrushOrgEx(paint, 0, 0, nullptr);
        paintedBackdrop = StretchBlt(paint, 0, 0, widthPx, heightPx, src, 0, 0,
                                     BackdropWidth(backdrop), BackdropHeight(backdrop), SRCCOPY) != FALSE;
        SelectObject(src, prevSrc);
        DeleteDC(src);
    }
    if (!paintedBackdrop)
        FillBlend(paint, base, widthPx, heightPx, 0, 0, widthPx, heightPx, p.base);

    const int padX = Sc(kPanelPadX, dpi);

    // ---- 4. tile grid (dynamic 1-row / 2-row reflow)
    const Row row = BuildRows(state);
    const int totalButtons = row.count;
    if (paintedBackdrop) {
        // Main content is a slightly brighter surface over the same acrylic. Stop before the
        // footer seam so that the ribbon retains the original material and the seam darkens it.
        FillBlend(paint, base, widthPx, heightPx, 0, 0, widthPx,
                  Sc(FooterDividerY(totalButtons), dpi), p.acrylicBodyWash);
    }
    const int captionH = Sc(kCaptionH, dpi);
    for (int i = 0; i < totalButtons; i++) {
        const Rect t = TileRect(i, dpi, totalButtons);
        const bool on = row.tileOn[i];
        const Color accent = p.thumb;
        const Color onAcc = p.onAccent;
        const int tgt = row.target[i];
        const bool hovered = state.hover == tgt;
        const bool held = state.pressed == tgt;
        if (on) {
            // Rest = the accent (AccentPalette[1]); hover = the highlight swatch Windows paints on
            // a hovered tile (AccentPalette[0]). Held adds one further small step so the press is
            // visible - that extra step is a UI choice, not a measured Windows value.
            Color fill = accent;
            if (hovered || held) {
                fill = accHover;
                if (held) fill = Color{ 255, (BYTE)(fill.r > 239 ? 255 : fill.r + 16),
                                             (BYTE)(fill.g > 239 ? 255 : fill.g + 16),
                                             (BYTE)(fill.b > 239 ? 255 : fill.b + 16) };
            }
            FillRound(paint, base, widthPx, heightPx, t, Sc(kTileRadius, dpi), fill);
        } else {
            FillRound(paint, base, widthPx, heightPx, t, Sc(kTileRadius, dpi), held ? p.tileBorder : (hovered ? p.tileHover : p.tileOff));
            StrokeRound(paint, base, widthPx, heightPx, t, Sc(kTileRadius, dpi), p.tileBorder);
        }
        const Color fg = on ? onAcc : p.text;
        DrawGlyph(paint, base, widthPx, heightPx, row.tileGlyph[i],
                  t.x + t.w / 2, t.y + t.h / 2, fg, dpi, 20);
        DrawTextA(paint, base, widthPx, heightPx, row.tileCaption[i],
                  Rect{ t.x - Sc(8, dpi), t.y + t.h + Sc(kCaptionGap, dpi), t.w + Sc(16, dpi), captionH },
                  p.text, p.base, FW_NORMAL, Sc(12, dpi), DT_CENTER);
    }

    // ---- 5. divider. .sep spans the whole flyout in the design, so this does too rather than
    //         being inset by the content padding; content padding is a property of the rows,
    //         not of the rules between them.
    int y = Sc(DividerY(totalButtons), dpi);
    FillBlend(paint, base, widthPx, heightPx, 0, y, widthPx, Sc(kSepH, dpi), p.divider);

    // ---- 6. WinUI-style slider row: thin track, fixed outer ring, resizing inner disc.
    //         Linear 1..60 mapping and the existing invisible magnets are unchanged.
    const Rect track = SliderTrackRect(dpi, totalButtons);
    const int centre = Sc(SliderCenterY(totalButtons), dpi);
    const Color accent = p.thumb;
    DrawGlyph(paint, base, widthPx, heightPx, gClock, padX + Sc(9, dpi), centre, p.text, dpi, 18);

    const int trackH = Sc(kSliderTrackHeight, dpi);
    const int fillW = MinutesToTrackPx(track.w, state.minutes);
    const bool thumbHeld = state.pressed == hitSliderTrack;
    const bool thumbHovered = state.hover == hitSliderTrack;
    Color active = accent;
    if (thumbHeld) active.a = 204;
    else if (thumbHovered) active.a = 230;

    const Color sliderTrack = state.dark ? RGBc(255, 255, 255, 138) : RGBc(0, 0, 0, 114);
    FillSliderCapsule(base, widthPx, heightPx,
                      Rect{ track.x, centre - trackH / 2, track.w, trackH }, sliderTrack);
    if (fillW > 0)
        FillSliderCapsule(base, widthPx, heightPx,
                          Rect{ track.x, centre - trackH / 2, fillW, trackH }, active);

    const int thumbX = track.x + fillW;
    const int outerD = Sc(kSliderThumbDiameter, dpi);
    const int border = Sc(1, dpi);
    FillSliderCapsule(base, widthPx, heightPx,
                      Rect{ thumbX - outerD / 2, centre - outerD / 2, outerD, outerD },
                      state.dark ? RGBc(59, 59, 59) : RGBc(218, 218, 218));
    FillSliderCapsule(base, widthPx, heightPx,
                      Rect{ thumbX - outerD / 2 + border, centre - outerD / 2 + border,
                            outerD - border * 2, outerD - border * 2 },
                      state.dark ? RGBc(69, 69, 69) : RGBc(255, 255, 255));
    const int innerD = Sc(thumbHeld ? kSliderPressedDiameter :
                         (thumbHovered ? kSliderHoverDiameter : kSliderInnerDiameter), dpi);
    FillSliderCapsule(base, widthPx, heightPx,
                      Rect{ thumbX - innerD / 2, centre - innerD / 2, innerD, innerD }, active);

    // Trailing value (.stime): right-aligned against the panel padding, on the track's centre
    // line, in the 54 px gap the track leaves to its right.
    wchar_t value[16];
    swprintf(value, 16, L"%d min", state.minutes);
    DrawTextA(paint, base, widthPx, heightPx, value,
              Rect{ widthPx - padX - Sc(60, dpi), centre - Sc(8, dpi), Sc(60, dpi), Sc(16, dpi) },
              Mix(p.textDim, p.base), p.base, FW_NORMAL, Sc(12, dpi), DT_RIGHT);

    // No preset dots: the slider is a continuous linear 1..60 control, and a seven-dot row
    // implies seven stops that no longer exist. The design's dots stay out.

    // ---- 6c. divider
    y = Sc(TicksDividerY(totalButtons), dpi);
    FillBlend(paint, base, widthPx, heightPx, 0, y, widthPx, Sc(kSepH, dpi), p.divider);

    // ---- 7. power dashboard card.
    {
        const Rect card = PowerCardRect(dpi, totalButtons);
        // Card = Fluent's CardBackgroundFillColorDefault over whatever is behind it (the acrylic, or
        // the flat plate when the material is off), framed by CardStrokeColorDefault. It used to be
        // the accent at 8 %, which stained a third of the panel with the user's accent colour - a
        // warm/brown card over a gold accent, and never what Windows paints. Kept on 2026-11-01
        // when the user rejected the tint change and asked to keep this build.
        const Color cardFill = p.cardFill;
        const Color cardEdge = p.cardEdge;
        FillRound(paint, base, widthPx, heightPx, card, Sc(kCardRadius, dpi), cardFill);
        StrokeRound(paint, base, widthPx, heightPx, card, Sc(kCardRadius, dpi), cardEdge);

        const int innerX = card.x + Sc(kCardPadX, dpi);
        const int innerW = card.w - Sc(kCardPadX * 2, dpi);
        const int labelW = innerW * 3 / 5;
        const Color faint = Mix(p.textDim, p.base);
        const wchar_t* labels[3] = { L"Wake count", L"Last dim", L"Display hours saved" };

        wchar_t values[3][32];
        swprintf(values[0], 32, L"%lu", state.wakesFromDim);
        if (state.lastDimKnown) {
            sessionstats::FormatWhen(state.msSinceDim, false, values[1], 32);
        } else {
            wcscpy(values[1], L"\u2014");
        }
        if (state.hasDimHistory) {
            sessionstats::FormatHoursSaved(state.dimmedMs, values[2], 32);
        } else {
            wcscpy(values[2], L"\u2014");
        }

        for (int i = 0; i < 3; i++) {
            const int rowY = card.y + Sc(kCardPad, dpi) + i * Sc(kCardRowH + kCardGap, dpi);
            DrawTextA(paint, base, widthPx, heightPx, labels[i],
                      Rect{ innerX, rowY, labelW, Sc(kCardRowH, dpi) },
                      faint, p.base, FW_NORMAL, Sc(12, dpi), DT_LEFT);
            DrawTextA(paint, base, widthPx, heightPx, values[i],
                      Rect{ innerX + labelW, rowY, innerW - labelW, Sc(kCardRowH, dpi) },
                      faint, p.base, FW_SEMIBOLD, Sc(13, dpi), DT_RIGHT);
        }
        // Progress bar: the share of this process's uptime spent dimmed, so the bar reads
        // against the "Display hours saved" row directly above it. The numerator is
        // session-only - see ProgressFillWidth for why the persisted lifetime total cannot
        // be used against a per-process denominator.
        const int barY = card.y + Sc(kCardPad + kCardRowH * 3 + kCardGap * 3, dpi);
        const int barH = Sc(kCardBarH, dpi);
        const Rect bar{ innerX, barY, innerW, barH };
        FillRound(paint, base, widthPx, heightPx, bar, barH / 2, p.track);
        const int barFill = ProgressFillWidth(innerW, state.sessionDimmedMs, state.sessionMs);
        if (barFill > 0)
            FillRound(paint, base, widthPx, heightPx,
                      Rect{ innerX, barY, barFill, barH }, barH / 2, accent);
    }

    // ---- 8. divider + distinct bottom dock bar
    y = Sc(FooterDividerY(totalButtons), dpi);
    FillBlend(paint, base, widthPx, heightPx, 0, y, widthPx, Sc(kSepH, dpi), p.footerDivider);

    const int footY = Sc(FooterY(totalButtons), dpi);
    const int footH = Sc(kFooterHeight, dpi);
    // The footer keeps the bare acrylic. Its darker appearance comes from the main surface wash
    // above, plus the dark seam, as seen in the user's Windows crop. The earlier claim that a
    // divider alone matched that reference was wrong. Without acrylic retain the flat dock plate.
    if (!paintedBackdrop) {
        FillBlend(paint, base, widthPx, heightPx, 0, footY, widthPx, footH, p.dock);
    }

    const Rect exitBtn = FooterExitRect(dpi, totalButtons);
    const Rect dimBtn = DimNowRect(dpi, totalButtons);
    const Rect offBtn = DisplaysOffRect(dpi, totalButtons);
    const int statW = offBtn.x - padX - Sc(8, dpi);

    // App name above a smaller version hint, in the former uptime line's position.
    DrawTextA(paint, base, widthPx, heightPx, L"Idle Dimmer",
              Rect{ padX, footY + Sc(11, dpi), statW, Sc(15, dpi) },
              p.text, p.base, FW_SEMIBOLD, Sc(12, dpi), DT_LEFT);
    DrawTextA(paint, base, widthPx, heightPx, IDLE_DIMMER_VERSION_LABEL,
              Rect{ padX, footY + Sc(28, dpi), statW, Sc(13, dpi) },
              Mix(p.textDim, p.base), p.base, FW_NORMAL, Sc(10, dpi), DT_LEFT);

    auto IconButton = [&](Rect r, int target, Glyph g, Color fg) {
        const bool hovered = state.hover == target;
        const bool held = state.pressed == target;
        if (held || hovered) {
            FillRound(paint, base, widthPx, heightPx, r, Sc(kTileRadius, dpi), held ? p.tileBorder : p.tileHover);
            StrokeRound(paint, base, widthPx, heightPx, r, Sc(kTileRadius, dpi), p.tileBorder);
        }
        DrawGlyph(paint, base, widthPx, heightPx, g, r.x + r.w / 2, r.y + r.h / 2, fg, dpi, 15);
    };
    IconButton(dimBtn, hitDimNow, gDimNow, p.text);
    IconButton(offBtn, hitDisplaysOff, gDisplay, p.text);
    IconButton(exitBtn, hitFooterExit, gExit, p.textDim);

    // GDI cannot alpha-blend text into a DIB, and its failure mode is worse than a wrong
    // colour: every DrawTextW writes the 4th byte of every pixel it touches to 0 - including
    // fully covered glyph cores and every antialiasing fringe pixel - discarding coverage
    // that RGB still holds.
    {
        const int total = widthPx * heightPx;
        for (int i = 0; i < total; i++)
            if (base[i * 4 + 3] == 0) base[i * 4 + 3] = 255;
    }
    // Present the composited DIB onto the caller's DC, then unwind in reverse order.
    BitBlt(dc, 0, 0, widthPx, heightPx, paint, 0, 0, SRCCOPY);
    SelectObject(paint, prevBmp);
    DeleteDC(paint);
    DeleteObject(tmp);
}

// Render over black and white to recover coverage, including GDI's grayscale text edges.
// This retains the existing SVG masks and all hit geometry, while leaving empty panel body clear.
HBITMAP PaintPanelOverlay(int widthPx, int heightPx, const PanelState& state, int dpi) {
    if (widthPx <= 0 || heightPx <= 0) return nullptr;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = widthPx;
    bi.bmiHeader.biHeight = -heightPx;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return nullptr;
    BYTE *black = nullptr, *white = nullptr;
    HBITMAP result = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&black, nullptr, 0);
    HBITMAP whiteResult = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&white, nullptr, 0);
    bi.bmiHeader.biWidth = 1; bi.bmiHeader.biHeight = -1;
    DWORD *blackPixel = nullptr, *whitePixel = nullptr;
    HBITMAP blackBase = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&blackPixel, nullptr, 0);
    HBITMAP whiteBase = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&whitePixel, nullptr, 0);
    bool ok = result && whiteResult && blackBase && whiteBase && black && white;
    if (ok) {
        *blackPixel = 0xFF000000; *whitePixel = 0xFFFFFFFF;
        HGDIOBJ old = SelectObject(dc, result);
        PaintPanel(dc, widthPx, heightPx, state, dpi, blackBase);
        SelectObject(dc, whiteResult);
        PaintPanel(dc, widthPx, heightPx, state, dpi, whiteBase);
        GdiFlush();
        SelectObject(dc, old);
        for (size_t i = 0; i < (size_t)widthPx * heightPx; i++) {
            BYTE* b = black + i * 4;
            BYTE* w = white + i * 4;
            int difference = 0;
            for (int c = 0; c < 3; c++) if (w[c] - b[c] > difference) difference = w[c] - b[c];
            const BYTE alpha = (BYTE)(255 - difference);
            for (int c = 0; c < 3; c++) if (b[c] > alpha) b[c] = alpha;
            b[3] = alpha;
        }
    }
    if (whiteBase) DeleteObject(whiteBase);
    if (blackBase) DeleteObject(blackBase);
    if (whiteResult) DeleteObject(whiteResult);
    DeleteDC(dc);
    if (!ok && result) { DeleteObject(result); result = nullptr; }
    return result;
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------
// The slider, the dim-now button and the footer buttons all sit on the same rows the
// painter already computes, so their rects are derived from the shared stack rather than
// restated. Restating them is exactly how the divider drifted into the captions earlier.

int SliderThumbX(int dpi, int minutes) {
    const Rect track = SliderTrackRect(dpi);
    return track.x + MinutesToTrackPx(track.w, minutes);
}

int SliderPermilleAtX(int x, int dpi) {
    const Rect track = SliderTrackRect(dpi);
    if (track.w <= 0) return 0;
    long long delta = (long long)x - track.x;
    if (delta < 0) delta = 0;
    if (delta > track.w) delta = track.w;
    return (int)((delta * kSliderPermilleMax + track.w / 2) / track.w);
}

// Footer icon buttons, laid out exactly as .foot does: 13 px from the top of the footer, 12 px
// in from the right, 10 px apart, 34x32. Exit is rightmost; dim-now sits beside it. These are
// the same rects the painter draws and the hit test reads, so what is drawn is what is clicked.
static Rect FooterButton(int fromRight, int dpi, int buttonCount) {
    const int left = kPanelWidth - kFooterPadRight - fromRight - kFooterBtnW;
    return Rect{ S(left, dpi), S(FooterY(buttonCount) + kFooterPadTop, dpi),
                 S(kFooterBtnW, dpi), S(kFooterBtnH, dpi) };
}

Rect DimNowRect(int dpi, int buttonCount) {
    return FooterButton(kFooterBtnW + kFooterGap, dpi, buttonCount);
}

// Left of dim-now, so the dock keeps Exit at the right edge: [displays off][dim now][exit].
Rect DisplaysOffRect(int dpi, int buttonCount) {
    return FooterButton((kFooterBtnW + kFooterGap) * 2, dpi, buttonCount);
}

Rect FooterExitRect(int dpi, int buttonCount) {
    return FooterButton(0, dpi, buttonCount);
}

static int HitTestImpl(int x, int y, int dpi, int totalButtons, bool tileBodyOnly) {
    if (totalButtons <= 0) totalButtons = 4;
    if (totalButtons > 6) totalButtons = 6;
    for (int i = 0; i < totalButtons; i++) {
        Rect t = TileRect(i, dpi, totalButtons);
        // Click area includes the caption; hover area is the box alone, because that is the shape
        // the highlight is drawn on. One function, two geometries, so the two can never drift.
        Rect area = tileBodyOnly
                        ? t
                        : Rect{ t.x, t.y, t.w, t.h + S(kCaptionH, dpi) + S(kCaptionGap, dpi) };
        if (area.Contains(x, y)) {
            // Same table the painter used to build the row, so a tile cannot be drawn as one
            // control and hit-tested as another.
            const int target = TileTargetForIndex(i);
            return target != hitNone ? target : hitTileDimming;
        }
    }
    // Keep the generous grab area around the thinner track and smaller native-style thumb.
    {
        Rect track = SliderTrackRect(dpi, totalButtons);
        Rect grab{ track.x - S(12, dpi), track.y - S(18, dpi),
                   track.w + S(24, dpi), S(36, dpi) };
        if (grab.Contains(x, y)) return hitSliderTrack;
    }
    if (DimNowRect(dpi, totalButtons).Contains(x, y)) return hitDimNow;
    if (DisplaysOffRect(dpi, totalButtons).Contains(x, y)) return hitDisplaysOff;
    if (FooterExitRect(dpi, totalButtons).Contains(x, y)) return hitFooterExit;
    return hitNone;
}
int HitTest(int x, int y, int dpi, int totalButtons) {
    return HitTestImpl(x, y, dpi, totalButtons, false);
}
int HitTestHover(int x, int y, int dpi, int totalButtons) {
    return HitTestImpl(x, y, dpi, totalButtons, true);
}
