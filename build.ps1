# Shared Windows entry point. All target options are forwarded to Python.
$ErrorActionPreference = 'Stop'
$python = if ($env:B310E_PYTHON) { $env:B310E_PYTHON } else {
    $found = Get-Command python -ErrorAction SilentlyContinue
    if ($found -and $found.Source -notlike '*WindowsApps*') { $found.Source }
    elseif (Test-Path -LiteralPath 'C:\msys64\mingw64\bin\python.exe') { 'C:\msys64\mingw64\bin\python.exe' }
    else { throw 'Python 3.10+ is required. Set B310E_PYTHON to its executable.' }
}
& $python (Join-Path $PSScriptRoot 'scripts\build.py') @args
exit $LASTEXITCODE
