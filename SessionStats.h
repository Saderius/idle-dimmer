// Session statistics for the power dashboard card.
//
// This module is deliberately pure: no window, no DC, no threads, no calls to GetTickCount.
// The scheduler and the dim/restore paths own the clock; this module only records what they
// tell it. That separation is what makes the counters testable in --self-test the same way
// Flyout.h's geometry is - a counter that reads the clock itself cannot be driven from a
// test without racing the real machine.
//
// Never-vs-zero: a tick of 0 is the sentinel "never happened". Counters that have never been
// set stay 0 and hasDimHistory stays false, so an honest blank (em dash) is distinguishable
// from a real zero. Do not replace these with "0 means midnight" or any other default that
// looks like a measurement.
//
// Threading: single-threaded on the UI thread. No locks, no atomics - the callers (Check(),
// Dim(), Restore()) all run on the message thread. If that ever changes, this module must
// grow synchronisation; until then, locking here would be dead weight.
#pragma once
#include <windows.h>

namespace sessionstats {

struct Counters {
    unsigned long      wakeCount;       // scheduler wakeups since Reset()
    unsigned long      dimCount;        // transitions into the dimmed state
    unsigned long      restoreCount;    // transitions back to full brightness
    unsigned long long lastDimTick;     // caller-supplied tick; 0 means "never dimmed"
    unsigned long long lastRestoreTick; // caller-supplied tick; 0 means "never restored"
    unsigned long long dimmedMs;        // accumulated time spent below full brightness
    bool               hasDimHistory;  // true iff dimCount > 0
};

void Reset();
// Seed counters from persisted storage (e.g. across process restarts). Overwrites all fields;
// ticks are expected to be 0 when seeding from disk, since process ticks are not durable.
void Seed(const Counters& c);
// Each records ONE event. No timers, no threads, no GetTickCount reads inside: the caller
// passes the tick so the module stays pure and testable.
void NoteWake();
void NoteDim(unsigned long long tick);
void NoteRestore(unsigned long long tick);
// Call once per scheduler tick while the screen is dimmed, with the elapsed delta.
void AccumulateDimmed(unsigned long long deltaMs);

Counters Get();
bool HasDimHistory(const Counters& c);

// Formatting helpers. Return the number of wchar_t written (excluding the terminator),
// always NUL-terminate, and never fail: given a null/empty destination they must still
// write nothing and return 0.
//   "1 min" / "2 min" / "60 min" style minutes label
int FormatMinutes(int minutes, wchar_t* out, int cap);
//   "0.6 h" style saved-display time. Under one hour must still show a decimal
//   (e.g. "0.6 h"), not "0 h", because zero is a different claim from "less than an hour".
int FormatHoursSaved(unsigned long long dimmedMs, wchar_t* out, int cap);
//   Absolute wall-clock "HH:MM" from a tick delta, for "last dim 3 min".
//   past==true  -> "09:14"    (what time it happened)
//   past==false -> "3 min"    (how long ago, from a delta in ms)
int FormatWhen(unsigned long long tickOrDeltaMs, bool past, wchar_t* out, int cap);

}  // namespace sessionstats

// The self-test for this module lives in IdleDimmer.cpp next to FlyoutLayoutSelfTest, not here.
// Those suites use the file-static Check() helper so every individual assertion is counted and
// named; a bool-returning test buried in the module under test could only ever register as one
// check, and AGENTS.md 5 requires the assertion count to be meaningful rather than merely higher.
