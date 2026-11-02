<div align="center">

# Idle Dimmer

### Step away. Let your displays rest.

A small Windows tray app that lowers display brightness when you leave,
then restores it when you return.

**[Download](https://github.com/Saderius/idle-dimmer/releases/latest)** · [Get started](#get-started) · [Build from source](#build-from-source)

[![Build](https://github.com/Saderius/idle-dimmer/actions/workflows/build.yml/badge.svg)](https://github.com/Saderius/idle-dimmer/actions/workflows/build.yml) [![Windows](https://img.shields.io/badge/Windows-x64-0078D4)](https://github.com/Saderius/idle-dimmer/releases/latest) [![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

<br>

<img src="docs/media/acrylic-demo.gif" width="440" alt="Idle Dimmer's native acrylic changing from teal to burgundy over a moving background">

<sub>Current source build · <a href="docs/media/acrylic-demo.mp4">MP4 with smoother gradients</a></sub>

<br>

**Portable. Native C++. No installer, service, or extra runtime.**

</div>

## Set it once. Leave it in the tray.

| Control | What it does |
| :--- | :--- |
| **Choose your delay** | Drag or scroll the slider to set how long the app waits before dimming. |
| **Keep watching** | Separate media, sound, and full-screen protections let you pause dimming during playback or a full-screen app. |
| **Dim or switch off** | Lower hardware brightness, use **Off at idle**, or switch the displays off from the footer. |
| **Pick up where you left off** | Input restores saved brightness after dimming. The app also saves recovery state before changing brightness. |
| **Fits Windows** | A native acrylic flyout with an upward entrance, system accent colours, and light/dark theme support. |
| **Quiet when closed** | The panel renderer shuts down when the flyout closes. No hidden UI animation loop. |

## Get started

1. Download the EXE or ZIP from [Releases](https://github.com/Saderius/idle-dimmer/releases/latest).
2. Run `IdleDimmer.exe` and click its tray icon. Check the **^** overflow if it is hidden.
3. Set your delay and protection toggles. Enable **Start at login** if you want it to run with Windows.

The slider saves changes automatically. Scroll up to increase the delay; scroll down to decrease it.
The footer's **moon** dims immediately, **monitor** schedules screen-off after a short settling delay,
and **exit arrow** quits after restoring brightness.

<details>
<summary><strong>Compatibility and SmartScreen</strong></summary>

Brightness control depends on the display and driver. Internal panels use WMI; external monitors
need DDC/CI support. Screen-off and wake behavior also vary by device. See the
[hardware notes](IMPLEMENTATION.md#51-display-off-sessions-automatic-off-and-the-manual-footer-button)
for implementation and testing limits.

The EXE is currently unsigned, so Windows may show a SmartScreen warning. Review the source and
release before choosing **More info → Run anyway**.

</details>

## Update without starting over

Quit Idle Dimmer normally, download the new version, and replace only `IdleDimmer.exe`.
Keep the adjacent settings and statistics files. The release workflow supplies download checksums;
in-app updating is planned separately.

The preview above shows the current source build. The latest published release can be older.

## Build from source

App version: **1.0.1**.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -SkipIconRender
Start-Process .\IdleDimmer.exe -ArgumentList '--self-test' -Wait
Get-Content diagnostics.txt
```

The build downloads the pinned Zig compiler on first use. It uses the committed icon assets,
builds the EXE, and checks documentation and icon geometry. `build.ps1 -NoRun` builds without
running or relaunching the app; close an existing instance first.

<details>
<summary><strong>Useful commands</strong></summary>

| Command | What it does |
| :--- | :--- |
| `IdleDimmer.exe --menu` | Open or close the flyout |
| `IdleDimmer.exe --quit` | Exit gracefully and restore brightness |
| `IdleDimmer.exe --status` | Write runtime status to `diagnostics.txt` |
| `IdleDimmer.exe --diagnose` | Report display, playback, and configuration details |
| `IdleDimmer.exe --self-test` | Run the assertion suite |

</details>

## Under the hood

[Architecture & measured footprint](IMPLEMENTATION.md) · [Release guide](docs/RELEASING.md) · [Research](docs/research/) · [Contributing rules](AGENTS.md)

Release builds are deliberate: ordinary commits run checks, while **Prepare release** creates
a tested draft with downloads and generated change notes. Nothing publishes on every commit.

---

[MIT](LICENSE) · Made for Windows.
