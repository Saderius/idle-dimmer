# Power efficiency references for Idle Dimmer

*Created 2026-10-17*

Citations and measurement backing for the app's near-zero-idle-CPU goal.
Status tags follow `AGENTS.md` §7.2: MEASURED / STALE / NEVER MEASURED / INFERRED.

## 1. Language and runtime choice (idle CPU)

- Pereira, R. et al. (2017). *Energy Efficiency across Programming Languages: How Do Energy, Time, and Memory Relate?* SLE 2017, ACM. DOI 10.1145/3136014.3136031. https://greenlab.di.uminho.pt/wp-content/uploads/2017/10/sleFinal.pdf
  - PROVEN (compute): compiled languages (C, Rust, C++) most energy-efficient; interpreted ~20x worse. Average energy 120 J compiled vs 576 J VM vs 2365 J interpreted.
  - CAVEAT: compute-bound benchmarks only; does not measure idle residency.
- Pereira, R. et al. (2021). *Ranking Programming Languages by Energy Efficiency.* Science of Computer Programming 205, 102609. DOI 10.1016/j.scico.2021.102609. https://states.github.io/files/j2.pdf
  - PROVEN: Rust statistically tied with C/C++.
- Kim, J. & Blackburn, S. M. (2024). *It's Not Easy Being Green.* arXiv:2410.05460. https://arxiv.org/html/2410.05460v1
  - CONTRADICTING: energy ~proportional to runtime after controlling for CPU utilisation; language matters less than often claimed. Cite this to avoid over-claiming.

Conclusion (INFERRED): "C is best for idle" is an inference from "no runtime threads / no GC", not a measurement. Rust satisfies the same property; Go/.NET/Node carry runtime threads and GC that can wake periodically.

## 2. Timer coalescing and timer resolution

- Microsoft. *Windows Timer Coalescing* whitepaper (2011). https://www.microsoft.com/en-us/download/details.aspx?id=27426
  - PROVEN: coalescing groups timers across processes to reduce idle-state exits; `WM_TIMER` is the best candidate; minimum 32 ms tolerance; periods in multiples of 50 ms.
- Microsoft. `SetCoalescableTimer` (winuser.h). https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcoalescabletimer
  - PROVEN: tolerance 0 == system default (same as `SetTimer` on Win8+); only non-zero tolerance changes behaviour; `TIMERV_NO_COALESCING` opts out.
- Microsoft. *Results for the Idle Energy Efficiency Assessment.* https://learn.microsoft.com/en-us/windows-hardware/test/assessments/results-for-the-idle-energy-efficiency-assessment
  - PROVEN: Microsoft idle gate = <=1% active C0, >=99% deepest C-state, 0 processes >1% CPU, 0 periodic sources <=300 ms, 15.6 ms system timer resolution.
  - PROVEN: shortening tick 15.6 ms -> 1 ms costs >=20% battery.
- Intel. *Energy-Efficient Platforms* (2011). https://www.intel.com/content/dam/doc/white-paper/energy-efficient-platforms-2011-white-paper.pdf
  - PROVEN: 1 ms tick disables deep C-states; use QueryPerformanceCounter for stamps; coalescing tolerance >=100 ms recommended.
- Intel PowerTOP primer. https://www.intel.com/content/www/us/en/developer/articles/tool/powertop-primer.html
  - PROVEN: userspace polling / 1 kHz ticks destroy C-state residency (target >=95% in C4).
- Bruce Dawson. *Windows Timer Resolution: Megawatts Wasted* (2013). https://randomascii.wordpress.com/2013/07/08/windows-timer-resolution-megawatts-wasted
  - PROVEN (field): raising timer resolution wastes megawatts-scale energy platform-wide.
- Chromium issue 40128284. *Throttle JavaScript timers to 1 wake up per minute in background.* https://issues.chromium.org/issues/40128284
  - PROVEN: throttling background timers to 1/min extended battery life by ~2 h (28% median); 36 min / 13% in YouTube test.

Note for this project: coalescing reduces system-wide idle-state exits; it does NOT reduce this app's own wakeup count. The wakeup reduction comes from `ScheduleNext` (§4.1), a separate mechanism. Do not conflate them in docs.

## 3. Event-driven vs polling; global hook cost

- Microsoft. `LowLevelMouseProc`. https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelmouseproc
  - PROVEN: callback runs in installing thread; latency charged against LowLevelHooksTimeout; slow callback => hook silently removed; Microsoft recommends raw input instead.
- Microsoft. `GetLastInputInfo`. https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getlastinputinfo
  - PROVEN: O(1) read, no subscription.
- Microsoft. `WTSRegisterSessionNotification`. https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/nf-wtsapi32-wtsregistersessionnotification
  - PROVEN: event-driven session lock/unlock.
- Microsoft. `WM_POWERBROADCAST`. https://learn.microsoft.com/en-us/windows/win32/power/wm-powerbroadcast
  - PROVEN: sleep/resume notifications, event-driven.
- Twinkle Tray issue #406. https://github.com/xanderfrangos/twinkle-tray/issues/406
  - PROVEN (field): global mouse hook for click-away dismissal cost 0.1-1% CPU on 1000 Hz mouse; making it on-demand fixed it. Direct evidence for "install hook only while panel visible".

## 4. Electron / managed runtime overhead

- Twinkle Tray issue #879. https://github.com/xanderfrangos/twinkle-tray/issues/879
  - PROVEN (maintainer): ~40 MB idle / ~70 MB in use, 5 processes; recommends Monitorian for RAM-conscious users.
- Twinkle Tray repo. https://github.com/xanderfrangos/twinkle-tray
  - PROVEN: Electron + Node.js + React; native addons node-ddcci, win32-displayconfig.
- Thangadurai et al. (2024). *Electron vs. Web: A Comparative Analysis of Energy and Performance in Communication Apps.* QUATIC 2024. http://www.ivanomalavolta.com/files/papers/QUATIC_2024_2.pdf
  - MIXED: Electron comparable to or slightly better than web on energy; not evidence it beats native.
- Go runtime internals (sysmon). https://internals-for-interns.com/posts/go-sysmon/
  - PROVEN: sysmon adaptive sleep (20 us -> 10 ms cap), forced GC every 2 min, scavenger wakeups; idle Go apps deep-sleep when all Ps idle.

## 5. DWM materials and cost

- Microsoft. *Acrylic material.* https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic
  - PROVEN: transient, light-dismiss surfaces only; reverts to solid fallback under battery saver.
- Microsoft. *Mica material.* https://learn.microsoft.com/en-us/windows/apps/design/style/mica
  - PROVEN: long-lived app backdrops only.
- Microsoft. *System backdrops.* https://learn.microsoft.com/en-us/windows/apps/develop/ui/system-backdrops
  - PROVEN: DwmSetWindowAttribute DWMA_SYSTEMBACKDROP_TYPE; Mica Alt (4) for elevated surfaces.
- Microsoft. *DWM best practices.* https://learn.microsoft.com/en-us/windows/win32/dwm/bestpractices-ovw
  - PROVEN: blur-behind is CPU+GPU intensive; avoid covering large, frequently-updated areas.
- Microsoft. *Geometry in Windows 11.* https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/geometry
  - PROVEN: OverlayCornerRadius 8 px (containers), ControlCornerRadius 4 px (controls).
- Microsoft. *Layering and elevation.* https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/layering
  - PROVEN: flyout elevation 32, stroke width 1 px; shadow drawn by system.
- WinUI source: FlyoutPresenter_themeresources.xaml. https://github.com/microsoft/microsoft-ui-xaml/blob/main/controls/dev/CommonStyles/FlyoutPresenter_themeresources.xaml
  - PROVEN: AcrylicInAppFillColorDefaultBrush background, SurfaceStrokeColorFlyoutBrush border, BorderThickness=1, CornerRadius={OverlayCornerRadius}.

Note: the shell's Quick Settings flyout is DirectUI-drawn by explorer, so XAML-derived numbers are guidance, not a guarantee. Also, a pure Win32 process cannot "just use Mica" via a clean API; the Win32 path is DwmSetWindowAttribute / SetWindowCompositionAttribute, which matches this project's no-runtime constraint.

## 6. Near-zero idle CPU for tray utilities

- Microsoft. Modern standby SleepStudy. https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/modern-standby-sleepstudy
  - PROVEN: flags sessions where drain >=1%/h from a single component or DRIPs idle interval collapses below ~1 s.
- Li et al. (2014). *An Empirical Study of the Energy Consumption of Android Applications.* ICSME 2014. https://viterbi-web.usc.edu/~halfond/papers/li14icsme.pdf
  - PROVEN: 61% of app energy spent in idle states; ignoring idle energy misleads measurement.
- Capra, Francalanci, Slaughter (2012). *Measuring Application Software Energy Efficiency.* IT Professional 14(2), 54-61. DOI 10.1109/MITP.2012.39
  - PROVEN: applications, not just hardware, determine software energy efficiency.
- EarTrumpet. https://github.com/File-New-Project/EarTrumpet
  - REFERENCE: C# WPF tray flyout; demonstrates native-feel flyout without heavy framework.
- FluentFlyout. https://fluentflyout.com
  - REFERENCE: native Windows 11 flyout replacement using system materials.
- TranslucentFlyouts / AcrylicMenus. https://github.com/ALTaleX531/TranslucentFlyouts ; https://github.com/krlvm/AcrylicMenus
  - REFERENCE: Win32 acrylic-on-Win32 tricks.

GAP (state explicitly): no peer-reviewed study measures idle CPU of tray utilities by runtime. Every "C tray icon costs nothing at idle" assertion is an INFERRED claim from timer/C-state literature plus Microsoft's <=1% C0 gate.

## 7. Measurement tooling

- Microsoft. `powercfg` command-line options. https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/powercfg-command-line-options
  - PROVEN: built-in energy report; flags timer resolution, CPU utilisation, wake sources.
- Microsoft. WPA *CPU Usage (Precise).* https://learn.microsoft.com/en-us/windows-hardware/test/wpt/cpu-usage-precise
  - PROVEN: per-thread CPU usage with call stacks; gold standard for idle-CPU attribution.
- EnergiBridge. arXiv:2312.13897. https://arxiv.org/abs/2312.13897
  - PROVEN: cross-platform energy measurement tool.
- arXiv:2401.15985 (2024). https://arxiv.org/abs/2401.15985
  - PROVEN: energy measurement itself perturbs idle power at high sampling rates; measure at low frequency.

## Suggested safe one-line claims

1. Coalescing and a 15.6 ms tick reduce idle-state exits and total system energy; a 1 ms tick costs >=20% battery. PROVEN (Microsoft).
2. Microsoft's own idle gate is <=1% active C0, >=99% deepest C-state, 0 processes >1% CPU, 0 periodic sources <=300 ms. PROVEN.
3. Global mouse hooks are charged at input rate and Microsoft recommends raw input instead; install them only while the panel is visible. PROVEN (MS docs + Twinkle Tray #406).
4. Background timer wakeups are worth hours of battery; Chromium throttling them to 1/min gained ~28% median. PROVEN.
5. Blur-behind is CPU+GPU intensive and should not cover a large, frequently-updated area. PROVEN (Microsoft DWM docs).
6. Compiled languages are most energy-efficient for compute; that does not measure idle. PROVEN for compute / INFERRED for idle (Pereira; contradicted in interpretation by Kim & Blackburn 2024).
7. "Bottom divider line" in tray flyouts: UNVERIFIED; likely the flyout's 1 px border on all edges (FlyoutPresenter BorderThickness=1). Confirm on the running build before documenting as a mechanism.

## See also

- `IMPLEMENTATION.md` — the mechanism reference this file backs. Its §15 is the measured-footprint
  section and §17 links back to here.
- `docs/research/` — the Win32 research behind the protection design (full-screen detection, GPU
  activity signals, non-interactive sessions).
- `AGENTS.md` §7.2 — the status-tag convention (`MEASURED` / `STALE` / `NEVER MEASURED` / `INFERRED`)
  used throughout this file.
