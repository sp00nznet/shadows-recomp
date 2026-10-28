# One-click setup for the Shadows of the Empire static recompilation.
# Run it by double-clicking Setup.cmd in the repo folder; the README's "Step by
# step" is the same thing by hand, command for command. The shape is Fallout
# Tactics' tools\setup.ps1.
#
# It copies the game from your Steam install into game\, removes SteamStub
# from Shadows.exe, writes the game's settings to HKCU, builds the function
# catalog, lifts it to C, builds the 32-bit host and leaves a shortcut. Each
# step is skipped when its output already exists (-Force redoes them).
# -Game takes the install folder; -Yes answers every question yes and never
# waits for input (the installer, recomp-launcher, runs it so, with
# RECOMP_LAUNCHER set, which also skips the "Press Enter" at the end).
# Everything it does goes to setup.log.
param([switch]$Force, [switch]$Yes, [string]$Game = "")

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Parent = Split-Path -Parent $Root
# The toolkit: PCRECOMP; else this checkout's own worktree; else a sibling
# ..\pcrecomp (where the installer puts it) or ..\tools; else ..\pcrecomp is
# cloned. It is handed to run_lift.py and CMake explicitly.
$Toolkit = if ($env:PCRECOMP) { $env:PCRECOMP }
  elseif (Test-Path (Join-Path $Root 'work\pcrecomp\runtime')) { Join-Path $Root 'work\pcrecomp' }
  elseif (Test-Path (Join-Path $Parent 'pcrecomp\runtime')) { Join-Path $Parent 'pcrecomp' }
  elseif (Test-Path (Join-Path $Parent 'tools\runtime\native32')) { Join-Path $Parent 'tools' }
  else { Join-Path $Parent 'pcrecomp' }
$T = Join-Path $Toolkit 'tools'
$Log = Join-Path $Root 'setup.log'
Set-Location $Root
$Steps = 8
function Log($t) { try { Add-Content -Path $Log -Value $t -Encoding UTF8 -ErrorAction Stop } catch {} }
Log "==== setup $(Get-Date -Format s)"

function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
function Step($n, $t) { Write-Host ""; Say "[$n/$Steps] $t" 'Cyan' }
function Pause-End { if (-not $env:RECOMP_LAUNCHER) { Read-Host "Press Enter to close" | Out-Null } }
function Fail($t) {
  Say "" ; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Pause-End; exit 1
}
function Ask($q) { if ($Yes) { Say "$q yes"; return $true }; $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }   # Enter = yes
function Refresh-Path {
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
# Run a program with its output in the log. Windows PowerShell turns a native
# program's stderr into errors, so 'Stop' is off while it runs.
function Exec([string[]]$cmd) {
  $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $rest = @($cmd | Select-Object -Skip 1)
  & $cmd[0] @rest 2>&1 | ForEach-Object { Log "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = $old
  return $code
}
function Run($what, [string[]]$cmd) {
  Say "  $what..."
  $code = Exec $cmd
  if ($code -ne 0) { Fail "$what failed (exit code $code)." }
}
function Winget-Install($id) {
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Fail "winget is missing. Install 'App Installer' from the Microsoft Store, then run Setup.cmd again."
  }
  Exec @('winget', 'install', '-e', '--id', $id, '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  Refresh-Path
}

Clear-Host
Say "Star Wars: Shadows of the Empire - static recompilation setup" 'White'
Say "You need your copy of Shadows of the Empire installed (Steam, or the CD with the 1.1 patch) and about 2 GB free."
Say "The function catalog, the lift and the build take 20 to 60 minutes, once."

# ---------------------------------------------------------------- tools
Step 1 "Checking the tools the pipeline needs"
function Find-Python {
  foreach ($c in @(@('py', '-3'), @('python'), @('python3'))) {
    if (-not (Get-Command $c[0] -ErrorAction SilentlyContinue)) { continue }
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $rest = @($c | Select-Object -Skip 1)
    $v = (& $c[0] @rest --version 2>&1 | Out-String).Trim()
    $ErrorActionPreference = $old
    if ($v -match '^Python 3\.(\d+)' -and [int]$Matches[1] -ge 10) { return ,$c }
  }
  return $null
}
$pyargs = Find-Python
if (-not $pyargs) {
  Say "  Python 3.10 or newer is not installed."
  if (-not (Ask "  Install Python 3.12 now (winget, for your user only, about 30 MB)?")) { Fail "Python 3 is required." }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Fail "winget is missing. Install 'App Installer' from the Microsoft Store, or install Python yourself."
  }
  Exec @('winget', 'install', '-e', '--id', 'Python.Python.3.12', '--scope', 'user', '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  Refresh-Path
  $pyargs = Find-Python
  if (-not $pyargs) { Fail "Python installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}
Say "  Python: $($pyargs -join ' ')"

# cryptography: SteamStub's code section is AES-encrypted.
if ((Exec ($pyargs + @('-c', 'import pefile, capstone, cryptography'))) -ne 0) {
  Say "  The Python packages pefile, capstone and cryptography are missing (about 20 MB)."
  if (-not (Ask "  Install them now (pip, for your user only)?")) { Fail "pefile, capstone and cryptography are required." }
  Run "Installing pefile, capstone and cryptography" ($pyargs + @('-m', 'pip', 'install', '--user', 'pefile', 'capstone', 'cryptography'))
}

if (-not (Test-Path (Join-Path $T 'lift\lift32.py'))) {
  Say "  The pcrecomp toolkit is not beside this folder ($Toolkit)."
  if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "git is needed to fetch pcrecomp. Install Git for Windows, then run Setup.cmd again." }
  if (-not (Ask "  Download it now (git, about 20 MB)?")) { Fail "pcrecomp is required at $Toolkit." }
  Run "Cloning pcrecomp" @('git', 'clone', '--depth', '1', 'https://github.com/sp00nznet/pcrecomp', $Toolkit)
}
# Two toolkit changes this game needs (docs/bringup.md 1 and 5).
if (-not (Select-String -Quiet -Path (Join-Path $T 'drm\steamstub.py') -Pattern 'V3_PROLOGUE')) {
  Fail "your pcrecomp at $Toolkit cannot remove SteamStub 3.x yet. Update it: git -C `"$Toolkit`" pull"
}
if (-not (Select-String -Quiet -Path (Join-Path $T 'lift\difftest.py') -Pattern 'fpu.fst-st1')) {
  Fail "your pcrecomp at $Toolkit lifts fst st(i) wrongly (3D draws flat). Update it: git -C `"$Toolkit`" pull"
}
Say "  pcrecomp: $Toolkit"

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) { (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) } else { $null }
if (-not $vs) {
  Say "  Visual Studio 2022 with the C++ tools is not installed."
  if (-not (Ask "  Install the Visual Studio 2022 Build Tools with the C++ workload now (winget, about 3 GB, asks for admin)?")) {
    Fail "Visual Studio 2022 (or its Build Tools) with 'Desktop development with C++' is needed. Install it, then run Setup.cmd again."
  }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Fail "winget is missing. Install 'App Installer' from the Microsoft Store, or the Visual Studio Build Tools yourself."
  }
  Exec @('winget', 'install', '-e', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '--accept-package-agreements', '--accept-source-agreements',
         '--override', '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended') | Out-Null
  $vs = if (Test-Path $vswhere) { (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) } else { $null }
  if (-not $vs) { Fail "the Visual Studio Build Tools did not install. Install them with 'Desktop development with C++', then run Setup.cmd again." }
}
Say "  Visual Studio: $vs"
foreach ($tool in @(@('cmake', 'Kitware.CMake', '30 MB'), @('ninja', 'Ninja-build.Ninja', '1 MB'))) {
  if (Get-Command $tool[0] -ErrorAction SilentlyContinue) { continue }
  Say "  $($tool[0]) is not installed."
  if (-not (Ask "  Install $($tool[0]) now (winget, about $($tool[2]))?")) { Fail "$($tool[0]) is required." }
  Winget-Install $tool[1]
  if (-not (Get-Command $tool[0] -ErrorAction SilentlyContinue)) { Fail "$($tool[0]) installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}

# ---------------------------------------------------------------- the game
Step 2 "Copying your Shadows of the Empire into game\"
function Is-Install([string]$d) { return ($d -and (Test-Path (Join-Path $d 'Sdata\Shadows.exe'))) }
# Steam keeps its library folders in steamapps\libraryfolders.vdf.
function Find-Steam {
  $steam = $null
  foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
    try { $p = Get-ItemProperty $k -ErrorAction Stop; $steam = if ($p.SteamPath) { $p.SteamPath } else { $p.InstallPath }; if ($steam) { break } } catch {}
  }
  if (-not $steam) { return "" }
  $libs = @($steam)
  $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
  if (Test-Path $vdf) { $libs += (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }) }
  foreach ($l in $libs) {
    $d = Join-Path $l 'steamapps\common\Star Wars Shadows of the Empire'
    if (Is-Install $d) { return $d }
  }
  return ""
}
if ((Is-Install 'game') -and -not $Force) {
  Say "  Already in game\ (skipping)."
} else {
  $Game = $Game.Trim('"', ' ')
  if (-not (Is-Install $Game)) { $Game = Find-Steam; if ($Game) { Say "  Found your Steam install: $Game" } }
  if (-not (Is-Install $Game) -and $Yes) { Fail "Shadows of the Empire was not found in your Steam libraries. Give its folder with -Game." }
  while (-not (Is-Install $Game)) {
    $Game = (Read-Host "  Paste the folder Shadows of the Empire is installed in (the one with Sdata\Shadows.exe)").Trim('"', ' ')
    if (-not (Is-Install $Game)) { Say "  That folder has no Sdata\Shadows.exe." 'Yellow' }
  }
  Say "  Copying $Game (about 400 MB)..."
  $code = Exec @('robocopy', $Game, (Join-Path $Root 'game'), '/E', '/NFL', '/NDL', '/NJH', '/NP')
  if ($code -ge 8) { Fail "copying the game failed (robocopy exit code $code)." }   # robocopy: <8 is success
}

Step 3 "Removing SteamStub from Shadows.exe (seconds)"
if (-not (Test-Path 'game\Sdata\Shadows.unwrapped.exe') -or $Force) {
  # The official 1.1 patch's Shadows.exe (a CD install) is the same build
  # without SteamStub: no .bind section to remove (docs/RECON.md, "CD release").
  $fs = [IO.File]::OpenRead((Join-Path $Root 'game\Sdata\Shadows.exe')); $head = New-Object byte[] 4096
  [void]$fs.Read($head, 0, 4096); $fs.Close()
  # v1.0, the CD as it shipped, is an older build (stamp 0x33F35193): none of
  # the lift's addresses fit it. The 1.1 patch replaces Shadows.exe.
  if ([BitConverter]::ToUInt32($head, [BitConverter]::ToInt32($head, 0x3C) + 8) -ne 0x34909499) {
    Fail "game\Sdata\Shadows.exe is not version 1.1. A CD install needs the official 1.1 patch (Shadowsp.exe): put its Shadows.exe in Sdata\, then run Setup.cmd -Force."
  }
  if ([Text.Encoding]::ASCII.GetString($head).Contains('.bind')) {
    Run "Unwrapping" ($pyargs + @("$T\drm\steamstub.py", 'game\Sdata\Shadows.exe', 'game\Sdata\Shadows.unwrapped.exe'))
  } else {
    Say "  No SteamStub (not the Steam release): used as it is."
    Copy-Item -Force 'game\Sdata\Shadows.exe' 'game\Sdata\Shadows.unwrapped.exe'
  }
} else { Say "  Already done (skipping)." }

Step 4 "Writing the game's settings (HKCU, no administrator needed)"
# The values Steam's Install.bat and installScript.vdf put in HKLM, pointing at
# game\. The host reads the game's HKLM key from here (docs/host.md).
$key = 'HKCU:\SOFTWARE\LucasArts Entertainment Company\Shadows of the Empire'
$g = Join-Path $Root 'game'
New-Item -Path $key -Force | Out-Null
$vals = [ordered]@{
  '3D Device' = 'Direct3D HAL'; 'CD Audio' = 'TRUE'; 'Cutscene Text' = 'TRUE'; 'Direct Movie' = 'FALSE';
  'Display Device' = 'Primary Display Driver'; 'Fog' = 'TRUE'; 'Force Alpha' = 'FALSE'; 'Game Pad' = 'FALSE';
  'Height' = '480'; 'Numb Hand' = 'FALSE'; 'Width' = '640'; 'Source Path' = '\'; 'Slow Machine' = 'FALSE';
  'Install Path' = $g; 'SData Path' = "$g\Sdata\"; 'Source Dir' = $g; 'Video Path' = "$g\Sdata\";
  'executable' = "$g\Sdata\Shadows.exe"; 'Analyze Path' = "$g\Install\SysCheck.exe" }
foreach ($n in $vals.Keys) {
  # an existing value is the player's choice from the launcher: kept
  $have = $null; try { $have = (Get-ItemProperty $key -Name $n -ErrorAction Stop).$n } catch {}
  if ($null -eq $have -or $Force -or $n -like '*Path' -or $n -in 'Source Dir', 'executable') {
    New-ItemProperty -Path $key -Name $n -Value $vals[$n] -PropertyType String -Force | Out-Null
  }
}
Say "  $key"

# ---------------------------------------------------------------- the pipeline
New-Item -ItemType Directory -Force work | Out-Null
Step 5 "Finding every function (about 3 minutes, once)"
if ((Test-Path 'work\functions.json') -and -not $Force) { Say "  Already done (skipping)." }
else { Run "Disassembling" ($pyargs + @("$T\disasm\disasm32.py", 'game\Sdata\Shadows.unwrapped.exe', '-o', 'work\functions.json')) }

Step 6 "Lifting to C (about a minute)"
if ((Test-Path 'src\recomp\gen\recomp_dispatch.c') -and -not $Force) { Say "  Already done (skipping)." }
else {
  $env:PCRECOMP = $Toolkit
  Run "Lifting" ($pyargs + @('run_lift.py', '--all'))
}

Step 7 "Building build\sote.exe (15 to 45 minutes)"
if ((Test-Path 'build\sote.exe') -and -not $Force) { Say "  Already done (skipping)." }
else {
  $env:CMAKE_ARGS = "-DPCRECOMP=$Toolkit"
  Run "Compiling" @('cmd', '/c', (Join-Path $Root 'build.cmd'))
}

# ---------------------------------------------------------------- shortcut
Step 8 "Making the shortcut"
$lnk = Join-Path $Root 'Shadows of the Empire (recomp).cmd'
Set-Content -Path $lnk -Encoding ASCII -Value @(
  '@echo off',
  'rem Plays the recompiled game. Controller and mouse settings: controls.ini (docs\controls.md).',
  'cd /d "%~dp0"',
  'start "" build\sote.exe --run')
Say "  $lnk"

Write-Host ""
Say "Done. Double-click 'Shadows of the Empire (recomp).cmd' to play." 'Green'
Pause-End
