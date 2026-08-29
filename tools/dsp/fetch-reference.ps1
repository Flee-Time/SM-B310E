<#
 fetch-reference.ps1 - T4 (b310e-audio-eq-tune): fetch-on-demand, hash-pinned
 download of the SDK reference files used to cross-verify the B310E EQ_*
 record decode.

 The files are external/untrusted (leaked W217 MOCOR SDK mirror on GitHub).
 AGENTS.md hard rule: NEVER commit SDK code into the repo. This script
 downloads into the GITIGNORED tools/dsp/reference/ directory (see .gitignore),
 which is a fetch cache only -- nothing in it is tracked.

 Pinning: each file has an expected SHA-256 recorded below (the hashes were
 taken from a verified download on 2026-08-26). If a cached file's hash
 matches it is reused (offline-safe); if it mismatches the script FAILS
 loudly (do not silently accept a different upstream snapshot). If upstream
 changes, re-pin by recording the new hash here.

 Usage:
   powershell -ExecutionPolicy Bypass -File tools\dsp\fetch-reference.ps1
   powershell -ExecutionPolicy Bypass -File tools\dsp\fetch-reference.ps1 -Force   # re-download even if cached

 Output: tools/dsp/reference/<name> for each file, verified SHA-256.
 Exit 0: all files present + hash-verified. Exit 1: any failure.
#>
[CmdletBinding()]
param(
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot   = Split-Path -Parent (Split-Path -Parent $scriptDir)
$refDir     = Join-Path $scriptDir 'reference'
$baseUrl    = 'https://raw.githubusercontent.com/obaidi2005/ZW217_W19_V4.1_yisai/master'

# name -> @{ url-rel-path; sha256 }
$Files = [ordered]@{
    'audio_eq.nvm'           = @{ path = 'common/nv_parameters/audio/audio_eq.nvm';
                                  sha = '421D6DEBF8190BA2EB77861250E116ADBD4CFE3356E21D80A1A452DA0F2E1022' }
    'audio_dsp_codec_6530.nvm' = @{ path = 'common/nv_parameters/audio/audio_dsp_codec_6530.nvm';
                                    sha = '68CB84AC993A68340C43BE3CB376C442DC1F7C37085C185636B2F1D98BD39E9E' }
    'aud_enha_exp.h'         = @{ path = 'MS_Ref/export/inc/aud_enha_exp.h';
                                  sha = '8B5A59B778A7166C563E79190E3183C880E9A82D8B42A217D3F26A961090C936' }
    'eq_exp.h'               = @{ path = 'MS_Ref/export/inc/eq_exp.h';
                                  sha = 'E88888325B012DFC9A4E13560110641A600410ADD8AB4079BDA9F4DB624C729E' }
}

if (-not (Test-Path -LiteralPath $refDir)) {
    New-Item -ItemType Directory -Path $refDir -Force | Out-Null
    Write-Host "created $refDir"
}

$fail = 0
foreach ($name in $Files.Keys) {
    $meta    = $Files[$name]
    $target  = Join-Path $refDir $name
    $url     = "$baseUrl/$($meta.path)"
    $wantSha = $meta.sha
    $mode    = 'download'

    if (-not $Force -and (Test-Path -LiteralPath $target)) {
        $have = (Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash
        if ($have -eq $wantSha) {
            Write-Host "OK   (cached) $name  sha256 $have"
            continue
        }
        if (-not $Force) {
            Write-Error "HASH MISMATCH for cached ${name} (have $have, want $wantSha). Use -Force to re-download. NOT overwriting."
            $fail = 1
            continue
        }
    }

    Write-Host "GET  $url"
    try {
        Invoke-WebRequest -Uri $url -OutFile $target -UseBasicParsing -ErrorAction Stop
    } catch {
        # fall back to curl.exe (some environments have a stricter IWR proxy policy)
        Write-Host "Invoke-WebRequest failed ($($_.Exception.Message)); retrying with curl.exe"
        & curl.exe -sS -L -o $target $url
        if ($LASTEXITCODE -ne 0) {
            Write-Error "download of ${name} failed (curl rc=$LASTEXITCODE)"
            if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force }
            $fail = 1
            continue
        }
    }
    $have = (Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash
    if ($have -ne $wantSha) {
        Write-Error "POST-DOWNLOAD HASH MISMATCH for ${name}: have $have, want $wantSha. File deleted; upstream changed or corrupted."
        Remove-Item -LiteralPath $target -Force
        $fail = 1
        continue
    }
    Write-Host "OK   (fetched) $name  sha256 $have  $((Get-Item $target).Length) bytes"
}

if ($fail) {
    Write-Host "FETCH-REFERENCE: FAILED (see above)" -ForegroundColor Red
    exit 1
}
Write-Host "FETCH-REFERENCE: all $($Files.Count) reference files present + hash-verified in $refDir"
exit 0
