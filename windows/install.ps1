# Installer for the laniol8926/zbitxd fork (generic-rig-backend branch),
# Windows/MinGW port -- companion to ../install.sh for Linux.
#
# Unlike install.sh, this does NOT build zbitxd.exe itself -- there's no
# MinGW toolchain here to build with (that's the whole point of a
# Windows target: install.sh assumes a real dev environment; this
# assumes a plain end-user machine). Expects to be run from a directory
# that already has, sitting right alongside this script:
#   - zbitxd.exe        (built elsewhere via `make WINDOWS=1` with an
#                         i686-w64-mingw32 cross-compiler -- see the
#                         Makefile's own WINDOWS=1 comment)
#   - sqlite3.exe        (optional but recommended -- cross-compiled the
#                         same way, from sqlite.org's own amalgamation
#                         source (shell.c + sqlite3.c) with the same
#                         MinGW toolchain. sqlite.org itself stopped
#                         shipping a precompiled 32-bit sqlite3.exe
#                         tools bundle (confirmed live, 2026-09-02 --
#                         only win-x86 DLL-only and win-x64/arm64 tools
#                         remain), so this can't just be downloaded the
#                         way the DLLs below are. Without it, this
#                         script can still install everything else, but
#                         can't create a fresh sbitx.db for you -- see
#                         the warning at the end if that happens.)
#   - web\                (the web UI, from the same checkout)
#   - data\default_hw_settings.ini, data\default_settings.ini,
#     data\create_db.sql  (all already in any zbitxd checkout)
#
# Downloads and installs everything this project needs beyond that:
# FFTW3's runtime DLLs, Hamlib (rigctld.exe/rigctl.exe -- confirmed
# live, 2026-09-02: NOT bundled with WSJT-Z despite both being
# FT8-adjacent software, a real gap found the hard way tonight), a
# fresh sbitx.db (if sqlite3.exe is present and one doesn't already
# exist), a Windows Firewall rule for the web UI port (confirmed live:
# a freshly-started zbitxd.exe was reachable from localhost but not
# from another machine on the LAN without this -- Windows doesn't
# prompt for an inbound allow the way it sometimes does for a
# double-clicked GUI app, since this installs it non-interactively),
# and a desktop shortcut.
#
# Safe to re-run: every step here either checks before it acts
# (existing config/db is never overwritten) or is naturally idempotent
# (re-downloading the same DLL, re-adding an already-present firewall
# rule is a no-op via the existence check below).
#
# Run as Administrator -- needed for the firewall rule; everything else
# here would work without it, but a single all-or-nothing elevation
# requirement up front is simpler than a partial-admin split partway
# through installing.

#Requires -RunAsAdministrator

$ErrorActionPreference = "Stop"

$InstallDir = "$env:LOCALAPPDATA\zbitxd"
$HamlibVersion = "4.7.2"
$FftwVersion = "3.3.5"

Write-Host "=== zbitxd Windows installer ===" -ForegroundColor Cyan
Write-Host "Install directory: $InstallDir"
Write-Host ""

# --- sanity checks -- must run from a real checkout with a built exe ---
if (-not (Test-Path ".\zbitxd.exe")) {
    Write-Error "zbitxd.exe not found in the current directory. Build it first via 'make WINDOWS=1' with an i686-w64-mingw32 cross-compiler (see the Makefile's own WINDOWS=1 comment), then run this script from the same directory as the resulting zbitxd.exe."
    exit 1
}
if (-not (Test-Path ".\web")) {
    Write-Error "web\ folder not found in the current directory -- run this script from the zbitxd checkout root (next to zbitxd.exe)."
    exit 1
}
if (-not (Test-Path ".\data\create_db.sql")) {
    Write-Error "data\create_db.sql not found -- run this script from the zbitxd checkout root."
    exit 1
}

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

# --- the app itself ---
Write-Host "Copying zbitxd.exe and web UI..."
Copy-Item ".\zbitxd.exe" "$InstallDir\zbitxd.exe" -Force
Copy-Item ".\web" "$InstallDir\web" -Recurse -Force

# --- default config -- STATEDIR/SHAREDIR are both "." for this build
# (see the Makefile's WINDOWS=1 branch), i.e. relative to zbitxd.exe's
# own working directory -- everything lives flat in $InstallDir, not
# split into separate state/share locations the way the Linux install
# is (/var/lib/zbitxd vs /usr/local/share/zbitxd). Never overwrites a
# real hw_settings.ini/user's own settings on a re-run. ---
if (-not (Test-Path "$InstallDir\default_hw_settings.ini")) {
    Copy-Item ".\data\default_hw_settings.ini" "$InstallDir\default_hw_settings.ini" -Force
}
if (-not (Test-Path "$InstallDir\default_settings.ini")) {
    Copy-Item ".\data\default_settings.ini" "$InstallDir\default_settings.ini" -Force
}

# --- database ---
$dbCreated = $false
if (-not (Test-Path "$InstallDir\sbitx.db")) {
    if (Test-Path ".\sqlite3.exe") {
        Write-Host "Creating sbitx.db from data\create_db.sql..."
        Get-Content ".\data\create_db.sql" | & ".\sqlite3.exe" "$InstallDir\sbitx.db"
        $dbCreated = $true
    }
} else {
    Write-Host "sbitx.db already exists, leaving it alone."
}

$tmpDir = New-Item -ItemType Directory -Force -Path "$env:TEMP\zbitxd_install_$PID"
try {
    # --- FFTW3 runtime DLLs ---
    Write-Host "Downloading FFTW3 $FftwVersion (32-bit)..."
    $fftwZip = "$tmpDir\fftw.zip"
    Invoke-WebRequest -Uri "https://fftw.org/pub/fftw/fftw-$FftwVersion-dll32.zip" -OutFile $fftwZip
    Expand-Archive -Path $fftwZip -DestinationPath "$tmpDir\fftw" -Force
    Copy-Item "$tmpDir\fftw\libfftw3-3.dll" "$InstallDir\" -Force
    Copy-Item "$tmpDir\fftw\libfftw3f-3.dll" "$InstallDir\" -Force

    # --- Hamlib (rigctld.exe/rigctl.exe) ---
    Write-Host "Downloading Hamlib $HamlibVersion (32-bit)..."
    $hamlibZip = "$tmpDir\hamlib.zip"
    Invoke-WebRequest -Uri "https://github.com/Hamlib/Hamlib/releases/download/$HamlibVersion/hamlib-w32-$HamlibVersion.zip" -OutFile $hamlibZip
    Expand-Archive -Path $hamlibZip -DestinationPath "$tmpDir\hamlib" -Force
    $hamlibBin = Get-ChildItem -Path "$tmpDir\hamlib" -Recurse -Filter "rigctld.exe" | Select-Object -First 1 -ExpandProperty DirectoryName
    Copy-Item "$hamlibBin\rigctld.exe" "$InstallDir\" -Force
    Copy-Item "$hamlibBin\rigctl.exe" "$InstallDir\" -Force
    Copy-Item "$hamlibBin\libhamlib-4.dll" "$InstallDir\" -Force
    Copy-Item "$hamlibBin\libusb-1.0.dll" "$InstallDir\" -Force
    Copy-Item "$hamlibBin\libgcc_s_dw2-1.dll" "$InstallDir\" -Force
    # winpthread: zbitxd.exe itself needs this too (MinGW's pthreads
    # runtime) -- hamlib's own bundle ships a copy, reused here instead
    # of a separate download, same DLL either way.
    if (-not (Test-Path "$InstallDir\libwinpthread-1.dll")) {
        Copy-Item "$hamlibBin\libwinpthread-1.dll" "$InstallDir\" -Force
    }
} finally {
    Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue
}

# --- jt9 decoder-merge bridge (optional) ---
# Companion to modem_ft8.c's own jt9_dump_slot_wav()/jt9_temp_dir() --
# runs jt9 (from an existing WSJT-X/WSJT-Z install) as a second,
# independent FT8/FT4 decoder and merges its catches with zbitxd's own
# (see scripts/jt9_bridge.py's own module comment). Entirely optional:
# zbitxd works fully without it, just without the extra decoder-merge
# sensitivity. Needs a real Python 3 (not the Microsoft Store's
# execution-alias stub, which prints an error and exits non-zero until
# someone actually completes a Store install) and an existing WSJT-X or
# WSJT-Z install for jt9.exe itself (jt9_bridge.py's own find_jt9()
# looks in the usual install locations) -- neither is installed by this
# script; both are only checked for.
if (Test-Path ".\scripts\jt9_bridge.py") {
    Copy-Item ".\scripts\jt9_bridge.py" "$InstallDir\jt9_bridge.py" -Force

    $pythonOk = $false
    try {
        $pyVersion = & python --version 2>&1
        if ($LASTEXITCODE -eq 0 -and $pyVersion -match "^Python 3") { $pythonOk = $true }
    } catch { }

    if ($pythonOk) {
        $pythonExe = (Get-Command python).Source
        $pythonwPath = $pythonExe -replace "python\.exe$", "pythonw.exe"
        if (-not (Test-Path $pythonwPath)) { $pythonwPath = $pythonExe }

        $taskName = "zbitxd jt9 bridge"
        if (-not (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue)) {
            Write-Host "Registering jt9 decoder-merge bridge to start at logon..."
            $action = New-ScheduledTaskAction -Execute $pythonwPath -Argument "`"$InstallDir\jt9_bridge.py`"" -WorkingDirectory $InstallDir
            $trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
            $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero)
            Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings -User $env:USERNAME | Out-Null
            Start-ScheduledTask -TaskName $taskName
        }
        Write-Host "jt9 decoder-merge bridge installed and running (Task Scheduler: `"$taskName`", starts at logon)."
    } else {
        Write-Host ""
        Write-Warning "jt9_bridge.py was copied but not started -- no real Python 3 found (the 'python'/'python3' commands on this machine are Microsoft Store execution-alias stubs until that install is completed). Install Python 3 from python.org, then run: pythonw `"$InstallDir\jt9_bridge.py`" -- or re-run this installer afterward to register the startup task automatically. zbitxd itself works fully without this; it's purely an optional decoder-merge sensitivity boost -- see scripts/jt9_bridge.py's own module comment."
    }
}

# --- Windows Firewall: web UI port ---
$ruleName = "zbitxd web UI"
if (-not (Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue)) {
    Write-Host "Adding firewall rule for TCP 8080 (web UI)..."
    New-NetFirewallRule -DisplayName $ruleName -Direction Inbound -Protocol TCP -LocalPort 8080 -Action Allow | Out-Null
}

# --- desktop shortcut ---
$shortcutPath = "$env:USERPROFILE\Desktop\zbitxd.lnk"
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = "$InstallDir\zbitxd.exe"
$shortcut.WorkingDirectory = $InstallDir
$shortcut.Description = "zbitxd (generic-rig-backend)"
$shortcut.Save()

Write-Host ""
Write-Host "=== Done ===" -ForegroundColor Green
Write-Host "Installed to: $InstallDir"
Write-Host "Launch via the new Desktop shortcut, or directly:"
Write-Host "  cd `"$InstallDir`"; .\zbitxd.exe"
Write-Host "Then open http://localhost:8080 (or this machine's LAN IP, from another device) in a browser."
Write-Host ""
Write-Host "Serial Device / CAT: pick your rig's COM port from the live list"
Write-Host "in the web UI's rig connection settings each time you connect --"
Write-Host "it's queried fresh from the registry, not something to hardcode,"
Write-Host "since Windows can reassign COM port numbers across replugs/reboots."
if (-not (Test-Path "$InstallDir\sbitx.db")) {
    Write-Host ""
    Write-Warning "No sbitx.db was created (sqlite3.exe wasn't found alongside this script) -- logging won't work until one exists. Cross-compile sqlite3.exe the same way as zbitxd.exe (MinGW, from sqlite.org's amalgamation source: shell.c + sqlite3.c), place it next to this script, and re-run; or copy an existing sbitx.db in from another install."
} elseif ($dbCreated) {
    Write-Host ""
    Write-Host "Created a fresh, empty sbitx.db from data\create_db.sql."
}
