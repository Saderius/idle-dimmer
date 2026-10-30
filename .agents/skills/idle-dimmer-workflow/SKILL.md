---
name: idle-dimmer-workflow
description: >-
  Development, building, testing, rendering, and deployment procedures for the Idle Dimmer application.
  Use this skill whenever modifying, building, debugging, testing, or deploying code in the idle-dimmer repository.
---

# Idle Dimmer: Development, Testing & Deployment Runbook

This skill defines the operational protocols, architectural constraints, and hard-learned lessons for working on the Idle Dimmer codebase. Follow this runbook to avoid regressions, false-positive test passes, and invisible background deployment failures.

---

## 1. Core Engineering Constraints & Invariants

| Invariant | Requirement | Rationale / Failure Mode |
|---|---|---|
| **Zero Runtime Overhead** | Plain Win32 C/C++ only. No C++ exceptions/RTTI, no .NET, no Electron, no background polling threads. | The app's purpose is 0% CPU at idle. Framework runtimes wake CPUs periodically. |
| **Assertion Count Gate** | Self-test assertion count must **never** be reduced (currently $\ge 666$). | Removing or weakening assertions masks regressions. |
| **Doc Consistency Gate** | Every new mechanism and user-visible status string must be added to `IMPLEMENTATION.md` and `tools/check-docs.ps1`. | Prevents code and architectural documentation from drifting. |
| **No Input Synthesis** | **Never** call `SetCursorPos`, `mouse_event`, or `SendInput`. | Forbidden. The user is the auditor and drives their own desktop. |
| **Single UI Surface** | The Quick Settings flyout panel is the only UI. Do not add tray context menus. | Two front-ends drift and create conflicting state. |

---

## 2. Win32 GDI Rendering & DIB Memory Layout

When writing pixel compositing or rendering code in `Flyout.cpp` or similar:

### The BGRA Trap
* Win32 32-bpp DIB sections (`CreateDIBSection` with `biBitCount = 32`, `BI_RGB`) store pixels in memory using `RGBQUAD` ordering:
  - `byte[0]` = **Blue**
  - `byte[1]` = **Green**
  - `byte[2]` = **Red**
  - `byte[3]` = **Alpha / Reserved**
* **The Monochromatic False Positive**: If a pixel is greyscale ($R = G = B$), swapping Blue and Red produces the exact same color. Rendering logic tested solely on black, white, and grey elements (borders, dividers, text) will appear completely correct.
* **The Colored Bug**: As soon as a non-monochrome color (such as the live Windows accent `#D4BB8B`, whose byte-reverse is `#8BBBD4`) is rendered, inverted channels swap Red and Blue.
* **Always verify surface pixels, not just math**: Do not rely on testing the arithmetic of `SystemAccent()`. In `--self-test`, render a test panel onto a memory DIB using `CreateDIBSection` and sample the actual memory bytes or use `GetPixel` to assert channel accuracy (`AccentChecks()`).

---

## 3. Desktop Sessions, Sandboxes & Process Lifetime

### Interactive Desktop Attachment
Agents and sandbox runners in CI or IDE environments execute on isolated desktop stations (e.g. `WinSta0\exebox-...`).
* Win32 APIs like `Shell_NotifyIconW` and `FindWindowW` only query the desktop associated with the current thread.
* If `IdleDimmer.exe` starts on the runner's desktop, its tray icon will not appear on the user's taskbar, and IPC discovery will fail.
* `IdleDimmer.cpp` explicitly binds `wWinMain` to the user's interactive desktop:
  ```cpp
  HDESK desk = OpenDesktopW(L"Default", 0, FALSE, GENERIC_ALL);
  if (desk) SetThreadDesktop(desk);
  ```
  Never remove or bypass this binding.

### Background Daemon Lifecycles
* Windows Job Objects terminate all child processes when a non-daemon PowerShell tool command terminates.
* To keep `IdleDimmer.exe` running continuously on the user's desktop:
  1. The tool call must specify `IsDaemon: true`.
  2. The command must be invoked via `Start-Process .\IdleDimmer.exe -Wait` from the `native` directory.
  3. Verify that the process is alive with `Get-Process IdleDimmer` and responds to `--status`.

---

## 4. Multi-Agent & Concurrent Workflow Discipline

Before touching files or committing:
1. Run `git status` and check for uncommitted files.
2. Check for active handoff specifications (e.g. `docs/superpowers/specs/*handoff*.md`).
3. If concurrent agents are operating on other tasks (e.g. display-off probe or power metrics), **do not touch, modify, or commit their files** (such as `ScreenOffProbe.cpp`, `check-lock-on-display-off.ps1`, `docs/visual-audit-*.md`, `stats.bin`).

---

## 5. Standard Build, Test & Deployment Protocol

Always execute these steps in order when making changes:

### Step 1: Stop Existing Instance
```powershell
Start-Process IdleDimmer.exe -ArgumentList '--quit' -Wait -NoNewWindow
```

### Step 2: Build App and Run Gates
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```
`build.ps1` automatically executes:
- Icon measurement gate (`measure-icons.ps1`).
- Documentation consistency gate (`check-docs.ps1`).
- Bundled Zig compiler flags (`-Os -s -municode ...`).

### Step 3: Execute Self-Test Gate
```powershell
Start-Process IdleDimmer.exe -ArgumentList '--self-test' -Wait -NoNewWindow
Get-Content diagnostics.txt
```
* Requirement: Must print `Decision tests: PASS (<count> checks)` where `<count>` is at least 666.

### Step 4: Deploy Live on Desktop
Launch the app as a daemon so the user can audit it:
```powershell
# Invoked via run_command with IsDaemon: true and Cwd: native
Start-Process .\IdleDimmer.exe -Wait
```

### Step 5: Verify Deployment via IPC
Confirm the live app is active and responsive:
```powershell
.\IdleDimmer.exe --status; Start-Sleep -Milliseconds 500; Get-Content diagnostics.txt
```
Expected output:
```text
Status: Enabled; waiting for idle
Enabled: 1
Dimmed: 0
...
```
Also check that `activity.log` contains:
```text
<timestamp> Native app started; adaptive checks; input notifications only while dimmed
```


---

## 6. Anti-Complexity Theater & Visual Verification Invariants

### 6.1 The Synthetic Self-Certification Trap
* **The Failure Mode**: A pipeline of 12 subagents burned ~915,000 tokens passing unit assertions while the UI remained jagged and unantialiased.
* **Invariant**: Never declare victory on visual/UI tasks using unit test passes or compiler return codes as proof.
* **Visual Evaluation**: When reviewing UI changes, always evaluate visual appearance directly. Passing coordinate math in `--self-test` proves only that integer formulas ran, not that the screen looks like the design.

### 6.2 Vector Glyph Antialiasing Protocol (4× Supersampling)
Plain GDI `StrokeLine` and `StrokeCircle` write binary pixels (0% or 100% coverage). At 16–24 px, this produces severe stair-stepping and heavy visual weight.
* To render smooth vector icons in Win32 GDI without external libraries:
  1. Allocate a temporary 4× oversampled single-channel mask (e.g. 80×80 px for a 20×20 px icon).
  2. Render the lines and arcs into the 4× mask using integer primitives scaled by 4.
  3. Box-filter downsample each 4×4 block (16 samples) to compute an exact coverage alpha (0..255).
  4. Composite onto the destination DIB using `BlendPixel`.

### 6.3 Post-Subagent Process Survival Audit
Subagent task termination in this environment terminates child processes created in that sub-tree.
* After ANY subagent or multi-agent run completes, the parent agent MUST immediately execute:
  `Get-Process IdleDimmer | Select-Object Id, SessionId`
* Never report to the user that the app is live based on a subagent's claim without independently verifying that PID > 0 exists in Session 1.
