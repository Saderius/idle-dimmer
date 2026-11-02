// Panel layout geometry for the Quick Settings flyout.
//
// Everything here is pure: no window, no DC, no global state. That is deliberate and it is
// what makes the layout testable. This environment has no interactive preview, so a layout
// bug found only by eye is a bug the user finds; pure maths is provable in --self-test.
//
// Win11 24H2 Quick Settings metrics, matched to docs\design-max-power-win11.html:
// 360 logical px wide, 8 px window radius, 46 px tiles, 10 px grid gutters, 40 px slider row.
// The values below are the design's CSS values in its own order, so a spacing change in the
// design is a one-line edit here rather than a hunt through the painter.
#pragma once
#include <windows.h>

struct Rect {
    int x, y, w, h;
    bool Contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
    static Rect FromLTRB(int l, int t, int r, int b) { return Rect{ l, t, r - l, b - t }; }
};

// Logical (unscaled) metrics. Every value below is multiplied by dpi/96 at paint time.
enum : int {
    kPanelWidth    = 360,   // .flyout width
    kWindowRadius  = 8,     // --radius-win
    kTileRadius    = 4,     // --radius-ctl (tiles, icon buttons)
    kCardRadius    = 12,    // --radius-card (power dashboard)
    kTileHeight    = 46,    // .tilegrid .tile .box height
    kTileGap       = 10,    // .tilegrid gap
    kCaptionH      = 15,    // .cap: 12 px at line-height 1.25
    kCaptionGap    = 6,     // tile column gap between .box and .cap
    kPanelPadX     = 16,    // .tilegrid / .slider-row / .foot side padding
    kSliderHeight  = 40,    // .track-wrap height
    kTickPadTop    = 2,     // .ticks padding-top (row kept as breathing room; dots removed)
    kTickDotH      = 2,     // .ticks span height, retained so the stack keeps its spacing
    kTickPadBottom = 14,    // .ticks padding-bottom
    kTickRowHeight = kTickPadTop + kTickDotH + kTickPadBottom,
    kFooterHeight  = 58,    // .foot: 13 top + 32 iconbtn + 13 bottom
};

// Seven slider detents (1/2/5/10/15/30/60). They drive the invisible magnet only; nothing is
// drawn for them. Named separately from the tray's six descent frames, which are a different
// thing that happens to share the number 6.
enum : int { kSliderDetentCount = 7 };

// Total logical height. Derived from the content stack rather than hardcoded, so adding a
// control cannot silently leave the panel too short to draw it.
//
// Every Y coordinate below comes from this one stack. An earlier version had PanelHeight()
// and the painter's divider Y each compute the stack separately, the two copies drifted,
// and the divider painted straight through the second row's captions - the content fitted
// the declared height but the elements did not fit the panel. One stack, one truth.
enum : int {
    kSepH          = 1,                                       // .sep height
    kGridTop       = 16,                                      // .tilegrid padding-top
    kRowHeight     = kTileHeight + kCaptionGap + kCaptionH,   // box, gap, caption
    kRowGap        = 10,                                      // vertical gap between tile rows
    kRowPitch      = kRowHeight + kRowGap,                    // box, gap, caption, row-gap
    kGridPadBottom = 16,                                      // .tilegrid padding-bottom
    kDividerY      = kGridTop + kRowHeight + kGridPadBottom,
    kSliderPadTop  = 12,                                      // .slider-row padding-top
    kSliderPadBottom = 2,                                     // .slider-row padding-bottom
    kSliderY       = kDividerY + kSepH + kSliderPadTop + kSliderHeight / 2,  // track centre
    kTickY         = kSliderY + kSliderHeight / 2 + kSliderPadBottom + kTickPadTop,  // empty tick row
    kTicksSepY     = kTickY + kTickDotH + kTickPadBottom,
    kCardMarginTop = 12,                                      // .pwrcard margin-top (matches kCardMarginBottom)
    kCardY         = kTicksSepY + kSepH + kCardMarginTop,     // .pwrcard top after divider + margin
    kCardPad       = 12,                                      // .pwrcard padding
    kCardPadX      = 14,                                      // .pwrcard padding, sides
    kCardRowH      = 18,                                      // .row: 12 px at line-height 1.5
    kCardGap       = 6,                                       // .pwrcard gap
    kCardBarH      = 4,                                       // .bar height
    kCardH         = kCardPad * 2 + kCardRowH * 3 + kCardGap * 3 + kCardBarH,
    kCardMarginBottom = 12,                                   // .pwrcard margin-bottom
    kFooterDividerY = kCardY + kCardH + kCardMarginBottom,
    kFooterY        = kFooterDividerY + kSepH,
    kFooterPadTop   = 13,                                     // .foot padding-top
    kFooterPadRight = 12,                                     // .foot padding-right
    kFooterBtnW     = 34,                                     // .iconbtn width
    kFooterBtnH     = 32,                                     // .iconbtn height
    kFooterGap      = 10,                                     // .foot gap
};
int PanelHeight(int buttonCount = 4);          // logical px
int DividerY(int buttonCount = 4);             // logical px
int SliderCenterY(int buttonCount = 4);        // logical px
int TickY(int buttonCount = 4);                // logical px
int TicksDividerY(int buttonCount = 4);        // logical px
int FooterDividerY(int buttonCount = 4);       // logical px
int FooterY(int buttonCount = 4);              // logical px

// The tile grid. Four tiles in one row (0..3) by default; dynamically reflows into 2 balanced rows
// when 5 or more buttons exist so no orphan buttons are left. Returns a zero rect for any index outside.
Rect TileRect(int index, int dpi, int totalButtons = 4);

// The power dashboard card's outline, including its 16 px side margins.
Rect PowerCardRect(int dpi, int buttonCount = 4);

// The threshold slider's draggable track, excluding the leading clock glyph and the trailing
// value label (".stime" in docs\design-max-power-win11.html).
Rect SliderTrackRect(int dpi, int buttonCount = 4);

// The slider selects every whole minute from 1 to 60. Position is per-mille, not percent:
// on the old 0..100 scale adjacent high values collapsed (38 and 39 both landed on 89%),
// which made 39 impossible to select. Per-mille keeps a distinct position for every minute.
// Linear, not logarithmic: 59 equal intervals, so 1->2 is the same width as 30->31. The
// earlier log scale made the low minutes enormous and the high ones a pixel or two.
enum : int { kSliderPermilleMax = 1000, kSliderSnapEnterPermille = 20 };
// WinUI slider proportions: fixed outer thumb, state-dependent inner accent disc.
enum : int { kSliderTrackHeight = 4, kSliderThumbDiameter = 20,
             kSliderInnerDiameter = 12, kSliderHoverDiameter = 14, kSliderPressedDiameter = 10 };
int MinutesFromPermille(int permille);
int PermilleFromMinutes(int minutes);
// Per-mille corresponding to one physical pixel on a track of this width. Used as the
// release threshold, so leaving a preset takes about one pixel rather than a wide dead zone.
int SliderReleasePermille(int trackWidthPx);

// Fill width in pixels for the power card's progress bar: `part` out of `whole`, clamped to
// [0, widthPx]. `whole == 0` is "no measurement yet" and paints nothing rather than a full bar
// or a divide-by-zero. Pure so --self-test can pin the rounding and the clamp.
int ProgressFillWidth(int widthPx, unsigned long long part, unsigned long long whole);

// One-way magnetic preset state for a drag. Presets (1/2/5/10/15/30/60) pull the value in
// when the pointer approaches one while moving toward it, but release on the first pixel
// moving away. Begin is a plain click and never snaps, so every minute stays clickable.
// No dots are painted: the magnets are invisible, so the slider reads as a plain 1..60
// control with equal steps.
struct SliderMagnet {
    int preset;        // 0 = free, otherwise the preset minute currently held
    int lastPermille;  // pointer position at the previous update
    int apexPermille;  // closest point reached while held, for the 1 px release test
};
int SliderMagnetBegin(SliderMagnet* state, int permille);
int SliderMagnetUpdate(SliderMagnet* state, int permille, int releasePermille);

// Exclude an auto-hidden taskbar from a monitor's work area. Edge uses ABE_LEFT/TOP/RIGHT/BOTTOM.
Rect ReserveTaskbarSpace(Rect work, Rect taskbar, unsigned int edge);

// Quick Settings-style bottom-right anchor, inset from the usable screen edges.
// An undersized work area pins to its origin and preserves the readable panel size.
Rect AnchorPanel(int dpi, int workX, int workY, int workW, int workH,
                 int buttonCount = 4);

// EarTrumpet's short quintic ease-out entrance, expressed as a remaining vertical offset.
enum : int { kPanelEdgeGap = 12, kPanelEntranceTravel = 25, kPanelEntranceMs = 130, kPanelExitMs = 150 };
int PanelEntranceOffset(unsigned long long elapsedMs, int dpi);
int PanelExitOffset(unsigned long long elapsedMs, int fromOffset, int dpi);

// Detent list, shared by the slider dots and by SliderMagnet.
int DetentCount();
int DetentAt(int index);

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
// Everything below takes a plain HDC so the headless renderer in tools\ can run the
// EXACT production drawing code with no window. That is deliberate: a contact sheet drawn
// by a copy of the paint routine silently drifts from the real one, and this environment
// has no interactive preview, so the sheet is the only visual check available.

// What the panel is currently showing. Plain data so it can be constructed in a test,
// built from live state on the real panel, and captured from either for the sheet.
struct PanelState {
    bool dark;               // follow the taskbar theme
    bool enabled;            // master switch
    bool dimmed;             // screen is at 0% right now
    bool mediaGuard;         // "pause while media plays" (Windows media sessions, SMTC)
    bool soundGuard;         // "pause while any app renders audio" (WASAPI fallback)
    bool fullscreenGuard;    // "pause for full-screen apps"
    bool offMode;            // "displays off at the idle threshold instead of dimming"
    bool autostart;          // Run key present
    int minutes;             // idle threshold, 1..60
    int idleSec;             // seconds since last input
    int design;              // selected tray icon design, 1-based
    int hover;               // HitTarget under the cursor, hitNone for none
    int pressed;             // HitTarget currently held down
    unsigned int accent;     // system accent colour (0x00RRGGBB), the resting tile fill
    // The next swatch up the AccentPalette ramp - what Windows paints on a hovered tile. Carried
    // separately from `accent` because the two are different entries of the same ramp: entry 1 is
    // the fill, entry 0 is the highlight, and a flat per-channel lift cannot reproduce it. 0 means
    // "unknown", and PaintPanel then derives a lift so a hovered tile still responds.
    unsigned int accentHover;

    // Counters for the power dashboard card, read from sessionstats at paint time. They are a
    // lifetime record (stats.bin), not a per-launch one: the counters survive restarts, which is
    // why "Last dim" can describe a dim from a previous session.
    //
    // hasDimHistory is the never-vs-zero discriminator for "Display hours saved": false means no
    // dim has ever been recorded, so it renders an em dash rather than 0.0 h. lastDimKnown is
    // separate because the clock stamp can be missing (a stats.bin written before the field
    // existed) even when dimmed time is known.
    unsigned long      wakesFromDim;   // restores from the dimmed state (persisted lifetime)
    unsigned long long dimmedMs;       // accumulated time below full brightness (persisted lifetime)
    unsigned long long sessionDimmedMs;// dimmed time accumulated by THIS process, the bar's numerator
    unsigned long long msSinceDim;     // now - last dim, valid only when lastDimKnown
    unsigned long long sessionMs;      // process uptime, for the power bar's denominator
    bool               hasDimHistory;
    bool               lastDimKnown;   // the wall-clock stamp for the last dim is usable
    int                buttonCount;    // total buttons in tile grid (0 defaults to 4)
};

// 0xAARRGGBB. Alpha is honoured by every primitive below.
struct Color {
    BYTE a, r, g, b;
};
constexpr Color RGBc(BYTE r, BYTE g, BYTE b, BYTE a = 255) { return Color{ a, r, g, b }; }

// Fluent 2 values, matching docs\tray-ui-proposal.html. Kept in one place so a palette
// change is a one-line edit and the sheet regenerates from the same source.
namespace theme {
constexpr Color accentDark()   { return RGBc(0x4C, 0xC2, 0xFF); }
constexpr Color accentLight()  { return RGBc(0x00, 0x78, 0xD4); }
constexpr Color onAccentDark() { return RGBc(0x00, 0x00, 0x00); }
constexpr Color onAccentLight(){ return RGBc(0xFF, 0xFF, 0xFF); }
}  // namespace theme

// Paints the entire panel into dc, sized widthPx x heightPx. dpi scales every logical
// metric. The caller owns the DC setup (font, text colour, background mode); PaintPanel
// sets those itself and restores nothing, because the only callers are the panel window
// and the headless renderer, and each creates a fresh DC per paint.
void PaintPanel(HDC dc, int widthPx, int heightPx, const PanelState& state, int dpi, HBITMAP backdrop = nullptr);
// Transparent premultiplied BGRA controls for the native compositor. Caller deletes the bitmap.
HBITMAP PaintPanelOverlay(int widthPx, int heightPx, const PanelState& state, int dpi);
// Screen-rect point containment, used by the WH_MOUSE_LL click-away hook in IdleDimmer.cpp.
// Deliberately the Win32 half-open convention: left/top inclusive, right/bottom exclusive, so
// two abutting rects never both claim a point and a click exactly on the window border belongs
// to the window underneath rather than to the panel. Extracted here so --self-test can pin it;
// the hook itself needs a real message pump and cannot be unit-tested.
static inline bool RectContainsPoint(RECT r, POINT p) {
    return p.x>=r.left && p.x<r.right && p.y>=r.top && p.y<r.bottom;
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------
// Pure, for the same reason the layout is: a mis-mapped control either swallows clicks
// silently or fires the wrong action, and neither is visible in a rendered image. The
// targets map onto the existing menu command ids in IdleDimmer.cpp, so the panel reuses
// the app's persistence and side effects rather than duplicating them.
enum HitTarget {
    hitNone = 0,
    hitTileDimming, hitTileMedia, hitTileSound, hitTileFullscreen, hitTileOffMode,
    hitTileIcon, hitTileAutostart, hitTileFolder,
    hitSliderTrack, hitDimNow, hitFooterGear, hitDisplaysOff, hitFooterExit,
};

// Click geometry: a tile owns its caption too, so the label toggles the control the box does -
// the same contract as Windows' own flyout.
int HitTest(int x, int y, int dpi, int totalButtons = 4);
// Hover geometry: the tile body only. The highlight is painted on the box, so a pointer resting on
// the caption must not light the tile even though a click there still activates it.
int HitTestHover(int x, int y, int dpi, int totalButtons = 4);

// Thumb position for the given threshold, shared by the painter and the drag handler so
// what you grab is exactly what you see.
int SliderThumbX(int dpi, int minutes);

// Slider per-mille from a panel-relative x, clamped to the track.
int SliderPermilleAtX(int x, int dpi);

// Controls outside the tile grid.
Rect DimNowRect(int dpi, int buttonCount = 4);
// The third footer button: turn the displays off now. Sits left of dim-now, so the rightmost
// control is still Exit and the dock reads dim-now, displays-off, exit.
Rect DisplaysOffRect(int dpi, int buttonCount = 4);
Rect FooterExitRect(int dpi, int buttonCount = 4);
