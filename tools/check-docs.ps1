<#
  check-docs.ps1 - documentation/code consistency gate.

  Documentation in this project went stale repeatedly, and each failure was the same
  shape: a document described a mechanism that the code no longer used, and nobody
  noticed until much later. The worst instance was a status string reading
  "Enabled; checking every 30 seconds" that survived a scheduler change which made it
  false - so anyone reading the code to learn the wakeup rate got a wrong answer.

  This gate turns those failures into build errors. Four checks:

    1. LIVE TERMS        every mechanism present in the code must be documented.
    2. STALE CLAIMS       specific false assertions must not appear anywhere in docs.
    3. STATUS STRINGS     every user-visible status string in the code must appear in
                          the docs. This is check 2 generalised - it catches the next
                          one automatically instead of waiting for someone to notice.
    4. DEAD TERMS (warn) symbols the code no longer contains but a doc still mentions,
                          reported with line numbers for human judgement. Mentions like
                          "does not call SetForegroundWindow" or "kIdleFirst are gone"
                          are legitimate, so this is a warning, never a failure.

  Exit 0 = pass. Exit 1 = at least one hard failure.
#>

[CmdletBinding()]
param(
    # Treat warnings as failures. Used in CI, off for local runs so the dead-term
    # report does not block work while a human triages it.
    [switch]$Strict
)

$ErrorActionPreference = 'Stop'
# This file lives in tools\, so the repo root is one level up.
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$codeFiles = @(
    'IdleDimmer.cpp', 'Flyout.h', 'Flyout.cpp',
    'TrayGesture.h', 'build.ps1', 'icons\build-icons.ps1',
    # SessionStats arrived after this list was written by hand and was left out, so the gate was
    # scanning a codebase that no longer existed. A gate blind to a module cannot catch that
    # module inventing a user-visible string.
    'SessionStats.h', 'SessionStats.cpp',
    # The guard predicate lives in a header of its own so --self-test can pin the
    # maximised-window regression. A gate that did not scan it would not see a change to it.
    'ScreenGuard.h'
) | Where-Object { Test-Path $_ }

$code = ($codeFiles | ForEach-Object { Get-Content $_ -Raw }) -join "`n"

# Docs that make factual claims about behaviour. AGENTS.md is deliberately excluded
# from the hard checks: it discusses mechanisms historically ("the old code polled
# the cursor position"), which is correct and must not be flagged.
$claimDocs = @('IMPLEMENTATION.md', 'README.md')
$claimDocText = ($claimDocs | Where-Object { Test-Path $_ } | ForEach-Object { Get-Content $_ -Raw }) -join "`n"

$failures = @()
$warnings = @()

function Test-DocMentions {
    param([string]$Term, [string]$Text = $claimDocText)
    return $Text -match [regex]::Escape($Term)
}

# True when a line is quoting or describing something in the past rather than asserting
# it about the current code. Documentation is allowed - and encouraged - to explain what
# used to be wrong; what it may not do is claim it is still true.
function Test-Historical {
    param([string]$Line)
    return $Line -match '(does not|do not|no longer|never|gone|removed|earlier|previously|used to|was a |were|obsolete|superseded|no-op|returns E_FAIL|E_FAIL|unavailable|legacy|wrong|bug|instead of)'
}

# --- 1. LIVE TERMS -------------------------------------------------------------
# Mechanisms the code uses that the docs must explain. Listed, not auto-derived:
# deriving them from code would flag every local variable. This list is the reviewer's
# contract - add a term here when you add a mechanism.
$liveTerms = @(
    'WH_MOUSE_LL',        # click-away dismissal
    'ScheduleNext',       # adaptive wake scheduling
    'ResolveTrayGesture', # tray gesture collapsing
    'ClassifyTrayMessage',
    'RectContainsPoint',
    'ParseSettings',           # settings.ini reader: BOM-tolerant, last [Settings] wins
    'FormatSettings',          # canonical settings.ini writer (no BOM, one section, CRLF)
    'SettingsTextIsCanonical', # the startup heal's "is this file already canonical" test
    'PowerCardRect'            # power dashboard card: documented skeleton (empty) state
    'sessionstats'             # session counters feeding the power dashboard card
    'SaveStats'                # persists the power-dashboard aggregates across restarts
    'hasDimHistory'            # never-vs-zero discriminator behind the card's em dashes
    'LooksFullScreen'          # full-screen guard: window-style test that rejects maximised windows
    'ScheduleWaitMs'           # scheduler sleep: ready-to-dim and dimmed cadences
    'kCheckReadyMs'            # ... and the cadence it introduced to stop the 15-minute sleep
    'kCheckDimmedMs'           # ... and the rare cadence once dimmed, replacing a 250 ms poll
    'WallDeltaMs'              # persisted stats.bin: wall-clock age, saturated so a clock step cannot underflow
    'SystemAccent'            # accent colour: AccentPalette entry 1, then the design's fallbacks
    'SystemAccentHover'       # the highlight swatch (entry 0) painted on a hovered tile
    'panelHover'              # hover/press state must be copied into PanelState, not just tracked
    'HitTestHover'            # hover geometry is the tile body; click geometry includes the caption
    'AccentPalette[1]'        # which entry of the ramp is the resting tile fill, and why
    'BlitPanelIcon',          # panel glyphs: 4x alpha mask -> area-filtered, tinted DIB pixels
    'build-panel-icons',      # SVG -> 80px alpha PNG -> generated header, run before the compile
    'DimManual',              # manual dimming with 2-second input grace period
    'set-app-icon',           # embeds tray icon as executable's main application icon
    'SliderMagnet',           # one-way preset magnet: 2% pull-in, ~1 px release, click never snaps
    'SliderPermilleAtX',      # continuous 1..60 mapping: per-mille avoids collapsing 38/39
    'MinutesToTrackPx'        # linear minute -> pixel position, one rounding step, 59 equal intervals
    'ProgressFillWidth'       # power-card bar: session dimmed time / process uptime, clamped, pure
    'lastDimKnown'            # last-dim clock stamp is missing/usable, separate from dim history
    'restoreCount'            # the card's "Wake count" source: restores from the dimmed state
    'v1.0.0'                 # footer version hint beneath the app name
    'dimmingFilled'          # enabled Dimming tile's filled bulb SVG alpha mask
    'gDimFilled'             # enabled panel bulb with transparent separator around its inset fill
    "on  = 'tray-dimmer-pick4SingleChevron-separated.svg'" # tray bulb outline separated from its fill
    'ReserveTaskbarSpace'     # auto-hidden taskbar excluded from panel placement
    'ABM_GETTASKBARPOS'       # expanded taskbar bounds, including when hidden
    'AnchorPanel'            # Quick Settings-style resting position
    'PanelEntranceOffset'    # pure quintic ease-out for the entrance slide
    'kPanelMotionTimer'      # shared bounded entrance/exit timer
    'PanelExitOffset'        # reverse closing motion, including interrupted entrances
    'DismissPanel'           # animated ordinary dismissal; HidePanel remains force-hide
    'TrayPressClosesPanel'   # tray mouse press/release retains close intent
    'trayPressStartedOpen'   # captured on tray press, consumed on release
    'FillSliderCapsule'      # supersampled antialiasing for native-style slider surfaces
    'kSliderThumbDiameter'   # fixed outer ring with state-dependent inner disc
    'SPI_GETCLIENTAREAANIMATION' # follows the Windows animation preference
)
Write-Host "== 1. live terms documented ==" -ForegroundColor Cyan
foreach ($t in $liveTerms) {
    if (-not $code.Contains($t)) {
        Write-Host "  SKIP  $t (not in code - remove it from this list)" -ForegroundColor DarkGray
        continue
    }
    if (Test-DocMentions $t) {
        Write-Host "  ok    $t" -ForegroundColor Green
    } else {
        $failures += "live mechanism '$t' is in the code but not documented in $(($claimDocs -join ', '))"
        Write-Host "  FAIL  $t used in code, absent from docs" -ForegroundColor Red
    }
}

# --- 2. STALE CLAIMS ------------------------------------------------------------
# Literal assertions that were true once and are now false. Add one here the moment a
# mechanism is replaced.
$staleClaims = @(
    'checking every 30 seconds',
    'footer shows the battery percentage',
    'session/battery info',
    'session and battery status',
    'panel is centered over the clicked tray icon',
    'Dismissal remains immediate',
    'checks every 30 seconds',
    'checked every 30 seconds',
    'kCheckTimer,30000',
    'every 30 s with a 1 s coalescing',
    'de-duplicated within 300 ms',
    # The startup log line claimed a fixed 30-second cadence while the scheduler had been adaptive
    # for a long time. It now says "adaptive checks". Old activity.log entries still say
    # "30-second checks" and are correct for the build that wrote them - a log is a record of what
    # a process did, so those entries are left alone rather than rewritten.
    '30-second checks',
    # The power dashboard card's values were fabricated - they were never measured, and nothing
    # in the app collects them. They are gone; the card renders an em dash per value. This entry
    # stops the old numbers coming back as if they were data.
    '09:14 restored 09:17',
    # The footer used to carry a hardcoded build date. A date compiled into a shipped binary is
    # wrong the moment the source moves. The later uptime line has also been removed.
    'build 2026-10-17',
    'The footer shows process uptime',
    'app identity and uptime',
    'app name/version vertically centered',
    'enabled/idle = wireframe glyph',
    'existing dimmed ICO''s per-size alpha masks were reused',
    'The panel glyph is unchanged by this tray-only update',
    # settings.ini is no longer the Windows profile API's file: it cannot see a section behind
    # a UTF-8 BOM, which is how the duplicate [Settings] header was created in the first place.
    'GetPrivateProfileInt',
    'WritePrivateProfileString',
    # The panel's glyphs used to be integer GDI line art drawn at paint time. That
    # produced binary edges (two colours in a 30x30 crop); the icons are now pre-rasterized
    # alpha masks. Keep the old claim out of the docs.
    'vector glyphs drawn at paint time',
    # The accent was read from AccentPalette entry 0 on the false claim that it was the tile
    # fill. Entry 0 is the lightest shade in the ramp - what a tile shows while the pointer is
    # over it - so the panel painted one swatch too light for weeks. The tile fill is entry 1.
    'entry 0 is the accent',
    # The colour that false claim produced. It is quoted in the docs only to explain the
    # mistake, never as a current value.
    '#EADFCD',
    # Tile hover used to be a flat per-channel addition, which cannot reproduce the ramp
    # step (+24,+33,+31) between entry 1 and entry 0. Hover now paints entry 0 exactly.
    'flat +16 lift',
    # The slider used to snap only to seven detents and deliberately paint no dots. It then
    # gained seven dots at logarithmic positions, which the user rejected: the log scale made
    # 1->2 enormous and 59->60 a pixel or two. It is now linear over 59 equal intervals with
    # no dots at all, so both the old detent-only claim and the dot row must stay out.
    'no detent tick marks',
    'snapping to 1 / 2 / 5',
    'seven preset dots',
    'Position is logarithmic',
    'log positions',
    # The executable embedded design 3's glyph while the tray showed whatever design the user
    # had picked. The app icon is design 4 now; naming tray3 as the embedded icon is stale.
    'tray3-ondark-on.ico',
    # The card's bar was a bare track on purpose. It now shows the session's dimmed share of
    # uptime, so the old "no denominator" reasoning is stale. Both spellings are listed because
    # the doc used "unfilled" while the anti-claim named "bare".
    'The bar stays a bare track',
    'The bar stays an unfilled track',
    # The age used to be computed from two clocks (process tick in-session, wall clock across
    # restarts). The row now has one source, so the two-branch description must not return.
    'computes the age on two branches',
    # A clock-time + compact-age variant of "Last dim" shipped once and was rejected: the user
    # wants a plain minute count. Keep the clock-time wording out of the docs.
    'local clock time plus a compact age',
    '09:14 (now)'
) | Select-Object -Unique
Write-Host "`n== 2. stale claims purged ==" -ForegroundColor Cyan
foreach ($s in $staleClaims) {
    $hit = $false
    foreach ($d in ($claimDocs + 'AGENTS.md')) {
        if (Test-Path $d) {
            $m = Select-String -Path $d -SimpleMatch $s -EA SilentlyContinue
            if ($m) {
                # Quoting the old text in order to explain what went wrong is legitimate and
                # common - AGENTS.md does exactly this. Only an unexplained restatement fails.
                $unexplained = $m | Where-Object { -not (Test-Historical $_.Line) }
                if ($unexplained) {
                    $failures += "stale claim '$s' still in $d line $($unexplained[0].LineNumber)"
                    Write-Host "  FAIL  '$s' -> $d line $($unexplained[0].LineNumber)" -ForegroundColor Red
                    $hit = $true
                } else {
                    Write-Host "  ok    '$s' (quoted historically at $d line $($m[0].LineNumber))" -ForegroundColor DarkGray
                }
            }
        }
    }
    if (-not $hit) { Write-Host "  ok    '$s'" -ForegroundColor Green }
}

# --- 3. STATUS STRINGS ----------------------------------------------------------
# Every literal the user can see in the tray tooltip must be accounted for in the docs.
# This is the check that would have caught the "checking every 30 seconds" lie on its own.
Write-Host "`n== 3. user-visible status strings documented ==" -ForegroundColor Cyan
$statuses = [regex]::Matches(
    (Get-Content 'IdleDimmer.cpp' -Raw),
    'wcscpy\(\s*status\s*,\s*L"([^"]*)"') |
    ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
if (-not $statuses) {
    $warnings += 'no status strings found - did the wcscpy(status, ...) pattern change?'
    Write-Host "  WARN  could not find any status strings" -ForegroundColor Yellow
} else {
    foreach ($s in $statuses) {
        if (Test-DocMentions $s) {
            Write-Host "  ok    '$s'" -ForegroundColor Green
        } else {
            $failures += "user-visible status '$s' is not documented"
            Write-Host "  FAIL  '$s' shown to the user but not in docs" -ForegroundColor Red
        }
    }
}

# --- 4. DEAD TERMS (warning only) ----------------------------------------------
Write-Host "`n== 4. symbols in docs but not in code (review) ==" -ForegroundColor Cyan
$dead = Select-String -Path ($claimDocs | Where-Object { Test-Path $_ }) `
    -Pattern '\b(Menu\(|ShowTrayMenu|kIdleFirst|kEnabled|kExit|SetForegroundWindow|Shell_NotifyIconGetRect|GetDC\(NULL\))' -EA SilentlyContinue
if (-not $dead) {
    Write-Host "  none" -ForegroundColor Green
} else {
    foreach ($m in $dead) {
        $line = $m.Line.Trim()
        # Heuristic: negative/historical framing is legitimate documentation of absence.
        $benign = Test-Historical $line
        if ($benign) {
            Write-Host "  ok    $($m.Filename):$($m.LineNumber) (documents absence)" -ForegroundColor DarkGray
        } else {
            $warnings += "$($m.Filename):$($m.LineNumber) mentions '$($m.Matches[0].Value)' which no longer exists in code"
            Write-Host "  WARN  $($m.Filename):$($m.LineNumber) $($m.Matches[0].Value)" -ForegroundColor Yellow
        }
    }
}

# --- 5. DOCUMENTED BINARY SIZE (warning only) ----------------------------------
# AGENTS.md section 7.2: every number carries a status tag, and a stale number repeated across
# two documents is expensive to unwind. The size claim drifted twice before this check existed
# (172 KB in AGENTS.md, 188 KB in IMPLEMENTATION.md, while the binary was 207,360 bytes).
Write-Host "`n== 5. documented binary size matches the built file ==" -ForegroundColor Cyan
$exe = Join-Path $repo 'IdleDimmer.exe'
if (-not (Test-Path $exe)) {
    Write-Host "  skip  IdleDimmer.exe not built yet" -ForegroundColor DarkGray
} else {
    $actualBytes = (Get-Item $exe).Length
    $actualKb = [math]::Round($actualBytes / 1KB)
    $sizeClaims = Select-String -Path ($claimDocs | Where-Object { Test-Path $_ }) `
        -Pattern '~?(\d{3})\s*KB' -AllMatches -EA SilentlyContinue
    if (-not $sizeClaims) {
        $warnings += "no documented binary size found; README should state the current size"
        Write-Host "  WARN  no size claim found" -ForegroundColor Yellow
    }
    foreach ($m in $sizeClaims) {
        foreach ($hit in $m.Matches) {
            $claimed = [int]$hit.Groups[1].Value
            # A 3 KB tolerance absorbs rounding (KB vs KiB) without hiding a real drift.
            if ([math]::Abs($claimed - $actualKb) -le 3) {
                Write-Host "  ok    $($m.Filename):$($m.LineNumber) '$($hit.Value)' (actual ${actualKb} KB)" -ForegroundColor Green
            } else {
                $warnings += "$($m.Filename):$($m.LineNumber) claims '$($hit.Value)' but IdleDimmer.exe is $actualKb KB"
                Write-Host "  WARN  $($m.Filename):$($m.LineNumber) '$($hit.Value)' vs actual ${actualKb} KB" -ForegroundColor Yellow
            }
        }
    }
}

# --- 6. LOCAL LINKS RESOLVE (warning only) -------------------------------------
# Deleting docs/ux-model.html left two dead links behind, and nothing noticed. A public README
# with a 404 link is the most visible kind of stale documentation.
Write-Host "`n== 6. local markdown links resolve ==" -ForegroundColor Cyan
$linkDocs = @('README.md', 'IMPLEMENTATION.md', 'AGENTS.md') + (Get-ChildItem 'docs' -Filter '*.md' -Recurse -File -EA SilentlyContinue | ForEach-Object { $_.FullName })
$deadLinks = 0
foreach ($doc in $linkDocs) {
    if (-not (Test-Path $doc)) { continue }
    $docDir = Split-Path -Parent (Resolve-Path $doc)
    foreach ($m in [regex]::Matches((Get-Content $doc -Raw), '\]\(([^)]+)\)')) {
        $target = $m.Groups[1].Value.Split('#')[0]
        if ($target -match '^[a-z]+:' -or $target -eq '' -or $target -match '^\.\./\.\.') { continue }
        $resolved = Join-Path $docDir $target
        if (-not (Test-Path $resolved)) {
            $rel = $doc.ToString().Replace("$repo\", '')
            $warnings += "${rel} links to '$target' which does not exist"
            Write-Host "  WARN  ${rel} -> $target" -ForegroundColor Yellow
            $deadLinks++
        }
    }
}
if ($deadLinks -eq 0) { Write-Host "  ok    every local link resolves" -ForegroundColor Green }

# --- RESULT ---------------------------------------------------------------------
Write-Host ""
if ($warnings) {
    Write-Host "warnings ($($warnings.Count)):" -ForegroundColor Yellow
    $warnings | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
}
if ($failures) {
    Write-Host "FAILED ($($failures.Count)):" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}
Write-Host "docs/code consistency: PASS" -ForegroundColor Green
if ($Strict -and $warnings) { Write-Host "strict mode: warnings present" -ForegroundColor Red; exit 1 }
exit 0
