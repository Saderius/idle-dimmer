# Windows full-screen detection — research report

> **Provenance** — produced 2026-10-17 by a dispatched research agent, model `opencode/beam-free`, via web search. Author confidence tags (`CONFIRMED` / `LIKELY` / `UNVERIFIED`) are the *author's*, not independently audited. The orchestrator re-fetched the primary source for exactly one claim — the `QUERY_USER_NOTIFICATION_STATE` enum values — which is confirmed and which **contradicts** part of this report's framing; see `2026-10-17-fullscreen-and-cutscene-findings.md` §3.1 and §5.1. Read that file first.
>
> Claim-level verification status, sources, and the resulting root cause are recorded there. Nothing in this report has been implemented or run.

Scope: native Win32 C++ tray app that suppresses screen dimming when a full-screen app is present. Current heuristic uses `SHQueryUserNotificationState` plus a foreground-window-rect-covers-monitor test. This report evaluates that heuristic and surveys better options.

## Summary

- **The rect heuristic is the de facto standard**, used by PowerToys (`WindowsInteropHelper.IsWindowFullscreen`) and ModernFlyouts, which compare the foreground window's rect to the *monitor's full bounds* (not the work area). CONFIRMED [S1, S2].
- **A maximised window can match the test** when the taskbar auto-hides, because the work area then equals the monitor; the test cannot distinguish "maximised" from "fullscreen". CONFIRMED [S3, S4].
- **`SHQueryUserNotificationState` is documented only as a *notification propriety* API**, not a fullscreen detector; `QUNS_RUNNING_D3D_FULL_SCREEN` is documented as "exclusive mode Direct3D", and fullscreen optimizations (FSO) commonly make it return `QUNS_BUSY` instead. CONFIRMED [S5, S6].
- **DXGI swapchain state is not cross-process observable**: `IDXGISwapChain::GetFullscreenState` operates on a swap chain your own process created; there is no API to enumerate another process's swap chains. CONFIRMED (by absence) [S7, S8].
- **No single API gives reliable fullscreen status**; shipped products combine `SHQueryUserNotificationState` with the rect heuristic and special-case desktop/console windows. LIKELY [S1, S2].

## Window styles

Documented styles (Microsoft Learn, "Window Styles") [S9]:

- `WS_OVERLAPPED` (0) — overlapped window with title bar and border (same as `WS_TILED`).
- `WS_POPUP` (0x80000000) — pop-up window; cannot combine with `WS_CHILD`. Used for splash screens/overlays and, by convention, for borderless fullscreen windows.
- `WS_CAPTION` — title bar.
- `WS_THICKFRAME` — sizing border (`WS_SIZEBOX`).
- `WS_OVERLAPPEDWINDOW` = `WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX`.

`WS_EX_TOPMOST` (extended style) makes a window overlap all non-topmost windows regardless of foreground status [S10]. It indicates z-order, not fullscreen.

Raymond Chen's documented fullscreen recipe [S11] removes `WS_OVERLAPPEDWINDOW` and resizes the window to `rcMonitor` (the *whole* monitor, including the taskbar area); the taskbar then "automatically gets out of the way". This implies a true fullscreen-by-convention window has **no** `WS_OVERLAPPEDWINDOW` bits and covers `rcMonitor`.

What modern games actually use (UNVERIFIED exact combinations, but the following is supported):

- Legacy exclusive fullscreen (DirectDraw/D3D9/D3D11 `SetFullscreenState(TRUE)`): changes the display mode; the window typically becomes a `WS_POPUP` covering the monitor. CONFIRMED for the style pattern [S11, S12].
- Borderless windowed ("fullscreen optimizations"/eFSE on Win10+): the DWM keeps ownership; the window is usually a normal overlapped window with borders removed, or a `WS_POPUP`, sized to the monitor. CONFIRMED that D3D12 has no true exclusive mode and uses an emulated path [S13].
- "Exclusive fullscreen" in modern titles is frequently emulated (eFSE) rather than a real mode change [S13, S14].

Conclusion: **window styles alone are not a reliable fullscreen signal.** A game in borderless windowed mode may still carry `WS_CAPTION`/`WS_THICKFRAME` bits until it removes them, and non-fullscreen apps can be `WS_POPUP` + topmost (splash screens, OSDs). The MS Q&A answer recommends combining the rect test with a check that `WS_OVERLAPPEDWINDOW` is absent [S15]. UNVERIFIED whether that check is safe in practice (some fullscreen apps keep overlapped bits).

## Failure modes of the rect heuristic

The heuristic in question:

```cpp
r.left<=m.rcMonitor.left && r.top<=m.rcMonitor.top &&
r.right>=m.rcMonitor.right && r.bottom>=m.rcMonitor.bottom
```

| Case | Result | Why |
|---|---|---|
| Maximised window, taskbar auto-hidden | False positive | Work area == monitor, so the maximised rect equals `rcMonitor` [S3, S4] |
| Window manually resized to fill monitor | False positive | Covers `rcMonitor` without being fullscreen |
| Console window (`ConsoleWindowClass`) | False negative (unless special-cased) | Console windows report negative coordinates [S1, S2] |
| Desktop window (`Progman`/`WorkerW`) | False positive if not filtered | Covers monitor; desktop icons window [S1, S2] |
| Win+Tab Flip3D (`Flip3D`) | False positive if not filtered | Covers monitor [S1, S2] |
| Exclusive fullscreen, rect is 1px larger than monitor (invisible borders) | OK with `<=/>=` (missed by `==` equality) | Godot reports `[-1,-1,W+1,H+1]` in fullscreen [S16] |
| Window on a secondary monitor with negative virtual-desktop coords | OK *if* `MonitorFromWindow` is used; false negative/positive if compared to primary monitor only | Virtual desktop origin is top-left of primary; secondary monitors have negative coords [S17] |
| Non-DPI-aware foreground process on a high-DPI monitor | False negative/positive | `GetWindowRect` returns virtualized coordinates; mismatch with physical monitor rect [S18, S19] |
| Cloaked window (other virtual desktop, suspended UWP) covering a monitor | False positive | Cloaked windows keep their rect but are not visible; filter via `DWMWA_CLOAKED` [S20] |
| `GetWindowRect` vs. visible bounds | Minor mismatch | `GetWindowRect` includes invisible resize borders; use `DWMWA_EXTENDED_FRAME_BOUNDS` for visible bounds [S21] |
| Exclusive fullscreen where foreground hwnd is a launcher/overlay | False negative | The game window is not foreground |

Note: the provided code uses `<=/>=` (cover test), which is more tolerant than the `==` equality used by PowerToys/ModernFlyouts and avoids the 1px-border false negative. Good.

## Better APIs

### `DwmGetWindowAttribute`

- `DWMWA_CLOAKED` (value 14): returns `DWM_CLOAKED_APP` / `DWM_CLOAKED_SHELL` / `DWM_CLOAKED_INHERITED` if the window is cloaked. Cross-process queryable (any process can call on any HWND). Use to skip windows that are cloaked by the shell (e.g. on another virtual desktop) before the rect test. CONFIRMED [S20].
- `DWMWA_EXTENDED_FRAME_BOUNDS` (value 9): returns the visible frame bounds in screen space, excluding invisible resize borders. Documented note: "unlike the Window Rect, the DWM Extended Frame Bounds are not adjusted for DPI." Cross-process queryable. CONFIRMED [S21, S22].

Neither attribute directly reports fullscreen; they improve the rect test's inputs.

### DXGI

- `IDXGISwapChain::GetFullscreenState` reports fullscreen state of **that swap chain** (the calling process's own). CONFIRMED [S23].
- There is **no public API to enumerate or inspect another process's swap chains**; swap chains are process-local COM objects. An external process cannot observe exclusive-fullscreen state via DXGI. CONFIRMED (by absence in the DXGI docs and architecture) [S7, S8]. Marked LIKELY rather than CONFIRMED by a single doc line, because Microsoft does not publish a "not supported" statement; the DXGI programming guide contains no cross-process enumeration API.
- `IDXGIOutput`/`WaitForVBlank` and display-mode lists are per-output, not per-app.

### Display-mode change detection

- Entering **legacy exclusive fullscreen** changes the display mode; the system broadcasts `WM_DISPLAYCHANGE` to all top-level windows and `EnumDisplaySettings` reflects the new mode. Usable as a *partial* signal for FSE only. CONFIRMED that `WM_DISPLAYCHANGE` is a documented broadcast on mode change [S24]; CONFIRMED that borderless/eFSE does **not** change the mode [S13].
- Borderless windowed fullscreen produces **no** mode change, so this signal misses the common case. CONFIRMED [S13].

### AppBar / `SHAppBarMessage`

- `SHAppBarMessage(ABM_GETSTATE)` returns the taskbar's autohide/always-on-top state (`ABS_AUTOHIDE`, `ABS_ALWAYSONTOP`). Relevant to the auto-hide false positive above. CONFIRMED [S25].
- An appbar receives `ABN_FULLSCREENAPP` (lParam TRUE/FALSE) when a full-screen app opens/closes; the shell's definition of "full-screen" drives this. An external process could register as an appbar to receive it, but that requires a window and registering with the shell; reliability for borderless-windowed games is UNVERIFIED. CONFIRMED notification exists and its documented meaning [S26]; UNVERIFIED whether it covers borderless-windowed titles.

## `SHQueryUserNotificationState`: documented meanings

From Microsoft Learn, `QUERY_USER_NOTIFICATION_STATE` [S5]:

| Value | Documented meaning |
|---|---|
| `QUNS_NOT_PRESENT` (1) | Screen saver displayed, machine locked, or non-active Fast User Switching session. |
| `QUNS_BUSY` (2) | A full-screen application is running **or** Presentation Settings are applied. |
| `QUNS_RUNNING_D3D_FULL_SCREEN` (3) | A full-screen (exclusive mode) Direct3D application is running. |
| `QUNS_PRESENTATION_MODE` (4) | The user has activated Windows presentation settings to block notifications and pop-up messages. |
| `QUNS_ACCEPTS_NOTIFICATIONS` (5) | None of the other states; notifications may be sent. |
| `QUNS_QUIET_TIME` (6) | Windows 7+: first hour after a new user logs in (also after OS upgrade/clean install). Suppressed if another blocked state is active. |
| `QUNS_APP` (7) | Windows 8+: a Windows Store app is running. |

Key documented caveats [S5, S6]:

- `QUNS_PRESENTATION_MODE` is documented as the user activating Windows presentation settings — **not** a generic "slideshow/presentation running" state in the sense of a running app; it is the shell's presentation mode. CONFIRMED.
- "Note that there are no notifications sent when the user starts or stops a full-screen application." CONFIRMED [S5]. So polling is required.
- The Microsoft Q&A confirms that with fullscreen optimizations (FSO) enabled, `SHQueryUserNotificationState` returns `QUNS_BUSY` instead of `QUNS_RUNNING_D3D_FULL_SCREEN` for full-screen games [S6]. CONFIRMED.

On third-party shell interference (Windhawk, etc.): **UNVERIFIED.** `SHQueryUserNotificationState` is implemented in Shell32 and reflects state maintained by the shell (explorer.exe). A mod that hooks explorer.exe could in principle stale or corrupt that state, and Windhawk mods have been reported to crash/replace explorer.exe [S27], but no source documents SHQueryUserNotificationState being affected by any specific mod. Treat any such claim as speculation, not fact.

## What real products do

- **PowerToys (Microsoft)**, `WindowsInteropHelper.IsWindowFullscreen` [S1]: calls `SHQueryUserNotificationState` and returns true if `QUNS_RUNNING_D3D_FULL_SCREEN`; otherwise compares the foreground window's rect to `Screen.FromHandle(hWnd).Bounds` (full monitor, including taskbar). Special-cases `ConsoleWindowClass`, `Progman`/`WorkerW`, and `Flip3D`.
- **ModernFlyouts** [S2]: borrows the PowerToys code verbatim, including the comment linking to PowerToys. Same heuristic.
- **clawd-on-desk** [S28]: uses the same rect-vs-`rcMonitor` heuristic and explicitly documents that comparing to `rcMonitor` (not the work area) is what prevents a merely maximised window from being treated as fullscreen.
- **Microsoft Q&A / Raymond Chen** [S11, S15]: recommend the rect test plus absence of `WS_OVERLAPPEDWINDOW`.

No shipped product was found relying solely on DXGI or `DwmGetWindowAttribute` for fullscreen detection; all combine the shell query with the rect heuristic. UNVERIFIED whether any product uses `ABN_FULLSCREENAPP` or ETW `ForegroundWindowFullScreen` events in production.

## Sources

1. PowerToys `WindowsInteropHelper.cs` — https://raw.githubusercontent.com/microsoft/PowerToys/master/src/modules/launcher/PowerLauncher/Helper/WindowsInteropHelper.cs
2. ModernFlyouts `NativeMethods.cs` (fullscreen section) — https://github.com/ModernFlyouts-Community/ModernFlyouts/blob/main/ModernFlyouts.Core/Interop/NativeMethods.cs
3. Microsoft Learn, `SHAppBarMessage` / `ABM_GETSTATE` (autohide state) — https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shappbarmessage
4. Microsoft Q&A: taskbar auto-hide / maximised windows — https://www.elevenforum.com/t/how-to-autohide-the-task-bar-when-a-window-maximized.34156
5. Microsoft Learn, `QUERY_USER_NOTIFICATION_STATE` — https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ne-shellapi-query_user_notification_state
6. Microsoft Q&A: `SHQueryUserNotificationState` returns `QUNS_BUSY` — https://learn.microsoft.com/en-us/answers/questions/1086527/shqueryusernotificationstate-returns-quns-busy-ins
7. Microsoft Learn, DXGI overview — https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi
8. Microsoft Learn, `IDXGISwapChain::SetFullscreenState` — https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-setfullscreenstate
9. Microsoft Learn, Window Styles — https://learn.microsoft.com/en-us/windows/win32/winmsg/window-styles
10. Microsoft Learn, Window Features (topmost) — https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features
11. Raymond Chen, "How do I switch a window between normal and fullscreen?" — https://devblogs.microsoft.com/oldnewthing/20100412-00/?p=14353
12. Microsoft Learn, `GetWindowRect` (DWM extended frame bounds note) — https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowrect
13. Walbourn, "Care and Feeding of Modern Swap Chains (part 3)" (eFSE) — https://walbourn.github.io/care-and-feeding-of-modern-swap-chains-3
14. Blur Busters Forums, Windows 10 FSO — https://forums.blurbusters.com/viewtopic.php?t=4890
15. Microsoft Q&A: detecting fullscreen — https://learn.microsoft.com/en-us/answers/questions/1516325/is-there-a-way-to-detect-when-another-app-enters-f
16. Godot issue #78530 (1px border in fullscreen) — https://github.com/godotengine/godot/issues/78530
17. Screen coordinate guide (negative virtual-desktop coords) — https://screencoordinates.com/screen-coordinates-platform-guide
18. StackOverflow: `DwmGetWindowAttribute` extended frame bounds and DPI — https://stackoverflow.com/questions/49002910/why-does-dwmgetwindowattribute-with-dwmwa-extended-frame-bounds-behave-unexpecte
19. WPF window positioning & DPI — https://www.scichart.com/blog/window-positioning-in-wpf-applications
20. Microsoft Learn, `DWMWINDOWATTRIBUTE` (`DWMWA_CLOAKED`) — https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute
21. Microsoft Learn, `DwmGetWindowAttribute`/`DWMWA_EXTENDED_FRAME_BOUNDS` — https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute
22. pywin32 issue #1801 (extended frame bounds) — https://github.com/mhammond/pywin32/issues/1801
23. Microsoft Learn, `IDXGISwapChain::GetFullscreenState` — https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getfullscreenstate
24. Microsoft Learn, `WM_DISPLAYCHANGE` — https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-displaychange (UNVERIFIED URL; cited via search)
25. Microsoft Learn, `SHAppBarMessage` — https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shappbarmessage
26. Microsoft Docs, `ABN_FULLSCREENAPP` — https://github.com/MicrosoftDocs/win32/blob/docs/desktop-src/shell/abn-fullscreenapp.md
27. Windhawk issue #894 (explorer crash) — https://github.com/ramensoftware/windhawk/issues/894
28. clawd-on-desk PR #562 (fullscreen detection) — https://github.com/rullerzhou-afk/clawd-on-desk/pull/562

## Open questions I could not resolve

- Whether `ABN_FULLSCREENAPP` fires for borderless-windowed (eFSE) titles or only for mode-changing exclusive fullscreen. UNVERIFIED.
- Whether ETW `Microsoft-Windows-Win32k` `ForegroundWindowFullScreen` Start/Stop events are stable/public enough to consume in production; only documented in a Q&A post [S15]. UNVERIFIED.
- Whether checking `!(style & WS_OVERLAPPEDWINDOW)` in addition to the rect test reduces false positives or introduces false negatives (e.g. games that keep overlapped bits). UNVERIFIED.
- Exact window-style combinations used by modern DX12 titles in borderless mode. UNVERIFIED (no authoritative source found).
- Whether Windhawk-style explorer hooks affect `SHQueryUserNotificationState`. UNVERIFIED (no evidence either way).
- Cross-process DXGI: no document explicitly states "not observable"; conclusion rests on the absence of any enumeration API in the DXGI reference. UNVERIFIED by direct statement.
