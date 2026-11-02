# Focused dead-code and legacy-behavior audit

Reviewed 2026-11-02 after removal of the unexpected global screenshot shortcut. This pass changed
no app source or behavior. It distinguishes unused code from active legacy functionality: an
unused-function check would not have caught the shortcut because startup actually registered it.

## Findings

**Cleanup follow-up, 2026-11-02:** the confirmed unused functions and the temporary presentation
instrumentation below have now been removed, and the stale submenu claim corrected. Existing
logs are preserved as evidence. The fallback, ABI declarations, explicit CLI commands, and test
assertions remain. The table records the findings that motivated this cleanup, not outstanding
removals. MEASURED 2026-11-02, cleanup build: all five application translation units compile
with unused-function/unused-variable warnings treated as errors; build gates PASS; self-test
PASS with 2,128 checks; executable 284,160 bytes. No tests were removed, and the timing,
interpolation, position handling, and control repaint logic are unchanged.
MEASURED 2026-11-02, cleanup runtime check: native IPC open/close completed with HRESULT
`0x00000000`; the panel was hidden and native worker absent after closing. The earlier
presentation-trace file's length and modification time were unchanged across that cycle.
This verifies the removed logger is inactive in that check, not visual animation fidelity.

| Finding | Evidence | Recommended disposition |
|---|---|---|
| Unused `Mmss()` formatter | `Flyout.cpp:694`; compiler reports `-Wunused-function`; project search finds no callers. | Remove in a cleanup change. |
| Unused `acrylic::CaptureRegionToBmp()` API | Declaration in `Acrylic.h:96`, implementation in `Acrylic.cpp:690`; no callers in project source. External linkage explains why the unused-static-function warning does not flag it. | Remove declaration and implementation together after checking associated comments/docs. |
| Stale tray-submenu claim | `IMPLEMENTATION.md` section 6 still says the tray submenu offers presets; the app has one panel and the submenu was removed. | Correct the documentation. `kIdlePresets` itself is still used by slider-detent self-tests and must not simply be deleted with its assertions. |
| Active temporary presentation tracing | `TracePresentation`, `presentationAuditUntil`, and `presentation-trace.log` are still used by the motion/presentation path. | Treat as debug-code cleanup, not dead code. Preserve needed reproduction evidence, then remove instrumentation and gate references before committing. Its performance cost has not been individually measured. |

## What remains intentionally reachable

- Software acrylic is still selected on native initialization failure. `AcrylicLive`, software
  screenshot handling, and software entrance/exit code therefore have callers and conditions.
  Removing that renderer would change compatibility behavior, not merely delete dead code.
- `--acrylic-test`, `--acrylic-pause`, and `--panel-shot` remain explicit CLI audit paths. Whether
  to retain those commands is a product-scope decision; they are not unused functions.
- Uncalled COM methods in `NativeCompositionAbi.h` and interface prefixes preserve vtable slots.
  Deleting those slots would change ABI layout and can break calls to the methods that are used.
- Win32 callbacks and handlers can be reached through registered pointers/messages rather than
  direct C++ calls. Reference counts alone are not sufficient to establish that they are dead.
- The visible-panel mouse hook and timers are lifecycle-controlled behavior, unlike the removed
  always-registered global hotkey. A follow-up cleanup should preserve their hide/shutdown paths.

## Verification and limits

MEASURED 2026-11-02: all five current application translation units (`IdleDimmer.cpp`, `Flyout.cpp`,
`Acrylic.cpp`, `NativeBackdrop.cpp`, `SessionStats.cpp`) compiled separately with the existing Zig
toolchain and unused-function/unused-variable warnings. Every object compile succeeded; `Mmss`
was the only reported unused function. Objects were written under ignored
`build-tools/deadcode-audit-2026-11-02/`; the application executable was not replaced.

An initial multi-input syntax-only invocation encountered a Zig driver error and is not counted
as a successful check; the individual object compiles above supersede it.

Source search found no remaining `RegisterHotKey`, `UnregisterHotKey`, `WM_HOTKEY`, or
`kAcrylicHotkey` references in app C/C++ files. The running shortcut-removal build's executable
has no `RegisterHotKey` reference. This confirms removal of that known registration; it is not
a blanket certification of all possible legacy behavior.

The build already uses function/data sections and linker garbage collection. Unreachable
helpers can therefore be omitted from the executable without being removed from source, but
source cleanup still reduces confusion. Compiler warnings do not establish whole-program
reachability or prove that registered behavior is desired. A subsequent cleanup should address
these concrete findings, check registrations/timers/hooks and user-visible documentation, and
retain all self-test assertions before rebuilding and deploying.
