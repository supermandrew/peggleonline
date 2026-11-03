# Installs (or removes) the Peggle Nights online duel mod.
# The mod is a replacement bass.dll; the game's own bass.dll is kept next to it as bass_orig.dll.
param([switch]$Uninstall, [string]$GameDir, [switch]$Elevated)

$ErrorActionPreference = 'Stop'
# The elevated copy runs in its own window: keep it open long enough to read.
trap { Write-Host "Error: $_"; if ($Elevated) { Read-Host 'Press Enter to close' }; exit 1 }
function Done([int]$code) { if ($Elevated) { Read-Host 'Press Enter to close' }; exit $code }
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$originalBassSize = 92216

function Find-GameDir {
    if ($GameDir) { return $GameDir }
    if (Test-Path (Join-Path $here 'PeggleNights.exe')) { return $here }
    $roots = @()
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        try {
            $p = Get-ItemProperty -Path $key -ErrorAction Stop
            foreach ($name in 'SteamPath', 'InstallPath') { if ($p.$name) { $roots += $p.$name } }
        } catch {}
    }
    $roots += "${env:ProgramFiles(x86)}\Steam"
    $libraries = @()
    foreach ($root in $roots | Select-Object -Unique) {
        $libraries += $root
        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libraries += $m.Groups[1].Value -replace '\\\\', '\'
            }
        }
    }
    foreach ($lib in $libraries | Select-Object -Unique) {
        $dir = Join-Path $lib 'steamapps\common\Peggle Nights'
        if (Test-Path (Join-Path $dir 'PeggleNights.exe')) { return $dir }
    }
    return $null
}

$dir = Find-GameDir
if (-not $dir) {
    Write-Host "Couldn't find Peggle Nights. Copy these files into the game's folder (the one with PeggleNights.exe) and run this again."
    exit 1
}
Write-Host "Peggle Nights folder: $dir"

# The Steam folder is under Program Files: ask Windows for permission if we can't write there.
$probe = Join-Path $dir 'netplay_write_test.tmp'
try { [IO.File]::WriteAllText($probe, ''); Remove-Item $probe } catch {
    $elevatedArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($MyInvocation.MyCommand.Path)`"", '-GameDir', "`"$dir`"", '-Elevated')
    if ($Uninstall) { $elevatedArgs += '-Uninstall' }
    Start-Process powershell -Verb RunAs -ArgumentList $elevatedArgs -Wait
    exit 0
}

$bass = Join-Path $dir 'bass.dll'
$orig = Join-Path $dir 'bass_orig.dll'

# A running game keeps this folder's bass.dll open.
try { [IO.File]::Open($bass, 'Open', 'ReadWrite', 'None').Close() } catch {
    Write-Host 'Please quit Peggle Nights first, then run this again.'
    Done 1
}

if ($Uninstall) {
    if (Test-Path $orig) {
        Remove-Item $bass -Force -ErrorAction SilentlyContinue
        Rename-Item $orig 'bass.dll'
        Write-Host 'The online mod was removed.'
    } else {
        Write-Host 'The online mod is not installed here.'
    }
    Done 0
}

if (-not (Test-Path $orig)) {
    if ((Get-Item $bass).Length -ne $originalBassSize) {
        Write-Host "bass.dll in the game folder isn't the game's original file, so nothing was changed."
        Done 1
    }
    Rename-Item $bass 'bass_orig.dll'
}
Copy-Item (Join-Path $here 'bass.dll') $bass -Force
Write-Host 'Installed. Start Peggle Nights from Steam, click Duel, then Online Play.'
Done 0
