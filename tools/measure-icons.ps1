<#
.SYNOPSIS
    Reports the ink bounding box of every frame in every generated tray .ico.

.DESCRIPTION
    The tray pipeline rasterises an SVG, trims it, re-centres it (Refit-Mask in
    build-icons.ps1) and emits one .ico per design x theme x state. Two defects
    that are invisible in verify.png are visible here:

      * vertical mis-centring - the glyph sits low because the Y offset was
        derived from the glyph width, so padT != padB;
      * vertical clipping - the glyph is drawn at its natural height starting
        from that offset and runs off the bottom of the square frame, so the
        ink height is short of the intended fill.

    Both were found by measuring, not by looking, so keep measuring.

.PARAMETER Check
    Enforce thresholds and exit non-zero on failure. Use as a build gate.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure-icons.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure-icons.ps1 -Check
#>
[CmdletBinding()]
param([switch]$Check)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$iconDir = Join-Path $PSScriptRoot '..\icons'

# Glyphs fill the frame to this fraction on their longest side (Refit-Mask's 0.88).
$FillLongest = 0.88
# Tolerance in pixels for |padT - padB|; 1 px absorbs rounding in [Math]::Round.
$CenterTol = 1
# The longest side of the ink, as a fraction of the canvas, must land in this band.
# The lower bound is the clipping gate: Refit-Mask used to push tall glyphs off the
# bottom of the square frame, which showed up as a fill well under $FillLongest.
# Ceiling rounding makes the value drift a little above $FillLongest, so the upper
# bound is generous; it only has to catch an outright overflow.
$LongestBand = @(0.85, 1.0)
# Advisory only: ink width as a fraction of the canvas. Design 1 is genuinely
# narrow (aspect ~0.64) and is reported here rather than gated, because widening
# it is an artwork decision, not a pipeline defect.
$WidthFloor = 0.68
# The sunset timer (design 5) is animated, so the two gates above do not describe it:
# its sun is SUPPOSED to sit off-centre, and its ink height changes every frame by
# design. What must hold is registration - the horizon rule has to land on the same
# row in every frame, or the "sun descends" animation turns into the rule jumping
# about. That failure is invisible in verify.png and in the eye test, so it is gated
# here instead. See the Raw branch in build-icons.ps1.
$SunsetPrefix = 'tray5-'
$LineTol = 1

function Get-InkBox([byte[]]$pngBytes) {
    $ms = New-Object IO.MemoryStream(, $pngBytes)
    try {
        $bmp = [System.Drawing.Bitmap]::FromStream($ms)
        try {
            $minX = $bmp.Width; $minY = $bmp.Height; $maxX = -1; $maxY = -1
            $rowWidth = New-Object int[] $bmp.Height
            for ($y = 0; $y -lt $bmp.Height; $y++) {
                for ($x = 0; $x -lt $bmp.Width; $x++) {
                    if ($bmp.GetPixel($x, $y).A -gt 0) {
                        if ($x -lt $minX) { $minX = $x }
                        if ($x -gt $maxX) { $maxX = $x }
                        if ($y -lt $minY) { $minY = $y }
                        if ($y -gt $maxY) { $maxY = $y }
                        $rowWidth[$y]++
                    }
                }
            }
            if ($maxX -lt 0) { return $null }
            # The horizon rule is the widest run of ink in the frame by a wide margin
            # (14 units vs the sun's 6.4), so locating it this way does not depend on
            # the sun, which is the thing that is moving between frames.
            $maxRun = 0
            foreach ($w in $rowWidth) { if ($w -gt $maxRun) { $maxRun = $w } }
            $lineSum = 0; $lineCount = 0
            if ($maxRun -gt 0) {
                for ($y = 0; $y -lt $bmp.Height; $y++) {
                    if ($rowWidth[$y] -eq $maxRun) { $lineSum += $y; $lineCount++ }
                }
            }
            $lineY = if ($lineCount) { [int][Math]::Round($lineSum / $lineCount) } else { -1 }
            return [pscustomobject]@{
                W = $maxX - $minX + 1
                H = $maxY - $minY + 1
                PadT = $minY
                PadB = $bmp.Height - 1 - $maxY
                PadL = $minX
                PadR = $bmp.Width - 1 - $maxX
                LineY = $lineY
            }
        } finally { $bmp.Dispose() }
    } finally { $ms.Dispose() }
}

function Get-IcoFrames([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $count = [BitConverter]::ToUInt16($bytes, 4)
    for ($i = 0; $i -lt $count; $i++) {
        $o = 6 + 16 * $i
        $w = if ($bytes[$o] -eq 0) { 256 } else { $bytes[$o] }
        $h = if ($bytes[$o + 1] -eq 0) { 256 } else { $bytes[$o + 1] }
        $len = [BitConverter]::ToUInt32($bytes, $o + 8)
        $off = [BitConverter]::ToUInt32($bytes, $o + 12)
        $blob = New-Object byte[] $len
        [Array]::Copy($bytes, $off, $blob, 0, $len)
        [pscustomobject]@{ Size = $w; Data = $blob }
    }
}

$icos = Get-ChildItem -LiteralPath $iconDir -Filter 'tray*.ico' | Sort-Object Name
if (-not $icos) { throw "No tray*.ico found in $iconDir" }

$failures = New-Object System.Collections.Generic.List[string]
$warnings = New-Object System.Collections.Generic.List[string]
$rows = New-Object System.Collections.Generic.List[object]
# Sunset frames register against each other: key "<theme>|<size>" -> line row seen.
$sunsetLines = @{}

foreach ($ico in $icos) {
    $isSunset = $ico.Name.StartsWith($SunsetPrefix)
    foreach ($frame in Get-IcoFrames $ico.FullName) {
        $ink = Get-InkBox $frame.Data
        if (-not $ink) {
            $failures.Add("$($ico.Name) $($frame.Size)px: frame is empty")
            continue
        }
        $size = $frame.Size
        $fillW = $ink.W / $size
        $fillH = $ink.H / $size
        $longest = [Math]::Max($fillW, $fillH)
        $offBy = $ink.PadT - $ink.PadB

        if ($isSunset) {
            # Registration gate instead of centring/fill. Also insist the sun never
            # touches the frame edge, which would clip it as it travels.
            $theme = ($ico.BaseName -split '-')[1]
            $key = "$theme|$size"
            if ($ink.PadT -eq 0) {
                $failures.Add("$($ico.Name) ${size}px: ink touches the top edge (sun clipped)")
            }
            if ($ink.PadB -eq 0) {
                $failures.Add("$($ico.Name) ${size}px: ink touches the bottom edge (sun clipped)")
            }
            if ($ink.LineY -lt 0) {
                $failures.Add("$($ico.Name) ${size}px: horizon rule not found")
            } elseif ($sunsetLines.ContainsKey($key)) {
                $drift = [Math]::Abs($ink.LineY - $sunsetLines[$key])
                if ($drift -gt $LineTol) {
                    $failures.Add("$($ico.Name) ${size}px: horizon at row $($ink.LineY) but sibling frames put it at $($sunsetLines[$key]) ($drift px drift)")
                }
            } else {
                $sunsetLines[$key] = $ink.LineY
            }
        } else {
            # Hard gates: centring and clipping. Both are pipeline defects.
            if ([Math]::Abs($offBy) -gt $CenterTol) {
                $failures.Add("$($ico.Name) ${size}px: off-centre by $offBy px (padT=$($ink.PadT) padB=$($ink.PadB))")
            }
            if ($longest -lt $LongestBand[0] -or $longest -gt $LongestBand[1]) {
                $failures.Add(("$($ico.Name) ${size}px: longest side {0:P1} outside {1:P0}-{2:P0} (clipped or overflowing)" -f $longest, $LongestBand[0], $LongestBand[1]))
            }
            # Advisory only: a glyph too narrow to read at tray size is an artwork
            # property, so it is surfaced per design rather than failing the build.
            if ($fillW -lt $WidthFloor) {
                $warnings.Add(("$($ico.Name) ${size}px: ink width {0:P1} below {1:P0} advisory floor" -f $fillW, $WidthFloor))
            }
        }

        $rows.Add([pscustomobject]@{
            Ico    = $ico.Name
            Size   = $size
            InkW   = $ink.W
            InkH   = $ink.H
            FillW  = $fillW
            FillH  = $fillH
            PadT   = $ink.PadT
            PadB   = $ink.PadB
            VOff   = $offBy
            LineY  = $ink.LineY
            Ok     = if ($isSunset) { $true } else { ($fillW -ge $WidthFloor) }
        })
    }
}

$rows | Format-Table Ico, Size, InkW, InkH, @{n='FillW';e={'{0,6:P1}' -f $_.FillW}},
    @{n='FillH';e={'{0,6:P1}' -f $_.FillH}}, PadT, PadB, VOff, LineY, Ok -AutoSize

"frames measured  : $($rows.Count) across $($icos.Count) ico files"
"longest-side fill: target {0:P0}, gated to {1:P0}-{2:P0}" -f $FillLongest, $LongestBand[0], $LongestBand[1]
"gates            : |padT-padB| <= $CenterTol px, longest side in band"
"advisory         : ink width >= {0:P0} (narrow artwork, reported not gated)" -f $WidthFloor
"sunset design    : horizon rule registered within $LineTol px across every frame; no edge clipping"

if ($warnings.Count) {
    ""
    "ADVISORY ($($warnings.Count)) - narrow artwork, grouped by design:"
    $warnings | ForEach-Object { ($_ -split ':')[0] } | ForEach-Object { ($_ -split ' ')[0] } |
        Group-Object | Sort-Object Name | ForEach-Object { "  {0}: {1} frames below width floor" -f $_.Name, $_.Count }
}

if ($failures.Count) {
    ""
    "FAILURES ($($failures.Count)):"
    $failures | ForEach-Object { "  $_" }
    if ($Check) { exit 1 }
} else {
    ""
    "PASS: static glyphs are centred and unclipped; sunset frames are registered."
    if ($Check) { exit 0 }
}
