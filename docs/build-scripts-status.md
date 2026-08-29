# Build-script status — handoff for the next session (2026-08-29, evening)

> **All Windows build scripts are fixed and verified green. Linux equivalents
> exist for every tool script and the Makefile runs both platforms.**
> This supersedes the earlier mid-debugging status (the gnuboy issue and the
> stale-flag issue documented there are RESOLVED — see below).

## What was done this session

### 1. Windows build suite — FULLY VERIFIED (all exit 0)

| Target / script | Result |
|---|---|
| `make` (os.bin) | PASS — 21402 B, `.rel` appended |
| `make os-sd` | PASS |
| `make hosttest` | PASS — 182 checks |
| `make check` | PASS — ELF entry 0x14000010 + .text 0x14000000 |
| `make debug` | PASS — 6 diag images + dsp-boot |
| `make sdcard` | PASS — full pipeline: fpmain.bin + 11 game bins (6 local + 5 prebuilt) + rockbox.bin (861296 B) + .rockbox/ (5.7M) + games/ dirs |
| tools/fpmain/build-fpmain.ps1 | PASS (fpmain.bin staged, clone restored) |
| tools/fpmain/fetch-sources.ps1 | PASS (idempotent skip; now verifies patch application) |
| tools/fpmain/rebuild-games.ps1 | PASS (6 local builds + 5 prebuilt fetched/extracted/staged) |
| tools/stockram/pack-stockram.ps1 | PASS — stock-ram.bin (1050624 B) |
| tools/stockram/diag-pack.ps1 | PASS — stock-ram-diag.bin |
| tools/stock-spy/pack-stock-spy.ps1 | PASS — stock-spy.bin (patches verified 0x6037/0xBDF8/0x6025) |
| tools/stock-spy/read-spy.ps1 | PASS (synthetic dump decoded) |
| tools/dsp/fetch-reference.ps1 | PASS (4 files hash-verified) |
| tools/ghidra-import.ps1 | prereqs OK (not run — takes ~1h) |
| tools/run-hw-test.ps1 / copy-to-card.ps1 | graceful-fail paths PASS (need hardware) |

The earlier status doc's two blockers are **resolved**:
- **gnuboy `sys_elapsed` conflict** — no longer fails. The port's real GCC-14
  issue is `-Wincompatible-pointer-types` (hard error in GCC 14), which
  `rebuild-games.ps1` already downgrades via its marker-guarded Makefile patch
  (`CFLAGS += -Wno-error=incompatible-pointer-types`). gnuboy built fine.
- **stale `-Wno-error=conflicting-types` flag** — gone; no current script adds
  it (that experiment was already reverted).

### 2. Hardening added

- `tools/fpmain/fetch-sources.ps1`: after every direct `patch -p1` (the 7
  port jobs + snes9x), the tree is scanned for `*.rej`/`*.orig` files and the
  script FAILS LOUDLY if any exist (catches exit-code-0-but-rejected-hunk
  cases). Both the loop patches and the snes9x patch are covered.

### 3. Makefile — now dual-platform

The Makefile auto-detects the platform (`ifeq ($(OS),Windows_NT)`):
- **Windows**: unchanged behavior (cmd shell, `where` probes, PowerShell
  recipes, winget fallbacks).
- **Linux**: `/bin/sh`, `command -v` probes, coreutils recipes
  (`mkdir -p`/`cp`/`cat >>`/`touch`/`rm -rf`/`unzip`/`dd`/`printf`/`date`),
  `check` uses `grep` instead of `findstr`, hosttest builds `build/hosttest`
  (no `.exe`), the tool scripts are invoked as `bash <script>.sh`.
- Verified on this machine by running make under git-bash with `OS` unset:
  `make all`, `make check`, `make debug` all green; the Linux `dd`+`sha256sum`
  dsp_seg0 extraction is byte-identical to the PowerShell version (a
  `count=134234` arithmetic bug was caught by that comparison and fixed to
  `133722` = 0x20A5A).
- Echo lines containing `(` are wrapped in a platform `MSG` shim (cmd prints
  raw; `/bin/sh` needs quoting).

### 4. Linux equivalents — ALL CREATED + TESTED (17 files)

| Linux script | Port of | Verification |
|---|---|---|
| tools/fpmain/build-fpmain.sh | build-fpmain.ps1 | bash -n + full run exit 0, clone restored |
| tools/fpmain/fetch-sources.sh | fetch-sources.ps1 | bash -n + skip-all exit 0 |
| tools/fpmain/rebuild-games.sh | rebuild-games.ps1 | bash -n + 7z-required error path exit 1; 4 game builds OK in git-bash (retris needs `cc` — present on real Linux) |
| tools/stockram/pack-stockram.sh | pack-stockram.ps1 | **byte-identical** to ps1 output (cmp + md5) |
| tools/stockram/diag-pack.sh | diag-pack.ps1 | **byte-identical** (cmp + md5) |
| tools/stock-spy/pack-stock-spy.sh | pack-stock-spy.ps1 | **byte-identical** (cmp + md5) |
| tools/stock-spy/read-spy.sh | read-spy.ps1 | decode output MATCHES ps1 (CRLF + content) |
| tools/run-hw-test.sh | run-hw-test.ps1 | bash -n + graceful-fail paths exit 1 |
| tools/ghidra-import.sh | ghidra-import.ps1 | bash -n + missing-GHIDRA_INSTALL_DIR error exit 1 |
| tools/copy-to-card.sh | copy-to-card.ps1 | bash -n + no-card error exit 1; happy path with CARD set → verified exit 0 |
| tools/dsp/fetch-reference.sh | fetch-reference.ps1 | bash -n + cached-verify exit 0 |
| tools/libc_server/run.sh | run.bat | bash -n only (waits on hardware) |
| tools/rockbox-port/build.sh | (already existed) | made platform-aware: Linux toolchain/PATH resolution, `unzip` instead of System32 tar, `zip` from distro not pacman |
| tools/qemu-b310e/scripts/run-b310e.sh | run-b310e.ps1 | bash -n + NOR-check graceful fail exit 1 |
| tools/qemu-b310e/scripts/install-machine.sh | install-machine.ps1 | bash -n + --whatif idempotent (md5 unchanged), real run idempotent, --uninstall, corrupt-marker refusal |
| tools/qemu-b310e/scripts/build-qemu.sh | build-qemu.ps1 | bash -n + invalid --qemu-src clean error, wrong-tag abort, v11.1.0 pin OK |
| tools/qemu-b310e/scripts/extract-audio-trace.sh | extract-audio-trace.ps1 | bash -n + synthetic log CSV **byte-identical to ps1**, MATCH/MISMATCH + exit codes |

Notes:
- All `.sh` files: `#!/usr/bin/env bash`, `set -euo pipefail`, ASCII-only,
  `$B310E_TOOLCHAIN` / `B310E_HOST_CC` overrides, no `C:\` paths, no
  PowerShell.
- The stockram/stock-spy byte-identity was verified by running the ps1 first,
  saving the outputs, then running the .sh and `cmp`/`md5sum` (all three
  images identical; the read-spy text output matches too).
- `BUILD.md` has a new "Building on Linux" section covering packages and the
  platform differences.
- On Windows, the `.sh` scripts can be exercised via git-bash login shell
  (`C:\Program Files\Git\bin\bash.exe -lc '...'`) — a bare `bash -c` from
  PowerShell lacks coreutils on PATH.

## What still needs a real Linux box

- `rebuild-games.sh` full pipeline (retris + the rest need `cc`, which
  git-bash lacks; p7zip must be installed).
- `build-qemu.sh` actual QEMU configure+build (only error paths tested here).
- `ghidra-import.sh` actual import (~1 h; needs GHIDRA_INSTALL_DIR + JDK 21+).
- Hardware-dependent paths: `run-hw-test.sh`, `copy-to-card.sh` with a real
  card, `run.sh`.
- `make sdcard` under the Linux branch end-to-end (rockbox + games on Linux).

## Open items (unchanged from AGENTS.md)

Nothing about the build scripts. The hardware items (30 s boot delay re-test,
USB replug, stock-ram LCD, DSP audio, scheduler v2) remain as listed in
AGENTS.md / learnings.md.
