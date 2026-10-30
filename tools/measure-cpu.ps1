<#
.SYNOPSIS
    Measures Idle Dimmer's steady-state CPU cost so the README figure can be trusted.

.DESCRIPTION
    The app's efficiency claim is its headline: "~0.02% CPU, ~4 MB, 2 wakeups/min".
    That number was measured once, long ago, and never re-verified. A recent measurement
    came back around 0.125%, so this script exists to produce a number that can be
    checked rather than believed.

    Two modes:
      - steady   : app runs, panel never opened. This is the number the README claims.
      - open     : panel is opened and left visible. What the animation costs.

    Usage:
      powershell -File tools\measure-cpu.ps1 -Seconds 120
      powershell -File tools\measure-cpu.ps1 -Mode open -Seconds 60
      powershell -File tools\measure-cpu.ps1 -CompareAgainst C:\path\to\old.exe
#>
[CmdletBinding()]
param(
    [int]$Seconds = 120,
    [ValidateSet('steady','open')][string]$Mode = 'steady',
    # Compare against a different build to attribute a cost to a specific change.
    [string]$CompareAgainst,
    [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'

function Stop-App {
    Get-Process IdleDimmer -ErrorAction SilentlyContinue |
        ForEach-Object { Start-Process "$PSScriptRoot\..\IdleDimmer.exe" -ArgumentList '--quit' -Wait -NoNewWindow -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 2
}

# The idle threshold is raised for the duration so a long sample never triggers a dim.
# A dim is a WMI round-trip plus raw-input registration, and charging that to the steady
# state would make the number meaningless. It is restored at the end.
$ini = Join-Path $PSScriptRoot '..\settings.ini'
$backup = "$ini.cpubak"
$hadIni = Test-Path $ini
if ($hadIni) { Copy-Item $ini $backup -Force }
function Set-Threshold([string]$value) {
    $content = if ($hadIni) { Get-Content $ini } else { @('[Settings]') }
    if ($content -match '^IdleMinutes=') { $content = $content -replace '^IdleMinutes=.*', "IdleMinutes=$value" }
    else { $content += "IdleMinutes=$value" }
    Set-Content $ini -Value $content -Encoding UTF8
}

function Measure-One([string]$exe, [string]$label) {
    Stop-App
    # IdleMinutes is read ONCE at startup, so the ini must already be correct before the
    # process starts. Writing it while an old instance is alive measures the OLD threshold -
    # and at 2 minutes a 150 s sample dims and restores repeatedly, each dim being a WMI
    # round-trip worth roughly 900 ms. That is how an earlier run of this script reported
    # 2.9% for a build whose real steady-state cost is an order of magnitude lower.
    Set-Threshold '60'
    $p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden -PassThru
    # Let startup settle: COM init, WMI connection and the first media probe all land here
    # and none of it is steady state.
    Start-Sleep -Seconds 6
    $p.Refresh()
    if ($p.HasExited) { Write-Error "${label} exited immediately"; return $null }
    # Fail loudly rather than silently benchmark dim/restore cycles.
    if ((Get-Content $ini -Raw) -notmatch 'IdleMinutes=60') {
        Write-Error "${label}: threshold did not apply; measurement would be invalid"
        return $null
    }

    if ($Mode -eq 'open') {
        # Drive the real left-click so the panel is genuinely open, then leave it open.
        Add-Type @"
using System;using System.Runtime.InteropServices;
public class TrayClick {
  [DllImport("user32.dll",SetLastError=true)] public static extern IntPtr FindWindow(string c,string w);
  [DllImport("user32.dll")] public static extern bool SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
}
"@ -ErrorAction SilentlyContinue
        $tray = [TrayClick]::FindWindow('IdleDimmerNativeWindow', 'Idle Dimmer')
        # kTray = WM_APP+1; lParam LOWORD = WM_LBUTTONUP
        [void][TrayClick]::SendMessage($tray, 0x8001, [IntPtr]1, [IntPtr]0x0202)
        Start-Sleep -Seconds 2
    }

    $p.Refresh()
    $cpu0 = $p.TotalProcessorTime.TotalMilliseconds
    $ws0 = $p.WorkingSet64
    $t0 = Get-Date
    Start-Sleep -Seconds $Seconds
    $p.Refresh()
    $cpu1 = $p.TotalProcessorTime.TotalMilliseconds
    $ws1 = $p.WorkingSet64
    $elapsed = ((Get-Date) - $t0).TotalSeconds

    $cpuMs = $cpu1 - $cpu0
    [pscustomobject]@{
        Build  = $label
        Mode   = $Mode
        CpuMs  = [Math]::Round($cpuMs, 2)
        Secs   = [Math]::Round($elapsed, 1)
        # Percent of ONE core over the sample window. This is the same basis the README uses.
        Percent = [Math]::Round(($cpuMs / 1000) / $elapsed * 100, 4)
        PerCheckMs = [Math]::Round($cpuMs / [Math]::Max(1, ($elapsed / 30)), 3)
        WorkingSetMB = [Math]::Round($ws1 / 1MB, 1)
    }
}

Set-Threshold '60'
$exe = Join-Path $PSScriptRoot '..\IdleDimmer.exe'
$results = @()
$results += Measure-One $exe 'current'

if ($CompareAgainst -and (Test-Path $CompareAgainst)) {
    $results += Measure-One $CompareAgainst 'comparison'
}

if (-not $KeepRunning) {
    Stop-App
    if ($hadIni) { Move-Item $backup $ini -Force } else { Remove-Item $ini -ErrorAction SilentlyContinue }
}

$results | Format-Table -AutoSize
""
"README / IMPLEMENTATION.md section 15 claims: 0.0156 s per 90 s = 0.02% CPU, ~4 MB."
"Percent above is of one core, same basis. PerCheckMs is the average cost of one 30-second
check cycle - divide by 30 to get the per-second budget at the documented wakeup rate."