<#
.SYNOPSIS
    Embeds an ICO file as the main application icon into a Windows PE executable.
.DESCRIPTION
    Uses kernel32 Win32 resource update APIs (BeginUpdateResourceW, UpdateResourceW, EndUpdateResourceW)
    to embed RT_ICON and RT_GROUP_ICON into the target executable without requiring external compilers.
#>
param(
    [Parameter(Mandatory=$true)]
    [string]$ExePath,

    [Parameter(Mandatory=$true)]
    [string]$IcoPath
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public class Win32ResUpdate {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr BeginUpdateResourceW(string pFileName, bool bDeleteExistingResources);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool UpdateResourceW(IntPtr hUpdate, IntPtr lpType, IntPtr lpName, ushort wLanguage, byte[] lpData, uint cbData);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool EndUpdateResourceW(IntPtr hUpdate, bool fDiscard);
}
"@ -ErrorAction SilentlyContinue

$resolvedExe = (Resolve-Path $ExePath).Path
$resolvedIco = (Resolve-Path $IcoPath).Path

$icoBytes = [System.IO.File]::ReadAllBytes($resolvedIco)
$count = [BitConverter]::ToUInt16($icoBytes, 4)

$hUpdate = [Win32ResUpdate]::BeginUpdateResourceW($resolvedExe, $false)
if ($hUpdate -eq [IntPtr]::Zero) {
    $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
    throw "BeginUpdateResourceW failed on '$resolvedExe' with error code $err"
}

try {
    # 1. Update individual RT_ICON (type = 3)
    for ($i = 0; $i -lt $count; $i++) {
        $off = 6 + $i * 16
        $size = [BitConverter]::ToUInt32($icoBytes, $off + 8)
        $imgOff = [BitConverter]::ToUInt32($icoBytes, $off + 12)
        $imgBytes = New-Object byte[] $size
        [Array]::Copy($icoBytes, $imgOff, $imgBytes, 0, $size)
        
        $iconId = [IntPtr]($i + 1)
        $rtIcon = [IntPtr]3
        $ok = [Win32ResUpdate]::UpdateResourceW($hUpdate, $rtIcon, $iconId, 0, $imgBytes, [uint32]$size)
        if (-not $ok) {
            $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "UpdateResourceW RT_ICON failed for index $i with error code $err"
        }
    }

    # 2. Build RT_GROUP_ICON (type = 14)
    # Header (6 bytes) + entries (14 bytes each)
    $grpSize = 6 + $count * 14
    $grpBytes = New-Object byte[] $grpSize
    [Array]::Copy($icoBytes, 0, $grpBytes, 0, 6) # Copy header

    for ($i = 0; $i -lt $count; $i++) {
        $srcOff = 6 + $i * 16
        $dstOff = 6 + $i * 14
        # Copy first 12 bytes of ICONDIRENTRY (width, height, colorCount, reserved, planes, bitCount, bytesInRes)
        [Array]::Copy($icoBytes, $srcOff, $grpBytes, $dstOff, 12)
        # Write 2-byte nID (1-based index)
        $nID = [uint16]($i + 1)
        $idBytes = [BitConverter]::GetBytes($nID)
        $grpBytes[$dstOff + 12] = $idBytes[0]
        $grpBytes[$dstOff + 13] = $idBytes[1]
    }

    $rtGroupIcon = [IntPtr]14
    $mainIconId = [IntPtr]1 # ID 1 is standard main application icon
    $ok = [Win32ResUpdate]::UpdateResourceW($hUpdate, $rtGroupIcon, $mainIconId, 0, $grpBytes, [uint32]$grpSize)
    if (-not $ok) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "UpdateResourceW RT_GROUP_ICON failed with error code $err"
    }

    $ok = [Win32ResUpdate]::EndUpdateResourceW($hUpdate, $false)
    if (-not $ok) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "EndUpdateResourceW failed with error code $err"
    }
    Write-Host "Embedded application icon from $resolvedIco into $resolvedExe ($count resolutions)"
} catch {
    [Win32ResUpdate]::EndUpdateResourceW($hUpdate, $true)
    throw
}
