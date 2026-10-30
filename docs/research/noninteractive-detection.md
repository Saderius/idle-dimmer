# Non-interactive detection on Windows: can an external observer tell a cutscene from gameplay?

> **Provenance** — produced 2026-10-17 by a dispatched research agent, model `opencode/beam-free`, via web search. Author confidence tags are the *author's*, not independently audited; the orchestrator read the citations but did not re-fetch them. Nothing here has been implemented or run.
>
> Consolidated findings, verification status, and the still-open GPU question: `2026-10-17-fullscreen-and-cutscene-findings.md`. Read that file first.

Research report for the Idle Dimmer tray app. Question under test: **is "the foreground app is showing non-interactive video" an observable property of a process from outside that process?**

Date of research: 2026-10-17. All URLs accessed 2026-10-17.

## Summary

- **GSMTC is opt-in.** `GlobalSystemMediaTransportControlsSessionManager` only enumerates apps that *integrated* `SystemMediaTransportControls`. The OS does not auto-detect media playback; the app must register a session. (HIGH confidence, Microsoft Learn + Old New Thing.)
- **No game engine publishes a media session for cutscenes by default.** One third-party Unreal plugin ("System Media Controller") bridges Unreal to SMTC, proving integration is *possible*, but there is no evidence any shipping game registers its in-engine cutscenes as a media session. (MEDIUM confidence — the negative is an absence-of-evidence finding.)
- **"Video playing" is not a queryable OS state.** Media Foundation (`MFCreateSourceResolver`, `IMFSourceReader`) operates on a caller-supplied URL/stream; there is no cross-process enumeration of another app's MF pipeline. DirectShow graphs are only visible via the Running Object Table if the process registered them (opt-in). (HIGH confidence.)
- **Audio sessions ARE enumerable cross-process** and attributable to a PID via `IAudioSessionManager2` + `IAudioSessionControl2::GetProcessId`, with an Active/Inactive/Expired state via `IAudioSessionControl::GetState`. But audio activity does **not** distinguish a cutscene from gameplay — both produce audio, and a game's audio session is Active during both. (HIGH confidence on API; HIGH confidence on the non-distinction.)
- **Foreground-window APIs only give identity, not activity.** `GetForegroundWindow` + `GetWindowThreadProcessId` tell you *which* process owns the foreground window; nothing documented reports whether that window is repainting/animating. (HIGH confidence.)
- **Input signals cannot separate "watching a cutscene" from "walked away."** `GetLastInputInfo` only records last input time; during a cutscene and during true idleness the user presses nothing, so the signals are identical. (HIGH confidence.)
- **No "cutscene"/"presentation mode" concept exists for third parties.** GameBar/GameDVR/`Windows.Gaming.Input`/`Windows.Graphics.Capture` expose capture, controller input, and overlay visibility — none expose game state or playback state. (HIGH confidence.)
- **No commercial product solves this precisely.** Battery-saver/game-mode features key off process identity (known game list) or media sessions, not off "non-interactive video." (MEDIUM — based on absence of documented mechanism; UNVERIFIED for specific vendor internals.)
- **Bottom line:** an external observer **cannot** distinguish a cutscene from gameplay. The best available proxy is **audio session activity of the foreground process** (keeps dimming suppressed while that process is producing audio), combined with the existing GSMTC check for media apps. This still fails for muted cutscenes and for games with constant ambient audio during menus. (HIGH confidence.)

## GSMTC: confirmed limits

`GlobalSystemMediaTransportControlsSessionManager` (namespace `Windows.Media.Control`) "Provides access to playback sessions throughout the system that have integrated with SystemMediaTransportControls to provide playback info and allow remote control." (Microsoft Learn, GlobalSystemMediaTransportControlsSessionManager Class.)

Key documented properties (Microsoft Learn, same page):
- Requires the `globalMediaControl` app capability.
- Minimum Windows 10 1809.
- `GetSessions()` returns sessions from apps that *integrated* with SMTC; `GetCurrentSession()` returns the one the system thinks the user wants to control.
- `SessionsChanged` / `CurrentSessionChanged` events fire on connect/disconnect.

The crucial qualifier is "that have integrated with SystemMediaTransportControls": the OS does not instrument playback. An app must explicitly create a `SystemMediaTransportControls` instance and update its playback status. Raymond Chen's Old New Thing post states: "Apps can publish their media playback information by using the SystemMediaTransportControls, but what about the other side of the coin?" and describes the manager as the retrieval side. (Old New Thing, 2023-11-08.)

**Games:** There is no evidence in the Microsoft documentation that any game engine registers cutscenes as a media session. A third-party Unreal Engine plugin ("System Media Controller", listed on Fab/Unreal forums) bridges Unreal's Blueprint layer to the OS media transport infrastructure, proving a game *could* publish a session if it opted in. (Fab listing; Unreal forums.) But nothing indicates this is standard, and cutscene playback in-engine does not go through SMTC. Therefore GSMTC visibility for games is effectively zero in practice, and any single game integrating it would be the exception. (MEDIUM confidence on "no shipping game does"; the existence of the plugin makes an absolute claim unsupportable.)

## Is "video playing" observable cross-process?

**Media Foundation — no.** `MFCreateSourceResolver` "Creates the source resolver, which is used to create a media source from a URL or byte stream" (Microsoft Learn, MFCreateSourceResolver). `IMFSourceResolver` "Creates a media source from a URL or a byte stream." `IMFSourceReader` processes media data from a source the caller opens. These APIs take a URL/stream supplied by the *calling* process and build a pipeline inside that process. There is no documented function to enumerate another process's Media Foundation topology, source reader, or playback state. (Microsoft Learn, Source Reader overview; MFCreateSourceResolver.) Conclusion: Media Foundation is a per-process media *processing* library, not a system playback monitor. (HIGH confidence.)

**DirectShow — only opt-in.** DirectShow graphs can be published to the global Running Object Table so another process can attach ("Loading a Graph from an External Process"). Registration is performed by the graph-owning process via `IRunningObjectTable`/`GetRunningObjectTable`; the ROT "maintains a table of the objects that have been registered as running on that computer." (Microsoft Learn, IRunningObjectTable; GetRunningObjectTable; DirectShow doc.) Modern media players do not register their graphs, and there is no documented enumeration of "graphs currently rendering video." (HIGH confidence that ROT is opt-in; MEDIUM that no modern player registers.)

**OS-level "video playing" state — none documented.** The only system-wide media query surface found is GSMTC. No documented API returns "a video is currently being presented." (UNVERIFIED absence; based on searches of Microsoft Learn.)

## Audio sessions as a signal

**Enumeration and PID attribution — yes, documented.** `IAudioSessionManager2::GetSessionEnumerator` returns an `IAudioSessionEnumerator` over sessions "active on the audio device." (Microsoft Learn.) `IAudioSessionControl2::GetProcessId` "retrieves the process identifier of the audio session," returning `AUDCLNT_S_NO_SINGLE_PROCESS` when the session spans multiple processes. (Microsoft Learn.) `IAudioSessionControl::GetState` returns `AudioSessionStateActive` "if the session has one or more streams that are running," `AudioSessionStateInactive` when the last running stream stops, and `AudioSessionStateExpired` when the last stream is released. (Microsoft Learn.) So an external process can enumerate audio sessions, attribute them to PIDs, and observe Active/Inactive transitions. (HIGH confidence.)

**Can audio distinguish cutscene from gameplay? No.** Both gameplay and a cutscene produce audio streams; the game's audio session stays `AudioSessionStateActive` throughout. Audio activity therefore cannot tell whether the user is interacting. It can only tell whether the foreground process is currently emitting sound. (HIGH confidence — follows from the state definitions above plus the observation that games output audio in both states; this non-distinction is a logical consequence, not a measured fact, and is flagged as such.)

Additional limits: sessions are per audio endpoint; an app rendering to a non-default endpoint or muted at the app level may not appear. Enumeration requires COM init and the same session (audio sessions are per Windows session; a service cannot see interactive-session sessions — Stack Overflow report of a service seeing only the master session). (MEDIUM confidence on cross-session invisibility; supported by the service-mode report.)

## Foreground / activity signals

`GetForegroundWindow` returns "a handle to the foreground window (the window with which the user is currently working)." `GetWindowThreadProcessId` (not separately fetched here; standard Win32, UNVERIFIED in this report beyond general knowledge — treat as known) maps an HWND to a PID. So identity of the foreground process is available. (HIGH confidence for GetForegroundWindow.)

Nothing documented reports whether the foreground window's thread is currently painting, animating, or producing output. There is no "is this window presenting frames" query in the Win32 windowing API as found. (UNVERIFIED absence.) `Windows.Graphics.Capture` can acquire frames from a window/display, but only after the user consents via the system picker, and it is a capture API, not a state query; it cannot be used silently to infer activity. (HIGH confidence from Microsoft Learn screen-capture doc.)

## Input signals cannot separate these states

`GetLastInputInfo` "Retrieves the time of the last input event" and is "useful for input idle detection," but "does not provide system-wide user input information across all running sessions. Rather, GetLastInputInfo provides session-specific user input information for only the session that invoked the function." (Microsoft Learn.) During a cutscene the user presses nothing; during true idleness the user also presses nothing. Both yield an ever-increasing idle time from `GetLastInputInfo`. The signals are therefore identical and cannot discriminate. (HIGH confidence.)

`GetRawInputData` / `GetAsyncKeyState` report current input state only; they cannot reconstruct whether the foreground app is consuming input. (UNVERIFIED specifics for GetRawInputData/GetAsyncKeyState — not fetched in this pass; stated from general API knowledge, flagged.)

## Does a "cutscene" concept exist for third parties?

**No.** Searches of Microsoft Learn and the WinRT API surface found no API exposing a game's playback/cutscene/presentation state:
- `Windows.Gaming.UI.GameBar` "Provides notifications and information about the visibility and input redirection state of Game bar" — about the overlay, not the game. (Microsoft Learn.)
- `Windows.Gaming.Input` exposes controller state, not game state.
- `Windows.Graphics.Capture` is a user-consented capture API.
- GameDVR/Xbox Game Bar SDK is for widget developers integrating with the overlay.

The Old New Thing post on Game Bar detection confirms the GameBar API's scope is the overlay's on-screen/input-redirection state, for apps that want to pause when the user opens the overlay. (Old New Thing, 2016-06-29.) No "cutscene," "non-interactive," or "presentation mode" concept is exposed to third parties. (HIGH confidence.)

## Precedent in commercial products

No commercial product was found that distinguishes cutscene from gameplay for screen dimming:
- **Windows Game Mode** keys off detection of a running game process (a known-game heuristic) and reprioritizes CPU/GPU; it does not expose or act on cutscene state. (Third-party summaries; Microsoft documentation referenced indirectly — UNVERIFIED for Microsoft's exact detection heuristic.)
- **NVIDIA App / GeForce Experience Battery Boost** and **AMD Adrenalin** power features are per-game/per-process toggles; no documented cutscene awareness. (UNVERIFIED internals.)
- **Steam Deck** game mode/idle handling is not documented to distinguish cutscenes. (UNVERIFIED — not researched in depth.)

Inference: vendors avoid the problem by either (a) not dimming during any fullscreen game (process-identity heuristic), or (b) relying on the user's own input idleness plus media-session detection for non-game video apps. No evidence of a cutscene-specific solution. (MEDIUM confidence.)

## Bottom line

An external observer **cannot** distinguish "the foreground app is playing a non-interactive cutscene" from "the foreground app is an interactive game the user has stopped touching." The two states are identical from every external signal examined (input idle time, foreground window identity, audio activity, media sessions, capture APIs). The difference exists only inside the game process.

Best available proxy if dimming must be suppressed during cutscenes: **treat the foreground process as "in use" while it has an Active audio session** (plus the existing GSMTC check for media apps). This suppresses dimming for voiced cutscenes and for any game emitting sound, at the cost of not dimming during silent cutscenes or during games whose audio is muted. A further heuristic (process is a known game binary) is process-identity-based, not state-based, and out of scope for this API question. (HIGH confidence on the conclusion; MEDIUM on proxy effectiveness, which is an engineering judgement.)

## Sources

- Microsoft Learn, "GlobalSystemMediaTransportControlsSessionManager Class (Windows.Media.Control)" — https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssessionmanager
- Microsoft Learn, "GlobalSystemMediaTransportControlsSession Class" — https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssession
- Old New Thing, "How can I get information about media playing on the system, and optionally control their playback?" (2023-11-08) — https://devblogs.microsoft.com/oldnewthing/20231108-00/?p=108980
- Fab, "System Media Controller — Native Media Control for Unreal Engine" — https://www.fab.com/listings/051fe835-4d42-42f8-8e30-70cb222e120d?lang=en
- Unreal forums thread on the same plugin — https://forums.unrealengine.com/t/bhfy-system-media-controller-native-media-control-for-unreal-engine/2747383
- Microsoft Learn, "IAudioSessionManager2 (audiopolicy.h)" — https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionmanager2
- Microsoft Learn, "IAudioSessionManager2::GetSessionEnumerator" — https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-getsessionenumerator
- Microsoft Learn, "IAudioSessionControl2::GetProcessId" — https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol2-getprocessid
- Microsoft Learn, "IAudioSessionControl::GetState" — https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol-getstate
- Microsoft Learn, "GetLastInputInfo function" — https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getlastinputinfo
- Microsoft Learn, "GetForegroundWindow function" — https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getforegroundwindow
- Microsoft Learn, "Screen capture (Windows.Graphics.Capture)" — https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture
- Microsoft Learn, "GameBar Class (Windows.Gaming.UI)" — https://learn.microsoft.com/en-us/uwp/api/windows.gaming.ui.gamebar
- Old New Thing, "How can I detect whether the Game Bar is covering my window?" (2016-06-29) — https://devblogs.microsoft.com/oldnewthing/20160629-00/?p=93775
- Microsoft Learn, "MFCreateSourceResolver function" — https://learn.microsoft.com/en-us/windows/win32/api/mfidl/nf-mfidl-mfcreatesourceresolver
- Microsoft Learn, "IMFSourceResolver interface" — https://learn.microsoft.com/en-us/windows/win32/api/mfidl/nn-mfidl-imfsourceresolver
- Microsoft Learn, "Source Reader" — https://learn.microsoft.com/en-us/windows/win32/medfound/source-reader
- Microsoft Learn, "IRunningObjectTable interface" — https://learn.microsoft.com/en-us/windows/win32/api/objidl/nn-objidl-irunningobjecttable
- Microsoft Learn, "GetRunningObjectTable function" — https://learn.microsoft.com/en-us/windows/win32/api/objbase/nf-objbase-getrunningobjecttable
- MicrosoftDocs win32, "Loading a Graph from an External Process" (DirectShow) — https://github.com/DN-Forks/MicrosoftDocs-win32/blob/docs/desktop-src/DirectShow/loading-a-graph-from-an-external-process.md
- DS4Windows deep-dive on Game Bar detection strategies — https://deepwiki.com/hbashton/DS4Windows/7.1-game-bar-detection-strategies

## Open questions I could not resolve

- Whether any shipping game (Unreal/Unity/id Tech) has ever registered an SMTC session for cutscenes. The Unreal plugin proves feasibility; absence of evidence is not evidence of absence. (UNVERIFIED.)
- Exact Windows Game Mode detection heuristic (how Windows decides a process is a game). Not found in Microsoft documentation in this pass. (UNVERIFIED.)
- Whether `GetRawInputData`/`GetAsyncKeyState` can be used to infer foreground-app input consumption. Not fetched; stated from general knowledge. (UNVERIFIED specifics.)
- Cross-session visibility of audio sessions (service vs interactive session) — supported by one Stack Overflow report only. (MEDIUM.)
- NVIDIA/AMD/Steam Deck internals for game-aware power handling — not documented publicly in a way found here. (UNVERIFIED.)