# Windows wrapper for the same isolated build used on Linux.
$ErrorActionPreference = 'Stop'
$bash = 'C:\msys64\usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash)) { $bash = (Get-Command bash -ErrorAction Stop).Source }
$script = (Join-Path $PSScriptRoot 'build-fpmain.sh').Replace('\', '/')
$env:B310E_FPM_BUILD_SCRIPT = '/' + $script.Substring(0, 1).ToLower() + $script.Substring(2)
& $bash -c 'export PATH=/usr/bin:/bin:$PATH; exec bash "$B310E_FPM_BUILD_SCRIPT"'
exit $LASTEXITCODE
