// Audio-playback fallback for "is the user watching or listening to something".
//
// WHY THIS FILE EXISTS
//
// The primary signal is Windows' own media session list (SMTC), read by MediaPlaying() in
// IdleDimmer.cpp. That contract is OPT-IN on the player's side: a Chromium browser only publishes
// a session while its "Hardware media key handling" integration is on, because the same
// MediaSessionService both handles media keys and registers the session. MEASURED 2026-11-01 on
// this machine, with Vivaldi and that flag off
// (Local State: browser.enabled_labs_experiments = ["hardware-media-key-handling@2"], 2 = Disabled):
//
//   YouTube playing  -> vivaldi's audio.mojom.AudioService process alive, +1.906 s CPU over a 2 s
//                       sample (audio really was rendering)
//   SMTC GetSessions -> EMPTY list, GetCurrentSession() = null, and --diagnose printed
//                       "Media: not playing" with HRESULT 0x00000000 (the call succeeded)
//   result           -> the app dimmed the screens over a playing video
//
// So the app was asking the right question of the wrong party. Audio rendering is not opt-in:
// every application that makes sound owns a WASAPI session on the default render endpoint. When
// SMTC reports nothing, this fallback asks WASAPI whether any non-system session there is in
// AudioSessionStateActive.
//
// TRADE-OFF, stated plainly: this signal cannot tell YouTube from Discord, a game, or a video
// call. Anything that renders audio suppresses dimming while it does. That is the cost of not
// depending on a browser flag; the user asked for exactly this after the measurement above.
// SMTC is still consulted first because it distinguishes Paused (which the protection also
// honours) and names the playing application.
//
// Sequencing note: a paused video stops rendering, so the session goes Inactive and dimming
// resumes. That is the intended behaviour - a paused tab is not "in use".
#pragma once
#include <windows.h>
#include <mmdeviceapi.h>   // IMMDeviceEnumerator, eRender
#include <audiopolicy.h>   // IAudioSessionManager2, IAudioSessionControl2

namespace audio {

// Minimal owning pointer. Deliberately not the app's Ptr: this header is included before it and
// must stay usable by a standalone harness, exactly like DdcBrightness.h.
template <class T>
struct Com {
    T* p;
    Com() : p(nullptr) {}
    ~Com() { if (p) p->Release(); }
    T** out() { return &p; }
    T* operator->() const { return p; }
    bool ok() const { return p != nullptr; }
private:
    Com(const Com&);
    Com& operator=(const Com&);
};

// Pure: whether one enumerated session counts as "the user is playing something".
//
//   * only AudioSessionStateActive counts - Active is what a session in the middle of rendering
//     reports, while Inactive and Expired are silent by definition;
//   * Windows' own notification/beep session is ignored, so a tray balloon or a device connect
//     chime cannot hold the display awake;
//   * this process is ignored for symmetry, even though the app renders no audio and therefore
//     owns no session.
//
// pid 0 means the query failed; such a session still counts, because the browser's audio service
// is the common case and refusing to see it would reproduce the bug this file exists to fix.
inline bool CountsSession(bool systemSounds, DWORD pid, DWORD selfPid, int state) {
    if (systemSounds) return false;
    if (selfPid && pid == selfPid) return false;
    return state == (int)AudioSessionStateActive;
}

// Pure: how to read a completed walk. A walk that found audio wins even if some part of it also
// errored, because "something is rendering" is a positive measurement. Only a walk that failed
// without finding anything is unknown - and unknown must pause dimming (fail-safe), never dim.
inline int Verdict(int activeSessions, HRESULT walkResult) {
    if (activeSessions > 0) return 1;
    return FAILED(walkResult) ? -1 : 0;
}

// One walk over the default render endpoint.
//   returns 1 = at least one counting session is Active, 0 = none, -1 = the audio stack could not
//   be queried at all.
//   *activeOut / *totalOut (optional) receive the counts, for --diagnose.
inline int AnyActive(HRESULT* errorOut, int* activeOut, int* totalOut) {
    if (errorOut) *errorOut = S_OK;
    if (activeOut) *activeOut = 0;
    if (totalOut) *totalOut = 0;

    // A cross-apartment enumerator: IdleDimmer runs single-threaded apartment (RoInitialize
    // RO_INIT_SINGLETHREADED), and CLSCTX_ALL lets COM marshal the in-proc server either way.
    Com<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)enumerator.out());
    if (FAILED(hr) || !enumerator.ok()) {
        if (errorOut) *errorOut = FAILED(hr) ? hr : E_NOINTERFACE;
        return -1;
    }
    Com<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.out());
    if (FAILED(hr) || !device.ok()) {
        if (errorOut) *errorOut = FAILED(hr) ? hr : E_NOINTERFACE;
        return -1;
    }
    Com<IAudioSessionManager2> manager;
    hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                          (void**)manager.out());
    if (FAILED(hr) || !manager.ok()) {
        if (errorOut) *errorOut = FAILED(hr) ? hr : E_NOINTERFACE;
        return -1;
    }
    Com<IAudioSessionEnumerator> sessions;
    hr = manager->GetSessionEnumerator(sessions.out());
    if (FAILED(hr) || !sessions.ok()) {
        if (errorOut) *errorOut = FAILED(hr) ? hr : E_NOINTERFACE;
        return -1;
    }
    int count = 0;
    hr = sessions->GetCount(&count);
    if (FAILED(hr)) {
        if (errorOut) *errorOut = hr;
        return -1;
    }
    if (totalOut) *totalOut = count;

    const DWORD selfPid = GetCurrentProcessId();
    int active = 0;
    for (int i = 0; i < count; i++) {
        Com<IAudioSessionControl> control;
        // A single unreadable session is not a failed walk: it is one session we cannot classify.
        // Failing the whole walk here would pause dimming on any odd audio driver.
        if (FAILED(sessions->GetSession(i, control.out())) || !control.ok()) continue;
        AudioSessionState state = AudioSessionStateExpired;
        if (FAILED(control->GetState(&state))) continue;
        bool systemSounds = false;
        DWORD pid = 0;
        Com<IAudioSessionControl2> control2;
        if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                              (void**)control2.out())) && control2.ok()) {
            systemSounds = control2->IsSystemSoundsSession() == S_OK;
            control2->GetProcessId(&pid);   // failure leaves pid 0, which still counts
        }
        if (CountsSession(systemSounds, pid, selfPid, (int)state)) active++;
    }
    if (activeOut) *activeOut = active;
    return Verdict(active, S_OK);
}

// Pure-logic checks, run from --self-test. No COM, no audio traffic.
// `checkCount` (optional) receives the number of assertions so the reported total grows with
// them, the way the other suites report theirs.
inline bool Checks(int* checkCount = nullptr) {
    int n = 0;
    bool ok = true;
    auto check = [&](bool value) { n++; ok = ok && value; };
    const DWORD self = 4242;
    // The measured YouTube case: a browser's audio session, active, not ours, not system sounds.
    check(CountsSession(false, 1234, self, (int)AudioSessionStateActive));
    check(!CountsSession(false, 1234, self, (int)AudioSessionStateInactive));
    check(!CountsSession(false, 1234, self, (int)AudioSessionStateExpired));
    check(!CountsSession(true, 1234, self, (int)AudioSessionStateActive));   // notification chime
    check(!CountsSession(false, self, self, (int)AudioSessionStateActive));  // our own process
    check(CountsSession(false, 0, self, (int)AudioSessionStateActive));      // pid unknown, real
    check(!CountsSession(false, 0, self, (int)AudioSessionStateInactive));
    check(Verdict(2, S_OK) == 1);
    check(Verdict(1, E_FAIL) == 1);          // found audio: the positive measurement wins
    check(Verdict(0, S_OK) == 0);            // silence
    check(Verdict(0, E_FAIL) == -1);         // stack unreachable: unknown, so pause dimming
    check(Verdict(0, E_ACCESSDENIED) == -1);
    if (checkCount) *checkCount = n;
    return ok;
}

}  // namespace audio
