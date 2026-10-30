// Reports which DWM window attributes this machine honours.
//
// The flyout panel needs a blurred backdrop and rounded corners. Both come from DWM window
// attributes rather than from rendering, and both are absent on older Windows builds. The
// project's constraint is "no new runtime dependency, OS DLLs only", so every call goes
// through GetProcAddress on a dynamically loaded dwmapi.dll: a machine without it must
// degrade to a solid fill, never fail to launch.
//
// Run this BEFORE building the panel, so the degradation chain is written against what
// this machine actually reports instead of what the documentation implies.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef HRESULT(WINAPI* DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);

// Win10 1809+. Value 20. Takes a BOOL.
static const DWORD kUseImmersiveDarkMode = 20;
// Win11 21H2+. Value 33. Takes an enum: 2 = round.
static const DWORD kWindowCornerPreference = 33;
// Win11 22H2+. Value 38. Takes an enum: 2 = Mica, 3 = Acrylic, 0 = none.
static const DWORD kSystemBackdropType = 38;

// Undocumented but universally shipped since Win10 1703. Takes an ACCENT_POLICY.
typedef struct {
    DWORD AccentState;   // 4 = ACCENT_ENABLE_ACRYLICBLURBEHIND
    DWORD AccentFlags;
    DWORD GradientColor; // AABBGGRR
    DWORD AnimationId;
} AccentPolicy;
typedef struct {
    DWORD Attribute;     // 19 = WCA_ACCENT_POLICY
    AccentPolicy Data;
    DWORD Size;
    int Unused;
} WindowCompositionAttributeData;
typedef BOOL(WINAPI* SetWindowCompositionAttributeFn)(HWND, WindowCompositionAttributeData*);

static const char* YesNo(BOOL value) { return value ? "yes" : "NO"; }

// DWMWA_SYSTEM_BACKDROP_TYPE and DWMWA_WINDOW_CORNER_PREFERENCE are WRITE-ONLY.
// Measured on this machine (see docs): the Set call returns S_OK, but calling DwmSetWindow-
// Attribute again as a "get" returns E_INVALIDARG (0x80070057) and leaves the out value
// untouched at -1. So a set-then-read round trip CANNOT verify these attributes, and an
// earlier version of this harness concluded they were unsupported when they are not.
// The only honest probe is: does the Set call succeed, and is composition enabled at all?
static HRESULT SetAttr(DwmSetWindowAttributeFn set, HWND w, DWORD attr, LPCVOID value, DWORD size) {
    return set ? set(w, attr, value, size) : E_FAIL;
}

static BOOL CompositionEnabled(void) {
    typedef HRESULT(WINAPI* CompFn)(BOOL*);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    CompFn fn = dwm ? (CompFn)GetProcAddress(dwm, "DwmIsCompositionEnabled") : nullptr;
    BOOL enabled = FALSE;
    if (!fn || FAILED(fn(&enabled))) return FALSE;
    return enabled;
}

// An offscreen message-only-ish window: DWM rejects attribute calls on windows that were
// never composited, so a throwaway real window is needed for the test to mean anything.
static HWND MakeProbeWindow(void) {
    static const wchar_t* kClass = L"IdleDimmerDwmProbe";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    return CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"", WS_POPUP,
                           0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
}

int wmain(void) {
    SetProcessDPIAware();

    HWND probe = MakeProbeWindow();
    if (!probe) { printf("FATAL: could not create probe window (%lu)\n", GetLastError()); return 2; }

    HMODULE user = GetModuleHandleW(L"user32.dll");
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    DwmSetWindowAttributeFn setDwm = dwm
        ? (DwmSetWindowAttributeFn)GetProcAddress(dwm, "DwmSetWindowAttribute") : nullptr;
    SetWindowCompositionAttributeFn setAccent = user
        ? (SetWindowCompositionAttributeFn)GetProcAddress(user, "SetWindowCompositionAttribute") : nullptr;

    printf("DWM attribute availability\n");
    printf("==========================\n");
    printf("dwmapi.dll loaded        : %s\n", dwm ? "yes" : "NO");
    printf("DwmSetWindowAttribute    : %s\n", setDwm ? "yes" : "NO");

    BOOL composing = CompositionEnabled();

    BOOL dark = TRUE;
    HRESULT hrDark = SetAttr(setDwm, probe, kUseImmersiveDarkMode, &dark, sizeof(dark));

    INT corner = 2;   // DWMWCP_ROUND
    HRESULT hrCorner = SetAttr(setDwm, probe, kWindowCornerPreference, &corner, sizeof(corner));

    // 2 = Mica, 3 = Acrylic. Acrylic is what the panel wants: a tile panel floating over an
    // arbitrary desktop reads better with it than with Mica.
    INT backdrop = 3;
    HRESULT hrBackdrop = SetAttr(setDwm, probe, kSystemBackdropType, &backdrop, sizeof(backdrop));

    AccentPolicy policy = {};
    policy.AccentState = 4;             // ACCENT_ENABLE_ACRYLICBLURBEHIND
    policy.AccentFlags = 2;
    policy.GradientColor = 0x99000000;  // AABBGGRR
    WindowCompositionAttributeData data = {};
    data.Attribute = 19;                // WCA_ACCENT_POLICY
    data.Data = policy;
    data.Size = sizeof(policy);
    BOOL accentWorks = setAccent ? setAccent(probe, &data) : FALSE;

    // Attributes 33 and 38 are WRITE-ONLY. Measured: Set returns S_OK, but calling
    // DwmSetWindowAttribute again as a "get" returns E_INVALIDARG (0x80070057) and leaves the
    // out value untouched. A set-then-read round trip therefore CANNOT verify them, and an
    // earlier version of this harness used one and wrongly concluded they were unsupported.
    // The honest probe is: does Set succeed, with composition enabled?
    BOOL backdropWorks = composing && SUCCEEDED(hrBackdrop);
    BOOL cornerWorks = composing && SUCCEEDED(hrCorner);

    printf("DWM composition enabled  : %s\n", YesNo(composing));
    printf("tier 1  DWMWA_SYSTEM_BACKDROP_TYPE (38) acrylic : %-4s  hr=0x%08lX\n",
           YesNo(backdropWorks), (unsigned long)hrBackdrop);
    printf("tier 2  SetWindowCompositionAttribute acrylic  : %-4s\n", YesNo(accentWorks));
    printf("        DWMWA_WINDOW_CORNER_PREFERENCE (33)     : %-4s  hr=0x%08lX\n",
           YesNo(cornerWorks), (unsigned long)hrCorner);
    printf("        DWMWA_USE_IMMERSIVE_DARK_MODE (20)      : hr=0x%08lX\n", (unsigned long)hrDark);
    printf("\nnote: 38 and 33 are write-only; the Set result is the only available probe.\n");
    printf("\ndegradation chain the panel must implement:\n");
    if (backdropWorks)      printf("  -> tier 1: DWMWA_SYSTEMBACKDROP_TYPE, Mica/Acrylic from the OS\n");
    else if (accentWorks)   printf("  -> tier 2: SetWindowCompositionAttribute acrylic (Win10 1703 path)\n");
    else                    printf("  -> tier 3: opaque theme fill; panel must still paint a legible background\n");
    printf("  -> corners: %s\n", cornerWorks ? "DWMWA_WINDOW_CORNER_PREFERENCE" : "manual rounded-rect clip in WM_PAINT");
    printf("  -> dark mode: %s\n", SUCCEEDED(hrDark) ? "DWMWA_USE_IMMERSIVE_DARK_MODE honored" : "unavailable");

    // Non-zero exit only when nothing at all works, which would mean the panel cannot be
    // styled and the plan needs rethinking before any of it is built.
    return (backdropWorks || accentWorks) ? 0 : 1;
}
