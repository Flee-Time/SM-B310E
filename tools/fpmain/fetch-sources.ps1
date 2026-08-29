# B310E-OS - tools/fpmain/fetch-sources.ps1
#
# Downloads + patches the fpdoom game sources into the fpdoom clone.
# Unlike helper.make (whose `src/*` globs only extract top-level files and
# miss subdirs like src/doom, src/strife), this does a FULL unzip of every
# archive - that bug broke the chocolate-doom patch earlier.
#
# Uses the bash shims in the clone's bin-shim/ (wget->curl, cc->gcc).
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\fpmain\fetch-sources.ps1

$ErrorActionPreference = "Stop"
$Repo   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
# The fpdoom clone lives in the repo's build/ tree - cloned here automatically
# if missing. Override with FPDOOM_DIR.
$Fpdoom = if ($env:FPDOOM_DIR) { $env:FPDOOM_DIR } else { Join-Path $Repo "build\fpdoom" }
$Unzip  = "C:\Program Files\Git\usr\bin\unzip.exe"
$Curl   = "C:\Windows\System32\curl.exe"

if (-not (Test-Path (Join-Path $Fpdoom ".git"))) {
    Write-Output "cloning fpdoom -> $Fpdoom"
    New-Item -ItemType Directory -Force -Path (Split-Path $Fpdoom) | Out-Null
    git clone https://github.com/ilyakurdyukov/fpdoom $Fpdoom
    if ($LASTEXITCODE -ne 0) { Write-Error "fpdoom clone failed" }
}
if (-not (Test-Path $Fpdoom)) { Write-Error "fpdoom clone not found: $Fpdoom" }

# curl the given URL to the given .zip path (full download)
function Get-Zip($url, $zip) {
    Write-Output "download: $zip"
    & $Curl -L -sS -o $zip $url
    if (-not (Test-Path $zip) -or (Get-Item $zip).Length -lt 1000) {
        Write-Error "download failed: $url"
    }
}

# After a direct patch run, verify the patch TRULY landed: GNU patch leaves
# .rej per failed hunk and .orig per touched file, so their absence proves a
# clean full application. Exit code 0 + rejected hunks can still happen, so
# scan the tree, not just $LASTEXITCODE.
function Assert-NoRejects([string]$root, [string]$name) {
    $rej = @(Get-ChildItem $root -Recurse -Include *.rej,*.orig -File -ErrorAction SilentlyContinue)
    if ($rej.Count -gt 0) {
        $list = ($rej | ForEach-Object { $_.FullName }) -join ', '
        throw "patch FAILED ($name): $($rej.Count) rejected hunk(s)/backup file(s) left behind: $list"
    }
}

# Normalize text files to LF (skip binaries - NUL bytes - which would be
# corrupted by a text read/write round-trip). Extracted sources (InfoNES,
# the Ubuntu snes9x tarball, ...) ship CRLF while the port .patch files
# assume LF - GNU patch then fails every hunk with "different line endings".
# $Path is a file or a directory (recursive).
function Convert-ToLF([string]$path) {
    if (Test-Path -LiteralPath $path -PathType Container) {
        Get-ChildItem $path -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
            try {
                $bytes = [System.IO.File]::ReadAllBytes($_.FullName)
                if ($bytes -contains 0) { return }   # binary - leave alone
                $t = [System.Text.Encoding]::UTF8.GetString($bytes)
                if ($t.Contains("`r`n")) {
                    [System.IO.File]::WriteAllText($_.FullName, $t.Replace("`r`n", "`n"))
                }
            } catch { }
        }
    } elseif (Test-Path -LiteralPath $path -PathType Leaf) {
        try {
            $bytes = [System.IO.File]::ReadAllBytes($path)
            if ($bytes -contains 0) { return }   # binary - leave alone
            $t = [System.Text.Encoding]::UTF8.GetString($bytes)
            if ($t.Contains("`r`n")) {
                [System.IO.File]::WriteAllText($path, $t.Replace("`r`n", "`n"))
            }
        } catch { }
    }
}

# full-extract a zip whose single top dir is "<name>-<hash>" -> rename to $dest
# $sub: optional inner path to move instead (e.g. the DOOM zip's linuxdoom-1.10
# subdir -> doom_src, matching helper.make's `mv $name/linuxdoom-1.10`).
function Full-Unzip($zip, $srcName, $dest, $sub) {
    Write-Output "extract: $zip -> $dest"
    if (Test-Path $dest) { Write-Output "  (already present)"; return }
    $tmp = Join-Path (Split-Path $zip) "_extract"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Path $tmp | Out-Null
    Push-Location $tmp
    try { & $Unzip -q $zip 2>&1 | Out-Null } finally { Pop-Location }
    $src = Join-Path $tmp $srcName
    if (-not (Test-Path $src)) {
        # find the single top-level dir
        $top = Get-ChildItem $tmp -Directory | Select-Object -First 1
        if (-not $top) { Write-Error "no top dir in $zip" }
        $src = $top.FullName
    }
    if ($sub) { $src = Join-Path $src $sub }
    if (-not (Test-Path $src)) { Write-Error ("source subpath not found in " + $zip + ": " + $sub) }
    Move-Item $src $dest
    Remove-Item -Recurse -Force $tmp
    Write-Output "  -> $dest"
}

$jobs = @(
    @{ Dir="chocolate-doom"; Zip="chocolate-doom.zip";
       Url="https://github.com/chocolate-doom/chocolate-doom/archive/0b3cb528c3f53c61d7a4ebe13a7d522570b98d83.zip";
       Src="chocolate-doom-0b3cb528c3f53c61d7a4ebe13a7d522570b98d83"; Dest="chocolate-doom";
       Patch="chocolate-doom.patch" },
    @{ Dir="wolf3d"; Zip="Wolf4SDL.zip";
       Url="https://github.com/KS-Presto/Wolf4SDL/archive/dc8b250af35fb0ace68db5eb879490b50068c20e.zip";
       Src="Wolf4SDL-dc8b250af35fb0ace68db5eb879490b50068c20e"; Dest="Wolf4SDL";
       Patch="wolf3d.patch" },
    @{ Dir="gnuboy"; Zip="gnuboy.zip";
       Url="https://github.com/rofl0r/gnuboy/archive/c367bb4ba96fb07cd62f72f5ecb43aeff7012564.zip";
       Src="gnuboy-c367bb4ba96fb07cd62f72f5ecb43aeff7012564"; Dest="gnuboy";
       Patch="gnuboy.patch" },
    @{ Dir="infones"; Zip="InfoNES.zip";
       Url="https://github.com/jay-kumogata/InfoNES/archive/363bac8bbb030c3d3708ab32cd719ae6b2919971.zip";
       Src="InfoNES-363bac8bbb030c3d3708ab32cd719ae6b2919971"; Dest="InfoNES";
       Patch="InfoNES.patch" },
    @{ Dir="fpdoom"; Zip="DOOM.zip";
       Url="https://github.com/id-Software/DOOM/archive/a77dfb96cb91780ca334d0d4cfd86957558007e0.zip";
       Src="DOOM-a77dfb96cb91780ca334d0d4cfd86957558007e0"; Sub="linuxdoom-1.10"; Dest="..\doom_src";
       Patch="..\doom.patch" },
    @{ Dir="fpbuild"; Zip="jfbuild.zip";
       Url="https://github.com/jonof/jfbuild/archive/efd88d9cc24f753038a28479c9d8e7ac398909c8.zip";
       Src="jfbuild-efd88d9cc24f753038a28479c9d8e7ac398909c8"; Dest="jfbuild";
       Patch="jfbuild.patch" },
    @{ Dir="retris"; Zip="retris.zip";
       Url="https://github.com/ilyakurdyukov/retris/archive/b81fc06381fc648ed8cb491c2018c5dd009c20c3.zip";
       Src="retris-b81fc06381fc648ed8cb491c2018c5dd009c20c3"; Dest="retris";
       Patch="retris.patch" }
)

foreach ($j in $jobs) {
    $dir = Join-Path $Fpdoom $j.Dir
    $zip = Join-Path $dir $j.Zip
    $dest = Join-Path $dir $j.Dest
    if (Test-Path $dest) { Write-Output "skip $($j.Dir): $dest present"; continue }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Get-Zip $j.Url $zip
    Full-Unzip $zip $j.Src $dest $j.Sub
    # apply the port patch DIRECTLY (patch -p1) - NOT via helper.make's `patch`
    # target: that target depends on `all`, which re-extracts with the partial
    # src/* globs and OVERWRITES the full tree we just unzipped (the very bug
    # this script exists to fix). chocolate-doom.patch etc. live in the port dir.
    if ($j.Patch) {
        $pf = Join-Path $dir $j.Patch
        if (Test-Path $pf) {
            Write-Output "patch: $($j.Dir) (direct)"
            # The extracted source may have CRLF line endings (InfoNES etc.)
            # while the .patch assumes LF - patch then fails every hunk with
            # "different line endings". Normalize both to LF (same approach
            # the rockbox port uses).
            Convert-ToLF $dest
            # drop any .rej files a previous failed patch attempt left behind
            Get-ChildItem $dest -Recurse -Filter *.rej -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
            # the .patch itself may have CRLF (checkout artifacts) while the
            # normalized tree is LF - GNU patch compares endings between the
            # patch and target, so normalize the patch file to LF as well.
            Convert-ToLF $pf
            Push-Location $dir
            try {
                # $env:ComSpec (absolute cmd.exe path) for the < stdin redirect
                # (PowerShell has no < operator). NEVER bare `cmd` - if PATH
                # resolves it oddly Windows pops a "pick an app for cmd" dialog.
                & $env:ComSpec /c "`"C:\Program Files\Git\usr\bin\patch.exe`" -p1 -d `"$dest`" < `"$pf`""
                if ($LASTEXITCODE -ne 0) { Write-Error "patch failed: $($j.Dir)" }
                Assert-NoRejects $dest $j.Dir
            } finally { Pop-Location }
        }
    }
}

# snes9x: the Makefile/helper fetches a .tar.gz with a specific layout
$snes = Join-Path $Fpdoom "snes9x"
$snesDest = Join-Path $snes "snes9x_src"
if (-not (Test-Path (Join-Path $snesDest "tile.h"))) {
    New-Item -ItemType Directory -Force -Path $snes | Out-Null
    $tgz = Join-Path $snes "snes9x_1.43.orig.tar.gz"
    Write-Output "download: $tgz"
    & $Curl -L -sS -o $tgz "https://old-releases.ubuntu.com/ubuntu/pool/multiverse/s/snes9x/snes9x_1.43.orig.tar.gz"
    New-Item -ItemType Directory -Force -Path $snesDest | Out-Null
    Push-Location $snes
    try {
        # MSYS2 tar handles gzip AND --wildcards; git-bash tar lacks gzip and
        # Windows bsdtar lacks --wildcards. msys64/usr/bin must precede git-bash
        # on PATH so gzip resolves to the MSYS one. Use RELATIVE paths inside
        # this Push-Location: MSYS tar's gzip child cannot resolve an absolute
        # Windows drive path ("Cannot connect to C: resolve failed").
        $env:Path = "C:\msys64\usr\bin;" + $env:Path
        & "C:\msys64\usr\bin\tar.exe" -xzf "snes9x_1.43.orig.tar.gz" -C "snes9x_src" --strip-components 3 --wildcards "snes9x-1.43.orig/snes9x-1.43-src/snes9x/*" 2>&1 | Out-Null
        # snes9x.h does `#include "../language.h"` - language.h is a SIBLING of
        # the snes9x/ subdir in the tarball, so it must land in the port root
        # (snes9x/language.h), not in snes9x_src. Extract by exact path (MSYS
        # tar's --strip-components silently no-ops on single-file extracts, so
        # grab the full path and copy).
        New-Item -ItemType Directory -Force -Path "_tgzroot" | Out-Null
        & "C:\msys64\usr\bin\tar.exe" -xzf "snes9x_1.43.orig.tar.gz" -C "_tgzroot" "snes9x-1.43.orig/snes9x-1.43-src/language.h" 2>&1 | Out-Null
        Copy-Item "_tgzroot\snes9x-1.43.orig\snes9x-1.43-src\language.h" "language.h" -Force
        Remove-Item -Recurse -Force "_tgzroot" -ErrorAction SilentlyContinue
        # apply the snes9x port patch directly (snes9x.patch, patch -p1).
        # The Ubuntu .orig.tar.gz ships CRLF - normalize tree + patch to LF
        # first (the 7-job loop does the same; without it every hunk fails
        # with "different line endings").
        Convert-ToLF $snesDest
        Convert-ToLF (Join-Path $snes 'snes9x.patch')
        # $env:ComSpec (absolute cmd.exe) - never bare `cmd` (see above).
        & $env:ComSpec /c "`"C:\Program Files\Git\usr\bin\patch.exe`" -p1 -d `"$snesDest`" < `"$(Join-Path $snes 'snes9x.patch')`""
        if ($LASTEXITCODE -ne 0) { Write-Error "patch failed: snes9x" }
        Assert-NoRejects $snesDest 'snes9x'
    } finally { Pop-Location }
    Write-Output "  -> $snesDest"
}

Write-Output "sources fetched."
