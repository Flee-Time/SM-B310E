#!/usr/bin/env python3
"""Prepare an isolated B310E game tree from the pinned, patched fpdoom cache."""
import argparse
import json
import math
import re
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
PIN = '04f19d6d54430693d00029970c51cd988c083b05'


def replace(path, old, new):
    text = path.read_text()
    if old not in text:
        raise RuntimeError(f'Pinned source no longer matches patch: {path.name}: {old[:60]}')
    path.write_text(text.replace(old, new))


def prepare(source, dest):
    head = subprocess.check_output(['git', '-c', f'safe.directory={source.as_posix()}',
                                    '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if head != PIN:
        raise RuntimeError(f'fpdoom must be at {PIN}, found {head}')
    expected = ROOT / 'build/game-ports'
    if dest != expected or dest.resolve() != expected or dest.is_symlink():
        raise RuntimeError('Refusing redirected or unexpected game source destination')
    if dest.exists() and not (dest / '.b310e-game-ports.json').is_file():
        raise RuntimeError('Refusing to replace an unrecognized game source directory')
    dest.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='game-prepare-', dir=dest.parent) as tmp:
        stage = Path(tmp) / 'game-ports'
        shutil.copytree(source, stage, ignore=shutil.ignore_patterns(
            '.git', 'obj*', '*.zip', '*.7z', '*.bin', '*.elf', '*.rel', '*.log', 'release', 'fpmain-b310e'))
        sys = stage / 'fpdoom'
        replace(sys / 'common.c',
                '\tchar *d = dst;\n\twhile (*d++);\n\tstrncpy(d, src, len);\n\treturn dst;',
                '\tchar *d = dst;\n\twhile (*d) d++;\n'
                '\twhile (len && *src) { *d++ = *src++; len--; }\n'
                '\t*d = 0;\n\treturn dst;')
        for file in (PORT / 'sys').glob('*'):
            if file.is_file():
                shutil.copy2(file, sys / file.name)
        codec = ROOT / 'ports/rockbox/firmware/target/arm/sc6530c'
        text = (codec / 'audiohw-sc6530c.c').read_text()
        first = text.index('#include "config.h"')
        last = text.index('#include "audio-target.h"') + len('#include "audio-target.h"')
        (sys / 'b310e-codec.c').write_text(text[:first] + '#include "b310e-audio.h"' + text[last:])
        with (sys / 'b310e-codec.c').open('a') as f:
            f.write('\nbool b310e_codec_ready(void) { return initialized; }\n')
        shutil.copy2(codec / 'audio-target.h', sys / 'b310e-registers.h')
        replace(sys / 'b310e-registers.h', 'void DMA(void);', '')
        gains = ','.join(str(round(32768 * math.pow(10, -db / 20))) for db in range(101))
        (sys / 'b310e-volume.h').write_text('static const unsigned b310e_volume_gain[101] = {' + gains + '};\n')
        notes = ','.join(str(round(440*2**((key-69)/12)*2**32/22050)) for key in range(128))
        sine = ','.join(str(round(math.sin(i*math.pi/128)*127)) for i in range(256))
        (sys / 'b310e-notes.h').write_text('static const unsigned b310e_note_step[128] = {' + notes + '};\n'
                                          'static const int8_t b310e_sine[256] = {' + sine + '};\n')
        fat = sys / 'microfat.c'
        replace(fat, 'fat_entry_t* fat_create_name(', 'static fat_entry_t* fat_create_name_sfn(')
        replace(fat, 'unsigned fat_make_dir(', '#include "b310e-lfn.h"\n\nunsigned fat_make_dir(')
        replace(fat, 'next = fat_alloc_clust(fatdata, clust, &start);\n\t\tif (start) goto create;',
                'buf = fat_read_chain(fatdata, clust);\n\t\tif (!buf) return NULL;\n'
                '\t\tunsigned added = ((uint32_t*)buf)[clust & 127] >= 0xffffff8;\n'
                '\t\tnext = fat_alloc_clust(fatdata, clust, &start);\n\t\tif (added) goto create;')
        replace(fat, 'fatdata->buf_pos = spos;\n\tFAT_LOG("fat: dir expand',
                'fatdata->buf_pos = spos;\n\tfatdata->flags |= FAT_FLUSH_BUF1;\n\tFAT_LOG("fat: dir expand')
        replace(fat, 'next = clust ? clust + 1 : 2;\n\tfor (;;) {',
                'next = clust ? clust + 1 : 2;\n\tunsigned scanned = 0;\n\tfor (;;) {')
        replace(fat, 'for (; next < end; next++)\n\t\t\tif (!((uint32_t*)buf)[next & 127]) goto found;',
                'for (; next < end; next++) {\n\t\t\tif (scanned++ >= fatdata->cnum) return FAT_CLUST_ERR;\n'
                '\t\t\tif (!((uint32_t*)buf)[next & 127]) goto found;\n\t\t}')
        replace(fat, 'buf = fat_read_sec(fatdata, fatdata->buf, spos);\n\t\t\tfor (idx',
                'buf = fat_read_sec(fatdata, fatdata->buf, spos);\n\t\t\tif (!buf) return NULL;\n\t\t\tfor (idx')
        replace(fat, 'fatdata->flags &= ~FAT_FLUSH_BUF1;\n\t\tfat_write_sec(fatdata, fatdata->buf, fatdata->buf_pos);',
                'if (fat_write_sec(fatdata, fatdata->buf, fatdata->buf_pos))\n'
                '\t\t\tfatdata->flags &= ~FAT_FLUSH_BUF1;')
        replace(fat, 'fatdata->flags &= ~FAT_FLUSH_BUF2;\n\t\tfat_write_sec(fatdata, fatdata->buf2, fatdata->buf2_pos + fatdata->fat1);\n\t\tif ((fat2 = fatdata->fat2))\n\t\t\tfat_write_sec(fatdata, fatdata->buf2, fatdata->buf2_pos + fat2);',
                'if (!fat_write_sec(fatdata, fatdata->buf2, fatdata->buf2_pos + fatdata->fat1)) return;\n'
                '\t\tif ((fat2 = fatdata->fat2))\n\t\t\tif (!fat_write_sec(fatdata, fatdata->buf2, fatdata->buf2_pos + fat2)) return;\n'
                '\t\tfatdata->flags &= ~FAT_FLUSH_BUF2;')
        replace(fat, 'fat_flush_buf2(fatdata);\n\t\t\tbuf = fatdata->buf2;',
                'fat_flush_buf2(fatdata);\n\t\t\tif (fatdata->flags & FAT_IO_ERROR) return NULL;\n\t\t\tbuf = fatdata->buf2;')
        header = sys / 'microfat.h'
        replace(header, '#define FAT_FLUSH_BUF2 0x20', '#define FAT_FLUSH_BUF2 0x20\n#define FAT_IO_ERROR 0x40')
        replace(header, '\treturn NULL;\n}\n#endif\n\n#ifdef FAT_READ_SYS',
                '\tfatdata->flags |= FAT_IO_ERROR;\n\treturn NULL;\n}\n#endif\n\n#ifdef FAT_READ_SYS')
        replace(header, 'fat_flush_buf1(fatdata);\n\t\t\tbuf = fatdata->buf;',
                'fat_flush_buf1(fatdata);\n\t\t\tif (fatdata->flags & FAT_IO_ERROR) return NULL;\n\t\t\tbuf = fatdata->buf;')
        replace(header, 'if (buf == fatdata->buf) fatdata->buf_pos = ~0; // discard cache',
                'fatdata->flags |= FAT_IO_ERROR;\n\tif (buf == fatdata->buf) fatdata->buf_pos = ~0; // discard cache')
        replace(header, 'if (buf == fatdata->buf) {',
                'if (fatdata->flags & FAT_IO_ERROR) return NULL;\n\tif (buf == fatdata->buf) {')
        file = sys / 'fatfile.c'
        replace(file, 'if (!(f->flags & 1)) return 0;', 'if (!f || !size || !count || !(f->flags & 1)) return 0;')
        replace(file, '// if (tmp <= pos) return 0;', 'if (tmp <= pos) return 0;')
        replace(file, 'if (!(f->flags & 2)) return 0;', 'if (!f || !size || !count || !(f->flags & 2)) return 0;')
        replace(file, 'fat_flush_buf1(fatdata);\n\t\t\t\t\tfatdata->buf_pos = spos;',
                'fat_flush_buf1(fatdata);\n\t\t\t\t\tif (fatdata->flags & FAT_IO_ERROR) goto end;\n\t\t\t\t\tfatdata->buf_pos = spos;')
        replace(file, "if (m0 == 'w') flags = 2;", "if (m0 == 'w' || m0 == 'a') flags = 2;")
        replace(file, 'if (tmp == \'+\') flags = 3;', "if (tmp == '+') flags = 3;\n\tif (m0 == 'a') flags |= 8;")
        replace(file, 'fatdata = &fatdata_glob;\n\tp = fat_find_path',
                'fatdata = &fatdata_glob;\n\tif (fatdata->flags & FAT_IO_ERROR) return NULL;\n\tp = fat_find_path')
        replace(file, 'p = fat_create_name(fatdata, fatdata->lastdir, name);\n\t\tif (!p) return NULL;',
                'p = fat_create_name(fatdata, fatdata->lastdir, name);\n'
                '\t\tif (!p) { printf("fs: cannot create %s dir=%u flags=%x\\n", name, fatdata->lastdir, fatdata->flags); return NULL; }')
        replace(file, "f->size = m0 == 'r' ? p->entry.size : 0;", "f->size = m0 == 'w' ? 0 : p->entry.size;")
        replace(file, 'f->pos = 0;', "f->pos = m0 == 'a' ? f->size : 0;")
        replace(file, 'pos = f->pos; tmp = 0xffffffff;',
                'if (f->flags & 8) f->pos = f->size;\n\tpos = f->pos; tmp = 0xffffffff;')
        replace(file, '\tfree(f);\n\treturn 0;',
                '\tfree(f);\n\tfat_flush_buf1(&fatdata_glob);\n\tfat_flush_buf2(&fatdata_glob);\n'
                '\treturn fatdata_glob.flags & FAT_IO_ERROR ? EOF : 0;')
        replace(file, '\tfat_flush_buf2(fatdata);\n#endif\n\treturn 0;',
                '\tfat_flush_buf2(fatdata);\n\tif (fatdata->flags & FAT_IO_ERROR) return EOF;\n#endif\n\treturn 0;')
        make = stage / 'build_sc6531.make'
        replace(make, 'SYS_SRCS += sdio microfat',
                'SYS_SRCS += sdio microfat b310e-fs b310e-audio b310e-codec b310e-irq b310e-mixer b310e-music')
        replace(make, '-DLIBC_SDIO=$(LIBC_SDIO) -DFAT_WRITE=1',
                '-DLIBC_SDIO=$(LIBC_SDIO) -DFAT_WRITE=1 -DB310E_GAMES=1')
        replace(make, '-fomit-frame-pointer', '-fomit-frame-pointer -Wno-error=incompatible-pointer-types')
        with make.open('a') as f:
            f.write('\n# ARM code avoids Thumb-1 software divides in the audio IRQ.\n'
                    '$(OBJDIR)/sys/b310e-mixer.o $(OBJDIR)/sys/b310e-music.o: CFLAGS += -marm -O2\n')
        replace(make, '$(OBJS2) -o $@', '$(OBJS2) -lm -lgcc -o $@')
        replace(make, '$(OBJS1) -o $@', '$(OBJS1) -lgcc -o $@')
        replace(make, '$(OBJS) -o $@', '$(OBJS) -lgcc -o $@')
        code = sys / 'syscode.c'
        replace(sys / 'entry.c', '#elif CHIP == 3\n\tinit_sc6530();',
                '#elif CHIP == 3\n'
                '\t/* The SD loader already put executable/data at these SMC mode-command addresses.\n'
                '\t * Preserve them when a host models mode cycles as ordinary RAM writes. */\n'
                '\tuint16_t mode_word = MEM2(ram_addr + 0x20);\n'
                '\tuint16_t mode_bank = MEM2(ram_addr + 0x10323e);\n'
                '\tinit_sc6530();\n'
                '\tMEM2(ram_addr + 0x20) = mode_word;\n'
                '\tMEM2(ram_addr + 0x10323e) = mode_bank;')
        replace(sys / 'syscode.h', '\tuint16_t keytrn[2][64];',
                '\tuint8_t b310e_keymap[64]; /* Copy before the stock NOR is unmapped. */\n\tuint16_t keytrn[2][64];')
        replace(code, '#include "syscode.h"', '#include "syscode.h"\n#include "b310e-audio.h"')
        replace(code, '\t\tkeypad_init();\n\t\tkeytrn_init();',
                '\t\tkeypad_init();\n\t\tsys_getkeymap(sys_data.b310e_keymap);\n\t\tkeytrn_init();')
        replace(code, 'int sys_event(int *rkey) {', 'int sys_event(int *rkey) {\n\tb310e_audio_poll();')
        replace(code, '\t\t\tkeytrn = sys_data.keytrn[0];',
                '\t\t\tunsigned physical = (k & 0x70) >> 1 | (k & 7);\n'
                '\t\t\tunsigned raw = sys_data.b310e_keymap[physical];\n'
                '\t\t\tstatic uint64_t volume_keys;\n'
                '\t\t\tuint64_t bit = (uint64_t)1 << physical;\n'
                '\t\t\tif ((i < 4 && (event >> 12) && (raw == 0x2a || raw == 0x23)) ||\n'
                '\t\t\t    (i >= 4 && (volume_keys & bit))) {\n'
                '\t\t\t\tif (i < 4) { volume_keys |= bit; b310e_audio_volume(raw == 0x23 ? 1 : -1); }\n'
                '\t\t\t\telse volume_keys &= ~bit;\n\t\t\t\tcontinue;\n\t\t\t}\n'
                '\t\t\tkeytrn = sys_data.keytrn[0];')
        replace(code, 'static void irq_handler(void) {',
                'static void irq_handler(void) {\n\tif (MEM4(0x80000000) & (1u << 20)) b310e_audio_irq();')
        replace(code, 'void sys_exit(void) {', 'void sys_exit(void) {\n\tb310e_audio_close();')
        replace(sys / 'include/stdio.h', 'void setbuf(FILE*, char*);',
                'void setbuf(FILE*, char*);\nint remove(const char*);\nint rename(const char*, const char*);')
        replace(sys / 'include/fcntl.h', '#define O_TRUNC 00001000', '#define O_TRUNC 00001000\n#define O_APPEND 00002000')
        replace(sys / 'include/sys/stat.h', '#define S_IFDIR 0040000', '#define S_IFDIR 0040000\n#define S_IFREG 0100000')
        # Preserve startup failures in bounded RAM instead of discarding NULL stdout/stderr.
        path=sys/'libc/printf.h'
        path.write_text('volatile char b310e_game_log[4096];\nvolatile unsigned b310e_game_log_count;\n'+path.read_text())
        replace(path, 'fputc(ch, x->file);',
                'if (x->file) fputc(ch, x->file);\n'
                '\telse b310e_game_log[b310e_game_log_count++ & 4095] = ch;')
        replace(sys/'libc/malloc.h', '\t\t\texit(-1);\n\t\t\treturn NULL;',
                '\t\t\treturn NULL; /* Standard fallible malloc; an unavailable sample must not reboot the game. */')
        libc = sys / 'libc.c'
        replace(libc, '\tstdout = _file_alloc(1, _IO_LINEBUF | _IO_WRITE | _IO_PIPE);\n\tstderr = _file_alloc(2, _IO_LINEBUF | _IO_WRITE | _IO_PIPE);',
                '\tstdout = stderr = NULL; /* SD-only builds retain console diagnostics in RAM. */')
        replace(libc, 'int putchar(int ch) {\n\treturn fputc(ch, stdout);',
                'int putchar(int ch) {\n\tif (!stdout) { b310e_game_log[b310e_game_log_count++ & 4095] = ch; return ch; }\n\treturn fputc(ch, stdout);')
        replace(libc, 'int puts(const char *str) {',
                'int puts(const char *str) {\n\tif (!stdout) return fprintf(NULL, "%s\\n", str);')
        replace(libc, 'int fputs(const char *str, FILE *f) {',
                'int fputs(const char *str, FILE *f) {\n\tif (!f) return fprintf(NULL, "%s", str);')
        replace(libc, '\t*argvp = argv;\n\tp = (char*)(argv + argc + 1);',
                '\tif (!argv) exit(252);\n\t*argvp = argv;\n\tp = (char*)(argv + argc + 1);')
        replace(stage / 'fpbuild/unistd.c', 'int *__errno(void) { return &sys_errno; }', '')
        replace(stage / 'fpbuild/unistd.c', 'return fseek(fd2file(fd), off, origin);',
                'FILE *f = fd2file(fd);\n\treturn fseek(f, off, origin) ? -1 : ftell(f);')
        unistd = stage / 'fpbuild/unistd.c'
        text = unistd.read_text(); start=text.index('int open('); end=text.index('\nint close(',start)
        unistd.write_text(text[:start]+(PORT/'build-open.c').read_text()+text[end:])
        replace(unistd, '#include <unistd.h>', '#include <unistd.h>\n#include "fatfile.h"\n#include <string.h>')
        start=unistd.read_text().index('int stat(');end=unistd.read_text().index('\nint open(',start)
        text=unistd.read_text()
        unistd.write_text(text[:start]+'''int stat(const char *name, struct stat *buf) {
    memset(buf,0,sizeof(*buf));
    fat_entry_t *entry=fat_find_path(&fatdata_glob,name);
    if(!entry)return -1;
    buf->st_size=entry->entry.size;
    buf->st_mode=(entry->entry.attr&FAT_ATTR_DIR?S_IFDIR:S_IFREG)|S_IRUSR|S_IWUSR;
    return 0;
}
''' + text[end:])
        # SDHCI must finish data transfers and card programming before gating its clock.
        sdio = sys / 'sdio.c'
        replace(sdio, 'sdio_shl = sd_ver == 1 ? resp[0] >> 30 & 0 : 9;',
                'sdio_shl = sd_ver && (resp[0] & (1u << 30)) ? 0 : 9;')
        replace(sdio, 'while (sdio->ctrl2 & 1 << 24);',
                'for (unsigned n = 1000000; (sdio->ctrl2 & 1 << 24) && n; --n);')
        replace(sdio, 'while (!(sdio->ctrl2 & 2)); // INT_CLK_STABLE',
                'for (unsigned n = 1000000; !(sdio->ctrl2 & 2) && n; --n); // INT_CLK_STABLE')
        replace(sdio, '\tdo cmd_int = sdio->int_st;\n\twhile (!(cmd_int & tmp));',
                '\tuint32_t started = sys_timer_ms();\n\tdo {\n\t\tcmd_int = sdio->int_st;\n'
                '\t\tif (sys_timer_ms() - started > 2000) {\n'
                '\t\t\tcmd_int |= SDIO_INT_ERR | SDIO_INT_DATA_TIMEOUT; break;\n\t\t}\n'
                '\t} while (!(cmd_int & tmp));\n'
                '\t/* DAT0 low means the card is still programming the written sector. */\n'
                '\tif (!(cmd_int & SDIO_INT_ERR) && data && !(cmd_tr & SDIO_CMD_DATA_READ))\n'
                '\t\twhile (!(sdio->state & (1u << 20)))\n'
                '\t\t\tif (sys_timer_ms() - started > 2000) {\n'
                '\t\t\t\tcmd_int |= SDIO_INT_ERR | SDIO_INT_DATA_TIMEOUT; break;\n\t\t\t}')
        replace(sdio, 'while (sdio->ctrl2 & 3 << 25);',
                'for (unsigned n = 1000000; sdio->ctrl2 & (3u << 25); --n)\n'
                '\t\tif (!n) { cmd_int |= SDIO_INT_ERR; break; }')
        for command in (17, 24):
            old = f'if (!(sd_int & SDIO_INT_CMD_COMPLETE)) {{\n\t\tDBG_LOG("sdio: CMD%u failed\\n", {command});'
            new = ('if ((sd_int & (SDIO_INT_CMD_COMPLETE | SDIO_INT_TR_COMPLETE | SDIO_INT_ERR)) !=\n'
                   '\t\t(SDIO_INT_CMD_COMPLETE | SDIO_INT_TR_COMPLETE)) {\n'
                   f'\t\tDBG_LOG("sdio: CMD%u failed\\n", {command});')
            replace(sdio, old, new)
        # Actual file operations replace the upstream embedded no-ops.
        misc = stage / 'chocolate-doom/chocolate-doom/src/m_misc.c'
        replace(misc, '#elif EMBEDDED\n    return 0;\n#else\n    return remove(path);',
                '#else\n    return remove(path);')
        replace(misc, '#elif EMBEDDED\n    return 0;\n#else\n    return rename(oldname, newname);',
                '#else\n    return rename(oldname, newname);')
        sound = (PORT / 'doom-sound.c').read_text()
        replace(stage / 'doom_src/s_sound.c', '#ifdef EMBEDDED\nvoid S_Init',
                '#if defined(EMBEDDED) && !B310E_GAMES\nvoid S_Init')
        replace(sys / 'scr_update.c', '\treturn cache_kb;\n}',
                '\t/* Keep heap outside the zone for display buffers, PCM and music. */\n'
                '\tif (cache_kb > 512) cache_kb -= 512;\n\treturn cache_kb;\n}')
        # The port-level backend is selected by NO_SOUND, but the engine must
        # still dispatch effects/music to it instead of compiling no-op S_*.
        for game in ('doom', 'heretic', 'hexen'):
            for path in (stage / f'chocolate-doom/chocolate-doom/src/{game}').glob('*.c'):
                text = path.read_text()
                text = text.replace('#if !NO_SOUND', '#if !NO_SOUND || B310E_GAMES')
                text = text.replace('#if NO_SOUND', '#if NO_SOUND && !B310E_GAMES')
                path.write_text(text)
        (stage / 'doom_src/i_sound.c').write_text('#define B310E_CLASSIC 1\n' + sound)
        with (sys / 'Makefile').open('a') as f:
            f.write('\nDOOM_CFLAGS += -I$(SYSDIR)\n')
        hacks = stage / 'chocolate-doom/port_hacks.c'
        text = hacks.read_text()
        start = text.index('#if NO_SOUND\n')
        end = text.index('\n#endif', start)+len('\n#endif')
        hacks.write_text(text[:start] + sound + text[end:])
        wolf = stage / 'wolf3d'
        replace(wolf / 'Wolf4SDL/wl_menu.c', 'file = fopen (savepath, "wb");',
                'file = fopen (savepath, "wb");\n'
                '            if (!file) { fprintf(stderr, "Cannot open save: %s\\n", savepath);\n'
                '                Message("Unable to open save file"); return 0; }')
        shutil.copy2(PORT / 'wolf-opl.cpp', wolf / 'b310e-opl.cpp')
        for ext in ('h','cpp'):
            text=(wolf / ('Wolf4SDL/dosbox/dbopl.'+ext)).read_text()
            text=text.replace('#include <SDL.h>', '#include <stdint.h>')
            text=text.replace('#include "../version.h"', '#define USE_GPL 1')
            text=text.replace('#pragma pack(1)', '')
            text=text.replace('#include "dbopl.h"', '#include "b310e-dbopl.h"')
            if ext=='cpp':
                text=text.replace('#include <math.h>', '#include <math.h>\nextern "C" double sin(double), pow(double,double);')
                first=text.index('\t//Generate the best matching attack rate')
                last=text.index('\tfor ( Bit8u i = 62; i < 76; i++ )',first)
                text=text[:first] + ('\t/* Fixed output rate: calibrate on the host, not for millions of\n'
                        '\t * simulated samples during phone startup. */\n'
                        '\t#include "b310e-opl-rates.h"\n'
                        '\tfor (unsigned i=0;i<62;i++) attackRates[i]=b310e_opl_attack_rates[i];\n') + text[last:]
            (wolf / ('b310e-dbopl.'+ext)).write_text(text)
        shutil.copy2(PORT / 'wolf-opl-rates.h', wolf / 'b310e-opl-rates.h')
        sound = (PORT / 'wolf-sound.c').read_text()
        path = wolf / 'Wolf4SDL/id_sd.c'
        text = path.read_text(); start=text.index('#if NO_SOUND'); end=text.index('\n#else',start)
        path.write_text(text[:start]+'#if NO_SOUND\n'+sound+text[end:])
        replace(wolf / 'Wolf4SDL/id_ca.c', '#if !NO_SOUND', '#if !NO_SOUND || B310E_GAMES')
        replace(wolf / 'Wolf4SDL/id_ca.c', '#include <sys/uio.h>', '')
        replace(wolf / 'Makefile', 'APP_OBJS2 = $(GAME_SRCS:%=$(OBJDIR)/game/%.o)',
                'APP_OBJS2 = $(GAME_SRCS:%=$(OBJDIR)/game/%.o) $(OBJDIR)/app/b310e-opl.o $(OBJDIR)/app/b310e-dbopl.o')
        with (wolf / 'Makefile').open('a') as f:
            f.write('\nSYS_CFLAGS += -DCXX_SUPPORT\nCXXFLAGS := $(CFLAGS) -std=c++14 -fno-exceptions -fno-rtti\n'
                    '$(OBJDIR)/app/%.o: %.cpp | objdir\n\t$(call compile_cxx,-I$(SYSDIR) -I.)\n'
                    'GAME_CFLAGS += -I$(SYSDIR)\n')
            f.write('$(OBJDIR)/app/b310e-opl.o $(OBJDIR)/app/b310e-dbopl.o: CXXFLAGS += -marm -O2\n')
        build = stage / 'fpbuild'
        shutil.copy2(PORT / 'build-sound.h', sys / 'b310e-build-sound.h')
        for game,adapter in (('jfduke3d','duke-sound.c'),('jfsw','sw-sound.c')):
            path=build / game / 'src/sounds.c'
            text=path.read_text(); start=text.index('#if NO_SOUND'); end=text.index('\n#else',start)
            path.write_text(text[:start]+'#if NO_SOUND\n'+(PORT/adapter).read_text()+text[end:])
        replace(build / 'jfsw/src/sounds.h', '#if NO_SOUND', '#if NO_SOUND && !B310E_GAMES')
        replace(build / 'jfsw/src/game.h', '#if NO_SOUND', '#if NO_SOUND && !B310E_GAMES')
        path=build / 'jfsw/src/sounds.c';text=path.read_text()
        tables=text[text.index('int PlayerPainVocs[]'):text.index('extern unsigned char lumplockbyte[];')]
        replace(path, '#undef DIGI_TABLE', '#undef DIGI_TABLE\n'+tables)
        replace(build / 'Makefile', '-I$(GAMESRC) -I$(ENGINEROOT)', '-I$(SYSDIR) -I$(GAMESRC) -I$(ENGINEROOT)')
        replace(build / 'fp_layer.c', 'cachesize += ram_size - (4 << 20);',
                'cachesize += ram_size - (4 << 20);\n\t\t/* Leave room for PCM, music and save buffers. */\n'
                '\t\tif (cachesize > (512u << 10)) cachesize -= 512u << 10;')
        replace(build / 'jfduke3d/src/game.c', '#if NO_SOUND\n    if (0)', '#if NO_SOUND && !B310E_GAMES\n    if (0)')
        # NO_SOUND selects our small backend instead of JFAudioLib. It must
        # not discard CON filenames or the reachable sound-options screen.
        replace(build / 'jfduke3d/src/gamedef.c', 'case 57:    //definesound\n#if NO_SOUND',
                'case 57:    //definesound\n#if NO_SOUND && !B310E_GAMES')
        sound_check = 'for(j=1;j<NUM_SOUNDS;j++)\n                if( SoundOwner[j][0].i == g_i )\n                    break;'
        replace(build / 'jfduke3d/src/gamedef.c', sound_check,
                '#if B310E_GAMES\n            j=issoundplaying(g_i,-1)?1:NUM_SOUNDS;\n'
                '#else\n            '+sound_check+'\n#endif')
        replace(build / 'jfduke3d/src/gamedef.c', '#if NO_SOUND\n            if(1) j = 0; else',
                '#if NO_SOUND && !B310E_GAMES\n            if(1) j = 0; else')
        replace(build / 'jfduke3d/src/menues.c', '#if NO_SOUND', '#if NO_SOUND && !B310E_GAMES')
        replace(build / 'jfduke3d/src/menues.c', '#if !NO_SOUND', '#if !NO_SOUND || B310E_GAMES')
        fx = build / 'jfaudiolib/include/fx_man.h'
        for old, new in (
            ('static inline void FX_SetVolume(int a) {}', 'void FX_SetVolume(int a);'),
            ('static inline void FX_SetReverseStereo(int a) {}', 'void FX_SetReverseStereo(int a);'),
            ('static inline int FX_StopAllSounds(void) { return 0; }', 'int FX_StopAllSounds(void);'),
        ):
            replace(fx, old, '#ifdef GAME_DUKE3D\n' + new + '\n#else\n' + old + '\n#endif')
        replace(build / 'jfsw/src/game.c', '#if !NO_SOUND\n    ASS_MessageOutputString', '#if !NO_SOUND || B310E_GAMES\n    ASS_MessageOutputString')
        replace(build / 'jfbuild/include/compat.h', '#if defined(__linux) || defined(__HAIKU__)',
                '#if B310E_GAMES\n# define B_LITTLE_ENDIAN 1\n# define B_BIG_ENDIAN 0\n# define B_ENDIAN_C_INLINE 1\n'
                '#elif defined(__linux) || defined(__HAIKU__)')
        path=build / 'jfbuild/src/compat.c'
        text=path.read_text(); start=text.index('typedef struct {\n#ifdef _MSC_VER\n\tHANDLE hfind;')
        end=text.index('\nchar *Bstrtoken(',start)
        text=text[:start]+(PORT/'build-dir.c').read_text()+text[end:]
        path.write_text(text.replace('# include <dirent.h>',''))
        blood=build / 'NBlood/source/blood/src'
        path=blood / 'sound.cpp';text=path.read_text();start=text.index('#if NO_SOUND');end=text.index('\n#else',start)
        path.write_text(text[:start]+'#if NO_SOUND\n'+(PORT/'blood-sound.cpp').read_text()+text[end:])
        replace(blood / 'compat.h', '#include "../jfbuild/include/compat.h"', '#include "../../../../jfbuild/include/compat.h"')
        replace(blood / 'blood.cpp', '#if !NO_SOUND\n    gSoundRes.Init', '#if !NO_SOUND || B310E_GAMES\n    gSoundRes.Init')
        replace(blood / 'compat.h', 'dawall, 0, tposx, tposy, &ang)', 'dawall, 0, (int*)tposx, (int*)tposy, &ang)')
        replace(blood / 'actor.cpp', 'nextsector, x, y, &cz, &fz)', 'nextsector, x, y, (int*)&cz, (int*)&fz)')
        replace(blood / 'blood.cpp', '#  include <sys/ioctl.h>', '')
        replace(blood / 'config.cpp', 'int CONFIG_FunctionNameToNum(', 'int32_t CONFIG_FunctionNameToNum(')
        replace(blood / 'blood.cpp', '                     GAME_clearbackground,\n                     BGetTime,\n                     GAME_onshowosd);',
                '                     [](int cols, int rows) { GAME_clearbackground(cols, rows); },\n'
                '                     []() -> int { return BGetTime(); },\n'
                '                     [](int shown) { GAME_onshowosd(shown); });')
        # JFBuild exposes int pointers and callbacks. Modern ARM newlib spells
        # int32_t as long; use the engine's 32-bit int ABI throughout NBlood.
        for path in blood.iterdir():
            if path.suffix in ('.h', '.cpp', '.c'):
                text = re.sub(r'\buint32_t\b', 'unsigned int', path.read_text())
                path.write_text(re.sub(r'\bint32_t\b', 'int', text))
        with (blood / 'compat.h').open('a') as f:
            f.write('\n#ifdef __cplusplus\nstatic_assert(sizeof(int)==4, "Build requires 32-bit int");\n#endif\n')
        for header in ('fx_man.h','music.h'):
            (sys/'include'/header).write_text('/* Legacy declarations unused by the embedded audio adapter. */\n')
        retris = stage / 'retris/retris/retris.c'
        replace(retris, '#include "../fpgfx.h"', '''#include "../fpgfx.h"
#include "b310e-mixer.h"
static void b310e_retris_click(unsigned lines) {
    static uint8_t pcm[2205]; static bool ready;
    if (!ready) { b310e_mixer_init(); ready = true; }
    unsigned period = lines ? 25 : 50;
    for (unsigned i=0; i<sizeof(pcm); i++) {
        int amplitude = 32 * (sizeof(pcm)-i) / sizeof(pcm);
        pcm[i] = 128 + (i % period < period/2 ? amplitude : -amplitude);
    }
    b310e_sample_play(0, pcm, sizeof(pcm), 22050, 180, 128, 128);
}
''')
        replace(retris, '\t\t\tgame_comp(T);',
                '\t\t\tgame_comp(T);\n#ifdef EMBEDDED\n\t\t\tb310e_retris_click(T->ncomp);\n#endif')
        # Game Boy outputs unsigned 8-bit stereo PCM.
        gb = stage / 'gnuboy/port_hacks.c'
        replace(gb, '#include "syscode.h"', '#include "syscode.h"\n#include "b310e-audio.h"')
        replace(gb, '\tpcm.hz = 11025;', '\tpcm.hz = 22050;\n\tpcm.stereo = 1;\n\tb310e_audio_init(pcm.hz);')
        replace(gb, 'void pcm_close() {', 'void pcm_close() {\n\tb310e_audio_close();')
        replace(gb, 'int pcm_submit() {', 'int pcm_submit() {\n\tb310e_audio_u8(pcm.buf, pcm.pos, pcm.stereo);')
        # SNES software SPC700 must run even when the ARM output is muted.
        replace(stage / 'snes9x/Makefile', '-DNOSOUND -DNDEBUG', '-DNDEBUG')
        replace(stage / 'snes9x/snes9x_src/memmap.cpp', '#include "unzip.h"',
                '#ifdef UNZIP_SUPPORT\n#include "unzip.h"\n#endif')
        snes = stage / 'snes9x/extra.cpp'
        replace(snes, '#include "display.h"', '#include "display.h"\nextern "C" { volatile unsigned b310e_snes_stage; }')
        replace(snes, '#include "display.h"', '#include "display.h"\n#include "../fpdoom/b310e-audio.h"')
        (stage / 'snes9x/b310e_apu.cpp').write_text(
                '#include "snes9x.h"\n#include "apu.h"\n#include "spc700.h"\n#include "cpuexec.h"\n'
                'static_assert(__builtin_offsetof(SIAPU,APUExecuting)==12,"SPC assembly ABI");\n'
                'static_assert(__builtin_offsetof(SAPU,Cycles)==0,"SPC assembly ABI");\n'
                'extern "C" void b310e_snes_apu_execute(void) { APU_EXECUTE(); }\n')
        replace(stage / 'snes9x/Makefile', 'APP_OBJS2 = $(SNES_SRCS:%=$(OBJDIR)/snes/%.o)',
                'APP_OBJS2 = $(SNES_SRCS:%=$(OBJDIR)/snes/%.o) $(OBJDIR)/app/b310e_apu.o')
        replace(stage / 'snes9x/snesasm.s', '15:\tldr\tr0, [r4, #12]',
                '15:\tldr\tr0, =IAPU\n\tldrb\tr0, [r0, #12]\n'
                '\tcmp\tr0, #0\n\tbeq\t18f\n'
                '\tldr\tr0, =APU\n\tldr\tr0, [r0]\n\tldr\tr1, [r4, #32]\n'
                '\tcmp\tr0, r1\n\tblle\tb310e_snes_apu_execute\n'
                '18:\tldr\tr0, [r4, #12]')
        replace(snes, 'if (!render && skipped < 1)', 'if (!render && skipped < 5)')
        replace(snes, 'Settings.Stereo = FALSE;', 'Settings.Stereo = TRUE;\n\tSettings.SixteenBitSound = TRUE;\n\tSettings.SoundSync = 1;')
        replace(snes, 'Settings.APUEnabled = Settings.NextAPUEnabled = FALSE;',
                'Settings.APUEnabled = Settings.NextAPUEnabled = TRUE;')
        replace(snes, 'Settings.Mute = TRUE;', 'Settings.Mute = FALSE;')
        replace(snes, 'Settings.DisableMasterVolume = TRUE;', 'Settings.DisableMasterVolume = FALSE;')
        replace(snes, 'Memory.LoadSRAM(S9xGetFilename(".srm"));',
                'Memory.LoadSRAM(S9xGetFilename(".srm"));\n\t\tS9xSetSoundMute(FALSE);')
        replace(snes, 'if (!Memory.Init() || !S9xInitAPU()) return 0;',
                'b310e_snes_stage=1; if (!Memory.Init()) return 0;\n'
                '\tb310e_snes_stage=2; if (!S9xInitAPU()) return 0;\n\tb310e_snes_stage=3;')
        replace(snes, 'if (!S9xGraphicsInit()) return 0;',
                'b310e_snes_stage=4; if (!S9xGraphicsInit()) return 0;\n\tb310e_snes_stage=5;')
        replace(snes, 'if (0) S9xInitSound', 'if (!S9xInitSound')
        replace(snes, 'Settings.Stereo, Settings.SoundBufferSize);\n\tS9xSetSoundMute(TRUE);',
                'Settings.Stereo, Settings.SoundBufferSize)) return 0;\n\tS9xSetSoundMute(FALSE);')
        replace(snes, 'bool8 S9xOpenSoundDevice(int mode, bool8 stereo, int buffer_size) { return 0; }\nvoid S9xGenerateSound() {}',
                'bool8 S9xOpenSoundDevice(int mode, bool8 stereo, int buffer_size) {\n'
                '\t(void)mode; (void)stereo; (void)buffer_size;\n'
                '\tso.stereo = TRUE; so.sixteen_bit = TRUE;\n'
                '\tS9xSetPlaybackRate(22050); b310e_audio_init(22050); return TRUE;\n}\n'
                'void S9xGenerateSound() {\n'
                '\tstatic int16_t samples[512]; static unsigned used;\n'
                '\tso.err_counter += so.err_rate;\n'
                '\tunsigned frames = so.err_counter >> 16;\n'
                '\tso.err_counter &= 65535;\n'
                '\twhile (frames--) { S9xMixSamples((uint8*)(samples+used*2), 2);\n'
                '\t\tif (++used == 256) { b310e_audio_submit(samples, used); used = 0; }\n\t}\n}')
        replace(snes, 'void S9xGenerateSound() {\n\tstatic int16_t samples[512]; static unsigned used;',
                'static unsigned b310e_pending_samples;\n'
                'extern "C" void b310e_snes_flush_sound() {\n'
                '\tstatic int16_t samples[512]; static unsigned used;\n'
                '\twhile (b310e_pending_samples) {\n'
                '\t\tunsigned n=MIN(b310e_pending_samples,256-used);\n'
                '\t\tS9xMixSamples((uint8*)(samples+used*2),n*2);\n'
                '\t\tused+=n; b310e_pending_samples-=n;\n'
                '\t\tif (used==256) {b310e_audio_submit(samples,used);used=0;}\n'
                '\t}\n}\nvoid S9xGenerateSound() {')
        replace(snes, '\twhile (frames--) { S9xMixSamples((uint8*)(samples+used*2), 2);\n'
                '\t\tif (++used == 256) { b310e_audio_submit(samples, used); used = 0; }\n\t}',
                '\tb310e_pending_samples+=frames;\n'
                '\tif (b310e_pending_samples>=64) b310e_snes_flush_sound();')
        apu = stage / 'snes9x/snes9x_src/apu.cpp'
        replace(apu, 'void S9xSetAPUDSP (uint8 byte)\n{',
                'extern "C" void b310e_snes_flush_sound();\n'
                'void S9xSetAPUDSP (uint8 byte)\n{\n\tb310e_snes_flush_sound();')
        replace(apu, 'uint8 S9xGetAPUDSP ()\n{',
                'uint8 S9xGetAPUDSP ()\n{\n\tb310e_snes_flush_sound();')
        # Keep the slow scanline mixer setup out of the per-sample loop.
        # Flush before DSP reads/writes, preserving register and ENDX timing.
        replace(stage / 'snes9x/snes9x_fp.c', '#include <stdio.h>', '#include <stdio.h>\n#include <time.h>')
        replace(stage / 'snes9x/snes9x_fp.c', 'uint32_t res2 = 0x180000, res0;',
                'uint32_t res2 = 0x100000, res0; /* Keep room for the SPC700 and 16-bit renderer on 4 MiB phones. */')
        replace(sys / 'include/time.h', 'typedef long time_t;', '#include <sys/types.h>')
        # InfoNES mixes its five generated unsigned waveforms into stereo.
        replace(stage / 'infones/Makefile', '-DAPU_Mute=1', '-DAPU_Mute=0')
        papu = stage / 'infones/InfoNES/src/InfoNES_pAPU.c'
        # The upstream per-frame counters were moved into sample loops without
        # changing their units. Scale them by the actual buffer length.
        for channel in (1,2,4):
            replace(papu, f'ApuC{channel}EnvPhase += ApuC{channel}EnvDelay;',
                    f'ApuC{channel}EnvPhase += ApuC{channel}EnvDelay * ApuSamplesPerSync;')
        for channel in (1,2):
            replace(papu, f'ApuC{channel}SweepPhase += ApuC{channel}SweepDelay;',
                    f'ApuC{channel}SweepPhase += ApuC{channel}SweepDelay * ApuSamplesPerSync;')
            replace(papu, f'ApuC{channel}SweepPhase -= 2;', f'int changed=0; ApuC{channel}SweepPhase -= 2;')
            replace(papu, f'ApuC{channel}SweepPhase += ApuC{channel}SweepDelay * ApuSamplesPerSync;',
                    f'changed=1; ApuC{channel}SweepPhase += ApuC{channel}SweepDelay * ApuSamplesPerSync;')
            replace(papu, f'if ( ApuC{channel}Freq ) {{', f'if ( changed && ApuC{channel}Freq ) {{')
            replace(papu, f'/ ApuC{channel}Freq;', f'/ (ApuC{channel}Freq + 1);')
            replace(papu, f'if ( ApuC{channel}Env )', f'if ( !ApuC{channel}Env )')
            replace(papu, f'( ApuC{channel}Vol + ApuC{channel}EnvVol )', f'( 15 - ApuC{channel}EnvVol )')
            replace(papu, f'if ( ApuC{channel}Atl ) {{ ApuC{channel}Atl--;  }}',
                    f'if ( ApuC{channel}Atl && !ApuC{channel}Hold ) {{ ApuC{channel}Atl--; }}')
            # Keep processing later register events when a channel is muted.
            replace(papu, f'wave_buffers[{channel-1}][i] = 0;\n      break;',
                    f'wave_buffers[{channel-1}][i] = 0;\n      continue;')
            # Timer-high writes restart the envelope.
            marker=f'case 3:\n\t  ApuC{channel}d = ApuEventQueue[event].data;'
            replace(papu, marker, marker + f'\n\t  ApuC{channel}EnvVol = 0;\n'
                    f'\t  ApuC{channel}EnvPhase = ApuC{channel}EnvDelay * ApuSamplesPerSync;')
        replace(papu, 'ApuC2Freq -= ~( ApuC2Freq >> ApuC2SweepShifts );',
                'ApuC2Freq -= ( ApuC2Freq >> ApuC2SweepShifts );')
        nes = stage / 'infones/InfoNES_System.c'
        replace(nes, 'void InfoNES_SoundInit(void) {}',
                '#include "b310e-audio.h"\nvoid InfoNES_SoundInit(void) {}')
        replace(nes, '(void)samples_per_sync; (void)sample_rate;',
                '(void)samples_per_sync; b310e_audio_init(sample_rate);')
        replace(nes, 'void InfoNES_SoundClose(void) {}',
                'void InfoNES_SoundClose(void) { b310e_audio_close(); }')
        replace(nes, '(void)samples; (void)wave1; (void)wave2; (void)wave3; (void)wave4; (void)wave5;',
                'int16_t out[320]; static int dc;\n'
                '\tfor (int pos=0; pos<samples;) {\n'
                '\t\tint n=MIN(160,samples-pos);\n'
                '\t\tfor (int i=0; i<n; i++,pos++) {\n'
                '\t\t\tint value=(wave1[pos]+wave2[pos]+wave3[pos]+wave4[pos]+wave5[pos])*64;\n'
                '\t\t\tdc += (value-dc)/256; value -= dc;\n'
                '\t\t\tout[i*2]=out[i*2+1]=MIN(32767,MAX(-32768,value));\n'
                '\t\t} b310e_audio_submit(out,n);\n\t}')
        (stage / '.b310e-game-ports.json').write_text(json.dumps({'upstream': PIN, 'format': 1}))
        if dest.exists():
            dest.rename(Path(tmp) / 'previous')
        stage.rename(dest)
    print(f'Prepared B310E game sources: {dest}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    args = parser.parse_args()
    prepare(args.source.resolve(), ROOT / 'build/game-ports')
