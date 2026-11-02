#pragma once
#include <windows.h>

// Windows owns backdrop sampling and composition. This module exists only while the panel is open.
namespace nativebackdrop {
constexpr UINT kFailureMessage = WM_APP + 56;
constexpr UINT kAnimationCompleteMessage = WM_APP + 57;
// wParam = (native session << 1) | exiting; lParam = signed screen Y.
constexpr UINT kAnimationUpdateMessage = WM_APP + 58;
// Approximation selected from the supplied Windows recording; exact shell timing is not known.
constexpr int kNativeSlideTravel = 96, kNativeSlideEntranceMs = 180, kNativeSlideExitMs = 150;
bool Allowed();
bool Start(HWND panel, int width, int height, bool dark, int dpi, const void* premultipliedBgra, bool animate = false);
bool AnimateShow();
bool AnimateHide();
bool Reveal();
bool FirstFrameReady();
bool Cloaked();
unsigned Session();
const wchar_t* AnimationPhase();
HRESULT AnimationError();
const wchar_t* AnimationStage();
unsigned CompletedAnimations();
bool Update(const void* premultipliedBgra, int width, int height);
void Stop();
bool Running();
bool WorkerAlive();
HRESULT LastError();
const wchar_t* Stage();
unsigned Frames();
}
