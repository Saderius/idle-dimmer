// Live acrylic implementation: capture what is behind the panel, blur it, tint it, publish it as
// the panel's background. See Acrylic.h for why this exists and what it costs.
#include "Acrylic.h"

#include <stdio.h>

namespace acrylic {

namespace {

const int kFrameIntervalMs = 90;       // ~11 fps; 250 ms read as "delayed" to the user in use
// Microsoft's acrylic constants, copied from WinUI's own source
// (controls/dev/Materials/Acrylic/AcrylicBrush.h + AcrylicBrush_themeresources.xaml):
//   sc_blurRadius = 30.0f
//   dark  AcrylicBackgroundFillColorBaseBrush: TintColor #202020, TintOpacity 0.50,
//                                              TintLuminosityOpacity 0.96
//   dark  AcrylicBackgroundFillColorDefaultBrush: TintColor #2C2C2C, TintOpacity 0.15,
//                                              TintLuminosityOpacity 0.96
//   light AcrylicBackgroundFillColorDefaultBrush: TintColor #FCFCFC, TintOpacity 0.00,
//                                              TintLuminosityOpacity 0.85
// This file deliberately takes the luminosity colour from Base (#202020 @ 0.96, which is what puts
// the body at Windows' measured luma ~41) and the colour opacity from Default (0.15, the brush
// flyouts actually use). MEASURED live on a static backdrop: at TintOpacity 0.50 the body is
// chroma 23.6, at 0.15 it is 40.4, with luma 40-41 either way. The Base *tint opacity* was what
// drained the wallpaper's colour - "we are losing colours" - not the tint colour.
// The tint is NOT what makes it dark - the luminosity blend is. That is why a flat tint mix could
// never match this material.
//
// sc_saturation and sc_exclusionColor are declared in WinUI's AcrylicBrush.h and *not* used by
// AcrylicBrush.cpp (its 19H1+ graph has no such steps), which is why the 2026-11-01 audit said to
// drop them. Measured against the user's own captures, dropping them took the panel from "really,
// really close to native" to "kilometers apart, like we lost the blur colouring": the saturation
// step is what puts back the wallpaper's chroma that the 50% Color blend takes away, and the
// exclusion step is what keeps the deep end from going flat. The shell's flyout is composited by
// DWM (DWMSBT_TRANSIENTWINDOW), not by this brush, so the brush's unused constants are evidence
// about *where* the steps come from, not that the system material lacks them. They stay.
const float kAcrylicSaturation = 1.25f;
const int kAcrylicExclusion = 26;   // sc_exclusionColor alpha (white): compresses contrast
const float kAcrylicNoise = 0.02f;  // sc_noiseOpacity

// 256x256 of deterministic noise. Microsoft's own asset is 256x256, opaque and full range
// (MEASURED 2026-11-01: mean 128.6, sd 52.8), so this is an honest stand-in rather than a
// low-amplitude one. At 2% it is worth about +1.9 luma on a 33-level body - the same lift Windows
// pays - and removing it is what made the panel read darker than the native flyout.
BYTE gNoiseTile[256 * 256];
bool gNoiseReady = false;

void EnsureNoise() {
    if (gNoiseReady) return;
    unsigned seed = 0x9E3779B9u;
    for (int i = 0; i < 256 * 256; i++) {
        seed = seed * 1664525u + 1013904223u;
        gNoiseTile[i] = (BYTE)((seed >> 24) & 0xFF);
    }
    gNoiseReady = true;
}

inline double Lum(double r, double g, double b) { return 0.3 * r + 0.59 * g + 0.11 * b; }

inline void ClipColor(double& r, double& g, double& b) {
    const double l = Lum(r, g, b);
    const double n = r < g ? (r < b ? r : b) : (g < b ? g : b);
    const double x = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (n < 0.0) {
        const double d = l - n;
        if (d > 0.0) { r = l + (r - l) * l / d; g = l + (g - l) * l / d; b = l + (b - l) * l / d; }
    }
    if (x > 1.0) {
        const double d = x - l;
        if (d > 0.0) {
            r = l + (r - l) * (1.0 - l) / d;
            g = l + (g - l) * (1.0 - l) / d;
            b = l + (b - l) * (1.0 - l) / d;
        }
    }
}

// Photoshop "Luminosity": keep the base's hue/saturation, take the blend colour's luminance.
inline void SetLum(double& r, double& g, double& b, double l) {
    const double d = l - Lum(r, g, b);
    r += d; g += d; b += d;
    ClipColor(r, g, b);
}

inline double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

// Blur strength, in box passes at 1/4 scale. Two passes of radius 8 approximate a Gaussian with
// sigma ~20 physical px. Overridable with IDLE_DIMMER_BLUR for tuning runs, because "matches
// Windows" is a judgement the user has to make by eye.
int gBlurRadius = 26;   // sigma ~52 physical px at 175% DPI, matching acrylic's 30 px logical
int gBlurPasses = 3;

void ReadTuning() {
    wchar_t value[16] = {};
    if (GetEnvironmentVariableW(L"IDLE_DIMMER_BLUR", value, 16) > 0) {
        const int parsed = _wtoi(value);
        if (parsed >= 1 && parsed <= 40) gBlurRadius = parsed;
    }
}

struct Surface {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    BYTE* bits = nullptr;
    BYTE* temp = nullptr;   // one separable-blur pass buffer
    int w = 0;
    int h = 0;
};

struct State {
    HWND panel = nullptr;
    HANDLE thread = nullptr;
    HANDLE stop = nullptr;
    CRITICAL_SECTION lock;
    bool lockReady = false;
    bool running = false;
    Surface frames[2];
    Surface scratch;                  // downscaled copy, worker-thread only
    volatile LONG published = -1;
    volatile LONG frameCount = 0;
    bool paused = false;              // stopped by Pause(): last frame kept, capture exclusion cleared
    RECT capture = {};
    COLORREF tint = RGB(0x2C, 0x2C, 0x2C);
    int tintAlpha = 38;           // TintOpacity 0.15 - the value both acrylic variants' flyout brush carries;
    int tintLuminosity = 245;     // TintLuminosityOpacity 0.96 (dark Base); light Base is 0.90 = 229
    bool excludeDuringCapture = true;
};

State g;

// Debug overrides for the two material parameters, applied AFTER the caller's values so a tuning run
// can sweep tint strength and the luminosity layer without a rebuild:
//   IDLE_DIMMER_TINT=0..255        colour tint (Windows' TintOpacity)
//   IDLE_DIMMER_LUMINOSITY=0..255  brightness-preserving layer (TintLuminosityOpacity)
void ApplyMaterialTuning() {
    wchar_t value[16] = {};
    if (GetEnvironmentVariableW(L"IDLE_DIMMER_TINT", value, 16) > 0) {
        const int parsed = _wtoi(value);
        if (parsed >= 0 && parsed <= 255) g.tintAlpha = parsed;
    }
    if (GetEnvironmentVariableW(L"IDLE_DIMMER_LUMINOSITY", value, 16) > 0) {
        const int parsed = _wtoi(value);
        if (parsed >= 0 && parsed <= 255) g.tintLuminosity = parsed;
    }
}

void DestroySurface(Surface& s);

// A 32bpp top-down DIB section. Deliberately NOT left selected into s.dc: the painter has to select
// this bitmap into its own DC to blit it, and GDI forbids one bitmap being selected into two DCs at
// once. Keeping it selected made the painter's SelectObject fail and the panel painted black.
bool CreateSurface(Surface& s, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    HDC screen = GetDC(nullptr);
    s.dc = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    s.bmp = CreateDIBSection(s.dc, &bi, DIB_RGB_COLORS, (void**)&s.bits, nullptr, 0);
    s.temp = (BYTE*)HeapAlloc(GetProcessHeap(), 0, (size_t)w * h * 4);
    ReleaseDC(nullptr, screen);
    if (!s.bmp || !s.bits || !s.temp) { DestroySurface(s); return false; }
    for (size_t i = 0; i < (size_t)w * h; i++) s.bits[i * 4 + 3] = 255;
    s.w = w;
    s.h = h;
    return true;
}

void DestroySurface(Surface& s) {
    if (s.bmp) DeleteObject(s.bmp);
    if (s.dc) DeleteDC(s.dc);
    if (s.temp) HeapFree(GetProcessHeap(), 0, s.temp);
    s.bmp = nullptr;
    s.dc = nullptr;
    s.bits = nullptr;
    s.temp = nullptr;
    s.w = 0;
    s.h = 0;
}

// Separable box blur over RGB (alpha ignored). The downscale/upscale alone is a mosaic: the
// upscaled blocks are visible as squares. Two small passes turn that into frosted glass.
void BlurSurface(Surface& s, int radius) {
    if (!s.bits || !s.temp || radius <= 0 || s.w <= 0 || s.h <= 0) return;
    const int taps = radius * 2 + 1;
    for (int y = 0; y < s.h; y++) {
        const BYTE* row = s.bits + (size_t)y * s.w * 4;
        BYTE* out = s.temp + (size_t)y * s.w * 4;
        for (int x = 0; x < s.w; x++) {
            int sum0 = 0, sum1 = 0, sum2 = 0;
            for (int d = -radius; d <= radius; d++) {
                int xx = x + d;
                if (xx < 0) xx = 0;
                if (xx >= s.w) xx = s.w - 1;
                const BYTE* p = row + (size_t)xx * 4;
                sum0 += p[0]; sum1 += p[1]; sum2 += p[2];
            }
            BYTE* q = out + (size_t)x * 4;
            q[0] = (BYTE)(sum0 / taps);
            q[1] = (BYTE)(sum1 / taps);
            q[2] = (BYTE)(sum2 / taps);
            q[3] = 255;
        }
    }
    for (int y = 0; y < s.h; y++) {
        for (int x = 0; x < s.w; x++) {
            int sum0 = 0, sum1 = 0, sum2 = 0;
            for (int d = -radius; d <= radius; d++) {
                int yy = y + d;
                if (yy < 0) yy = 0;
                if (yy >= s.h) yy = s.h - 1;
                const BYTE* p = s.temp + ((size_t)yy * s.w + x) * 4;
                sum0 += p[0]; sum1 += p[1]; sum2 += p[2];
            }
            BYTE* q = s.bits + ((size_t)y * s.w + x) * 4;
            q[0] = (BYTE)(sum0 / taps);
            q[1] = (BYTE)(sum1 / taps);
            q[2] = (BYTE)(sum2 / taps);
            q[3] = 255;
        }
    }
}

// One frame: screen -> downscale -> blur -> saturate/tint -> upscale.
//
// Everything per-pixel happens on the SMALL buffer. Doing it at full resolution cost 8.8% of a core
// at 11 fps; at 1/12 scale it is ~150x less work and visually identical, because the image is
// already low-frequency by then.
// One frame, two halves so the lock is never held across the ~16.7 ms screen capture:
//   CaptureIntoScratch() - screen -> downscale -> blur -> saturate/tint, worker-only buffer
//   UpscaleInto(target)  - scratch -> full-size frame
bool CaptureIntoScratch() {
    // Copied under the lock: the UI thread rewrites the rect on every animation tick, and a torn
    // RECT (mixed old/new edges) would distort the capture.
    RECT want;
    EnterCriticalSection(&g.lock);
    want = g.capture;
    LeaveCriticalSection(&g.lock);
    const RECT bounds = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                          GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                          GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
    const RECT rect = ClampRect(want, bounds);
    if (rect.right - rect.left <= 0 || rect.bottom - rect.top <= 0) return false;
    // A clamped rect means the panel is partly off-screen: stretching that sub-rect across the whole
    // frame would distort the blur, so skip the frame and keep the last good one instead.
    if (rect.left != want.left || rect.top != want.top || rect.right != want.right || rect.bottom != want.bottom)
        return false;

    HDC screen = GetDC(nullptr);
    HGDIOBJ oldScratch = SelectObject(g.scratch.dc, g.scratch.bmp);
    SetStretchBltMode(g.scratch.dc, HALFTONE);
    SetBrushOrgEx(g.scratch.dc, 0, 0, nullptr);
    const BOOL down = StretchBlt(g.scratch.dc, 0, 0, g.scratch.w, g.scratch.h, screen,
                                 rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                                 SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    if (!down) { SelectObject(g.scratch.dc, oldScratch); return false; }

    // Box passes at 1/4 scale: two of radius 8 approximate a Gaussian with sigma ~20 px at full
    // size. A single huge box (the first version) flattened every detail below 30 px and read as a
    // smear, which is what "too loose" meant.
    for (int pass = 0; pass < gBlurPasses; pass++) BlurSurface(g.scratch, gBlurRadius);
    {
        // The real recipe, in the order WinUI builds it (AcrylicBrush.cpp:417-471):
        //   blur -> Luminosity blend with the tint colour at TintLuminosityOpacity
        //        -> Color blend with the tint colour at TintOpacity
        // The saturation and exclusion steps in front of it are the shell material's, not the
        // XAML brush's - see the note on the constants above. They are what keeps the wallpaper's
        // colour alive through a 50% Color blend, which is exactly the thing the user reported
        // losing when they were briefly removed.
        const double tr = GetRValue(g.tint) / 255.0;
        const double tg = GetGValue(g.tint) / 255.0;
        const double tb = GetBValue(g.tint) / 255.0;
        const double colourAlpha = g.tintAlpha / 255.0;        // TintOpacity   (dark: 0.50)
        EnsureNoise();
        const double lumAlpha = g.tintLuminosity / 255.0;      // TintLuminosityOpacity (dark: 0.96)
        const double lumTint = Lum(tr, tg, tb);
        for (int y = 0; y < g.scratch.h; y++) {
            for (int x = 0; x < g.scratch.w; x++) {
                BYTE* p = g.scratch.bits + ((size_t)y * g.scratch.w + x) * 4;
                double r = p[2] / 255.0, gr = p[1] / 255.0, b = p[0] / 255.0;

                // Saturation boost (sc_saturation = 1.25), around the pixel's own luminance.
                const double l0 = Lum(r, gr, b);
                r = Clamp01(l0 + (r - l0) * kAcrylicSaturation);
                gr = Clamp01(l0 + (gr - l0) * kAcrylicSaturation);
                b = Clamp01(l0 + (b - l0) * kAcrylicSaturation);

                // Exclusion blend against white (sc_exclusionColor 26/255): out = a + b - 2ab with
                // b = 1 gives 1 - a, i.e. a 10% inversion that pulls both ends toward mid-grey. This
                // is the "reduce contrast" step in Microsoft's recipe.
                {
                    const double a = kAcrylicExclusion / 255.0;
                    r = r + ((1.0 - r) - r) * a;
                    gr = gr + ((1.0 - gr) - gr) * a;
                    b = b + ((1.0 - b) - b) * a;
                }

                if (lumAlpha > 0.0) {
                    // Luminosity blend: the panel takes its brightness from the tint colour and
                    // keeps the backdrop's hue/saturation. This is what makes the material dark.
                    double lr = r, lg = gr, lb = b;
                    SetLum(lr, lg, lb, lumTint);
                    r = r + (lr - r) * lumAlpha;
                    gr = gr + (lg - gr) * lumAlpha;
                    b = b + (lb - b) * lumAlpha;
                }
                if (colourAlpha > 0.0) {
                    // Color blend: hue/saturation from the tint, brightness from what we have.
                    const double l1 = Lum(r, gr, b);
                    double cr = tr, cg = tg, cb = tb;
                    SetLum(cr, cg, cb, l1);
                    r = r + (cr - r) * colourAlpha;
                    gr = gr + (cg - gr) * colourAlpha;
                    b = b + (cb - b) * colourAlpha;
                }

                // Noise at sc_noiseOpacity (2%), from the tiled 256x256 pattern. Kept: it carries
                // roughly +1.9 luma of the body, and without it the panel reads darker than the
                // native flyout over the same wallpaper.
                {
                    const BYTE n = gNoiseTile[((y & 255) << 8) | (x & 255)];
                    const double nv = n / 255.0;
                    r = r + (nv - r) * kAcrylicNoise;
                    gr = gr + (nv - gr) * kAcrylicNoise;
                    b = b + (nv - b) * kAcrylicNoise;
                }

                p[0] = (BYTE)(Clamp01(b) * 255.0 + 0.5);
                p[1] = (BYTE)(Clamp01(gr) * 255.0 + 0.5);
                p[2] = (BYTE)(Clamp01(r) * 255.0 + 0.5);
                p[3] = 255;
            }
        }
    }
    SelectObject(g.scratch.dc, oldScratch);
    return true;
}

bool UpscaleInto(Surface& target) {
    HGDIOBJ oldScratch = SelectObject(g.scratch.dc, g.scratch.bmp);
    HGDIOBJ oldTarget = SelectObject(target.dc, target.bmp);
    SetStretchBltMode(target.dc, HALFTONE);
    SetBrushOrgEx(target.dc, 0, 0, nullptr);
    const BOOL up = StretchBlt(target.dc, 0, 0, target.w, target.h, g.scratch.dc, 0, 0,
                               g.scratch.w, g.scratch.h, SRCCOPY);
    SelectObject(target.dc, oldTarget);
    SelectObject(g.scratch.dc, oldScratch);
    return up != FALSE;
}

// Sampled difference between two frames: is the blur actually changing? Repainting the whole panel
// at 11 fps costs far more than the capture, and on a static desktop every frame is identical, so
// publishing nothing keeps the panel at its idle cost while still updating as soon as something
// behind it moves.
bool FramesDiffer(const Surface& a, const Surface& b, double* deltaOut = nullptr) {
    if (!a.bits || !b.bits || a.w != b.w || a.h != b.h) return true;
    long long sum = 0;
    int samples = 0;
    for (int y = 0; y < a.h; y += 8) {
        const BYTE* pa = a.bits + (size_t)y * a.w * 4;
        const BYTE* pb = b.bits + (size_t)y * b.w * 4;
        for (int x = 0; x < a.w; x += 8) {
            const int la = (pa[x * 4 + 2] * 299 + pa[x * 4 + 1] * 587 + pa[x * 4 + 0] * 114) / 1000;
            const int lb = (pb[x * 4 + 2] * 299 + pb[x * 4 + 1] * 587 + pb[x * 4 + 0] * 114) / 1000;
            sum += la > lb ? la - lb : lb - la;
            samples++;
        }
    }
    if (!samples) return true;
    return (double)sum / samples > 1.5;
}

DWORD WINAPI Worker(LPVOID) {
    // A new thread inherits the PROCESS's window station/desktop, not the main thread's. This app
    // moves its main thread to WinSta0\Default at startup, so without this the worker captured a
    // different, empty desktop and every frame came back black (measured).
    HDESK desktop = OpenDesktopW(L"Default", 0, FALSE, GENERIC_ALL);
    if (desktop) SetThreadDesktop(desktop);
    for (;;) {
        const DWORD wait = WaitForSingleObject(g.stop, kFrameIntervalMs);
        if (wait != WAIT_TIMEOUT) break;          // stop signalled
        if (!CaptureIntoScratch()) continue;
        // The lock covers only the upscale and the swap, never the screen capture: the painter holds
        // it for the duration of a paint, and a 16.7 ms capture inside it would stall the UI.
        EnterCriticalSection(&g.lock);
        const int current = (int)g.published;
        const int next = (current == 0) ? 1 : 0;
        bool published = false;
        if (UpscaleInto(g.frames[next]) &&
            (current < 0 || current > 1 || FramesDiffer(g.frames[next], g.frames[current]))) {
            g.published = next;
            InterlockedIncrement(&g.frameCount);
            published = true;
        }
        LeaveCriticalSection(&g.lock);
        if (published && g.panel) PostMessageW(g.panel, kFrameMessage, 0, 0);
    }
    if (desktop) CloseDesktop(desktop);
    return 0;
}

void FreeSurfacesLocked() {
    for (int i = 0; i < 2; i++) DestroySurface(g.frames[i]);
    DestroySurface(g.scratch);
    g.published = -1;
}

// ---- measurement helpers for Probe() ----
inline int LumaOf(const BYTE* p) { return (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000; }

double DetailMetric(const BYTE* bits, int w, int h, int lag) {
    if (!bits || w <= lag) return 0;
    double sum = 0;
    long long n = 0;
    for (int y = 0; y < h; y++) {
        const BYTE* row = bits + (size_t)y * w * 4;
        for (int x = 0; x + lag < w; x++) {
            const int a = LumaOf(row + (size_t)x * 4);
            const int b = LumaOf(row + (size_t)(x + lag) * 4);
            sum += (a > b ? a - b : b - a);
            n++;
        }
    }
    return n ? sum / (double)n : 0;
}

double MeanMetric(const BYTE* bits, int w, int h) {
    if (!bits || w <= 0 || h <= 0) return 0;
    double sum = 0;
    for (int y = 0; y < h; y++) {
        const BYTE* row = bits + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) sum += LumaOf(row + (size_t)x * 4);
    }
    return sum / ((double)w * h);
}

// 32bpp BMP, written bottom-up; the DIB itself is top-down.
bool WriteBmp(const Surface& s, const wchar_t* path) {
    if (!s.bits || !path) return false;
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    const DWORD pixelBytes = (DWORD)s.w * 4;
    const DWORD imageSize = pixelBytes * (DWORD)s.h;
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + imageSize;
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof(ih);
    ih.biWidth = s.w;
    ih.biHeight = s.h;          // positive: BMP rows are bottom-up
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = imageSize;
    bool ok = fwrite(&fh, sizeof(fh), 1, f) == 1 && fwrite(&ih, sizeof(ih), 1, f) == 1;
    for (int y = s.h - 1; y >= 0 && ok; y--)
        ok = fwrite(s.bits + (size_t)y * pixelBytes, pixelBytes, 1, f) == 1;
    fclose(f);
    return ok;
}

}  // namespace

bool Start(HWND panel, const RECT& screenRect, int widthPx, int heightPx, COLORREF tint, int tintAlpha) {
    if (!g.lockReady) { InitializeCriticalSection(&g.lock); g.lockReady = true; }
    Stop();
    ReadTuning();
    if (!panel || widthPx <= 0 || heightPx <= 0) return false;

    EnterCriticalSection(&g.lock);
    const int smallW = BlurDownscale(widthPx);
    const int smallH = BlurDownscale(heightPx);
    const bool built = CreateSurface(g.frames[0], widthPx, heightPx) &&
                       CreateSurface(g.frames[1], widthPx, heightPx) &&
                       CreateSurface(g.scratch, smallW, smallH);
    LeaveCriticalSection(&g.lock);
    if (!built) {
        EnterCriticalSection(&g.lock);
        FreeSurfacesLocked();
        LeaveCriticalSection(&g.lock);
        return false;
    }

    g.panel = panel;
    g.capture = screenRect;
    g.tint = tint;
    g.tintAlpha = tintAlpha;
    g.tintLuminosity = (GetRValue(tint) + GetGValue(tint) + GetBValue(tint) > 384) ? 217 : 245;
    ApplyMaterialTuning();   // after the caller's values, so the debug overrides win
    g.published = -1;
    g.frameCount = 0;

    // Held for the whole session, not toggled per capture: toggling left a window in which
    // screenshots caught a hole where the panel should be, which reads as a glitch. The cost is that
    // the panel is absent from screenshots while it is open; --panel-shot renders it instead.
    // IDLE_DIMMER_NO_EXCLUSION=1 suppresses it for audits that need a screen capture.
    wchar_t noExclusion[8] = {};
    g.excludeDuringCapture = !(GetEnvironmentVariableW(L"IDLE_DIMMER_NO_EXCLUSION", noExclusion, 8) > 0 &&
                               noExclusion[0] == L'1');
    if (g.excludeDuringCapture) SetWindowDisplayAffinity(panel, 0x00000011 /* WDA_EXCLUDEFROMCAPTURE */);

    g.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g.stop) {
        EnterCriticalSection(&g.lock);
        FreeSurfacesLocked();
        LeaveCriticalSection(&g.lock);
        if (g.excludeDuringCapture) SetWindowDisplayAffinity(panel, 0);
        g.panel = nullptr;
        return false;
    }
    g.thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!g.thread) {
        CloseHandle(g.stop);
        g.stop = nullptr;
        EnterCriticalSection(&g.lock);
        FreeSurfacesLocked();
        LeaveCriticalSection(&g.lock);
        if (g.excludeDuringCapture) SetWindowDisplayAffinity(panel, 0);
        g.panel = nullptr;
        return false;
    }
    g.running = true;
    return true;
}

// Snapshot mode: the default. One capture, no thread, no capture exclusion.
//
// The panel is not visible yet when this runs (ShowPanel calls it before SWP_SHOWWINDOW), so nothing
// has to be excluded and Windows' capture tools are unaffected. The cost is that the material is a
// photograph of the desktop at open time rather than a live refresh - for a flyout that is open for
// a few seconds that is the right trade, because excluding the window from capture makes Windows
// report "a protected app is preventing desktop capture" to Snipping Tool and the Game Bar.
bool StartSnapshot(HWND panel, const RECT& screenRect, int widthPx, int heightPx, COLORREF tint, int tintAlpha) {
    if (!g.lockReady) { InitializeCriticalSection(&g.lock); g.lockReady = true; }
    Stop();
    ReadTuning();
    if (!panel || widthPx <= 0 || heightPx <= 0) return false;

    EnterCriticalSection(&g.lock);
    // Two frames: the refresh renders into the one that is not published, so it can compare before
    // swapping instead of overwriting what is on screen.
    const bool built = CreateSurface(g.frames[0], widthPx, heightPx) &&
                       CreateSurface(g.frames[1], widthPx, heightPx) &&
                       CreateSurface(g.scratch, BlurDownscale(widthPx), BlurDownscale(heightPx));
    LeaveCriticalSection(&g.lock);
    if (!built) {
        EnterCriticalSection(&g.lock);
        FreeSurfacesLocked();
        LeaveCriticalSection(&g.lock);
        return false;
    }

    g.panel = panel;
    g.tint = tint;
    g.tintAlpha = tintAlpha;
    g.tintLuminosity = (GetRValue(tint) + GetGValue(tint) + GetBValue(tint) > 384) ? 217 : 245;
    ApplyMaterialTuning();   // after the caller's values, so the debug overrides win
    g.capture = screenRect;
    g.published = -1;
    if (!CaptureIntoScratch()) return false;
    EnterCriticalSection(&g.lock);
    const bool ok = UpscaleInto(g.frames[0]);
    if (ok) {
        g.published = 0;
        InterlockedIncrement(&g.frameCount);
    }
    LeaveCriticalSection(&g.lock);
    g.running = false;      // no worker: nothing refreshes, nothing is excluded
    g.paused = false;
    return ok;
}

void Stop() {    if (!g.running && !g.paused) return;
    Pause();
    // Freeing under the lock: a paint may still be inside LockFrame()/UnlockFrame().
    EnterCriticalSection(&g.lock);
    FreeSurfacesLocked();
    LeaveCriticalSection(&g.lock);
    g.paused = false;
    g.panel = nullptr;
}

void Pause() {
    if (!g.running) {
        // Already paused or never started: still make sure the window is capturable again.
        if (g.panel) SetWindowDisplayAffinity(g.panel, 0 /* WDA_NONE */);
        return;
    }
    g.running = false;
    g.paused = true;              // surfaces and the published frame survive: the panel keeps its look
    if (g.stop) SetEvent(g.stop);
    bool joined = true;
    if (g.thread) {
        const DWORD waited = WaitForSingleObject(g.thread, 2000);
        joined = (waited == WAIT_OBJECT_0);
        CloseHandle(g.thread);
        g.thread = nullptr;
    }
    if (g.stop) { CloseHandle(g.stop); g.stop = nullptr; }
    if (g.panel) SetWindowDisplayAffinity(g.panel, 0 /* WDA_NONE */);
    if (!joined) g.published = -1;   // never hand a half-written frame to the painter
}

void SetCaptureRect(const RECT& screenRect) {    // Written by the UI thread every animation tick and read by the worker: under the lock, because
    // a torn RECT is possible on 64-bit (no tearing fix-up for a plain struct copy).
    if (!g.lockReady) { g.capture = screenRect; return; }
    EnterCriticalSection(&g.lock);
    g.capture = screenRect;
    LeaveCriticalSection(&g.lock);
}
bool Running() { return g.running; }
bool Paused() { return g.paused; }
int Frames() { return (int)g.frameCount; }

HBITMAP LockFrame() {
    if (!g.lockReady) return nullptr;
    EnterCriticalSection(&g.lock);
    const int i = (int)g.published;
    if (i < 0 || i > 1) return nullptr;   // caller must still call UnlockFrame()
    return g.frames[i].bmp;
}

void UnlockFrame() {
    if (g.lockReady) LeaveCriticalSection(&g.lock);
}

bool Probe(const RECT& screenRect, int widthPx, int heightPx, const wchar_t* bmpPath, ProbeStats* out) {
    if (widthPx <= 0 || heightPx <= 0) return false;
    ReadTuning();
    Surface before, after, small;
    const RECT bounds = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                          GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                          GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
    const RECT rect = ClampRect(screenRect, bounds);
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0) return false;
    const bool ok = CreateSurface(before, w, h) && CreateSurface(after, w, h) &&
                    CreateSurface(small, BlurDownscale(w), BlurDownscale(h));
    if (!ok) { DestroySurface(before); DestroySurface(after); DestroySurface(small); return false; }

    HDC screen = GetDC(nullptr);
    HGDIOBJ oldBefore = SelectObject(before.dc, before.bmp);
    BitBlt(before.dc, 0, 0, w, h, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    SelectObject(before.dc, oldBefore);
    HGDIOBJ oldSmall = SelectObject(small.dc, small.bmp);
    SetStretchBltMode(small.dc, HALFTONE);
    StretchBlt(small.dc, 0, 0, small.w, small.h, screen, rect.left, rect.top, w, h, SRCCOPY | CAPTUREBLT);
    // Same passes at the same scale the live pipeline uses, so this artifact describes the
    // production blur rather than a softer stand-in for it.
    for (int pass = 0; pass < gBlurPasses; pass++) BlurSurface(small, gBlurRadius);
    HGDIOBJ oldAfter = SelectObject(after.dc, after.bmp);
    SetStretchBltMode(after.dc, HALFTONE);
    StretchBlt(after.dc, 0, 0, w, h, small.dc, 0, 0, small.w, small.h, SRCCOPY);
    SelectObject(after.dc, oldAfter);
    SelectObject(small.dc, oldSmall);
    ReleaseDC(nullptr, screen);

    if (out) {
        out->fineBefore = DetailMetric(before.bits, w, h, 1);
        out->coarseBefore = DetailMetric(before.bits, w, h, 16);
        out->fineAfter = DetailMetric(after.bits, w, h, 1);
        out->coarseAfter = DetailMetric(after.bits, w, h, 16);
        out->meanBefore = MeanMetric(before.bits, w, h);
        out->meanAfter = MeanMetric(after.bits, w, h);
    }
    const bool wrote = bmpPath ? WriteBmp(after, bmpPath) : true;
    DestroySurface(before);
    DestroySurface(after);
    DestroySurface(small);
    return wrote;
}

// Write a top-down 32bpp BGRA buffer to a BMP (same format the pipeline uses internally).
bool SaveBitsToBmp(const void* bgraTopDown, int w, int h, const wchar_t* bmpPath) {
    if (!bgraTopDown || w <= 0 || h <= 0 || !bmpPath) return false;
    Surface surface;
    surface.bits = (BYTE*)bgraTopDown;
    surface.w = w;
    surface.h = h;
    return WriteBmp(surface, bmpPath);   // WriteBmp reads only bits/w/h
}

// Capture a screen region verbatim. Same pipeline as the blur but with no downscale and no tint.
bool CaptureRegionToBmp(const RECT& screenRect, const wchar_t* bmpPath) {
    const RECT bounds = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                          GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                          GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
    const RECT rect = ClampRect(screenRect, bounds);
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0) return false;
    Surface surface;
    if (!CreateSurface(surface, w, h)) return false;
    HGDIOBJ old = SelectObject(surface.dc, surface.bmp);
    HDC screen = GetDC(nullptr);
    const BOOL ok = BitBlt(surface.dc, 0, 0, w, h, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    SelectObject(surface.dc, old);
    const bool wrote = ok && WriteBmp(surface, bmpPath);
    DestroySurface(surface);
    return wrote;
}

}  // namespace acrylic
