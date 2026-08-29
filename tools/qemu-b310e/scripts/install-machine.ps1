<#
.SYNOPSIS
    Install the B310E machine sources (tools/qemu-b310e/machine/) into a QEMU
    source tree and wire the meson/Kconfig build entries. Idempotent.

.DESCRIPTION
    Copies machine/hw/arm/*.c   -> <qemu-src>/hw/arm/
             machine/hw/misc/*.c -> <qemu-src>/hw/misc/
    and appends marker-guarded wiring blocks (# B310E-MACHINE-BEGIN/END) to
    hw/arm/meson.build, hw/arm/Kconfig, hw/misc/meson.build, hw/misc/Kconfig.
    The wiring is GENERATED from the .c files actually present in machine/, so
    Wave-3/4 todos just drop files in and re-run this script.

    - Idempotent: a re-run strips the old block and re-appends an identical
      one -> no file changes (byte-identical), exit 0.
    - An EMPTY machine dir is a no-op: copies nothing, wires nothing, exit 0.
    - Optional machine/hw/{arm,misc}/trace-events files (when present) are
      appended marker-guarded to the matching qemu-src trace-events files
      (Wave-3 trace-event prep).
    - -Uninstall strips the marker blocks and deletes the copied files.
    - -WhatIf prints what would change without touching anything.

    Text edits are LF-preserving and BOM-free; a tree with UNBALANCED markers
    is refused (throw) rather than repaired, so a corrupt state is never
    silently rewritten.

.PARAMETER MachineDir
    Directory containing hw/arm/ + hw/misc/ subtrees. Default: ..\machine
    relative to this script ($PSScriptRoot).

.PARAMETER QemuSrc
    QEMU source tree to install into. Default <home>\qemu-b310e\qemu-src
    (local dir - the UNC repo breaks meson; never point this at the repo).

.PARAMETER Uninstall
    Reverse a previous install: strip the marker blocks and remove the copied
    files (basename match against the current MachineDir contents).

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\install-machine.ps1
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\install-machine.ps1 -QemuSrc <home>\qemu-b310e\qemu-src -WhatIf
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\install-machine.ps1 -Uninstall
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
param(
    # NOTE: no $PSScriptRoot in defaults - with [CmdletBinding()], PS 5.1
    # evaluates param defaults BEFORE $PSScriptRoot is populated (empty
    # string). Resolve below in the body instead.
    [string]$MachineDir,
    [string]$QemuSrc    = (Join-Path $env:USERPROFILE 'qemu-b310e\qemu-src'),
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
if (-not $MachineDir) { $MachineDir = Join-Path $PSScriptRoot '..\machine' }

$MarkerBegin = '# B310E-MACHINE-BEGIN'
$MarkerEnd   = '# B310E-MACHINE-END'

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

function ConvertTo-SymbolName {
    # sc6530_aux.c -> SC6530_AUX ; b310e.c -> B310E
    param([System.IO.FileInfo]$File)
    return $File.BaseName.ToUpperInvariant()
}

function New-ArmMesonBlock {
    # hw/arm/meson.build uses arm_common_ss (see hw/arm/meson.build, e.g. CONFIG_MUSICPAL)
    param([System.IO.FileInfo[]]$Files)
    $lines = foreach ($f in $Files) {
        "arm_common_ss.add(when: 'CONFIG_$(ConvertTo-SymbolName $f)', if_true: files('$($f.Name)'))"
    }
    return ($lines -join "`n")
}

function New-ArmKconfigBlock {
    # machine stanza per hw/arm/Kconfig convention (MUSICPAL: bool/default y/depends on TCG && ARM)
    param([System.IO.FileInfo[]]$Files)
    $stanzas = foreach ($f in $Files) {
        "config $(ConvertTo-SymbolName $f)`n    bool`n    default y`n    depends on TCG && ARM"
    }
    return ($stanzas -join "`n`n")
}

function New-MiscMesonBlock {
    # hw/misc/meson.build uses system_ss (see hw/misc/meson.build, e.g. CONFIG_ARM11SCU)
    param([System.IO.FileInfo[]]$Files)
    $lines = foreach ($f in $Files) {
        "system_ss.add(when: 'CONFIG_$(ConvertTo-SymbolName $f)', if_true: files('$($f.Name)'))"
    }
    return ($lines -join "`n")
}

function New-MiscKconfigBlock {
    # device stanza per hw/misc/Kconfig convention; ARM-gated so non-ARM targets skip it
    param([System.IO.FileInfo[]]$Files)
    $stanzas = foreach ($f in $Files) {
        "config $(ConvertTo-SymbolName $f)`n    bool`n    default y`n    depends on ARM"
    }
    return ($stanzas -join "`n`n")
}

function Update-MarkerBlock {
    <#
    .SYNOPSIS
        Replace (or append) the # B310E-MACHINE-BEGIN/END block in a text file.
        $Block = '' strips the block. Returns $true if the file changed.
    #>
    param(
        [string]$Path,
        [string]$Block
    )

    $orig = [System.IO.File]::ReadAllText($Path)  # preserves existing line endings exactly

    # corruption guard: markers must be balanced (BEGIN count == END count)
    $begins = [regex]::Matches($orig, '(?m)^# B310E-MACHINE-BEGIN\r?$').Count
    $ends   = [regex]::Matches($orig, '(?m)^# B310E-MACHINE-END\r?$').Count
    if ($begins -ne $ends) {
        throw "Marker state corrupt in $Path (BEGIN=$begins END=$ends) - refusing to modify"
    }

    # strip the old block (if any) so re-runs are no-ops
    $newText = [regex]::Replace($orig, '(?ms)^# B310E-MACHINE-BEGIN\r?\n.*?^# B310E-MACHINE-END(?:\r?\n|$)', '')

    if ($Block) {
        if (-not $newText.EndsWith("`n")) { $newText += "`n" }
        $newText += "$MarkerBegin`n$Block`n$MarkerEnd`n"
    }

    if ($newText -ne $orig) {
        if ($PSCmdlet.ShouldProcess($Path, ($(if ($Block) { 'update B310E wiring block' } else { 'strip B310E wiring block' })))) {
            [System.IO.File]::WriteAllText($Path, $newText, (New-Object System.Text.UTF8Encoding($false)))
            Write-Host "    $([IO.Path]::GetFileName($Path)): $(if ($Block) { 'wired' } else { 'stripped' })"
        }
        return $true
    }
    Write-Host "    $([IO.Path]::GetFileName($Path)): up to date (no change)"
    return $false
}

# ---------------------------------------------------------------------------
# resolve + validate paths
# ---------------------------------------------------------------------------

$MachineDir = [System.IO.Path]::GetFullPath($MachineDir)
$QemuSrc    = [System.IO.Path]::GetFullPath($QemuSrc)

if (-not (Test-Path -LiteralPath $QemuSrc)) { throw "QEMU source tree not found: $QemuSrc" }
foreach ($rel in @('hw\arm\meson.build', 'hw\arm\Kconfig', 'hw\misc\meson.build', 'hw\misc\Kconfig')) {
    if (-not (Test-Path -LiteralPath (Join-Path $QemuSrc $rel))) {
        throw "Not a QEMU source tree (missing $rel) in: $QemuSrc"
    }
}

$ArmDir    = Join-Path $MachineDir 'hw\arm'
$MiscDir   = Join-Path $MachineDir 'hw\misc'
$ArmTrace  = Join-Path $ArmDir  'trace-events'
$MiscTrace = Join-Path $MiscDir 'trace-events'

$ArmFiles  = @(Get-ChildItem -LiteralPath $ArmDir  -Filter '*.c' -File -ErrorAction SilentlyContinue | Sort-Object Name)
$MiscFiles = @(Get-ChildItem -LiteralPath $MiscDir -Filter '*.c' -File -ErrorAction SilentlyContinue | Sort-Object Name)

$WiringTargets = @(
    @{ Rel = 'hw\arm\meson.build';  Block = (New-ArmMesonBlock  $ArmFiles)  },
    @{ Rel = 'hw\arm\Kconfig';      Block = (New-ArmKconfigBlock $ArmFiles)  },
    @{ Rel = 'hw\misc\meson.build'; Block = (New-MiscMesonBlock $MiscFiles) },
    @{ Rel = 'hw\misc\Kconfig';     Block = (New-MiscKconfigBlock $MiscFiles) }
)

Write-Host "B310E machine installer"
Write-Host "  MachineDir: $MachineDir  (arm: $($ArmFiles.Count) file(s), misc: $($MiscFiles.Count) file(s))"
Write-Host "  QemuSrc:    $QemuSrc"

if (-not $Uninstall) {
    if ($ArmFiles.Count -eq 0 -and $MiscFiles.Count -eq 0 -and
        -not (Test-Path -LiteralPath $ArmTrace) -and -not (Test-Path -LiteralPath $MiscTrace)) {
        Write-Host "  Machine dir is empty - nothing to copy or wire (no-op, exit 0)."
    }

    # 1) copy the .c sources
    foreach ($f in $ArmFiles) {
        $dst = Join-Path $QemuSrc ("hw\arm\" + $f.Name)
        if ($PSCmdlet.ShouldProcess($dst, 'copy machine source')) {
            Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
            Write-Host "    copied hw\arm\$($f.Name)"
        }
    }
    foreach ($f in $MiscFiles) {
        $dst = Join-Path $QemuSrc ("hw\misc\" + $f.Name)
        if ($PSCmdlet.ShouldProcess($dst, 'copy machine source')) {
            Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
            Write-Host "    copied hw\misc\$($f.Name)"
        }
    }

    # 2) wire meson/Kconfig (marker-guarded, generated from the file list)
    foreach ($t in $WiringTargets) {
        $path = Join-Path $QemuSrc $t.Rel
        if ($t.Block) {
            Update-MarkerBlock -Path $path -Block $t.Block | Out-Null
        } else {
            Write-Host "    $([IO.Path]::GetFileName($path)): no sources - wiring skipped"
        }
    }

    # 3) optional trace-events files (Wave-3 prep; appended marker-guarded)
    foreach ($pair in @(@($ArmTrace, 'hw\arm\trace-events'), @($MiscTrace, 'hw\misc\trace-events'))) {
        if (Test-Path -LiteralPath $pair[0]) {
            $block = ([System.IO.File]::ReadAllText($pair[0])).TrimEnd("`r", "`n")
            Update-MarkerBlock -Path (Join-Path $QemuSrc $pair[1]) -Block $block | Out-Null
        }
    }
} else {
    # -Uninstall: strip wiring + trace blocks, remove the copied files
    Write-Host "  Uninstall mode"
    foreach ($t in $WiringTargets) {
        Update-MarkerBlock -Path (Join-Path $QemuSrc $t.Rel) -Block '' | Out-Null
    }
    foreach ($pair in @(@($ArmTrace, 'hw\arm\trace-events'), @($MiscTrace, 'hw\misc\trace-events'))) {
        # NOTE: bare `Test-Path -or ...` would swallow -or as a parameter name
        # (PS 5.1 parsing gotcha) - parenthesize the command.
        if ((Test-Path -LiteralPath $pair[0]) -or (Test-Path -LiteralPath (Join-Path $QemuSrc $pair[1]))) {
            Update-MarkerBlock -Path (Join-Path $QemuSrc $pair[1]) -Block '' | Out-Null
        }
    }
    foreach ($f in @($ArmFiles + $MiscFiles)) {
        $dst = Join-Path $QemuSrc ($(if ($f.DirectoryName -eq $ArmDir) { 'hw\arm\' } else { 'hw\misc\' }) + $f.Name)
        if (Test-Path -LiteralPath $dst) {
            if ($PSCmdlet.ShouldProcess($dst, 'remove copied machine source')) {
                Remove-Item -LiteralPath $dst -Force
                Write-Host "    removed $([IO.Path]::GetFileName($dst))"
            }
        }
    }
}

# ---------------------------------------------------------------------------
# summary: what changed in the qemu tree
# ---------------------------------------------------------------------------

if (Test-Path -LiteralPath (Join-Path $QemuSrc '.git')) {
    Write-Host "`n  --- qemu-src git status (our additions only) ---"
    & git -C $QemuSrc status --short
    Write-Host "  --- end git status ---"
}

Write-Host "Done."
