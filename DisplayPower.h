#pragma once
#include <stdint.h>

// Pure display-off session decisions. No Windows API, input hook, timer or hardware access.
namespace displaypower {
constexpr uint64_t kManualSettleMs = 5000;
enum Phase { idle, settling, offRequested };

inline bool HasMouseAction(long dx, long dy, unsigned buttons) {
    return dx != 0 || dy != 0 || buttons != 0;
}
inline bool After(uint32_t eventTick, uint32_t fence) {
    return (int32_t)(eventTick - fence) > 0;
}

struct Session {
    Phase phase = idle;
    uint32_t generation = 0;
    uint64_t due = 0;
    uint32_t inputFence = 0;
    bool manual = false;

    bool Active() const { return phase != idle; }
    bool OffRequested() const { return phase == offRequested; }
    bool Begin(uint64_t now, bool fromButton, bool inputReady) {
        if (Active() || !inputReady) return false;
        ++generation;
        manual = fromButton;
        phase = settling;
        due = now + (manual ? kManualSettleMs : 0);
        return true;
    }
    bool Commit(uint64_t now, uint32_t lastInputTick) {
        if (phase != settling || now < due) return false;
        inputFence = lastInputTick;
        phase = offRequested;
        return true;
    }
    bool CancelForInput(uint32_t idleMs, uint32_t threshold, bool manualDim) const {
        return phase == settling && !manual && !manualDim && idleMs < threshold;
    }
    bool AllowsWake(uint32_t eventTick, bool meaningful) const {
        return OffRequested() && meaningful && After(eventTick, inputFence);
    }
    void Cancel() {
        ++generation; // invalidates already-posted restore messages, including between sessions
        phase = idle;
        due = 0;
        manual = false;
    }
};

inline bool Checks(int* count = nullptr) {
    int n = 0;
    bool ok = true;
    auto check = [&](bool value) { ++n; ok = value && ok; };
    Session s;
    check(!s.Active());
    check(!s.Begin(100, true, false)); // no input subscription: never start a power request
    check(!s.Active() && s.generation == 0);
    check(s.Begin(100, true, true));
    const uint32_t first = s.generation;
    check(s.Active() && !s.OffRequested() && s.manual);
    check(s.due == 5100);
    check(!s.CancelForInput(0, 60000, false)); // manual settling input is expected
    check(!s.Begin(200, true, true)); // a second click must not create another off request
    check(s.generation == first && s.due == 5100);
    check(!s.AllowsWake(1000, true)); // settling input neither cancels nor restarts the countdown
    check(!s.Commit(5099, 4800));
    check(s.Commit(5100, 4800));
    check(s.OffRequested());
    check(!s.Commit(5101, 4800)); // one command per session, never an off loop
    check(!s.AllowsWake(4799, true));
    check(!s.AllowsWake(4800, true));
    check(!s.AllowsWake(4801, false)); // zero-motion notifications cannot wake the app
    check(s.AllowsWake(4801, true));
    s.Cancel();
    check(!s.Active() && !s.manual && s.generation != first);
    check(!s.AllowsWake(4802, true));
    check(!s.Commit(10000, 9000)); // queued timer after cancellation is harmless
    check(s.Begin(20000, false, true));
    check(s.due == 20000 && !s.manual);
    check(s.CancelForInput(0, 60000, false)); // automatic work loses to fresh input
    check(s.CancelForInput(59999, 60000, false));
    check(!s.CancelForInput(60000, 60000, false));
    check(!s.CancelForInput(0, 60000, true)); // manual dim's explicit second stage
    check(s.Commit(20000, 19000));
    check(s.AllowsWake(19001, true));
    check(!s.CancelForInput(0, 60000, false)); // a settled off session is not an idle decision
    s.Cancel();
    check(s.Begin(30000, true, true));
    s.Cancel(); // suspend, lock or disable during the manual countdown
    check(!s.Commit(35000, 34000));
    check(s.Begin(40000, false, true));
    check(s.Commit(40000, 0xfffffff0u));
    check(s.AllowsWake(0x10u, true)); // 32-bit Windows input timestamps wrap
    check(!s.AllowsWake(0xffffffefu, true));
    check(!HasMouseAction(0, 0, 0));
    check(HasMouseAction(1, 0, 0));
    check(HasMouseAction(0, -1, 0));
    check(HasMouseAction(0, 0, 1));
    // Repeated device chatter must neither wake nor generate another off command.
    for (int i = 0; i < 100; ++i) {
        check(!s.AllowsWake(0x20u + (uint32_t)i, false));
        check(!s.Commit(41000 + (uint64_t)i, 0x20u));
    }
    if (count) *count = n;
    return ok;
}
} // namespace displaypower
