// Live acrylic for the panel, done with GDI and a screen capture.
//
// WHY THIS FILE EXISTS AND WHAT IT IS NOT
//
// Three native routes to Windows' live material were measured on this machine and all failed
// (IMPLEMENTATION.md §9.3, MEASURED 2026-11-01):
//
//   legacy accent policy      -> flat fill (no backdrop at all)
//   DWMWA_SYSTEM_BACKDROP_TYPE over a DComp swapchain -> constant tint, zero variance
//   Windows.UI.Composition    -> ActivateInstance returns E_ACCESSDENIED for an unpackaged process
//
// So "live" here is ours: capture the pixels behind the panel, blur them, tint them, and paint the
// result as the panel's background. It refreshes while the panel is open and stops when it closes,
// which is the rule this project cares about - nothing runs when the UI is absent.
//
// The capture must not include the panel, or the blur would feed on its own output and smear.
// WDA_EXCLUDEFROMCAPTURE removes the window from capture APIs, measured working here (255 -> 71.5
// on the probe, clean on reset). It is set for the lifetime of the session, so while the panel is
// open it is also absent from screenshots.
//
// COST (MEASURED 2026-11-01, 360x520, one core):
//   1.56 ms of CPU per frame for capture + downscale + upscale
//   one 16.7 ms wait per capture (the DWM composition tick) - hence the worker thread: the wait
//   must never sit on the UI thread, or hovering the panel would hitch 4x a second.
#pragma once
#include <windows.h>

namespace acrylic {

// Integration contract: Start() when the panel is shown, Stop() when it is hidden, LockFrame() /
// UnlockFrame() around the paint so a hide cannot free a bitmap mid-draw.
bool Start(HWND panel, const RECT& screenRect, int widthPx, int heightPx, COLORREF tint, int tintAlpha);
// Snapshot mode: one capture, taken BEFORE the panel is shown, and no worker thread. No capture
// exclusion is involved, so screenshots and recordings keep working - which matters more than a
// live refresh for a panel that is open for a few seconds. This is the default.
bool StartSnapshot(HWND panel, const RECT& screenRect, int widthPx, int heightPx, COLORREF tint, int tintAlpha);
void Stop();
// Freeze: stop capturing and clear the screenshot exclusion, but KEEP the last frame so the panel
// looks unchanged. This is what makes the panel screenshottable at all - it is excluded from capture
// only while the worker is running.
void Pause();
void SetCaptureRect(const RECT& screenRect);
HBITMAP LockFrame();     // nullptr until the first frame; always paired with UnlockFrame()
void UnlockFrame();
bool Running();
bool Paused();           // frozen by Pause(): last frame kept, screenshots allowed
int Frames();            // frames published since Start()

// Pure: is this executable one of the Windows capture tools (Snipping Tool, Snip & Sketch, Game
// Bar) or a common third-party one? While acrylic is live the panel is excluded from capture, so
// these tools refuse with "a protected app is preventing desktop capture"; the panel watches for
// them and freezes itself first, which drops the exclusion without the user pressing anything.
inline bool IsCaptureTool(const wchar_t* exeName) {
    if (!exeName || !exeName[0]) return false;
    static const wchar_t* kTools[] = {
        L"snippingtool.exe", L"snippingtool.exe.mui", L"screensketch.exe", L"snippingtool.ui.exe",
        L"gamebar.exe", L"gamebarftserver.exe", L"gamebarelevated.exe",
        L"sharex.exe", L"greenshot.exe", L"snipaste.exe", L"lightshot.exe", L"flameshot.exe",
        L"obs64.exe", L"obs32.exe"
    };
    for (const wchar_t* tool : kTools) {
        const wchar_t* a = exeName;
        const wchar_t* b = tool;
        bool same = true;
        while (*a && *b) {
            wchar_t ca = *a++, cb = *b++;
            if (ca >= L'A' && ca <= L'Z') ca = (wchar_t)(ca - L'A' + L'a');
            if (ca != cb) { same = false; break; }
        }
        if (same && !*a && !*b) return true;
    }
    return false;
}

// Posted to the panel when a new frame is ready, so the UI thread repaints with it.
const UINT kFrameMessage = WM_APP + 11;

// Pure: the downscale divisor used to make the blur.
//
// Windows' background acrylic is a large-radius Gaussian (the published recipe is blur -> exclusion
// blend -> tint -> noise). A 1/4 downscale plus a small box is *not* that: it is a mosaic, and
// against the same backdrop it reads as blocky where Windows reads as smooth. Half scale with three
// box passes is a proper Gaussian approximation, and in snapshot mode the cost is paid twice per
// panel open, not per frame.
inline int BlurDownscale(int sizePx) {
    int target = sizePx / 2;
    if (target < 32) target = 32;
    if (target > 512) target = 512;
    return target;
}

// One-shot diagnostic: runs the real pipeline once over a screen region, reports how much detail
// the blur removed, and optionally writes the result as a BMP. Used by --acrylic-test so the
// pipeline can be verified without screenshotting the panel (which the session excludes from
// capture by design).
struct ProbeStats {
    double fineBefore, coarseBefore, fineAfter, coarseAfter, meanBefore, meanAfter;
};
bool Probe(const RECT& screenRect, int widthPx, int heightPx, const wchar_t* bmpPath, ProbeStats* out);

// Capture a screen region verbatim to a BMP. Used by --panel-shot so the panel's own appearance can
// be inspected on disk; with the capture exclusion active that shows what is behind the panel, so
// the exclusion can be suppressed for the debug run with IDLE_DIMMER_NO_EXCLUSION=1.
bool CaptureRegionToBmp(const RECT& screenRect, const wchar_t* bmpPath);

// Write a top-down 32bpp BGRA buffer to a BMP. Used by --panel-shot to save a headless render of
// the panel: the panel cannot be screen-captured conventionally (it excludes itself), so the only
// honest way to see it is to run the painter into a bitmap and write that.
bool SaveBitsToBmp(const void* bgraTopDown, int w, int h, const wchar_t* bmpPath);

// Pure: one channel of "tint over blurred backdrop". alpha 255 = pure tint, 0 = pure blur.
inline unsigned char MixChannel(unsigned char tint, unsigned char blur, int alpha) {
    if (alpha <= 0) return blur;
    if (alpha >= 255) return tint;
    const int mixed = (tint * alpha + blur * (255 - alpha) + 127) / 255;
    return (unsigned char)(mixed < 0 ? 0 : (mixed > 255 ? 255 : mixed));
}

// Pure: intersect the wanted capture rect with the virtual screen. A panel anchored partly
// off-screen must not ask GDI for pixels that do not exist.
inline RECT ClampRect(const RECT& want, const RECT& bounds) {
    RECT out;
    out.left = want.left < bounds.left ? bounds.left : want.left;
    out.top = want.top < bounds.top ? bounds.top : want.top;
    out.right = want.right > bounds.right ? bounds.right : want.right;
    out.bottom = want.bottom > bounds.bottom ? bounds.bottom : want.bottom;
    if (out.right < out.left) out.right = out.left;
    if (out.bottom < out.top) out.bottom = out.top;
    return out;
}

// Pure: should a session exist at all? Off means "paint the flat base colour", which is the
// behaviour the app had before this file existed.
inline bool ShouldStart(bool enabled, bool panelVisible) { return enabled && panelVisible; }

// Pure-logic checks, run from --self-test. No GDI, no threads, no capture.
inline bool Checks(int* checkCount = nullptr) {
    int n = 0;
    bool ok = true;
    auto check = [&](bool value) { n++; ok = ok && value; };
    check(BlurDownscale(360) == 180);
    check(BlurDownscale(20) == 32);         // floor: still a blur, not a copy
    check(BlurDownscale(8000) == 512);      // ceiling: bounded work on a very wide panel
    check(MixChannel(44, 200, 255) == 44);  // full tint
    check(MixChannel(44, 200, 0) == 200);   // no tint
    check(MixChannel(44, 200, 128) == 122); // rounded midpoint, not truncated
    check(MixChannel(0, 255, 128) == 127);  // 255 * (255-128) / 255, exact: the expectation was wrong, not the maths
    const RECT bounds = { -100, -100, 900, 900 };
    const RECT inside = { 10, 10, 210, 210 };
    const RECT clamped = ClampRect(inside, bounds);
    check(clamped.left == 10 && clamped.right == 210);
    const RECT outside = { -300, -300, 60, 60 };
    const RECT clamped2 = ClampRect(outside, bounds);
    check(clamped2.left == -100 && clamped2.top == -100 && clamped2.right == 60);
    const RECT crossed = { 500, 500, 400, 400 };   // inverted input must not become 0xFFFFFFFF
    const RECT clamped3 = ClampRect(crossed, bounds);
    check(clamped3.right >= clamped3.left && clamped3.bottom >= clamped3.top);
    check(ShouldStart(true, true));
    check(!ShouldStart(true, false));
    check(!ShouldStart(false, true));
    // Capture-tool detection: case-insensitive, full-name (not prefix) match.
    check(IsCaptureTool(L"SnippingTool.exe"));
    check(IsCaptureTool(L"SNIPPINGTOOL.EXE"));
    check(IsCaptureTool(L"ScreenSketch.exe"));
    check(IsCaptureTool(L"GameBar.exe"));
    check(IsCaptureTool(L"obs64.exe"));
    check(!IsCaptureTool(L"explorer.exe"));
    check(!IsCaptureTool(L"SnippingToolX.exe"));   // suffix must not match
    check(!IsCaptureTool(L""));
    check(!IsCaptureTool(nullptr));
    if (checkCount) *checkCount = n;
    return ok;
}

}  // namespace acrylic
