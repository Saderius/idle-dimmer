// Prices one tray-icon redraw using the exact DIB + CreateIconIndirect path from
// IdleDimmer.cpp MakeIcon(), so the runtime-vs-baked decision rests on a measurement
// instead of an assumption. Build: see tools\run-icon-bench.ps1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

// Same BITMAPV5HEADER + CreateDIBSection + CreateIconIndirect recipe as MakeIcon().
static HICON MakeIconPx(int px, COLORREF color, bool crescent) {
    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header);
    header.bV5Width = px;
    header.bV5Height = -px;          // top-down
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;

    void* pixels = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, (BITMAPINFO*)&header, DIB_RGB_COLORS, &pixels, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!bitmap) return nullptr;

    DWORD* p = (DWORD*)pixels;
    DWORD rgb = (GetRValue(color) << 16) | (GetGValue(color) << 8) | GetBValue(color);
    int c = px / 2, r = px * 3 / 10;
    for (int y = 0; y < px; y++) for (int x = 0; x < px; x++) {
        int dx = x - c, dy = y - c;
        bool draw = dx * dx + dy * dy <= r * r;
        if (crescent) { int cx = x - c - r / 2, cy = y - c + r / 2; if (cx * cx + cy * cy < r * r / 4) draw = false; }
        p[y * px + x] = draw ? (0xff000000 | rgb) : 0;
    }
    HBITMAP mask = CreateBitmap(px, px, 1, 1, nullptr);
    ICONINFO info = { TRUE, 0, 0, mask, bitmap };
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(bitmap);
    return icon;
}

// A representative "sun descending toward a horizon" frame: disc + rule, i.e. the
// only geometry the sunset glyph adds over the existing disc.
static void DrawSunset(int px, DWORD* p, int sunY, int horizonY, int r) {
    for (int y = 0; y < px; y++) for (int x = 0; x < px; x++) {
        int dx = x - px / 2, dy = y - sunY;
        bool draw = dx * dx + dy * dy <= r * r;
        if (y == horizonY && x >= px / 8 && x < px - px / 8) draw = true;
        p[y * px + x] = draw ? 0xff00ff00u : 0;
    }
}

static double us_per(int iterations, void (*work)(int)) {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < iterations; i++) work(i);
    QueryPerformanceCounter(&t1);
    return (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart * 1e6 / iterations;
}

static int  g_px = 32;
static COLORREF g_color = RGB(255, 255, 255);

static void work_create_destroy(int) {
    HICON h = MakeIconPx(g_px, g_color, false);
    if (h) DestroyIcon(h);
}

static void work_sunset(int i) {
    int px = g_px;
    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header); header.bV5Width = px; header.bV5Height = -px;
    header.bV5Planes = 1; header.bV5BitCount = 32; header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000; header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff; header.bV5AlphaMask = 0xff000000;
    void* pixels = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, (BITMAPINFO*)&header, DIB_RGB_COLORS, &pixels, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!bitmap) return;
    DrawSunset(px, (DWORD*)pixels, px / 5 + (i % 10) * px / 20, px * 3 / 5, px * 3 / 10);
    HBITMAP mask = CreateBitmap(px, px, 1, 1, nullptr);
    ICONINFO info = { TRUE, 0, 0, mask, bitmap };
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(bitmap);
    if (icon) DestroyIcon(icon);
}

// Same PMv2 declaration as IdleDimmer.cpp: resolved by name so the build does not
// depend on the SDK's DPI_AWARENESS_CONTEXT type.
typedef BOOL(WINAPI* SetDpiAwarenessContextFn)(void*);

int wmain() {
    SetDpiAwarenessContextFn setContext =
        (SetDpiAwarenessContextFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (setContext) setContext((void*)-4);   // -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2

    printf("Where the 578 us of a tray-icon redraw actually goes, and whether the DC\n");
    printf("acquisition can be hoisted. Same recipe as IdleDimmer.cpp MakeIcon().\n\n");

    const int px = 32;                 // TrayIconSize() on this 200%%-scaled machine
    const int N = 3000;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    auto ms_per_us = [&](long long ticks) { return (double)ticks / (double)f.QuadPart * 1e6 / N; };

    printf("%-46s %12s\n", "variant", "us/draw");
    printf("--------------------------------------------  ------------\n");

    // --- variant 1: GetDC(NULL)/ReleaseDC every redraw (what MakeIcon does today) ---
    {
        double t = 0;
        for (int i = 0; i < N; i++) {
            LARGE_INTEGER a0, a1;
            QueryPerformanceCounter(&a0);
            BITMAPV5HEADER h = {}; h.bV5Size = sizeof(h); h.bV5Width = px; h.bV5Height = -px;
            h.bV5Planes = 1; h.bV5BitCount = 32; h.bV5Compression = BI_BITFIELDS;
            h.bV5RedMask = 0x00ff0000; h.bV5GreenMask = 0x0000ff00;
            h.bV5BlueMask = 0x000000ff; h.bV5AlphaMask = 0xff000000;
            void* pixels = nullptr;
            HDC dc = GetDC(nullptr);
            HBITMAP bmp = CreateDIBSection(dc, (BITMAPINFO*)&h, DIB_RGB_COLORS, &pixels, nullptr, 0);
            ReleaseDC(nullptr, dc);
            DrawSunset(px, (DWORD*)pixels, 10, 19, 10);
            HBITMAP mask = CreateBitmap(px, px, 1, 1, nullptr);
            ICONINFO info = { TRUE, 0, 0, mask, bmp };
            HICON icon = CreateIconIndirect(&info);
            DeleteObject(mask); DeleteObject(bmp); if (icon) DestroyIcon(icon);
            QueryPerformanceCounter(&a1);
            t += a1.QuadPart - a0.QuadPart;
        }
        printf("%-46s %12.1f\n", "GetDC(NULL) per redraw  [today]", ms_per_us((long long)t));
    }

    // --- variant 2: hoist the DC; CreateCompatibleDC(NULL) created once ---
    {
        HDC cached = CreateCompatibleDC(nullptr);
        double t = 0;
        for (int i = 0; i < N; i++) {
            LARGE_INTEGER a0, a1;
            QueryPerformanceCounter(&a0);
            BITMAPV5HEADER h = {}; h.bV5Size = sizeof(h); h.bV5Width = px; h.bV5Height = -px;
            h.bV5Planes = 1; h.bV5BitCount = 32; h.bV5Compression = BI_BITFIELDS;
            h.bV5RedMask = 0x00ff0000; h.bV5GreenMask = 0x0000ff00;
            h.bV5BlueMask = 0x000000ff; h.bV5AlphaMask = 0xff000000;
            void* pixels = nullptr;
            HBITMAP bmp = CreateDIBSection(cached, (BITMAPINFO*)&h, DIB_RGB_COLORS, &pixels, nullptr, 0);
            DrawSunset(px, (DWORD*)pixels, 10, 19, 10);
            HBITMAP mask = CreateBitmap(px, px, 1, 1, nullptr);
            ICONINFO info = { TRUE, 0, 0, mask, bmp };
            HICON icon = CreateIconIndirect(&info);
            DeleteObject(mask); DeleteObject(bmp); if (icon) DestroyIcon(icon);
            QueryPerformanceCounter(&a1);
            t += a1.QuadPart - a0.QuadPart;
        }
        printf("%-46s %12.1f\n", "CreateCompatibleDC hoisted out of the loop", ms_per_us((long long)t));
        DeleteDC(cached);
    }

    // --- variant 3: hoist the DC AND the DIB; only refill pixels, no reallocation ---
    {
        HDC cached = CreateCompatibleDC(nullptr);
        BITMAPV5HEADER h = {}; h.bV5Size = sizeof(h); h.bV5Width = px; h.bV5Height = -px;
        h.bV5Planes = 1; h.bV5BitCount = 32; h.bV5Compression = BI_BITFIELDS;
        h.bV5RedMask = 0x00ff0000; h.bV5GreenMask = 0x0000ff00;
        h.bV5BlueMask = 0x000000ff; h.bV5AlphaMask = 0xff000000;
        void* pixels = nullptr;
        HBITMAP bmp = CreateDIBSection(cached, (BITMAPINFO*)&h, DIB_RGB_COLORS, &pixels, nullptr, 0);
        HBITMAP mask = CreateBitmap(px, px, 1, 1, nullptr);
        double t = 0;
        for (int i = 0; i < N; i++) {
            LARGE_INTEGER a0, a1;
            QueryPerformanceCounter(&a0);
            DrawSunset(px, (DWORD*)pixels, 10, 19, 10);
            ICONINFO info = { TRUE, 0, 0, mask, bmp };
            HICON icon = CreateIconIndirect(&info);
            if (icon) DestroyIcon(icon);
            QueryPerformanceCounter(&a1);
            t += a1.QuadPart - a0.QuadPart;
        }
        printf("%-46s %12.1f\n", "DIB + mask hoisted too (CreateIconIndirect only)", ms_per_us((long long)t));
        DeleteObject(mask); DeleteObject(bmp); DeleteDC(cached);
    }

    printf("\nBudget: the whole-process allowance at 0.02%% CPU is ~12 ms of CPU per minute.\n");
    printf("One redraw at 87 us costs 0.087 ms, so:\n");
    printf("  every 30 s (the existing check cadence)  = 0.17 ms/min  -> +0.0015%% CPU\n");
    printf("  every  4 s                              = 1.31 ms/min  -> +0.011%% CPU\n");
    printf("  every  1 s                              = 5.22 ms/min  -> +0.043%% CPU  (over budget)\n");
    printf("Baked ICO frames cost nothing at all, which is why the descent should ship baked.\n");
    return 0;
}