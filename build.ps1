$ErrorActionPreference = 'Stop'
$compiler = Join-Path $PSScriptRoot 'build-tools\zig-x86_64-windows-0.15.2\zig.exe'
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $PSScriptRoot 'build-tools\native-cache'
# The toolchain is not committed (25k files, re-downloadable), so a fresh clone and CI both
# bootstrap it here. Pinned to an exact version: the build depends on this Zig's linker
# behaviour, and a silently newer release is how a reproducible build stops being one.
if (-not (Test-Path $compiler)) {
    $zigVersion = '0.15.2'
    $zip = Join-Path $env:TEMP "zig-x86_64-windows-$zigVersion.zip"
    $url = "https://ziglang.org/download/$zigVersion/zig-x86_64-windows-$zigVersion.zip"
    Write-Host "build-tools not found; downloading Zig $zigVersion..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath (Join-Path $PSScriptRoot 'build-tools') -Force
    Remove-Item $zip -Force
    if (-not (Test-Path $compiler)) { throw "Zig bootstrap failed: $compiler not found after extracting $url" }
}

# The running app maps its own executable, so the linker cannot replace IdleDimmer.exe and
# reports "Permission denied" - an error that names the file but not the cause. Quit it
# gracefully first: --quit restores brightness on the way out, so this never leaves the
# screen dimmed. A build on a clean machine (CI) finds nothing running and skips this.
$exePath = Join-Path $PSScriptRoot 'IdleDimmer.exe'
$wasRunning = $false
if (Get-Process IdleDimmer -ErrorAction SilentlyContinue) {
    $wasRunning = $true
    Write-Host 'Idle Dimmer is running; asking it to exit before relinking (brightness is restored)...' -ForegroundColor Cyan
    Start-Process $exePath -ArgumentList '--quit' -Wait -NoNewWindow -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 50 -and (Get-Process IdleDimmer -ErrorAction SilentlyContinue); $i++) {
        Start-Sleep -Milliseconds 100
    }
    if (Get-Process IdleDimmer -ErrorAction SilentlyContinue) {
        throw 'IdleDimmer.exe is still running and holds its own executable open. Quit it from the tray, then rebuild.'
    }
}
# Every .cpp in the repo root is linked, so a new module cannot be silently left out of the build.
# SessionStats.cpp was added after the list was spelled out by hand and the omission would have
# surfaced only as an undefined symbol at link time, or worse, as a card stuck on em dashes.
#
# ScreenOffProbe.cpp is excluded: it is a SEPARATE program with its own main() entry point, and
# linking it here would put two entry points in one binary. It is built by its own command.
$excluded = @('ScreenOffProbe.cpp')
$srcs = Get-ChildItem $PSScriptRoot -Filter '*.cpp' |
        Where-Object { $excluded -notcontains $_.Name } |
        Select-Object -ExpandProperty FullName
# Panel glyphs are antialiased alpha masks, not integer GDI line art. This runs before the
# compile because Flyout.cpp includes the generated header, and it caches on timestamps.
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'tools\build-panel-icons.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Panel icon generation failed' }
& $compiler cc -x c++ -target x86_64-windows-gnu -std=c++17 -Os -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections '-Wl,--gc-sections' '-Wl,--subsystem,windows' -s -municode @srcs -o (Join-Path $PSScriptRoot 'IdleDimmer.exe') -lole32 -loleaut32 -lwbemuuid -lapi-ms-win-core-winrt-l1-1-0 -lapi-ms-win-core-winrt-string-l1-1-0 -lshell32 -luser32 -lgdi32 -ladvapi32 -lwtsapi32 -ldwmapi
if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
# Embed the tray icon as the executable's main application icon (RT_ICON + RT_GROUP_ICON).
# Design 4 (bulb and single chevron) is the user's chosen app identity, matching the default
# tray glyph. It is not design 3 (horizon sun and arrow), which used to be embedded here and
# looked like a different application in Explorer.
$mainIco = Join-Path $PSScriptRoot 'icons\tray4-ondark-on.ico'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'tools\set-app-icon.ps1') -ExePath (Join-Path $PSScriptRoot 'IdleDimmer.exe') -IcoPath $mainIco
if ($LASTEXITCODE -ne 0) { throw 'Application icon embedding failed' }
# Regenerate themed tray icons when the SVG artwork changed (needs Edge; failures keep existing .ico files).
$iconDir = Join-Path $PSScriptRoot 'icons'
$svg = Get-ChildItem $iconDir -Filter '*.svg' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$ico = Get-ChildItem $iconDir -Filter '*.ico' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($svg -and (-not $ico -or $svg.LastWriteTime -gt $ico.LastWriteTime)) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $iconDir 'build-icons.ps1')
    if ($LASTEXITCODE -ne 0) { Write-Warning 'Icon generation failed; keeping existing .ico files' }
}
# Gate on icon geometry: off-centre or clipped glyphs are invisible in verify.png but obvious
# in the tray, and this pipeline silently produced both before the gates existed.
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'tools\measure-icons.ps1') -Check | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Tray icon geometry check failed; see tools\measure-icons.ps1 output' }
# Gate on documentation agreeing with the code. Every mechanism replaced in this project left
# a confident, wrong paragraph behind, and a status string outlived the behaviour it described.
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'tools\check-docs.ps1') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Docs/code consistency check failed; run tools\check-docs.ps1 to see what disagrees' }
Get-Item -LiteralPath $exePath | Select-Object Name,Length
# Re-deploy what was running before the build. AGENTS.md section 1: a build that leaves the
# app closed has not been delivered, and the user is the one who has to audit it.
if ($wasRunning) {
    Write-Host 'Relaunching Idle Dimmer...' -ForegroundColor Cyan
    Start-Process $exePath -WorkingDirectory $PSScriptRoot -WindowStyle Hidden
}

