#pragma once
// Tray gesture classification, kept pure so --self-test can pin it (AGENTS.md section 5).
//
// Under NOTIFYICON_VERSION_4 the uCallbackMessage is built as MAKELPARAM(notificationCode,
// iconId): LOWORD(lParam) is the code, HIWORD(lParam) is the icon id, and wParam carries the
// cursor anchor via GET_X_LPARAM / GET_Y_LPARAM. See NOTIFYICONDATAW.
//
// Only real user gestures may open the panel. Everything else - balloon lifecycle, popup
// lifecycle, hover - must be ignored, or the panel opens by itself about 10 seconds after
// launch when the welcome balloon appears.
//
// The codes below are the ones this build's own shellapi.h defines:
//   NIN_SELECT WM_USER+0, NIN_KEYSELECT NIN_SELECT|NINF_KEY, NIN_BALLOONSHOW WM_USER+2,
//   NIN_BALLOONHIDE WM_USER+3, NIN_BALLOONTIMEOUT WM_USER+4, NIN_BALLOONUSERCLICK WM_USER+5,
//   NIN_POPUPOPEN WM_USER+6, NIN_POPUPCLOSE WM_USER+7.
//
// Requires windows.h and shellapi.h to be included first. Include guard is #pragma once; the
// header has exactly one use site, in IdleDimmer.cpp.
enum TrayAction { trayIgnore = 0, trayToggle = 1 };
// A tray press begun while the panel was visible closes it even if click-away already
// finished the exit before the shell delivers button-up. Keyboard selection still toggles.
static inline bool TrayPressClosesPanel(UINT code,bool pressStartedOpen) {
    return pressStartedOpen && (code==WM_LBUTTONUP || code==WM_CONTEXTMENU);
}
static inline TrayAction ClassifyTrayMessage(UINT code) {
    switch(code){
    case WM_LBUTTONUP:     // left click
    case WM_CONTEXTMENU:   // right click (keyboard shortcut menu under VERSION_4)
    case NIN_SELECT:       // keyboard/mouse activate
    case NIN_KEYSELECT:    // keyboard Space/Enter
        return trayToggle;
    default:
        // WM_LBUTTONDBLCLK is deliberately absent. A double-click arrives as
        // LBUTTONUP + DBLCLK + LBUTTONUP; handling DBLCLK made that three toggles, which is
        // the visible flicker. Ignoring it leaves two toggles, i.e. open then close.
        return trayIgnore;
    }
}

// One physical left click does NOT arrive as one notification. Measured 2026-10-15 by
// capturing every delivery on the live app, one click on this taskbar produced, in order:
//
//   19:50:19.201  TRAY code=513  WM_LBUTTONDOWN
//   19:50:19.203  TRAY code=514  WM_LBUTTONUP
//   19:50:19.229  TRAY code=1024 NIN_SELECT        <- the icon's activation callback
//
// 513 is not a gesture, but 514 and 1024 both are, so a single click called TogglePanel twice
// 12-47 ms apart: the panel opened and closed inside a frame. That is the flicker. A right click
// is a single gesture by contrast (516 RBUTTONDOWN, 517 RBUTTONUP, 123 WM_CONTEXTMENU, of which
// only 123 counts), which is why right-clicking the icon opened the panel and it stayed open.
//
// So the activation half of a left click is collapsed into the click half. The window is 400 ms:
// the measured half-to-half gap peaked at 47 ms across a burst of real clicks, while two real
// clicks in that same burst were 410-630 ms apart. Only DISTINCT codes pair - two WM_LBUTTONUPs
// are two clicks however fast, which is what keeps a double click at two toggles (open, close).
static const DWORD kTrayPairWindowMs = 400;
static inline bool IsTrayGesturePair(UINT code,UINT previousCode) {
    return (code==WM_LBUTTONUP&&previousCode==NIN_SELECT)
        || (code==NIN_SELECT&&previousCode==WM_LBUTTONUP);
}

// The gesture decision, given the previous gesture and how long ago it was.
//   code             LOWORD(lParam) of this delivery.
//   previousCode     code of the last delivery that actually toggled, 0 if none yet.
//   msSincePrevious  milliseconds since that toggle. The caller must only advance
//                    previousCode when a toggle happened, so that an ignored balloon or
//                    hover delivery cannot reset the pairing window underneath a real click.
//   openState        1 if the panel is visible. Carried for a later rule (suppressing a
//                    toggle that would open an already-open panel); the contract pinned in
//                    --self-test does not depend on it yet.
static inline TrayAction ResolveTrayGesture(UINT code,UINT previousCode,DWORD msSincePrevious,UINT openState) {
    (void)openState;
    if(ClassifyTrayMessage(code)!=trayToggle)return trayIgnore;
    if(previousCode&&msSincePrevious<=kTrayPairWindowMs&&IsTrayGesturePair(code,previousCode))return trayIgnore;
    return trayToggle;
}
