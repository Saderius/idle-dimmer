$ErrorActionPreference = 'Stop'
# Build-time tray icon generator: SVG -> alpha mask -> theme/state variants -> multi-size .ico
# Design ids MUST stay in sync with kIconNames[] in IdleDimmer.cpp.
Add-Type -AssemblyName System.Drawing

$here = $PSScriptRoot
# A design either maps ONE svg across the shared $states list, or carries an explicit
# state -> svg map. The sunset timer needs the latter: its artwork is a different file per
# descent step, because the sun's position is what encodes the idle countdown.
# Raw designs skip Refit-Mask (see the comment at the main loop) and are drawn by hand
# already normalised into the 20x20 frame.
$designs = @(
    @{ Id = 1; States = [ordered]@{
        on  = 'tray-dimmer-bulbCollarChevron.svg'
        off = 'tray-dimmer-bulbCollarChevron.svg'
        dim = 'tray-dimmer-bulbCollarChevron-dim.svg'
    } },
    @{ Id = 2; States = [ordered]@{
        on  = 'tray-dimmer-horizonSunDoubleChevron.svg'
        off = 'tray-dimmer-horizonSunDoubleChevron.svg'
        dim = 'tray-dimmer-horizonSunDoubleChevron-dim.svg'
    } },
    @{ Id = 3; States = [ordered]@{
        on  = 'tray-dimmer-horizonSunOriginal.svg'
        off = 'tray-dimmer-horizonSunOriginal.svg'
        dim = 'tray-dimmer-horizonSunOriginal-dim.svg'
    } },
    @{ Id = 4; States = [ordered]@{
        on  = 'tray-dimmer-pick4SingleChevron-separated.svg'
        off = 'tray-dimmer-pick4SingleChevron.svg'
        dim = 'tray-dimmer-pick4SingleChevron-separated.svg'
    } },
    @{ Id = 5; Raw = $true; States = [ordered]@{
        off = 'tray-dimmer-sunset-off.svg'
        dim = 'tray-dimmer-sunset-dim.svg'
        d0  = 'tray-dimmer-sunset-d0.svg'
        d1  = 'tray-dimmer-sunset-d1.svg'
        d2  = 'tray-dimmer-sunset-d2.svg'
        d3  = 'tray-dimmer-sunset-d3.svg'
        d4  = 'tray-dimmer-sunset-d4.svg'
        d5  = 'tray-dimmer-sunset-d5.svg'
    } }
)
# ondark = shown on a dark taskbar; onlight = shown on a light taskbar.
$palette = @{
    'ondark'  = @{ on = @(255,255,255); off = @(154,154,154); dim = @(112,182,255) }
    'onlight' = @{ on = @(32,32,32);    off = @(154,154,154); dim = @(31,122,224) }
}
$states = @('on','off','dim')
# One frame per SM_CXSMICON value Windows can report at 100%-300% scaling
# (16, 20, 24, 28, 32, 36, 40, 48), so LoadImageW never has to resample.
$sizes = @(16,20,24,28,32,36,40,48)
$renderSize = 400

$edge = @(
    (Join-Path $env:ProgramFiles 'Microsoft\Edge\Application\msedge.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft\Edge\Application\msedge.exe')
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge not found; cannot rasterize SVG.' }

function Get-Screenshot([string]$svgPath, [int]$size) {
    $svgUri = ([Uri]$svgPath).AbsoluteUri
    $html = Join-Path $env:TEMP ('idleicons-' + [IO.Path]::GetFileNameWithoutExtension($svgPath) + '.html')
    $doc = "<html><head><style>html,body{margin:0;padding:0;background:#FF00FF}img{position:absolute;left:0;top:0;width:${size}px;height:${size}px}</style></head><body><img src=`"$svgUri`"></body></html>"
    Set-Content -LiteralPath $html -Value $doc -Encoding UTF8
    $out = Join-Path $env:TEMP ('idleicons-' + [IO.Path]::GetFileNameWithoutExtension($svgPath) + '.png')
    if (Test-Path $out) { Remove-Item $out -Force }
    $profile = Join-Path $env:TEMP 'idleicons-edge-profile'
    # Edge logs to stderr; tolerate it (PS 5.1 would otherwise raise a terminating error).
    # Bounded wait: an unbounded Edge call is what hung the first CI run for 15 minutes.
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'SilentlyContinue'
    try {
        $proc = Start-Process -FilePath $edge -PassThru -WindowStyle Hidden -ArgumentList @(
            '--headless', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
            "--user-data-dir=`"$profile`"", "--screenshot=`"$out`"", "--window-size=$size,$size",
            ([Uri]$html).AbsoluteUri
        )
        if (-not $proc.WaitForExit(60000)) {
            try { $proc.Kill() } catch { }
            throw "Edge timed out after 60 s rasterizing $svgPath"
        }
    } finally { $ErrorActionPreference = $prevEAP }
    if (-not (Test-Path $out)) { throw "Rasterization failed for $svgPath" }
    return $out
}

# White glyph composited over pure magenta (#FF00FF): alpha = G/255, color = white. Fringe-free.
function Get-AlphaMask([string]$pngPath) {
    $src = New-Object Drawing.Bitmap($pngPath)
    $mask = New-Object Drawing.Bitmap($src.Width, $src.Height)
    for ($y = 0; $y -lt $src.Height; $y++) {
        for ($x = 0; $x -lt $src.Width; $x++) {
            $c = $src.GetPixel($x, $y)
            $a = [int][Math]::Round($c.G)   # magenta G=0, white G=255
            if ($a -gt 0) { $mask.SetPixel($x, $y, [Drawing.Color]::FromArgb($a, 255, 255, 255)) }
        }
    }
    $src.Dispose()
    return $mask
}

# Trim transparent margins and re-center the glyph so it fills ~88% of a square frame.
# Without this each design's internal padding makes it look smaller than shell tray icons
# at the same canvas size (measured against volume/wi-fi on the taskbar).
function Refit-Mask([Drawing.Bitmap]$mask) {
    $minX = $mask.Width; $minY = $mask.Height; $maxX = -1; $maxY = -1
    for ($y = 0; $y -lt $mask.Height; $y++) {
        for ($x = 0; $x -lt $mask.Width; $x++) {
            if ($mask.GetPixel($x, $y).A -gt 0) {
                if ($x -lt $minX) { $minX = $x }
                if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }
                if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }
    if ($maxX -lt 0) { return $mask }
    $cw = $maxX - $minX + 1
    $ch = $maxY - $minY + 1
    $longest = [Math]::Max($cw, $ch)
    $frame = [int][Math]::Ceiling($longest / 0.88)
    if ($frame -lt $longest) { $frame = $longest + 1 }
    $b = New-Object Drawing.Bitmap($frame, $frame)
    $g = [Drawing.Graphics]::FromImage($b)
    $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([Drawing.Color]::Transparent)
    # Centre each axis independently. A single offset derived from the glyph *width* was
    # applied to both axes, which pushed tall glyphs (designs 1 and 4) down and ran their
    # feet off the bottom of the square frame, so they emitted short and bottom-flush.
    $dx = [int][Math]::Round(($frame - $cw) / 2.0)
    $dy = [int][Math]::Round(($frame - $ch) / 2.0)
    $g.DrawImage($mask, ([Drawing.Rectangle]::new($dx, $dy, $cw, $ch)), $minX, $minY, $cw, $ch, [Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    $mask.Dispose()
    return $b
}

function Scale-Mask([Drawing.Bitmap]$mask, [int]$size) {
    $b = New-Object Drawing.Bitmap($size, $size)
    $g = [Drawing.Graphics]::FromImage($b)
    $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.Clear([Drawing.Color]::Transparent)
    $g.DrawImage($mask, 0, 0, $size, $size)
    $g.Dispose()
    return $b
}

function Set-Tint([Drawing.Bitmap]$b, [int[]]$rgb) {
    for ($y = 0; $y -lt $b.Height; $y++) {
        for ($x = 0; $x -lt $b.Width; $x++) {
            $c = $b.GetPixel($x, $y)
            if ($c.A -gt 0) { $b.SetPixel($x, $y, [Drawing.Color]::FromArgb($c.A, $rgb[0], $rgb[1], $rgb[2])) }
        }
    }
    return $b
}

function Write-Ico([Drawing.Bitmap[]]$Images, [string]$Path) {
    $blobs = @()
    foreach ($img in $Images) {
        $ms = New-Object IO.MemoryStream
        $img.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
        $blobs += ,$ms.ToArray()
        $ms.Dispose()
    }
    $offset = 6 + (16 * $blobs.Count)
    $fs = [IO.File]::Create($Path)
    $bw = New-Object IO.BinaryWriter($fs)
    $bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$blobs.Count)
    for ($i = 0; $i -lt $blobs.Count; $i++) {
        $w = $Images[$i].Width; $h = $Images[$i].Height
        $bw.Write([byte]$(if ($w -ge 256) { 0 } else { $w }))
        $bw.Write([byte]$(if ($h -ge 256) { 0 } else { $h }))
        $bw.Write([byte]0); $bw.Write([byte]0)
        $bw.Write([uint16]1); $bw.Write([uint16]32)
        $bw.Write([uint32]$blobs[$i].Length)
        $bw.Write([uint32]$offset)
        $offset += $blobs[$i].Length
    }
    foreach ($b in $blobs) { $bw.Write($b) }
    $bw.Dispose(); $fs.Dispose()
}

# ---- main ----
$built = @{}
foreach ($d in $designs) {
    $svgFor = $d.States
    if (-not $svgFor) { $svgFor = [ordered]@{}; foreach ($s in $states) { $svgFor[$s] = $d.Svg } }

    $masks = [ordered]@{}
    foreach ($state in $svgFor.Keys) {
        $svg = Join-Path $here $svgFor[$state]
        if (-not (Test-Path $svg)) { Write-Warning "missing $($svgFor[$state])"; continue }
        $alpha = Get-AlphaMask (Get-Screenshot $svg $renderSize)
        # Refit-Mask trims to each frame's own content box and re-centres on both axes. For the
        # sunset that is fatal: frames differ only in the sun's height, so their content boxes
        # differ, and re-centring each independently makes the horizon line jump between steps.
        # Raw designs are authored in a fixed frame and skip it.
        $masks[$state] = if ($d.Raw) { $alpha } else { Refit-Mask $alpha }
    }

    foreach ($theme in @('ondark','onlight')) {
        foreach ($state in $svgFor.Keys) {
            $mask = $masks[$state]
            if (-not $mask) { continue }
            # Descent steps are a normal "on" tint; only off and dim have their own colour.
            $tint = if ($palette[$theme].ContainsKey($state)) { $palette[$theme][$state] } else { $palette[$theme]['on'] }
            $images = @()
            foreach ($s in $sizes) {
                $img = Scale-Mask $mask $s
                Set-Tint $img $tint | Out-Null
                $images += $img
            }
            $icoPath = Join-Path $here ("tray{0}-{1}-{2}.ico" -f $d.Id, $theme, $state)
            Write-Ico $images $icoPath
            $built[("{0}-{1}-{2}" -f $d.Id, $theme, $state)] = $images
        }
    }
    foreach ($m in $masks.Values) { $m.Dispose() }
    Write-Output ("built design {0} from {1} state(s)" -f $d.Id, $svgFor.Count)
}

# Verification sheet: every design x theme x state at 48 px, plus a true-size tray strip.
# The 48 px row shows the artwork; the strip below shows the only size that actually
# matters, because the taskbar renders each glyph inside SM_CXSMICON (32 px at 200% on
# this machine). An icon that reads at 48 px can still be mush at 32.
$cellW = 192; $cellH = 72
$sheet = New-Object Drawing.Bitmap(($cellW * 2 + 24), (($cellH + 16) * $designs.Count + 8))
$g = [Drawing.Graphics]::FromImage($sheet)
$g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::Half
$g.Clear([Drawing.Color]::FromArgb(64,64,64))
for ($row = 0; $row -lt $designs.Count; $row++) {
    $d = $designs[$row]
    $id = $d.Id
    $y = 8 + $row * ($cellH + 16)
    $dark = [Drawing.Rectangle]::new(8, $y, $cellW, $cellH)
    $light = [Drawing.Rectangle]::new(16 + $cellW, $y, $cellW, $cellH)
    $g.FillRectangle([Drawing.Brushes]::DarkGray, $dark)
    $g.FillRectangle((New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(240,240,240))), $light)
    $xLight = 16 + $cellW
    foreach ($pair in @(@('ondark', 8), @('onlight', $xLight))) {
        $theme = $pair[0]; $x0 = [int]$pair[1] + 12
        $i = 0
        $stateList = if ($d.States) { @($d.States.Keys) } else { $states }
        foreach ($state in $stateList) {
            $key = "{0}-{1}-{2}" -f $id, $theme, $state
            if ($built.ContainsKey($key)) { $g.DrawImage($built[$key][4], ($x0 + $i * 56), ($y + 12)) }  # [4] = 48 px
            $i++
        }
    }
}
$g.Dispose()

# --- true-size strip: the sunset descent at 16/20/24/28/32/36/40/48 px on both themes ---
# Each row is filled with that theme's TASKBAR colour across its FULL width, because the
# glyph tint is only legible against the background it will actually sit on. Tinting just a
# label column made every onlight frame look like it had gone missing.
$stripW = 620; $stripH = 8 * 46 + 12
$strip = New-Object Drawing.Bitmap($stripW, $stripH)
$sg = [Drawing.Graphics]::FromImage($strip)
$sg.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$sg.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::Half
$sg.Clear([Drawing.Color]::FromArgb(32, 32, 32))
$order = @('off', 'd0', 'd1', 'd2', 'd3', 'd4', 'd5', 'dim')
$idxOf = @{ '16' = 0; '20' = 1; '24' = 2; '28' = 3; '32' = 4; '36' = 5; '40' = 6; '48' = 7 }
$font = New-Object Drawing.Font -ArgumentList 'Consolas', 9
for ($r = 0; $r -lt 8; $r++) {
    $sz = $sizes[$r]
    $y = 4 + $r * 46
    $theme = if ($r -lt 4) { 'ondark' } else { 'onlight' }
    $br = if ($r -lt 4) { 32 } else { 240 }

    $bandRect = New-Object Drawing.Rectangle -ArgumentList 0, $y, $stripW, 42
    $bandBrush = New-Object Drawing.SolidBrush -ArgumentList ([Drawing.Color]::FromArgb($br, $br, $br))
    $sg.FillRectangle($bandBrush, $bandRect)
    $bandBrush.Dispose()

    $labelColor = if ($r -lt 4) { [Drawing.Color]::FromArgb(255, 255, 255, 255) } else { [Drawing.Color]::FromArgb(255, 0, 0, 0) }
    $labelBrush = New-Object Drawing.SolidBrush -ArgumentList $labelColor
    $sg.DrawString([string]$sz, $font, $labelBrush, 6, ($y + 14))

    # Every frame sits in the same 68 px slot so consecutive steps can be compared side by
    # side at a fixed eye distance rather than at a shifting one.
    $x = 60
    foreach ($st in $order) {
        $key = "5-$theme-$st"
        if ($built.ContainsKey($key)) {
            $img = $built[$key][$idxOf[[string]$sz]]
            $sg.DrawImage($img, $x, ($y + [int]((42 - $sz) / 2)))
        }
        $x += 68
    }
    $labelBrush.Dispose()
}
$font.Dispose()
$sg.Dispose()
$stripPath = Join-Path $here 'verify-tray.png'
$strip.Save($stripPath, [Drawing.Imaging.ImageFormat]::Png)
$strip.Dispose()
$verifyPath = Join-Path $here 'verify.png'
$sheet.Save($verifyPath, [Drawing.Imaging.ImageFormat]::Png)
$sheet.Dispose()
foreach ($b in $built.Values) { foreach ($img in $b) { $img.Dispose() } }

$icoCount = (Get-ChildItem $here -Filter 'tray*.ico' -ErrorAction SilentlyContinue).Count
Write-Output "generated $icoCount .ico files; sheet: $verifyPath"
