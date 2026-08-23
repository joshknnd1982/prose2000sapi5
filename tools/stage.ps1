<#
.SYNOPSIS
    Assembles the install layout in output\ from the two build trees.

.DESCRIPTION
    The layout is what prose_paths.cpp expects to find at run time:

        output\
          Prose2000SAPI5.dll       32-bit SAPI 5 engine, for 32-bit applications
          Prose2000Config.exe      the configuration utility (64-bit)
          ProseHost.exe            the emulator, spawned by both engines
          roms\                    the six firmware images
          prose_speak.exe          speaks through the registered SAPI 5 stack
          prose_render.exe         renders WAV files with no SAPI involved
          prose_sapitest.exe       drives the DLL without registering it
          open_logs.cmd            opens the engine's log folder
          x64\Prose2000SAPI5.dll   64-bit SAPI 5 engine, for 64-bit applications
          x64\prose_speak.exe
          x64\prose_render.exe
          x64\prose_sapitest.exe

    The 64-bit DLL sits one directory down and walks up to find ProseHost.exe, which is why
    the emulator and its ROMs live at the root rather than being duplicated.
#>
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$Output,
    [switch]$Copy
)

$ErrorActionPreference = 'Stop'
if (-not $Output) { $Output = Join-Path $Root 'output' }

$x86 = Join-Path $Root 'build_x86\bin\Release'
$x64 = Join-Path $Root 'build_x64\bin\Release'
$data = Join-Path $Root 'bin\prose2000'

foreach ($required in @($x86, $x64, $data)) {
    if (-not (Test-Path $required)) { throw "missing $required - build first" }
}

# A running emulator holds ProseHost.exe open, and the configuration utility holds its own
# executable. Both are safe to stop: the next client starts a fresh emulator on demand.
foreach ($name in @('ProseHost', 'Prose2000Config')) {
    Get-Process -Name $name -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 300

New-Item -ItemType Directory -Force -Path $Output | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Output 'x64') | Out-Null

# ---- 32-bit half ------------------------------------------------------------------------
Copy-Item (Join-Path $x86 'Prose2000SAPI5.dll') $Output -Force
Copy-Item (Join-Path $x86 'prose_speak.exe')    $Output -Force
Copy-Item (Join-Path $x86 'prose_render.exe')   $Output -Force
Copy-Item (Join-Path $x86 'prose_sapitest.exe') $Output -Force

# ---- 64-bit half ------------------------------------------------------------------------
$x64Out = Join-Path $Output 'x64'
Copy-Item (Join-Path $x64 'Prose2000SAPI5.dll') $x64Out -Force
Copy-Item (Join-Path $x64 'prose_speak.exe')    $x64Out -Force
Copy-Item (Join-Path $x64 'prose_render.exe')   $x64Out -Force
Copy-Item (Join-Path $x64 'prose_sapitest.exe') $x64Out -Force

# The configuration utility is built only as 64-bit and lives at the root, because it edits
# the one per-user settings key that both engines read.
Copy-Item (Join-Path $x64 'Prose2000Config.exe') $Output -Force

# ---- the emulator and its firmware ------------------------------------------------------
Copy-Item (Join-Path $data 'ProseHost.exe') $Output -Force
if (Test-Path (Join-Path $data 'LICENSE.txt')) {
    Copy-Item (Join-Path $data 'LICENSE.txt') (Join-Path $Output 'ProseHost-LICENSE.txt') -Force
}
Copy-Item (Join-Path $Root 'installer\open_logs.cmd') $Output -Force

$romTarget = Join-Path $Output 'roms'
$romSource = Join-Path $data 'roms'
if (Test-Path $romTarget) {
    $item = Get-Item $romTarget -Force
    if ($item.LinkType) {
        # Remove-Item would ask about the junction's contents; this does not.
        [System.IO.Directory]::Delete($romTarget, $false)
    } else {
        Remove-Item $romTarget -Recurse -Force
    }
}
if ($Copy) {
    Copy-Item $romSource $romTarget -Recurse -Force
} else {
    # The firmware is only 260 KB, so unlike a large voice set there is nothing to gain from
    # a junction. Copying keeps the staged layout self-contained and portable.
    Copy-Item $romSource $romTarget -Recurse -Force
}

$romCount = (Get-ChildItem $romTarget -File).Count
if ($romCount -ne 6) {
    throw "expected 6 firmware images in $romTarget, found $romCount"
}

Write-Host "staged to $Output"
Get-ChildItem $Output | Select-Object Mode, Name, Length
