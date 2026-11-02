<#
  Prepare a portable release package and notes. Never publishes, tags, launches the app,
  or changes runtime state. CI builds and self-tests before calling this script.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [AllowEmptyString()][string]$PreviousTag = '',
    [switch]$ValidateOnly,
    [switch]$AllowDirty,
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

# Narrow syntax is intentional: versions also become tag names and asset filenames.
if ($Version -notmatch '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$') {
    throw 'Version must be three numeric components without a v prefix, for example 1.0.1.'
}
[void][version]$Version
$header = Get-Content -LiteralPath (Join-Path $repo 'AppVersion.h') -Raw
$match = [regex]::Match($header, '(?m)^#define IDLE_DIMMER_VERSION L"([0-9]+\.[0-9]+\.[0-9]+)"\s*$')
if (-not $match.Success -or $match.Groups[1].Value -cne $Version) {
    throw "Requested version $Version does not match AppVersion.h. Bump the source version before releasing."
}
$readme = Get-Content -LiteralPath (Join-Path $repo 'README.md') -Raw
if ($readme -notmatch ([regex]::Escape("App version: **$Version**."))) {
    throw 'README.md app version does not match AppVersion.h.'
}
if ($PreviousTag -and $PreviousTag -notmatch '^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$') {
    throw 'PreviousTag must be a stable version tag, for example v1.0.0.'
}
if ($PreviousTag) {
    & git rev-parse --verify "$PreviousTag^{commit}" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Previous release tag $PreviousTag is missing; fetch tags first." }
    & git merge-base --is-ancestor $PreviousTag HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Previous release must be an ancestor of the release commit.' }
}
if ($ValidateOnly) { Write-Host "Release version validated: v$Version"; exit 0 }

$dirty = @(& git status --porcelain --untracked-files=normal)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect source status.' }
if ($dirty.Count -and -not $AllowDirty) {
    throw 'Release packaging requires a clean checkout. Commit changes first, or use -AllowDirty for a local preview that must not be published.'
}

$binary = Join-Path $repo 'IdleDimmer.exe'
if (-not (Test-Path -LiteralPath $binary)) { throw 'Build IdleDimmer.exe before packaging.' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo 'build-tools/release' }
$destination = [IO.Path]::GetFullPath($OutputDirectory)
# Do not clean directories or sweep the working folder: only explicitly listed release files
# are copied. settings.ini, recovery state, stats, logs and credentials cannot enter the ZIP.
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$zipName = "IdleDimmer-$Version-windows-x64.zip"
$releaseBinary = Join-Path $destination 'IdleDimmer.exe'
Copy-Item -LiteralPath $binary -Destination $releaseBinary -Force
$licence = Join-Path $destination 'LICENSE'
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $licence -Force
Compress-Archive -LiteralPath @($releaseBinary, $licence) -DestinationPath (Join-Path $destination $zipName) -Force
$hashLines = foreach ($name in @('IdleDimmer.exe', $zipName)) {
    $hash = (Get-FileHash -LiteralPath (Join-Path $destination $name) -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $name"
}
[IO.File]::WriteAllLines((Join-Path $destination 'SHA256SUMS.txt'), [string[]]$hashLines, [Text.UTF8Encoding]::new($false))

$commit = (& git rev-parse HEAD | Out-String).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine source commit.' }
$range = if ($PreviousTag) { "$PreviousTag..HEAD" } else { 'HEAD' }
$commits = @(& git log --reverse '--format=%H%x09%s' $range)
if ($LASTEXITCODE -ne 0) { throw 'Cannot generate commit notes.' }
$notes = [Collections.Generic.List[string]]::new()
if ($dirty.Count) {
    $notes.Add('> LOCAL PREVIEW ONLY: this package includes uncommitted source changes. The commit list is incomplete. Do not publish this package; use the clean GitHub workflow.')
    $notes.Add('')
}
$highlights = Join-Path $repo "docs/releases/v$Version.md"
if (Test-Path -LiteralPath $highlights) {
    $notes.Add((Get-Content -LiteralPath $highlights -Raw).Trim())
} else {
    $notes.Add("Idle Dimmer $Version")
}
$notes.Add('')
$notes.Add('## Download')
$notes.Add('')
$notes.Add("Download ``IdleDimmer.exe`` for the standalone app, or ``$zipName`` for the app and licence. Windows x64; no installer or additional runtime required.")
$notes.Add('')
$notes.Add('For an existing installation, quit Idle Dimmer normally so it restores brightness, then replace only the EXE. Keep your settings and statistics files. The app is currently unsigned; Windows may show a SmartScreen warning.')
$notes.Add('')
$notes.Add('SHA-256 checksums are attached as `SHA256SUMS.txt`.')
$notes.Add('')
$notes.Add('## Commits included')
$notes.Add('')
foreach ($entry in $commits) {
    $parts = $entry -split "`t", 2
    if ($parts.Length -ne 2) { throw 'Unexpected git log output.' }
    # HTML-encode text before putting untrusted commit subjects into Markdown.
    $subject = [Net.WebUtility]::HtmlEncode($parts[1]).Replace('[','&#91;').Replace(']','&#93;').Replace('`','&#96;')
    $notes.Add("- $subject ([$($parts[0].Substring(0,7))](https://github.com/Saderius/idle-dimmer/commit/$($parts[0])))")
}
if ($PreviousTag) {
    $notes.Add('')
    $notes.Add("[Compare with $PreviousTag](https://github.com/Saderius/idle-dimmer/compare/$PreviousTag...v$Version)")
}
$notes.Add('')
$notes.Add("Source commit: ``$commit``.")
[IO.File]::WriteAllLines((Join-Path $destination 'release-notes.md'), $notes, [Text.UTF8Encoding]::new($false))
Write-Host "Prepared v$Version in $destination. Nothing was published."
