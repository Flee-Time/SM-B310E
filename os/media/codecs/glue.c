/* B310E OS — codec module glue (glue.h): Rockbox's codec_crt0 for our
 * loader, and the probe that runs the format's metadata parser.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <setjmp.h>
#include "glue.h"

struct codec_api *ci;
struct mp3entry glue_id3;

/* The loader zeroed bss and wrote the image through the D-cache, which
 * it cleaned; Rockbox's crt0 has nothing else to do without IRAM. */
enum codec_status codec_start(enum codec_entry_call_reason reason)
{
    return codec_main(reason);
}

/* Each module links its own copy of this file: the buffer is the module's.
 * Armed only while the engine is inside the codec, so a panicf from the
 * probe's parser (no jump set) still just returns. setjmp.S keeps r4-r11,
 * sp and lr, all an ARM-built codec has to keep (no VFP). */
static jmp_buf glue_jmp;
static bool glue_armed;

enum codec_status glue_start(enum codec_entry_call_reason reason)
{
    enum codec_status st;

    if (setjmp(glue_jmp)) {
        glue_armed = false;
        return CODEC_ERROR;
    }
    glue_armed = true;
    st = codec_start(reason);
    glue_armed = false;
    return st;
}

enum codec_status glue_run(void)
{
    enum codec_status st;

    if (setjmp(glue_jmp)) {
        glue_armed = false;
        return CODEC_ERROR;
    }
    glue_armed = true;
    st = codec_run();
    glue_armed = false;
    return st;
}

/* rt.c's panicf, after the engine has its flag: out of the codec. Its
 * memory is the arena, which the engine frees with the module; it holds
 * no lock of the engine's (it is in its own code when it panics). */
void glue_panic(void)
{
    if (glue_armed)
        longjmp(glue_jmp, 1);
}

static const char *label(unsigned type)
{
    switch (type) {
    case AFMT_MPA_L1: return "MP1";
    case AFMT_MPA_L2: return "MP2";
    case AFMT_MPA_L3: return "MP3";
    case AFMT_PCM_WAV: return "WAV";
    case AFMT_OGG_VORBIS: return "Vorbis";
    case AFMT_FLAC: return "FLAC";
    case AFMT_WAVPACK: return "WavPack";
    case AFMT_MP4_ALAC: return "ALAC";
    case AFMT_MP4_AAC: return "AAC";
    case AFMT_MP4_AAC_HE: return "HE-AAC";
    case AFMT_OPUS: return "Opus";
    case AFMT_AIFF: return "AIFF";
    default: return "?";
    }
}

static void copy(char *dst, size_t n, const char *src)
{
    strlcpy(dst, src ? src : "", n);
}

int glue_probe(struct codec_api *api, int fd, codec_track_t *out,
               const int *formats, glue_parser_t parse)
{
    struct mp3entry *id3 = &glue_id3;
    bool ok = false;

    ci = api;
    api->id3 = id3;
    memset(id3, 0, sizeof(*id3));
    id3->codectype = (unsigned)formats[0];  /* as Rockbox's get_metadata_afmt */
    id3->tracknum = -1;
    lseek(fd, 0, SEEK_SET);
    if (!parse(fd, id3))
        return -1;
    lseek(fd, 0, SEEK_SET);
    for (const int *f = formats; *f; f++)
        ok |= id3->codectype == (unsigned)*f;
    if (!ok)
        return -1;                      /* another module's format */
    id3->elapsed = id3->offset = 0;
    copy(out->title, sizeof(out->title), id3->title);
    copy(out->artist, sizeof(out->artist), id3->artist ? id3->artist : id3->albumartist);
    copy(out->album, sizeof(out->album), id3->album);
    copy(out->codec, sizeof(out->codec), label(id3->codectype));
    out->length_ms = id3->length;
    out->frequency = id3->frequency;
    out->bitrate = id3->bitrate;
    out->codectype = id3->codectype;
    if (id3->has_embedded_albumart && id3->albumart.size > 0) {
        unsigned ty = (unsigned)id3->albumart.type;

        /* AA_TYPE_BMP 1, PNG 2, JPG 3 = MEDIA_ART_*; the flags above them */
        out->art_type = ty & 15u;
        out->art_flags = (ty & AA_FLAG_ID3_UNSYNC ? 1u : 0u) |
                         (ty & AA_FLAG_VORBIS_BASE64 ? 2u : 0u);
        out->art_pos = (uint32_t)id3->albumart.pos;
        out->art_size = (uint32_t)id3->albumart.size;
    }
    return 0;
}

/* Rockbox resumes a track from id3->elapsed when offset is 0 */
void glue_start_at(unsigned long ms)
{
    glue_id3.offset = 0;
    glue_id3.elapsed = ms;
}
