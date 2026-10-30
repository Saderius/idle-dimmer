// Full-screen detection, split so the VERDICT is pure and testable.
//
// Why this is not inline in IdleDimmer.cpp: the defect this file exists to prevent is a
// geometry test that cannot tell a maximised window from a full-screen one. No amount of
// reading the code catches that, and --self-test is the only executable check available in
// this environment. So the caller gathers the inputs - it owns the Win32 handles - and the
// decision is computed here from plain values, the same split Flyout.h uses for layout.
//
// THE MEASUREMENT THAT MOTIVATES IT (2026-10-17, development machine):
//   foreground window  Chrome_WidgetWin_1, style 0x15C70000, rect -7,-7 - 1626,1086
//   monitor            0,0 - 1620,1080
//   taskbar            auto-hiding, so the work area equals the monitor
// A maximised window is inflated past the monitor edges by the invisible resize border, and an
// auto-hiding taskbar removes the bottom gap that would otherwise stop it. So a "covers the
// monitor" test returned TRUE for an ordinary maximised browser, the app believed a full-screen
// app was permanently present, and it never dimmed. The display still slept on Windows' own
// timeout, which is what made it look like the app was being overridden.
//
// The discriminator the geometry test never consulted is the window style:
//   measured Chrome   WS_MAXIMIZE set, WS_OVERLAPPEDWINDOW bits present (0x00C70000)
//   full-screen game  styles stripped by the title: neither set
// Raymond Chen's documented recipe is the rect test PLUS the absence of WS_OVERLAPPEDWINDOW.
//
// KNOWN GAP, deliberately not handled here: a window cloaked by the shell (another virtual
// desktop, a suspended store app) keeps its full rect but is not on screen, so the cover test
// still false-positives on it. Fixing that needs DWMWA_CLOAKED, resolved by name from the
// dynamically loaded dwmapi.dll like every other DWM entry point in this project. UNVERIFIED
// either way: no second virtual desktop was available to measure against.
#pragma once
#include <windows.h>

namespace guard {

// Spelled out rather than taken from <windows.h>, because the tests build styles by hand and a
// macro that quietly differs between SDKs would make a passing test meaningless.
enum : unsigned long {
    kStyleMaximize   = 0x01000000,
    kStylePopup      = 0x80000000,
    kStyleVisible    = 0x10000000,
    kStyleCaption    = 0x00C00000,
    kStyleSysMenu    = 0x00080000,
    kStyleThickFrame = 0x00040000,
    kStyleMinBox     = 0x00020000,
    kStyleMaxBox     = 0x00010000,
    kStyleOverlapped = 0x00000000,
};

// WS_OVERLAPPEDWINDOW. A window carrying any of these is a window a person can grab and move;
// a full-screen presentation surface has had them stripped.
constexpr unsigned long kOverlappedWindow =
    kStyleOverlapped | kStyleCaption | kStyleSysMenu | kStyleThickFrame | kStyleMinBox | kStyleMaxBox;

// Everything the decision needs, and nothing it does not.
struct FgInfo {
    bool resolved = false;      // GetForegroundWindow returned a real window
    bool shellSurface = false;  // shell window, desktop, Progman/WorkerW, Flip3D, XAML island
    bool cloaked = false;       // has a rect but is not on screen; see KNOWN GAP above
    unsigned long style = 0;    // GWL_STYLE
    RECT rect = { 0, 0, 0, 0 };
    RECT monitor = { 0, 0, 0, 0 };
};

// True only for a window that is occupying the whole monitor by its own choice.
inline bool LooksFullScreen(const FgInfo& f) {
    if (!f.resolved) return false;
    if (f.shellSurface) return false;
    if (f.cloaked) return false;
    // Maximised is the measured false positive. Excluded before the cover test, because the
    // cover test is exactly what a maximised window defeats.
    if (f.style & kStyleMaximize) return false;
    // Chen's recipe. Requires the title to have actually removed its window furniture.
    if (f.style & kOverlappedWindow) return false;
    // <=/ >= rather than ==, because exclusive full-screen commonly overshoots the monitor by a
    // pixel of invisible border (Godot, for one, reports [-1,-1,W+1,H+1]). Equality would miss it.
    return f.rect.left <= f.monitor.left && f.rect.top <= f.monitor.top &&
           f.rect.right >= f.monitor.right && f.rect.bottom >= f.monitor.bottom;
}

}  // namespace guard
