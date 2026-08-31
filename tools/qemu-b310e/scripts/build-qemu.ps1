<#
.SYNOPSIS
    Full QEMU build recipe for the B310E machine: clone-or-pin v11.1.0 ->
    install-machine.ps1 -> configure -> make -> sanity. Idempotent.

.DESCRIPTION
    Recreates the todo-6..9 build environment from scratch, with the todo-9b
    addition of --enable-png --enable-gtk (screendump needs PNG now). Steps:

      1. CLONE-OR-PIN  - if <QemuSrc>/.git is missing, shallow-clone
         https://gitlab.com/qemu-project/qemu at tag v11.1.0 into it; if the
         tree already exists, verify `git describe --tags` pins v11.1.0*
         (our installed wiring makes it v11.1.0-dirty - tolerated). A tree at
         a DIFFERENT tag aborts with instructions instead of clobbering.
      2. INSTALL        - runs scripts\install-machine.ps1 (no-op until
         Wave 3 drops sources into tools\qemu-b310e\machine\).
      3. CONFIGURE      - ./configure --target-list=<arm-softmmu> --enable-png --enable-gtk
         --disable-werror inside the MSYS2 MINGW64 shell. Skipped when
         build/config-host.mak exists and the args fingerprint matches, so
         re-runs do not reconfigure.
      4. BUILD          - make -j<N> in the MINGW64 shell (bounded by
         -BuildTimeoutSec; kill + log tail on timeout).
      5. SANITY         - build\qemu-system-arm.exe exists, prints the
         "QEMU emulator version" banner, and CONFIG_PNG=y is confirmed.

    ELEVATION (todo-8 lesson): meson's postconf scripts\symlink-install-tree.py
    calls os.symlink() and fails with WinError 1314 unless the token has
    SeCreateSymbolicLinkPrivilege. The script detects a non-elevated session,
    prints a prominent warning and continues; pass -AutoElevate to relaunch
    elevated (UAC prompt) automatically. msys2_shell.cmd itself fails when
    launched elevated, so this script drives C:\msys64\usr\bin\bash.exe
    directly with MSYSTEM=MINGW64 (the todo-8 workaround).

    Logs land next to the tree in %USERPROFILE%\qemu-b310e\:
    clone.log, configure.log, build.log, sanity.log (outside the repo).

.PARAMETER QemuSrc
    QEMU source tree (default <home>\qemu-b310e\qemu-src - local dir,
    never the UNC repo; meson breaks on UNC).

.PARAMETER Msys64
    MSYS2 install root (default C:\msys64).

.PARAMETER RepoUrl
    QEMU clone URL (default https://gitlab.com/qemu-project/qemu).

.PARAMETER Tag
    Tag to pin (default v11.1.0).

.PARAMETER TargetList
    configure --target-list value (default arm-softmmu).

.PARAMETER Jobs
    make -j value (default: NUMBER_OF_PROCESSORS, min 1).

.PARAMETER BuildTimeoutSec
    Bounded wait for clone/configure/build (default 1800).

.PARAMETER SkipClone / SkipInstall / SkipConfigure / SkipBuild / SkipSanity
    Phase skips for partial re-runs.

.PARAMETER AutoElevate
    Relaunch this script elevated (UAC) when the current session is not.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\build-qemu.ps1
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\build-qemu.ps1 -WhatIf
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\build-qemu.ps1 -AutoElevate -Jobs 16
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [string]$QemuSrc   = (Join-Path $env:USERPROFILE 'qemu-b310e\qemu-src'),
    [string]$Msys64    = 'C:\msys64',
    [string]$RepoUrl   = 'https://gitlab.com/qemu-project/qemu',
    [string]$Tag       = 'v11.1.0',
    [string]$TargetList = 'arm-softmmu',
    [int]$Jobs         = $env:NUMBER_OF_PROCESSORS,
    [int]$BuildTimeoutSec = 1800,
    [switch]$SkipClone,
    [switch]$SkipInstall,
    [switch]$SkipConfigure,
    [switch]$SkipBuild,
    [switch]$SkipSanity,
    [switch]$AutoElevate
)

$ErrorActionPreference = 'Stop'

if ($Jobs -lt 1) { $Jobs = 8 }

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function ConvertTo-MsysPath {
    # C:\Users\<you>\... -> /c/Users/<you>/... (MSYS2 style)
    param([string]$Path)
    $drive = $Path.Substring(0, 1).ToLowerInvariant()
    return '/' + $drive + ($Path.Substring(2) -replace '\\', '/')
}

function Invoke-Bounded {
    <#
    Run an external command with a bounded wait; kill + throw on timeout;
    throw with stdout/stderr tails on nonzero exit. Output goes to log files.
    #>
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$StdOutLog,
        [string]$StdErrLog,
        [int]$TimeoutSec,
        [string]$What
    )
    if (-not $PSCmdlet.ShouldProcess($What, 'run')) { return 0 }
    Write-Host "==> $What  (bounded: ${TimeoutSec}s)"
    $p = Start-Process -FilePath $FilePath -ArgumentList $Arguments -NoNewWindow `
        -RedirectStandardOutput $StdOutLog -RedirectStandardError $StdErrLog -PassThru
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        try { $p.Kill() } catch { }
        $tail = @(Get-Content -LiteralPath $StdErrLog -Tail 20 -ErrorAction SilentlyContinue) -join "`n"
        throw "TIMEOUT after ${TimeoutSec}s: $What`n--- stderr tail ---`n$tail"
    }
    if ($p.ExitCode -ne 0) {
        $outTail = @(Get-Content -LiteralPath $StdOutLog -Tail 10 -ErrorAction SilentlyContinue) -join "`n"
        $errTail = @(Get-Content -LiteralPath $StdErrLog -Tail 30 -ErrorAction SilentlyContinue) -join "`n"
        throw "FAILED (exit $($p.ExitCode)): $What`n--- stdout tail ---`n$outTail`n--- stderr tail ---`n$errTail"
    }
    Write-Host "    OK ($What)"
    return $p.ExitCode
}

# ---------------------------------------------------------------------------
# elevation check (todo-8 lesson: WinError 1314 on os.symlink in meson postconf)
# ---------------------------------------------------------------------------

$isAdmin = Test-Admin
if (-not $isAdmin) {
    Write-Host "WARNING: not an elevated session." -ForegroundColor Yellow
    Write-Host "  QEMU's meson postconf (scripts/symlink-install-tree.py) calls os.symlink()" -ForegroundColor Yellow
    Write-Host "  and fails with WinError 1314 unless the token has SeCreateSymbolicLinkPrivilege." -ForegroundColor Yellow
    Write-Host "  Run this script from an ELEVATED PowerShell (or pass -AutoElevate for a UAC relaunch)." -ForegroundColor Yellow
    if ($AutoElevate) {
        if ($PSCommandPath -like '\\*') {
            Write-Host "  -AutoElevate: script lives on a UNC path ($PSCommandPath); the elevated token" -ForegroundColor Yellow
            Write-Host "  may lose share access - run from an elevated shell instead. Continuing non-elevated." -ForegroundColor Yellow
        } else {
            Write-Host "  Relaunching elevated (UAC prompt)..."
            $argLine = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" " +
                "-QemuSrc `"$QemuSrc`" -Msys64 `"$Msys64`" -RepoUrl `"$RepoUrl`" -Tag `"$Tag`" " +
                "-TargetList `"$TargetList`" -Jobs $Jobs -BuildTimeoutSec $BuildTimeoutSec"
            foreach ($s in @('SkipClone', 'SkipInstall', 'SkipConfigure', 'SkipBuild', 'SkipSanity', 'AutoElevate')) {
                if (Get-Variable -Name $s -ValueOnly -ErrorAction SilentlyContinue) { $argLine += " -$s" }
            }
            $psPath = (Get-Process -Id $PID).Path
            $p = Start-Process -FilePath $psPath -ArgumentList $argLine -Verb RunAs -PassThru -Wait
            exit $p.ExitCode
        }
    }
} else {
    Write-Host "Elevated session: OK (symlink privilege available)."
}

$QemuSrc = [System.IO.Path]::GetFullPath($QemuSrc)
$Msys64  = [System.IO.Path]::GetFullPath($Msys64)
$LocalDir = Split-Path $QemuSrc -Parent
if (-not (Test-Path -LiteralPath $LocalDir)) { New-Item -ItemType Directory -Path $LocalDir -Force | Out-Null }

$Bash = Join-Path $Msys64 'usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $Bash)) { throw "MSYS2 bash not found at $Bash (install MSYS2 per todo 6)" }

Write-Host "B310E QEMU build"
Write-Host "  QemuSrc: $QemuSrc (tag $Tag)"
Write-Host "  MSYS2:   $Msys64"

# ---------------------------------------------------------------------------
# 1) clone-or-pin
# ---------------------------------------------------------------------------

if (-not (Test-Path -LiteralPath (Join-Path $QemuSrc '.git'))) {
    if ($SkipClone) { throw "QemuSrc missing at $QemuSrc and -SkipClone given" }
    $cloneLog = Join-Path $LocalDir 'clone.log'
    $env:GIT_TERMINAL_PROMPT = '0'   # fail instead of prompting on auth
    if ($PSCmdlet.ShouldProcess("clone $RepoUrl @ $Tag -> $QemuSrc", 'run')) {
        Invoke-Bounded -FilePath 'git' -Arguments @('clone', '--branch', $Tag, '--depth', '1', $RepoUrl, $QemuSrc) `
            -StdOutLog $cloneLog -StdErrLog $cloneLog -TimeoutSec $BuildTimeoutSec -What "git clone (tag $Tag)"
    }
} else {
    Write-Host "  qemu-src present - verifying pin"
    $desc = (& git -C $QemuSrc describe --tags 2>$null | Out-String).Trim()
    if ($desc -notlike "$Tag*") {
        throw "qemu-src is pinned to '$desc', expected '$Tag*'. Refusing to auto-reset (the tree may hold our installed wiring). Fix manually: git -C $QemuSrc checkout $Tag, or remove the dir and re-run."
    }
    Write-Host "  pin OK: $desc"
}

# ---------------------------------------------------------------------------
# 2) install our machine sources + wiring
# ---------------------------------------------------------------------------

if (-not $SkipInstall) {
    # HASHTABLE splatting: array splatting passes '-QemuSrc' POSITIONALLY,
    # which lands in install-machine.ps1's MachineDir param (e.g.
    # "C:\Windows\system32\-QemuSrc") -> "machine dir empty" no-op -> the
    # machine sources never get copied into qemu-src. Hashtable splatting
    # binds named params unambiguously.
    $installArgs = @{ QemuSrc = $QemuSrc }
    if ($WhatIfPreference) { $installArgs.WhatIf = $true }
    if ($PSCmdlet.ShouldProcess('install-machine.ps1', 'run')) {
        & (Join-Path $PSScriptRoot 'install-machine.ps1') @installArgs
        if ($LASTEXITCODE -ne 0) { throw "install-machine.ps1 failed (exit $LASTEXITCODE)" }
    }
}

# ---------------------------------------------------------------------------
# 3) configure (fingerprint-gated: re-runs do not reconfigure)
# ---------------------------------------------------------------------------

$BuildDir = Join-Path $QemuSrc 'build'
$FingerprintFile = Join-Path $BuildDir '.b310e-configure-fingerprint'
$Fingerprint = "target-list=$TargetList;png=1;gtk=1;werror=0"

if (-not $SkipConfigure) {

    # Auto-provision GTK3 via pacman
    if (-not (Test-Path (Join-Path $Msys64 "mingw64/include/gtk-3.0"))) {
        Write-Host "  Installing mingw-w64-x86_64-gtk3..."
        $cmd = "pacman -S --noconfirm mingw-w64-x86_64-gtk3"
        Invoke-Bounded -FilePath $Bash -Arguments @('-lc', $cmd) -StdOutLog (Join-Path $LocalDir 'pacman.log') -StdErrLog (Join-Path $LocalDir 'pacman.log') -TimeoutSec $BuildTimeoutSec -What 'pacman gtk3'
    }
    # fingerprint gate: configure only when (a) no build dir yet, (b) a build
    # dir exists but was NOT created by us (no fingerprint - e.g. the todo-8
    # build, which predates --enable-png --enable-gtk), or (c) the stored args differ.
    $needConfigure = -not (Test-Path -LiteralPath (Join-Path $BuildDir 'config-host.mak'))
    if (-not $needConfigure) {
        if (Test-Path -LiteralPath $FingerprintFile) {
            $stored = (Get-Content -LiteralPath $FingerprintFile -Raw -ErrorAction SilentlyContinue).Trim()
            $needConfigure = ($stored -ne $Fingerprint)
        } else {
            $needConfigure = $true   # foreign build dir: args unknown, reconfigure
        }
    }
    if ($needConfigure) {
        $cfgLog = Join-Path $LocalDir 'configure.log'
        $msysQemu = ConvertTo-MsysPath $QemuSrc
        $cmd = "cd '$msysQemu' && ./configure --target-list=$TargetList --enable-png --enable-gtk --disable-werror"
        $env:MSYSTEM = 'MINGW64'   # meson/make resolve to the MINGW64 toolchain (todo-8 workaround)
        if ($PSCmdlet.ShouldProcess("configure --target-list=$TargetList --enable-png --enable-gtk --disable-werror", 'run')) {
            Invoke-Bounded -FilePath $Bash -Arguments @('-lc', $cmd) `
                -StdOutLog $cfgLog -StdErrLog $cfgLog -TimeoutSec $BuildTimeoutSec -What 'configure'
            if ($PSCmdlet.ShouldProcess($FingerprintFile, 'write configure fingerprint')) {
                [System.IO.File]::WriteAllText($FingerprintFile, $Fingerprint, (New-Object System.Text.UTF8Encoding($false)))
            }
        }
    } else {
        Write-Host "  configure up to date (fingerprint matches) - skipping"
    }
}

# ---------------------------------------------------------------------------
# 4) build
# ---------------------------------------------------------------------------

if (-not $SkipBuild) {
    $buildLog = Join-Path $LocalDir 'build.log'
    $msysQemu = ConvertTo-MsysPath $QemuSrc
    $cmd = "cd '$msysQemu' && make -j$Jobs"
    $env:MSYSTEM = 'MINGW64'
    if ($PSCmdlet.ShouldProcess("make -j$Jobs", 'run')) {
        Invoke-Bounded -FilePath $Bash -Arguments @('-lc', $cmd) `
            -StdOutLog $buildLog -StdErrLog $buildLog -TimeoutSec $BuildTimeoutSec -What 'make'
    }
}

# ---------------------------------------------------------------------------
# 5) sanity
# ---------------------------------------------------------------------------

if (-not $SkipSanity) {
    $exe = Join-Path $BuildDir 'qemu-system-arm.exe'
    if ($PSCmdlet.ShouldProcess('sanity checks', 'run')) {
        if (-not (Test-Path -LiteralPath $exe)) {
            throw "SANITY FAILED: $exe missing (build incomplete or -SkipBuild?)"
        }
        $ver = (& $exe --version 2>&1 | Out-String)
        if ($ver -notmatch 'QEMU emulator version') {
            throw "SANITY FAILED: unexpected --version output:`n$ver"
        }
        $pngLine = Select-String -Path (Join-Path $BuildDir 'config-host.mak') -Pattern '^CONFIG_PNG=' -ErrorAction SilentlyContinue
        if ($pngLine -and $pngLine.Line -notmatch '=y$') {
            throw "SANITY FAILED: CONFIG_PNG not enabled (screendump writes PPM): $($pngLine.Line.Trim())"
        }
        Write-Host "`n  SANITY OK:"
        Write-Host ($ver.Trim())
        if ($pngLine) { Write-Host "  $($pngLine.Line.Trim())" }
        $head = (& git -C $QemuSrc rev-parse --short HEAD 2>$null | Out-String).Trim()
        if ($head) { Write-Host "  qemu-src HEAD: $head" }
    }
}

Write-Host "`nDone. Usage doc: docs/b310e-qemu.md (Wave 5/7)."
