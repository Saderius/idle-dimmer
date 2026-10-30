<#
.SYNOPSIS
    Builds the panel's raster icon assets from the design's SVG artwork.

.DESCRIPTION
    The flyout painter used to approximate the design's 20x20 SVG paths with integer GDI
    lines. That produced binary pixels with no edge coverage, so every glyph was visibly
    stair-stepped (measured: exactly two colours in a 30x30 crop). This script uses the
    same headless-Edge rasterizer as the tray-icon pipeline, but keeps the result as an
    antialiased alpha mask and embeds it as a C header for the panel to tint and blit.

    Two stages, both cached by timestamp:

      1. SVG -> 80x80 PNG (white glyph + alpha), under icons\panel\.
      2. PNG -> PanelIconsData.h (one byte of coverage per source pixel).

    The 80px source is 4x the design's 20px box, so a 20px draw is an exact 4x4 box
    downsample. At 200% DPI it is an exact 2x2 downsample. Intermediate DPIs use an
    area-weighted filter, which is why the output stays smooth instead of snapping to
    whole-pixel geometry.

    Usage:
      powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-panel-icons.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$iconDir = Join-Path $PSScriptRoot '..\icons\panel'
$header = Join-Path $PSScriptRoot '..\PanelIconsData.h'
$renderSize = 80
$icons = @('sun','media','full','folder','power','clock','gear','exit','apps','dimnow','dimming','dimmingFilled')

$edge = @(
    (Join-Path $env:ProgramFiles 'Microsoft\Edge\Application\msedge.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft\Edge\Application\msedge.exe')
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1

function Get-Screenshot([string]$svgPath, [int]$size) {
    if (-not $edge) { throw "Microsoft Edge not found; cannot rasterize $svgPath" }
    $svgUri = ([Uri]$svgPath).AbsoluteUri
    $stem = [IO.Path]::GetFileNameWithoutExtension($svgPath)
    $html = Join-Path $env:TEMP ("idlepanel-$stem.html")
    $doc = "<html><head><style>html,body{margin:0;padding:0;background:#FF00FF;overflow:hidden}img{position:absolute;left:0;top:0;width:${size}px;height:${size}px}</style></head><body><img src=`"$svgUri`"></body></html>"
    Set-Content -LiteralPath $html -Value $doc -Encoding UTF8
    $out = Join-Path $env:TEMP ("idlepanel-$stem.png")
    if (Test-Path $out) { Remove-Item $out -Force }
    $profile = Join-Path $env:TEMP 'idlepanel-edge-profile'
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'SilentlyContinue'
    try {
        & $edge --headless --disable-gpu --hide-scrollbars --force-device-scale-factor=1 `
            --user-data-dir="$profile" --screenshot="$out" --window-size=$size,$size ([Uri]$html).AbsoluteUri 2>$null | Out-Null
    } finally {
        $ErrorActionPreference = $prevEAP
    }
    if (-not (Test-Path $out)) { throw "Rasterization failed for $svgPath" }
    return $out
}

# White glyph composited over pure magenta: alpha = green channel. The RGB source is
# discarded and a white straight-alpha mask is saved, so the panel can tint it any colour.
function Get-AlphaMask([string]$pngPath, [int]$size) {
    $src = New-Object Drawing.Bitmap($pngPath)
    try {
        if ($src.Width -ne $size -or $src.Height -ne $size) {
            throw "Expected ${size}x${size} from $pngPath, got $($src.Width)x$($src.Height)"
        }
        $mask = New-Object Drawing.Bitmap($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            for ($y = 0; $y -lt $size; $y++) {
                for ($x = 0; $x -lt $size; $x++) {
                    $a = [int]$src.GetPixel($x, $y).G
                    if ($a -gt 0) {
                        $mask.SetPixel($x, $y, [Drawing.Color]::FromArgb($a, 255, 255, 255))
                    }
                }
            }
            return $mask
        } catch {
            $mask.Dispose()
            throw
        }
    } finally {
        $src.Dispose()
    }
}

$needsRender = $false
foreach ($name in $icons) {
    $svg = Join-Path $iconDir "$name.svg"
    $png = Join-Path $iconDir "$name.png"
    if (-not (Test-Path $svg)) { throw "Missing panel icon artwork: $svg" }
    if (-not (Test-Path $png)) { $needsRender = $true; break }
    if ((Get-Item $svg).LastWriteTime -gt (Get-Item $png).LastWriteTime) { $needsRender = $true; break }
}

if ($needsRender) {
    foreach ($name in $icons) {
        $svg = Join-Path $iconDir "$name.svg"
        $png = Join-Path $iconDir "$name.png"
        if ((Test-Path $png) -and ((Get-Item $svg).LastWriteTime -le (Get-Item $png).LastWriteTime)) { continue }
        $shot = Get-Screenshot $svg $renderSize
        $mask = Get-AlphaMask $shot $renderSize
        try {
            $mask.Save($png, [Drawing.Imaging.ImageFormat]::Png)
        } finally {
            $mask.Dispose()
        }
        Write-Host "panel icons: rasterized $name.png"
    }
}

$needsHeader = -not (Test-Path $header)
if (-not $needsHeader) {
    $headerTime = (Get-Item $header).LastWriteTime
    if ((Get-Item $PSCommandPath).LastWriteTime -gt $headerTime) { $needsHeader = $true }
    if (-not $needsHeader) {
        foreach ($name in $icons) {
            if ((Get-Item (Join-Path $iconDir "$name.png")).LastWriteTime -gt $headerTime) {
                $needsHeader = $true
                break
            }
        }
    }
}
if (-not $needsHeader) { exit 0 }

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('// Generated by tools\build-panel-icons.ps1 - do not edit by hand.')
[void]$sb.AppendLine('// Source artwork: icons\panel\*.svg, rasterized at 4x the design box.')
[void]$sb.AppendLine('#pragma once')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('namespace panelicons {')
[void]$sb.AppendLine("constexpr int kSourcePx = $renderSize;")
[void]$sb.AppendLine('')
[void]$sb.AppendLine('struct Mask {')
[void]$sb.AppendLine('    const unsigned char* alpha;')
[void]$sb.AppendLine('    int width;')
[void]$sb.AppendLine('    int height;')
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')

foreach ($name in $icons) {
    $png = Join-Path $iconDir "$name.png"
    $bmp = New-Object Drawing.Bitmap($png)
    try {
        if ($bmp.Width -ne $renderSize -or $bmp.Height -ne $renderSize) {
            throw "Expected ${renderSize}x${renderSize} from $png, got $($bmp.Width)x$($bmp.Height)"
        }
        [void]$sb.AppendLine("static const unsigned char k$($name.Substring(0,1).ToUpper()+$name.Substring(1))Alpha[$($renderSize * $renderSize)] = {")
        $line = New-Object System.Text.StringBuilder
        for ($y = 0; $y -lt $renderSize; $y++) {
            for ($x = 0; $x -lt $renderSize; $x++) {
                [void]$line.Append([int]$bmp.GetPixel($x, $y).A)
                [void]$line.Append(',')
                if ($line.Length -ge 120) {
                    [void]$sb.AppendLine($line.ToString())
                    $null = $line.Clear()
                }
            }
        }
        if ($line.Length -gt 0) { [void]$sb.AppendLine($line.ToString()) }
        [void]$sb.AppendLine('};')
        [void]$sb.AppendLine('')
    } finally {
        $bmp.Dispose()
    }
}

[void]$sb.AppendLine('static const Mask kMasks[] = {')
foreach ($name in $icons) {
    $cap = $name.Substring(0,1).ToUpper()+$name.Substring(1)
    [void]$sb.AppendLine("    { k${cap}Alpha, $renderSize, $renderSize },")
}
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('}  // namespace panelicons')

Set-Content -LiteralPath $header -Value $sb.ToString() -Encoding ASCII
Write-Host "panel icons: wrote $header"
