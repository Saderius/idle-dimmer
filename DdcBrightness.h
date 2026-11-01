// DDC/CI brightness for external monitors.
//
// WHY THIS FILE EXISTS
//
// The app's original and only brightness mechanism is WMI's WmiSetBrightness, which exists for a
// laptop's INTERNAL panel and nowhere else. MEASURED 2026-10-31 on this machine (a desktop with
// three external monitors):
//
//   --diagnose  ->  Brightness HRESULT: 0x8004100C (WBEM_E_NOT_SUPPORTED), Internal panels: 0
//   standalone dxva2 probe -> 3 monitors, GetMonitorBrightness OK on all three
//                             (97%, 100%, 78%), no-op SetMonitorBrightness OK on all three
//
// So on this machine the app had no working dim path at all: Dim() aborted with
// "Brightness unavailable" and the screen never changed. It worked on the Surface Book 3 because
// that machine HAS an internal panel, which is exactly what WMI drives.
//
// External monitors are reached over DDC/CI - the same mechanism Twinkle Tray and every other
// working brightness tool uses - through dxva2.dll:
//
//   EnumDisplayMonitors -> GetPhysicalMonitorsFromHMONITOR -> GetMonitorBrightness
//                        / SetMonitorBrightness
//
// dxva2 is loaded BY NAME, never linked: this project forbids adding hard runtime dependencies,
// and a machine without dxva2 must still start (same rule as the DWM entry points in
// IdleDimmer.cpp). GetMonitorCapabilities is deliberately not trusted as a gate - the three
// measured monitors answered caps=no, flags=0x00000000 while GetMonitorBrightness worked on all
// three, so gating on the caps call would reproduce the bug it was meant to prevent.
//
// Pure decisions (ClampLevel, FindMonitor) live here, separate from the Win32 walk, so
// --self-test can pin them exactly as Flyout.h and ScreenGuard.h are pinned.
#pragma once
#include <windows.h>

namespace ddc {

constexpr int kMaxMonitors = 8;
constexpr int kMaxDesc = 128;

struct Monitor {
    wchar_t desc[kMaxDesc];
    DWORD min;
    DWORD current;
    DWORD max;
};

// Pure: the level to actually write, bounded by what the monitor reports. A monitor that
// reports no usable range (max == 0) is handed the value unchanged; SetLevel refuses it anyway.
inline DWORD ClampLevel(DWORD value, DWORD low, DWORD high) {
    if (high <= low) return value;
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// Pure: which current monitor a saved entry refers to.
//
// A description alone is NOT a key: MEASURED 2026-10-31, all three external monitors report
// "Generic PnP Monitor" through DDC, so matching on the string alone aimed every restore at
// monitor 1. The key is (description, occurrence) - the second "Generic PnP Monitor" restores
// the second such monitor - which survives two identical panels and still finds a panel that
// was moved to another port. Ordinal is only the fallback when the description is empty.
inline int FindMonitor(const Monitor* monitors, int count, const wchar_t* desc, int occurrence) {
    if (count <= 0 || !monitors) return -1;
    if (desc && desc[0]) {
        int seen = 0;
        for (int i = 0; i < count; i++) {
            if (!wcscmp(monitors[i].desc, desc)) {
                if (seen == occurrence) return i;
                seen++;
            }
        }
        return -1;
    }
    return (occurrence >= 0 && occurrence < count) ? occurrence : -1;
}

// Pure: how many earlier entries in the saved list carry the same description. This is the
// occurrence number FindMonitor looks for, so a restore targets the same physical monitor the
// capture read from rather than the first one with that string.
inline int Occurrence(const wchar_t* const* descs, int before, const wchar_t* desc) {
    if (!descs || before <= 0 || !desc || !desc[0]) return 0;
    int seen = 0;
    for (int i = 0; i < before; i++) {
        if (descs[i] && !wcscmp(descs[i], desc)) seen++;
    }
    return seen;
}

// mingw's windows.h does not pull in physicalmonitorenumerationapi.h; this is the SDK layout.
typedef struct _PHYSICAL_MONITOR {
    HANDLE hPhysicalMonitor;
    wchar_t szPhysicalMonitorDescription[128];
} PHYSICAL_MONITOR, *LPPHYSICAL_MONITOR;

struct Api {
    HMODULE module;
    BOOL (WINAPI* countMonitors)(HMONITOR, LPDWORD);
    BOOL (WINAPI* getMonitors)(HMONITOR, DWORD, LPPHYSICAL_MONITOR);
    BOOL (WINAPI* destroyMonitors)(DWORD, LPPHYSICAL_MONITOR);
    BOOL (WINAPI* getBrightness)(HANDLE, LPDWORD, LPDWORD, LPDWORD);
    BOOL (WINAPI* setBrightness)(HANDLE, DWORD);
    bool resolved;
    bool ready;
};

inline Api& Library() {
    static Api api = {};
    if (!api.resolved) {
        api.resolved = true;
        api.module = LoadLibraryW(L"dxva2.dll");
        if (api.module) {
            api.countMonitors = (BOOL (WINAPI*)(HMONITOR, LPDWORD))
                GetProcAddress(api.module, "GetNumberOfPhysicalMonitorsFromHMONITOR");
            api.getMonitors = (BOOL (WINAPI*)(HMONITOR, DWORD, LPPHYSICAL_MONITOR))
                GetProcAddress(api.module, "GetPhysicalMonitorsFromHMONITOR");
            api.destroyMonitors = (BOOL (WINAPI*)(DWORD, LPPHYSICAL_MONITOR))
                GetProcAddress(api.module, "DestroyPhysicalMonitors");
            api.getBrightness = (BOOL (WINAPI*)(HANDLE, LPDWORD, LPDWORD, LPDWORD))
                GetProcAddress(api.module, "GetMonitorBrightness");
            api.setBrightness = (BOOL (WINAPI*)(HANDLE, DWORD))
                GetProcAddress(api.module, "SetMonitorBrightness");
            api.ready = api.countMonitors && api.getMonitors && api.destroyMonitors &&
                        api.getBrightness && api.setBrightness;
        }
    }
    return api;
}

inline bool Available() { return Library().ready; }

// One walk over the display tree. `want` < 0 collects monitors; otherwise it applies `level` to
// the want-th brightness-capable physical monitor in the same display order Enumerate uses.
struct Walk {
    Monitor* out;
    int cap;
    int count;
    int want;
    DWORD level;
    bool wrote;
};

inline BOOL CALLBACK Step(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    Walk* w = (Walk*)param;
    Api& api = Library();
    DWORD count = 0;
    if (!api.countMonitors(monitor, &count) || !count) return TRUE;
    if (count > 16) count = 16;
    PHYSICAL_MONITOR physical[16] = {};
    if (!api.getMonitors(monitor, count, physical)) return TRUE;
    for (DWORD i = 0; i < count; i++) {
        DWORD low = 0, current = 0, high = 0;
        bool hasBrightness = api.getBrightness(physical[i].hPhysicalMonitor, &low, &current, &high) != FALSE;
        if (hasBrightness && high > 0) {
            if (w->want < 0) {
                if (w->count < w->cap) {
                    Monitor& m = w->out[w->count];
                    wcsncpy(m.desc, physical[i].szPhysicalMonitorDescription, kMaxDesc - 1);
                    m.desc[kMaxDesc - 1] = 0;
                    m.min = low;
                    m.current = current;
                    m.max = high;
                }
            } else if (w->count == w->want) {
                w->wrote = api.setBrightness(physical[i].hPhysicalMonitor,
                                             ClampLevel(w->level, low, high)) != FALSE;
            }
            w->count++;
        }
    }
    api.destroyMonitors(count, physical);
    return TRUE;
}

// Monitors that expose a brightness control, in display order.
inline int Enumerate(Monitor* out, int cap) {
    if (!Available() || !out || cap <= 0) return 0;
    Walk walk = { out, cap, 0, -1, 0, false };
    EnumDisplayMonitors(nullptr, nullptr, Step, (LPARAM)&walk);
    return walk.count > cap ? cap : walk.count;
}

// Write one level, aimed at the monitor a saved entry was captured from. `occurrence` is that
// entry's Occurrence() among the saved entries, not its absolute index.
inline bool SetLevel(const wchar_t* desc, int occurrence, DWORD level) {
    if (!Available()) return false;
    Monitor monitors[kMaxMonitors];
    int count = Enumerate(monitors, kMaxMonitors);
    int index = FindMonitor(monitors, count, desc, occurrence);
    if (index < 0) return false;
    Walk walk = { nullptr, 0, 0, index, level, false };
    EnumDisplayMonitors(nullptr, nullptr, Step, (LPARAM)&walk);
    return walk.wrote;
}

// Pure-logic checks, run from --self-test. No display walk, no DDC traffic.
// `checkCount` (optional) receives the number of assertions, so the self-test's reported total
// grows with them the way the other suites do instead of hiding new checks behind one &&.
inline bool Checks(int* checkCount = nullptr) {
    int n = 0;
    bool ok = true;
    auto check = [&](bool value) { n++; ok = ok && value; };
    check(ClampLevel(50, 0, 100) == 50);
    check(ClampLevel(0, 20, 80) == 20);
    check(ClampLevel(99, 20, 80) == 80);
    check(ClampLevel(7, 0, 0) == 7);          // no reported range: unchanged
    check(ClampLevel(7, 80, 20) == 7);        // inverted range: unchanged, not a divide or a wrap
    Monitor m[3] = {};
    wcscpy(m[0].desc, L"Generic PnP Monitor");
    wcscpy(m[1].desc, L"DELL U2720Q");
    wcscpy(m[2].desc, L"Generic PnP Monitor");
    // The measured case: three panels that report the same string must still be distinguishable.
    check(FindMonitor(m, 3, L"Generic PnP Monitor", 0) == 0);
    check(FindMonitor(m, 3, L"Generic PnP Monitor", 1) == 2);
    check(FindMonitor(m, 3, L"Generic PnP Monitor", 2) == -1);   // only two exist
    check(FindMonitor(m, 3, L"DELL U2720Q", 0) == 1);
    check(FindMonitor(m, 3, L"Unknown", 2) == -1);
    check(FindMonitor(m, 3, L"", 1) == 1);              // empty description falls back to ordinal
    check(FindMonitor(m, 3, L"", 5) == -1);             // out of range, nothing to aim at
    check(FindMonitor(m, 0, L"DELL U2720Q", 0) == -1);  // no monitors at all
    check(FindMonitor(nullptr, 2, L"x", 0) == -1);
    const wchar_t* descs[3] = { L"Generic PnP Monitor", L"DELL U2720Q", L"Generic PnP Monitor" };
    check(Occurrence(descs, 1, L"DELL U2720Q") == 0);    // its own entry is not counted
    check(Occurrence(descs, 2, L"Generic PnP Monitor") == 1);
    check(Occurrence(descs, 3, L"Unknown") == 0);       // a never-seen description has none
    check(Occurrence(descs, 0, L"Generic PnP Monitor") == 0);
    check(Occurrence(nullptr, 2, L"x") == 0);
    // The two helpers have to agree: the occurrence the saved list computes must land on the
    // same monitor the live walk reports.
    for (int i = 0; i < 3; i++) {
        int occurrence = Occurrence(descs, i, descs[i]);
        check(FindMonitor(m, 3, descs[i], occurrence) == i);
    }
    if (checkCount) *checkCount = n;
    return ok;
}

}  // namespace ddc
