#!/usr/bin/env bash
# Fetch full, pinned game source archives and apply the fpdoom patches.
set -euo pipefail

fail() { echo "error: $*" >&2; exit 1; }

Repo="$(cd "$(dirname "$0")/../.." && pwd)"
# The fpdoom clone lives in the repo's build/ tree - cloned here automatically
# if missing. Override with FPDOOM_DIR.
Fpdoom="${FPDOOM_DIR:-$Repo/build/fpdoom}"

source "$(dirname "$0")/fpdoom.sh"
ensure_fpdoom

# curl the given URL to the given .zip path (full download)
get_zip() {
    local url="$1" zip="$2"
    echo "download: $zip"
    "${B310E_CURL:-curl}" -fL -sS -o "$zip" "$url" || fail "download failed: $url"
    if [ ! -f "$zip" ] || [ "$(stat -c %s "$zip")" -lt 1000 ]; then
        fail "download failed: $url"
    fi
}

# After a direct patch run, verify the patch TRULY landed: GNU patch leaves
# .rej per failed hunk and .orig per touched file, so their absence proves a
# clean full application. Exit code 0 + rejected hunks can still happen, so
# scan the tree, not just the patch exit status.
assert_no_rejects() {
    local root="$1" name="$2"
    local list
    list="$(find "$root" -type f \( -name '*.rej' -o -name '*.orig' \) 2>/dev/null || true)"
    if [ -n "$list" ]; then
        local n
        n="$(printf '%s\n' "$list" | wc -l)"
        echo "patch FAILED ($name): $n rejected hunk(s)/backup file(s) left behind:"
        echo "$list"
        exit 1
    fi
}

# full-extract a zip whose single top dir is "<name>-<hash>" -> rename to $dest
# $sub: optional inner path to move instead (e.g. the DOOM zip's linuxdoom-1.10
# subdir -> doom_src, matching helper.make's `mv $name/linuxdoom-1.10`).
full_unzip() {
    local zip="$1" src_name="$2" dest="$3" sub="$4"
    echo "extract: $zip -> $dest"
    if [ -e "$dest" ]; then echo "  (already present)"; return 0; fi
    local tmp
    tmp="$(dirname "$zip")/_extract"
    rm -rf "$tmp"
    mkdir -p "$tmp"
    "${B310E_PYTHON:-python3}" -m zipfile -e "$zip" "$tmp" || fail "invalid source archive: $zip"
    local src="$tmp/$src_name"
    if [ ! -e "$src" ]; then
        # find the single top-level dir
        local top
        top="$(find "$tmp" -mindepth 1 -maxdepth 1 -type d -print -quit 2>/dev/null || true)"
        if [ -z "$top" ]; then fail "no top dir in $zip"; fi
        src="$top"
    fi
    if [ -n "$sub" ]; then src="$src/$sub"; fi
    if [ ! -e "$src" ]; then fail "source subpath not found in $zip: $sub"; fi
    mv "$src" "$dest"
    rm -rf "$tmp"
    echo "  -> $dest"
}

# Normalize text files (skip binaries - NUL bytes) from CRLF to LF, so the
# LF-based .patch applies cleanly. Same approach the rockbox port uses.
crlf_to_lf_tree() {
    "${B310E_PYTHON:-python3}" -c '
import sys
from pathlib import Path
for path in Path(sys.argv[1]).rglob("*"):
    if path.is_file():
        data = path.read_bytes()
        if b"\x00" not in data and b"\r\n" in data:
            path.write_bytes(data.replace(b"\r\n", b"\n"))
' "$1"
}

# jobs: dir|zip|url|src|dest|patch|sub
jobs=(
"chocolate-doom|chocolate-doom.zip|https://github.com/chocolate-doom/chocolate-doom/archive/0b3cb528c3f53c61d7a4ebe13a7d522570b98d83.zip|chocolate-doom-0b3cb528c3f53c61d7a4ebe13a7d522570b98d83|chocolate-doom|chocolate-doom.patch|"
"gnuboy|gnuboy.zip|https://github.com/rofl0r/gnuboy/archive/c367bb4ba96fb07cd62f72f5ecb43aeff7012564.zip|gnuboy-c367bb4ba96fb07cd62f72f5ecb43aeff7012564|gnuboy|gnuboy.patch|"
"fpdoom|DOOM.zip|https://github.com/id-Software/DOOM/archive/a77dfb96cb91780ca334d0d4cfd86957558007e0.zip|DOOM-a77dfb96cb91780ca334d0d4cfd86957558007e0|../doom_src|../doom.patch|linuxdoom-1.10"
"retris|retris.zip|https://github.com/ilyakurdyukov/retris/archive/b81fc06381fc648ed8cb491c2018c5dd009c20c3.zip|retris-b81fc06381fc648ed8cb491c2018c5dd009c20c3|retris|retris.patch|"
)

cr="$(printf '\r')"
for job in "${jobs[@]}"; do
    IFS='|' read -r dir zip url src dest patch sub <<< "$job"
    d="$Fpdoom/$dir"
    zipf="$d/$zip"
    destf="$d/$dest"
    marker="$d/.b310e-source-patched"
    if [ -f "$marker" ] && [ "$(cat "$marker")" = "$url" ] && [ -d "$destf" ]; then
        echo "skip $dir: verified patched source present"; continue
    fi
    mkdir -p "$d"
    if [ ! -d "$destf" ]; then
        get_zip "$url" "$zipf"
        full_unzip "$zipf" "$src" "$destf" "$sub"
    fi
    # apply the port patch DIRECTLY (patch -p1) - NOT via helper.make's `patch`
    # target: that target depends on `all`, which re-extracts with the partial
    # src/* globs and OVERWRITES the full tree we just unzipped (the very bug
    # this script exists to fix). chocolate-doom.patch etc. live in the port dir.
    if [ -n "$patch" ]; then
        pf="$d/$patch"
        if [ -f "$pf" ]; then
            echo "patch: $dir (direct)"
            # The extracted source may have CRLF line endings (InfoNES etc.)
            # while the .patch assumes LF - patch then fails every hunk with
            # "different line endings". Normalize TEXT files to LF first (skip
            # binaries - NUL bytes - which would be corrupted by a text read/
            # write round-trip).
            crlf_to_lf_tree "$destf"
            # drop any .rej files a previous failed patch attempt left behind
            find "$destf" -type f -name '*.rej' -delete 2>/dev/null || true
            # the .patch itself may have CRLF (checkout artifacts) while the
            # normalized tree is LF - GNU patch compares endings between the
            # patch and target, so normalize the patch file to LF as well.
            nuls="$(tr -cd '\000' < "$pf" | wc -c)"
            if [ "$nuls" -eq 0 ] && grep -q "${cr}$" "$pf"; then
                sed "s/${cr}$//" "$pf" > "$pf.lf.tmp" && mv "$pf.lf.tmp" "$pf"
            fi
            if git apply --check --ignore-space-change --unsafe-paths --directory="$destf" "$pf"; then
                git apply --ignore-space-change --unsafe-paths --directory="$destf" "$pf"
            elif git apply --reverse --check --ignore-space-change --unsafe-paths --directory="$destf" "$pf"; then
                echo "  patch already applied"
            else
                fail "source differs from the pinned patch: $dir"
            fi
            assert_no_rejects "$destf" "$dir"
        fi
    fi
    printf '%s' "$url" > "$marker"
done


echo "sources fetched and patches verified."
