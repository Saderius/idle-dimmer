#include "SessionStats.h"
// <wchar.h>, not <cwchar>: this toolchain's MinGW headers do not ship the C++ wrapper, and
// IdleDimmer.cpp already includes the C header for the same reason.
#include <wchar.h>

namespace sessionstats {

static Counters g_counters = { 0, 0, 0, 0ULL, 0ULL, 0ULL, false };

static int WriteTrunc(wchar_t* out, int cap, const wchar_t* tmp, int n) {
    if (!out || cap <= 0) return 0;
    if (n < 0) n = 0;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) out[i] = tmp[i];
    out[n] = L'\0';
    return n;
}

void Reset() {
    g_counters.wakeCount = 0;
    g_counters.dimCount = 0;
    g_counters.restoreCount = 0;
    g_counters.lastDimTick = 0ULL;
    g_counters.lastRestoreTick = 0ULL;
    g_counters.dimmedMs = 0ULL;
    g_counters.hasDimHistory = false;
}
void Seed(const Counters& c) {
    g_counters = c;
    g_counters.hasDimHistory = (g_counters.dimCount > 0);
}

void NoteWake() {
    g_counters.wakeCount++;
}

void NoteDim(unsigned long long tick) {
    // 0 is the "never" sentinel: a tick of zero must not register as a history event,
    // otherwise "never dimmed" becomes indistinguishable from "dimmed at boot".
    if (tick == 0ULL) return;
    g_counters.dimCount++;
    g_counters.lastDimTick = tick;
    g_counters.hasDimHistory = true;
}

void NoteRestore(unsigned long long tick) {
    if (tick == 0ULL) return;
    g_counters.restoreCount++;
    g_counters.lastRestoreTick = tick;
}

void AccumulateDimmed(unsigned long long deltaMs) {
    // Monotonic by construction: we only ever add. Never subtract, never reset here -
    // Reset() is the only thing that zeroes dimmedMs, and it zeroes everything.
    g_counters.dimmedMs += deltaMs;
}

Counters Get() {
    Counters c = g_counters;
    c.hasDimHistory = (c.dimCount > 0);
    return c;
}

bool HasDimHistory(const Counters& c) {
    return c.dimCount > 0;
}

int FormatMinutes(int minutes, wchar_t* out, int cap) {
    if (!out || cap <= 0) return 0;
    wchar_t tmp[64];
    int n = swprintf(tmp, 64, L"%d min", minutes);
    if (n < 0) n = 0;
    if (n > 63) n = 63;
    return WriteTrunc(out, cap, tmp, n);
}

int FormatHoursSaved(unsigned long long dimmedMs, wchar_t* out, int cap) {
    if (!out || cap <= 0) return 0;
    wchar_t tmp[64];
    // dimmedMs is milliseconds; one hour is 3600000 ms. Keeping a decimal even below one
    // hour means "0.0 h" (nothing saved) stays distinct from "0 h" (a rounded claim).
    int n = swprintf(tmp, 64, L"%.1f h", (double)dimmedMs / 3600000.0);
    if (n < 0) n = 0;
    if (n > 63) n = 63;
    return WriteTrunc(out, cap, tmp, n);
}

int FormatWhen(unsigned long long tickOrDeltaMs, bool past, wchar_t* out, int cap) {
    if (!out || cap <= 0) return 0;
    wchar_t tmp[64];
    int n = 0;
    if (past) {
        unsigned long long h = (tickOrDeltaMs / 3600000ULL) % 24ULL;
        unsigned long long m = (tickOrDeltaMs / 60000ULL) % 60ULL;
        n = swprintf(tmp, 64, L"%02llu:%02llu", h, m);
    } else {
        unsigned long long m = tickOrDeltaMs / 60000ULL;
        n = swprintf(tmp, 64, L"%llu min", m);
    }
    if (n < 0) n = 0;
    if (n > 63) n = 63;
    return WriteTrunc(out, cap, tmp, n);
}

}  // namespace sessionstats
