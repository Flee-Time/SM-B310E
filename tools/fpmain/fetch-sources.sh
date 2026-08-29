#!/usr/bin/env bash
# B310E-OS - tools/fpmain/fetch-sources.sh
#
# Downloads + patches the fpdoom game sources into the fpdoom clone.
# Unlike helper.make (whose `src/*` globs only extract top-level files and
# miss subdirs like src/doom, src/strife), this does a FULL unzip of every
# archive - that bug broke the chocolate-doom patch earlier.
#
# Linux port of tools/fpmain/fetch-sources.ps1. Uses plain curl/unzip/tar/
# patch from PATH (no Windows shims).
#
# Usage: tools/fpmain/fetch-sources.sh

set -euo pipefail

fail() { echo "error: $*" >&2; exit 1; }

Repo="$(cd "$(dirname "$0")/../.." && pwd)"
# The fpdoom clone lives in the repo's build/ tree - cloned here automatically
# if missing. Override with FPDOOM_DIR.
Fpdoom="${FPDOOM_DIR:-$Repo/build/fpdoom}"

if [ ! -d "$Fpdoom/.git" ]; then
    echo "cloning fpdoom -> $Fpdoom"
    mkdir -p "$(dirname "$Fpdoom")"
    git clone https://github.com/ilyakurdyukov/fpdoom "$Fpdoom" || fail "fpdoom clone failed"
fi
if [ ! -d "$Fpdoom" ]; then fail "fpdoom clone not found: $Fpdoom"; fi

# curl the given URL to the given .zip path (full download)
get_zip() {
    local url="$1" zip="$2"
    echo "download: $zip"
    curl -L -sS -o "$zip" "$url" || true
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
    ( cd "$tmp" && unzip -q "$zip" ) || true
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
    local root="$1"
    local cr
    cr="$(printf '\r')"
    local f nuls
    while IFS= read -r -d '' f; do
        nuls="$(tr -cd '\000' < "$f" | wc -c)"
        if [ "$nuls" -gt 0 ]; then continue; fi   # binary - leave alone
        if grep -q "${cr}$" "$f"; then
            sed "s/${cr}$//" "$f" > "$f.lf.tmp" && mv "$f.lf.tmp" "$f"
        fi
    done < <(find "$root" -type f -print0 2>/dev/null || true)
}

# jobs: dir|zip|url|src|dest|patch|sub
jobs=(
"chocolate-doom|chocolate-doom.zip|https://github.com/chocolate-doom/chocolate-doom/archive/0b3cb528c3f53c61d7a4ebe13a7d522570b98d83.zip|chocolate-doom-0b3cb528c3f53c61d7a4ebe13a7d522570b98d83|chocolate-doom|chocolate-doom.patch|"
"wolf3d|Wolf4SDL.zip|https://github.com/KS-Presto/Wolf4SDL/archive/dc8b250af35fb0ace68db5eb879490b50068c20e.zip|Wolf4SDL-dc8b250af35fb0ace68db5eb879490b50068c20e|Wolf4SDL|wolf3d.patch|"
"gnuboy|gnuboy.zip|https://github.com/rofl0r/gnuboy/archive/c367bb4ba96fb07cd62f72f5ecb43aeff7012564.zip|gnuboy-c367bb4ba96fb07cd62f72f5ecb43aeff7012564|gnuboy|gnuboy.patch|"
"infones|InfoNES.zip|https://github.com/jay-kumogata/InfoNES/archive/363bac8bbb030c3d3708ab32cd719ae6b2919971.zip|InfoNES-363bac8bbb030c3d3708ab32cd719ae6b2919971|InfoNES|InfoNES.patch|"
"fpdoom|DOOM.zip|https://github.com/id-Software/DOOM/archive/a77dfb96cb91780ca334d0d4cfd86957558007e0.zip|DOOM-a77dfb96cb91780ca334d0d4cfd86957558007e0|../doom_src|../doom.patch|linuxdoom-1.10"
"fpbuild|jfbuild.zip|https://github.com/jonof/jfbuild/archive/efd88d9cc24f753038a28479c9d8e7ac398909c8.zip|jfbuild-efd88d9cc24f753038a28479c9d8e7ac398909c8|jfbuild|jfbuild.patch|"
"retris|retris.zip|https://github.com/ilyakurdyukov/retris/archive/b81fc06381fc648ed8cb491c2018c5dd009c20c3.zip|retris-b81fc06381fc648ed8cb491c2018c5dd009c20c3|retris|retris.patch|"
)

cr="$(printf '\r')"
for job in "${jobs[@]}"; do
    IFS='|' read -r dir zip url src dest patch sub <<< "$job"
    d="$Fpdoom/$dir"
    zipf="$d/$zip"
    destf="$d/$dest"
    if [ -e "$destf" ]; then echo "skip $dir: $destf present"; continue; fi
    mkdir -p "$d"
    get_zip "$url" "$zipf"
    full_unzip "$zipf" "$src" "$destf" "$sub"
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
            patch -p1 -d "$destf" < "$pf" || fail "patch failed: $dir"
            assert_no_rejects "$destf" "$dir"
        fi
    fi
done

# snes9x: the Makefile/helper fetches a .tar.gz with a specific layout
snes="$Fpdoom/snes9x"
snes_dest="$snes/snes9x_src"
if [ ! -f "$snes_dest/tile.h" ]; then
    mkdir -p "$snes"
    tgz="$snes/snes9x_1.43.orig.tar.gz"
    echo "download: $tgz"
    curl -L -sS -o "$tgz" "https://old-releases.ubuntu.com/ubuntu/pool/multiverse/s/snes9x/snes9x_1.43.orig.tar.gz"
    mkdir -p "$snes_dest"
    (
        cd "$snes" || fail "cannot cd to $snes"
        tar -xzf "snes9x_1.43.orig.tar.gz" -C "snes9x_src" --strip-components 3 --wildcards "snes9x-1.43.orig/snes9x-1.43-src/snes9x/*"
        # snes9x.h does `#include "../language.h"` - language.h is a SIBLING of
        # the snes9x/ subdir in the tarball, so it must land in the port root
        # (snes9x/language.h), not in snes9x_src. Extract by exact path.
        mkdir -p "_tgzroot"
        tar -xzf "snes9x_1.43.orig.tar.gz" -C "_tgzroot" "snes9x-1.43.orig/snes9x-1.43-src/language.h"
        cp "_tgzroot/snes9x-1.43.orig/snes9x-1.43-src/language.h" "language.h"
        rm -rf "_tgzroot"
        # apply the snes9x port patch directly (snes9x.patch, patch -p1).
        # The Ubuntu .orig.tar.gz ships CRLF - normalize tree + patch to LF
        # first (the 7-job loop does the same; without it every hunk fails
        # with "different line endings").
        crlf_to_lf_tree "$snes_dest"
        nuls="$(tr -cd '\000' < "snes9x.patch" | wc -c)"
        if [ "$nuls" -eq 0 ] && grep -q "${cr}$" "snes9x.patch"; then
            sed "s/${cr}$//" "snes9x.patch" > "snes9x.patch.lf.tmp" && mv "snes9x.patch.lf.tmp" "snes9x.patch"
        fi
        patch -p1 -d "$snes_dest" < "snes9x.patch" || fail "patch failed: snes9x"
    )
    assert_no_rejects "$snes_dest" snes9x
    echo "  -> $snes_dest"
fi

echo "sources fetched."
