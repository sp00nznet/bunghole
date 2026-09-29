# One-click setup for the Bunghole in One static recompilation. Run it by
# double-clicking Setup.cmd in the repo folder; the README's "Step by step" is
# the same thing by hand, command for command.
#
# It checks the tools, copies your CD's files into game\disc, finds every
# function, lifts both executables to C, builds build\bunghole.exe and leaves a
# shortcut to it in this folder. Each step is skipped when its output already
# exists (-Force redoes them), so a rerun after a failure resumes. Everything
# it does goes to setup.log.
#
#   -Game <drive, folder, .cue, .bin or .iso>   skip the question
#   -Yes                                        answer yes to every install question
param([switch]$Force, [switch]$Yes, [string]$Game = "")

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Toolkit = if ($env:PCRECOMP) { $env:PCRECOMP } else { Join-Path (Split-Path -Parent $Root) 'tools' }
$Log = Join-Path $Root 'setup.log'
Set-Location $Root
function Log($t) { Add-Content -Path $Log -Value $t -Encoding UTF8 }
Log "==== setup $(Get-Date -Format s)"

function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
function Step($n, $t) { Write-Host ""; Say "[$n/6] $t" 'Cyan' }
function Pause-End { if (-not $Yes) { Read-Host "Press Enter to close" | Out-Null } }
function Fail($t) {
  Say ""; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Pause-End; exit 1
}
function Ask($q) { if ($Yes) { Say "$q yes"; return $true }; $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }
function Refresh-Path {
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
# Run a program with its output in the log. Windows PowerShell turns a native
# program's stderr into errors, so 'Stop' is off while it runs.
function Exec([string[]]$cmd) {
  $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $rest = @($cmd | Select-Object -Skip 1)   # @(): a one-element slice would splat as characters
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

Clear-Host
Say "Bunghole in One: static recompilation setup" 'White'
Say "You need your own Bunghole in One CD (or a BIN/CUE or ISO of it) and about 2 GB free."

# ---------------------------------------------------------------- tools
Step 1 "Checking the tools"
# A Python that answers "Python 3.x". The Store's placeholder "python" (the
# one that opens the Store) answers nothing, so asking is the reliable test.
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

if ((Exec ($pyargs + @('-c', 'import pefile, capstone'))) -ne 0) {
  Say "  The Python packages pefile and capstone are missing (about 20 MB)."
  if (-not (Ask "  Install them now (pip, for your user only)?")) { Fail "pefile and capstone are required." }
  Run "Installing pefile and capstone" ($pyargs + @('-m', 'pip', 'install', '--user', 'pefile', 'capstone'))
}

if (-not (Test-Path (Join-Path $Toolkit 'tools\lift\lift32.py'))) {
  Say "  The pcrecomp toolkit is not beside this folder ($Toolkit)."
  if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "git is needed to fetch pcrecomp. Install Git for Windows, then run Setup.cmd again." }
  if (-not (Ask "  Download it now (git, about 20 MB)?")) { Fail "pcrecomp is required at $Toolkit." }
  Run "Cloning pcrecomp" @('git', 'clone', '--depth', '1', 'https://github.com/sp00nznet/pcrecomp', $Toolkit)
}
# The toolkit changes this game needs (docs\toolkit.md), each by a line it adds.
$need = @(
  @('#7 and #17 (native32, guest modules)', 'runtime\native32\native32.c', 'native32_export'),
  @('#16 (native32 stack headroom)', 'runtime\native32\native32.c', 'BRIDGE_SLOTS \* 4 - 64'),
  @('#18 (disasm32 export seeds)', 'tools\disasm\disasm32.py', 'exports seeded'),
  @('#19 (disasm32 callees beat guesses)', 'tools\disasm\disasm32.py', 'called=called'),
  @('#20 (flags across tail jumps)', 'runtime\recomp32\recomp_types.h', 'RECOMP_ENTER\(va\) \(g_cur_func = \(va\), RECOMP_FLAGS_IN'))
$missing = @($need | Where-Object {
  $f = Join-Path $Toolkit $_[1]
  -not ((Test-Path $f) -and (Select-String -Path $f -Pattern $_[2] -Quiet)) } | ForEach-Object { $_[0] })
if ($missing) {
  Fail "your pcrecomp at $Toolkit is missing pcrecomp $($missing -join ', '). Update it (git -C `"$Toolkit`" pull), or until those merge, point PCRECOMP at a checkout that has them (docs\toolkit.md)."
}
Say "  pcrecomp: $Toolkit"

# Visual Studio 2022 or its Build Tools, with the C++ x86 compiler; CMake and
# Ninja ship inside that workload, so build.cmd needs nothing else.
function Find-VS {
  foreach ($p in @("${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
                   "$env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe")) {
    if (Test-Path $p) {
      $vs = & $p -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
      if ($vs) { return $vs }
    }
  }
  foreach ($d in (Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio\2022", "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022" -Directory -ErrorAction SilentlyContinue)) {
    if (Test-Path "$($d.FullName)\VC\Auxiliary\Build\vcvarsall.bat") { return $d.FullName }
  }
  return $null
}
if (-not (Find-VS)) {
  Say "  Visual Studio 2022 with the C++ tools is not installed. The recompiled game is C, and this is the compiler."
  if (-not (Ask "  Install the Visual Studio 2022 Build Tools with the C++ workload now (winget, about 3 GB)?")) { Fail "a C compiler (Visual Studio 2022 C++) is required." }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is missing. Install Visual Studio 2022 Build Tools with 'Desktop development with C++' yourself." }
  Exec @('winget', 'install', '-e', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '--accept-package-agreements', '--accept-source-agreements',
         '--override', '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended') | Out-Null
  if (-not (Find-VS)) { Fail "the Build Tools did not install. Install Visual Studio 2022 with 'Desktop development with C++' yourself." }
}
Say "  C compiler: $(Find-VS)"

# ---------------------------------------------------------------- the game
Step 2 "Copying your CD's files into game\disc"
if ((Test-Path 'game\disc\GOLF.EXE') -and -not $Force) {
  Say "  Already there (skipping)."
} else {
  $Game = $Game.Trim('"', ' ')
  if (-not $Game) {
    # A drive with the CD in it answers for itself.
    foreach ($d in (Get-PSDrive -PSProvider FileSystem)) {
      if (Test-Path (Join-Path $d.Root 'GOLF.EXE')) { $Game = $d.Root; Say "  Found the CD in $Game"; break }
    }
  }
  while (-not $Game) {
    $Game = (Read-Host "  Where is the game? A drive (E:\), a folder with GOLF.EXE, or a .cue/.bin/.iso file").Trim('"', ' ')
  }
  Run "Copying the game (344 MB)" ($pyargs + @('tools\disc.py', $Game))
}

# ---------------------------------------------------------------- catalog
Step 3 "Finding every function (half a minute)"
if ((Test-Path 'work\functions_game.json') -and -not $Force) { Say "  Already done (skipping)." }
else { Run "Disassembling GOLF.EXE and 00170001.DLL" ($pyargs + @('tools\catalog.py')) }

# ---------------------------------------------------------------- lift
Step 4 "Recompiling both executables to C (a few seconds)"
if ((Test-Path 'src\recomp\gen\recomp_dispatch.c') -and -not $Force) { Say "  Already done (skipping)." }
else { Run "Lifting" ($pyargs + @('run_lift.py')) }

# ---------------------------------------------------------------- build
Step 5 "Building build\bunghole.exe (a minute or two)"
if ((Test-Path 'build\bunghole.exe') -and -not $Force) { Say "  Already built (skipping)." }
else {
  $env:CMAKE_ARGS = "-DPCRECOMP=$($Toolkit -replace '\\', '/')"
  Run "Compiling" @('cmd', '/d', '/c', "`"$Root\build.cmd`"")
}

# ---------------------------------------------------------------- shortcut
Step 6 "Making the shortcut"
$lnk = Join-Path $Root 'Bunghole in One.lnk'
$sh = New-Object -ComObject WScript.Shell
$s = $sh.CreateShortcut($lnk)
$s.TargetPath = Join-Path $Root 'build\bunghole.exe'
$s.Arguments = '--run --scale 2'
$s.WorkingDirectory = $Root
$s.IconLocation = (Join-Path $Root 'game\disc\GOLF.ICO')
$s.Save()
Say "  $lnk"

Write-Host ""
Say "Done. Double-click 'Bunghole in One' in this folder to play." 'Green'
Pause-End
