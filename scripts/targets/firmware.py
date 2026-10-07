#!/usr/bin/env python3
"""Native firmware and host checks. Called by scripts/build.py on every platform."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'firmware'
OUTPUT = ROOT / 'build/firmware'
BIN = ROOT / 'build/bin'
ARM = 'arm-none-eabi-'
CFLAGS = ['-Os', '-Wall', '-Wextra', '-funsigned-char', '-fno-PIE', '-ffreestanding',
          '-march=armv5te', '-mthumb', '-fomit-frame-pointer', '-ffunction-sections',
          '-fdata-sections', '-std=c11', '-pedantic']
INCLUDES = ['-I' + str(SOURCE / p) for p in ('include', 'kernel')] + ['-I' + str(OUTPUT)]
STARTUP = ['arch/' + name + '.s' for name in ('start', 'smc_init', 'ctx', 'cache', 'vectors', 'mmu')]


def run(command, capture=False):
    return subprocess.run([str(p) for p in command], cwd=ROOT, check=True,
                          text=True, stdout=subprocess.PIPE if capture else None).stdout


def require(program):
    if not shutil.which(program):
        raise RuntimeError(f'{program} missing; configure --toolchain or --host-cc at the main entry point')


def hosttest():
    require('gcc')
    directory = ROOT / 'build/host'
    directory.mkdir(parents=True, exist_ok=True)
    exe = directory / ('hosttest.exe' if os.name == 'nt' else 'hosttest')
    sources = [SOURCE / 'tests/hosttest.c', SOURCE / 'kernel/ctx_host.S', *sorted((SOURCE / 'kernel').glob('*.c'))]
    run(['gcc', '-std=c11', '-Wall', '-Wextra', '-DHOST_TEST', *INCLUDES, '-o', exe, *sources])
    run([exe])


def build(target, jobs, blob=None):
    for program in ('gcc', ARM + 'gcc', ARM + 'objcopy'):
        require(program)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    BIN.mkdir(parents=True, exist_ok=True)
    now = datetime.now()
    (OUTPUT / 'build_time.h').write_text(''.join(
        f'#define BUILD_TIME_{name} {value}\n' for name, value in zip(
            ('YEAR', 'MONTH', 'DAY', 'HOUR', 'MIN', 'SEC'),
            (now.year, now.month, now.day, now.hour, now.minute, now.second))), encoding='ascii')
    pack = OUTPUT / ('pack_reloc.exe' if os.name == 'nt' else 'pack_reloc')
    run(['gcc', '-O2', '-Wall', '-Wextra', '-std=c99', '-pedantic', '-Wno-unused',
         '-I' + str(ROOT / 'tools/pack_reloc/inc'), '-o', pack, ROOT / 'tools/pack_reloc/pack_reloc.c'])
    common = STARTUP + [str(p.relative_to(SOURCE)) for d in ('kernel', 'drivers') for p in sorted((SOURCE / d).glob('*.c'))]
    variants = [('os', 'arch/main.c', False, False)]
    if target == 'os-sd':
        variants = [('os-sd', 'arch/main.c', True, False)]
    elif target == 'debug':
        variants = [(f'os-diag-{name}', 'arch/' + entry, False, name == 'nor') for name, entry in (
            ('lcd', 'diag_lcd_main.c'), ('rot', 'diag_rot_main.c'), ('sd', 'diag_sd_main.c'),
            ('sdmmu', 'diag_sd_mmu_main.c'), ('nor', 'diag_nor_main.c'))]
    elif target == 'dsp-diag':
        if not blob:
            raise RuntimeError('dsp-diag needs --dsp-blob; ordinary firmware/card builds do not need stock data')
        data = Path(blob).read_bytes()
        if hashlib.sha256(data).hexdigest() != 'bc64236a0613d9838755bdc8e819811d07aad9c29742f55f9989e422d8ee5604':
            raise RuntimeError('DSP bundle hash differs from the verified reference')
        (OUTPUT / 'dsp_seg0.bin').write_bytes(data[0x28:0x28 + 0x20a5a])
        obj = OUTPUT / 'dsp_seg0.o'
        run([ARM + 'objcopy', '-I', 'binary', '-O', 'elf32-littlearm', '-B', 'arm',
             '--redefine-sym', '_binary_build_firmware_dsp_seg0_bin_start=_binary_build_dsp_seg0_bin_start',
             '--redefine-sym', '_binary_build_firmware_dsp_seg0_bin_end=_binary_build_dsp_seg0_bin_end',
             'build/firmware/dsp_seg0.bin', obj])
        variants = [('os-dsp-boot', 'arch/diag_dsp_main.c', True, False)]

    # A full compile avoids stale flags and missed header dependencies. Objects
    # are shared within this invocation by source+flags, never written in source.
    objects = {}
    link_sets = []
    for name, entry, sd_boot, nor in variants:
        sources = common + [entry]
        if entry == 'arch/main.c':
            sources += [str(p.relative_to(SOURCE)) for p in sorted((SOURCE / 'app').glob('*.c'))]
        if nor:
            sources += ['arch/menu_boot.s']
        linked = []
        for source in sources:
            defines = []
            if sd_boot and source in ('drivers/usb_debug.c', 'arch/diag_dsp_main.c'):
                defines += ['-DSD_BOOT_NO_USB']
            if source == 'arch/menu_boot.s':
                defines += ['-DMENU_NO_KEYCHECK']
            key = (source, tuple(defines))
            if key not in objects:
                suffix = '-sd' if defines == ['-DSD_BOOT_NO_USB'] else '-nor' if defines else ''
                obj = OUTPUT / Path(source).with_suffix('')
                obj = obj.with_name(obj.name + suffix + '.o')
                obj.parent.mkdir(parents=True, exist_ok=True)
                flags = ['-march=armv5te', '-x', 'assembler-with-cpp'] if source.endswith('.s') else CFLAGS + INCLUDES
                objects[key] = (obj, [ARM + 'gcc', *flags, *defines, '-c', '-o', obj, SOURCE / source])
            linked.append(objects[key][0])
        if target == 'dsp-diag':
            linked.append(OUTPUT / 'dsp_seg0.o')
        link_sets.append((name, linked, nor))
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(lambda item: run(item[1]), objects.values()))
    for name, linked, nor in link_sets:
        elf, binary = BIN / (name + '.elf'), BIN / (name + '.bin')
        script = SOURCE / ('link/menu.ld' if nor else 'link/os.ld')
        run([ARM + 'gcc', '-pie', '-nostartfiles', '-nodefaultlibs', '-nostdlib',
             '-Wl,-T,' + str(script), '-Wl,--gc-sections', '-Wl,-z,notext', '-o', elf, *linked])
        run([ARM + 'objcopy', '-O', 'binary', '-j', '.text', '-j', '.rodata', '-j', '.data', elf, binary])
        if name in ('os', 'os-sd', 'os-dsp-boot'):
            reloc = BIN / (name + '.rel')
            run([pack, elf, reloc])
            with binary.open('ab') as output:
                output.write(reloc.read_bytes())
        print(f'{binary.relative_to(ROOT)}: {binary.stat().st_size} bytes', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=('firmware', 'os-sd', 'debug', 'dsp-diag', 'hosttest', 'check', 'dis', 'size'))
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--dsp-blob', type=Path)
    args = parser.parse_args()
    try:
        if args.target in ('hosttest', 'check'):
            hosttest()
        if args.target != 'hosttest':
            build(args.target, args.jobs, args.dsp_blob)
        if args.target == 'check':
            header = run([ARM + 'readelf', '-h', BIN / 'os.elf'], capture=True)
            sections = run([ARM + 'readelf', '-S', BIN / 'os.elf'], capture=True)
            if not re.search(r'Entry point address:\s*0x14000010\b', header) or not re.search(r'\.text\s+PROGBITS\s+14000000\b', sections):
                raise RuntimeError('Firmware entry/text addresses differ from the boot contract')
            print('check: ALL PASS')
        elif args.target in ('dis', 'size'):
            run([ARM + ('objdump' if args.target == 'dis' else 'size'),
                 *(['-d'] if args.target == 'dis' else []), BIN / 'os.elf'])
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
