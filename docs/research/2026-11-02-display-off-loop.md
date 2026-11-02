# Display-off loop — candidate fix, 2026-11-02

## Status and user constraint

UNVERIFIED on real monitors. The user reported monitor on/off cycling, stopped the app, and
explicitly requested research/fixing without running it. This overrides the repository's
normal launch-after-change workflow for this task. IdleDimmer.exe is not executed in any mode,
including --self-test, --quit, --status or --diagnose. No monitor-power, brightness or input
commands are issued by the investigation. Only a separate hardware-free test program is run.

## Evidence preserved

OBSERVED 2026-11-02 from the existing activity.log: last off entry for the reported incident
is `2026-11-02 13:44:49 Displays switched off`. The log does not record input packets, scheduler
restore causes, power messages, individual broadcast recipients, or physical display state.
It therefore does not establish why the monitor flashed repeatedly. No local reproduction
was attempted. Process inspection confirmed Idle Dimmer was stopped.

The before-change sources, build script, documentation gate and logs are copied under the
ignored `build-tools/display-off-fix-2026-11-02/before/` directory. Existing logs are never
truncated. The earlier safety audit in `2026-11-02-surface-monitor-off-safety-audit.md` describes
the pre-fix source, not this candidate.

SOURCE-INSPECTED 2026-11-02, before-change defects relevant to the report:

- Off used synchronous HWND_BROADCAST, which visits arbitrary application windows and can
  wait for them. The prior log records after that call, so it cannot show individual effects
  while the call is in progress. It is a plausible mechanism for repeated transitions, not
  proof that a hung application or the broadcast caused this particular incident.
- Off's input guard was 600 ms, not the five seconds the user expected. Wake handling ran
  before the empty-mouse-packet filter; an empty packet could take the display-awake claim.
- If dimmed was also true, WM_INPUT could post Restore independently of the off guard.
- The idle/media/fullscreen scheduler remained active during explicit off and could restore
  it independently of fresh user input. With off-at-idle enabled, that creates competing
  decisions. The old log cannot establish whether this happened in this incident.
- The footer attempted Dim before off, adding hardware-brightness traffic and slow monitor
  queries to a button whose requested purpose was screen-off.
- Suspend used Restore, which issued explicit power-on when marked off, competing with the
  OS's transition into sleep.

## Candidate behavior

UNVERIFIED 2026-11-02: manual monitor button hides the flyout, registers input successfully,
then enters one five-second pre-off settling period. Movement while settling neither cancels
nor restarts it. After settling, one OS display-off request is made on the app's own window
through documented DefWindowProcW / SC_MONITORPOWER. The manual off path performs no WMI/DDC
brightness discovery or writes. It does not broadcast to other applications or repeatedly
force monitors dark. It does not install a global input-blocking hook.

The settling delay is **before off**. Windows can wake displays independently of the app's
WM_INPUT handling, so no promise is made that Windows input is suppressed after off. This
choice gives the user time to leave the mouse before the one off request.

A pure displaypower::Session in DisplayPower.h owns the off phase, deadline, generation and
last-input timestamp fence. It prevents duplicate commits and rejects wake during settling,
empty mouse packets, and stale events from before the committed off request. Deferred restore
and automatic-off messages carry the generation; a stale queued event cannot wake a later
session. Meaningful mouse movement/button/wheel, keyboard or HID input after off can end it.
No key contents are logged.

Idle/media/fullscreen decisions and their timer are paused during the active session. The
manual off operation remains active regardless of media playback. After an accepted input
restore, the scheduler is re-armed with a full idle-threshold grace period. There is no retry
power-off timer while settled off. Automatic off-instead-of-dim shares the one-shot path,
without the manual delay, and rechecks recent input before dispatch. The existing delayed
second stage after a successful brightness dim remains supported.

Lock/suspend cancel off state and queued work without an explicit power-on request. Suspend
releases execution requirements. Resume/unlock re-arm the scheduler; an automatic maintenance
resume does not deliberately retake the display-awake claim. The normal existing enabled/
keep-system-awake policy is otherwise retained, including releasing it after wake when the
master switch is disabled.

LogDisplayPower temporarily appends display-power.log next to the executable. Transition
records contain session, phase, input fence, accepted event timestamp, aggregate ignored
notifications, and the off request's entry/return. No per-packet or frame logging occurs.
This supplies evidence for the user's later test; it is not itself proof of the diagnosis.
Remove temporary instrumentation before committing, per AGENTS.md.

## Reference implementation and documentation

READ 2026-11-02: Twinkle Tray's actual sleepDisplays implementation in src/electron.js supports
an OS-message mode and optional DDC modes. Its OS mode delays and posts the monitor-off command.
The previous assertion that Twinkle Tray only used DDC power writes was wrong. This candidate
keeps OS-managed off and does not add DDC power-state control.

- [Twinkle Tray source](https://github.com/xanderfrangos/twinkle-tray/blob/master/src/electron.js), sleepDisplays and sleep-displays IPC.
- [Microsoft WM_SYSCOMMAND](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-syscommand): documents carrying out a system command through DefWindowProc.
- [Microsoft SendMessageTimeoutW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw): a broadcast timeout can accumulate per recipient; merely replacing a broadcast with a nominal timeout is not a fixed total deadline.

## Verification and later user test

MEASURED 2026-11-02: standalone `tools/test-display-power.cpp` decision tests PASS, 240 checks,
including the five-second boundary, duplicate requests, stale events/timestamp wrap, cancellation,
fresh-input priority over queued automatic off, and repeated empty notifications. This program
has no Win32, WMI or DDC calls, and does not link or launch Idle Dimmer. All prior application
self-test assertions are retained and these checks are included for a later authorized run.

The app is built with `build.ps1 -NoRun -SkipIconRender`. -NoRun refuses to execute even --quit
when an app instance exists, and prevents auto-relaunch. Full app self-test: NOT RUN by user
instruction. Monitor behavior and hardware safety of this candidate: NOT LIVE-VERIFIED.

When the user chooses to test later: press the footer monitor button once, leave the controls
alone for the settling delay, then provide fresh movement/key input. If it flashes or wakes
unexpectedly, stop the app and inspect display-power.log before another change. The command
entry/return, session sequence and input fence will distinguish repeated app requests from
OS/driver wake behavior. Do not synthesize real input for this test.

Final build artifact, MEASURED 2026-11-02: 287,744 bytes; SHA-256
`FE82764DE3ABC48D8FFEE6BC0FEA12EF87161A62DA17ED143356A5D705D8B098`.
Build, icon and documentation gates PASS. Full app self-test NOT RUN. Final independent
process inspection confirms the app is stopped; before/after activity.log and diagnostics.txt
are unchanged. No app or monitor-control command was executed during this change.

## Authorized live-review follow-up — 2026-11-02

The user returned and explicitly asked to proceed, superseding the initial no-run constraint. The artifact hash matches the built-only candidate above. Full self-test MEASURED PASS (2,368 checks). The app was launched on the interactive desktop for the user to test. No screen-off request, hardware-brightness test or synthetic input was triggered by the agent. Hardware behavior remains UNVERIFIED pending user feedback. Earlier stopped/no-run statements describe the initial build-only phase.

## User confirmation and commit cleanup — 2026-11-02

USER-CONFIRMED 2026-11-02: "behavior generally seems correct now. commit". The live log from
PID 49956 records two manual sessions (16:17 and 16:18 local time), each with one off command,
its return, fresh input, and one on command. This supports the user's general desktop verdict;
it does not prove the exact original cause or certify Surface/suspend/other monitor behavior.

Temporary LogDisplayPower calls/helper, the ignored-input diagnostic counter, and their doc-gate
entry were removed before committing, as required by AGENTS.md. The off/wake decisions, input
filter/fence, timer/session ownership, and power commands are preserved. Before-cleanup source
and the original append-only log are retained under ignored
`build-tools/display-off-fix-2026-11-02/pre-commit/`. Earlier sections describe the instrumented
candidate and no-run phase historically; the current source does not write display-power.log.

MEASURED 2026-11-02, post-review build with temporary logging removed: 286,208 bytes;
full self-test PASS (2,368 checks), with all original assertions retained. Build/doc/icon
gates PASS. The cleaned app is relaunched for normal use. No monitor-power or synthetic-input
test was triggered by the agent; the user-confirmed behavior refers to the preceding live test.
