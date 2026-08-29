#!/usr/bin/env bash
# fetch-reference.sh - T4 (b310e-audio-eq-tune): fetch-on-demand, hash-pinned
# download of the SDK reference files used to cross-verify the B310E EQ_*
# record decode.
#
# The files are external/untrusted (leaked W217 MOCOR SDK mirror on GitHub).
# AGENTS.md hard rule: NEVER commit SDK code into the repo. This script
# downloads into the GITIGNORED tools/dsp/reference/ directory (see .gitignore),
# which is a fetch cache only -- nothing in it is tracked.
#
# Pinning: each file has an expected SHA-256 recorded below (the hashes were
# taken from a verified download on 2026-08-26). If a cached file's hash
# matches it is reused (offline-safe); if it mismatches the script FAILS
# loudly (do not silently accept a different upstream snapshot). If upstream
# changes, re-pin by recording the new hash here.
#
# Usage:
#   tools/dsp/fetch-reference.sh
#   tools/dsp/fetch-reference.sh --force   # re-download even if cached
#
# Output: tools/dsp/reference/<name> for each file, verified SHA-256.
# Exit 0: all files present + hash-verified. Exit 1: any failure.
set -euo pipefail

scriptDir=$(cd "$(dirname "$0")" && pwd)
refDir="$scriptDir/reference"
baseUrl='https://raw.githubusercontent.com/obaidi2005/ZW217_W19_V4.1_yisai/master'

force=0
case "${1:-}" in
    -Force|-force|--force) force=1 ;;
    '') ;;
    *)
        echo "Usage: $0 [--force]" >&2
        exit 1
        ;;
esac

if ! command -v curl >/dev/null 2>&1; then
    echo "ERROR: curl not found on PATH" >&2
    exit 1
fi

fail=0

fetch_one() {
    local name=$1 rel=$2 want=$3
    local target="$refDir/$name"
    local url="$baseUrl/$rel"
    local have want_lc

    if [[ $force -eq 0 && -f "$target" ]]; then
        have=$(sha256sum "$target" | awk '{print $1}')
        want_lc=$(printf '%s' "$want" | tr '[:upper:]' '[:lower:]')
        if [[ "$have" == "$want_lc" ]]; then
            echo "OK   (cached) $name  sha256 $have"
            return 0
        fi
        echo "HASH MISMATCH for cached ${name} (have $have, want $want). Use --force to re-download. NOT overwriting." >&2
        fail=1
        return 1
    fi

    echo "GET  $url"
    if ! curl -sS -L -o "$target" "$url"; then
        rc=$?
        echo "download of ${name} failed (curl rc=$rc)" >&2
        rm -f "$target"
        fail=1
        return 1
    fi
    have=$(sha256sum "$target" | awk '{print $1}')
    want_lc=$(printf '%s' "$want" | tr '[:upper:]' '[:lower:]')
    if [[ "$have" != "$want_lc" ]]; then
        echo "POST-DOWNLOAD HASH MISMATCH for ${name}: have $have, want $want. File deleted; upstream changed or corrupted." >&2
        rm -f "$target"
        fail=1
        return 1
    fi
    echo "OK   (fetched) $name  sha256 $have  $(wc -c < "$target") bytes"
}

if [[ ! -d "$refDir" ]]; then
    mkdir -p "$refDir"
    echo "created $refDir"
fi

# name -> rel-path sha256
fetch_one audio_eq.nvm           'common/nv_parameters/audio/audio_eq.nvm'                 '421D6DEBF8190BA2EB77861250E116ADBD4CFE3356E21D80A1A452DA0F2E1022' || true
fetch_one audio_dsp_codec_6530.nvm 'common/nv_parameters/audio/audio_dsp_codec_6530.nvm'    '68CB84AC993A68340C43BE3CB376C442DC1F7C37085C185636B2F1D98BD39E9E' || true
fetch_one aud_enha_exp.h          'MS_Ref/export/inc/aud_enha_exp.h'                       '8B5A59B778A7166C563E79190E3183C880E9A82D8B42A217D3F26A961090C936' || true
fetch_one eq_exp.h                'MS_Ref/export/inc/eq_exp.h'                             'E88888325B012DFC9A4E13560110641A600410ADD8AB4079BDA9F4DB624C729E' || true

if [[ $fail -ne 0 ]]; then
    echo "FETCH-REFERENCE: FAILED (see above)"
    exit 1
fi
echo "FETCH-REFERENCE: all 4 reference files present + hash-verified in $refDir"
exit 0
