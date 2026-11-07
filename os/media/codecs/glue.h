/* B310E OS — codec module glue: the module header (os/codec.h) around a
 * Rockbox decoder and its metadata parser. One small file per module in
 * os/media/codecs/ says what it is:
 *
 *     #include "glue.h"
 *     static const int formats[] = { AFMT_MPA_L3, AFMT_MPA_L2, AFMT_MPA_L1, 0 };
 *     CODEC_MODULE("mpa", "mp3 mp2 mp1 mpa", "MPEG audio (libmad)",
 *                  64 * 1024, formats, get_mp3_metadata);
 *
 * formats[0] is the format Rockbox's parser starts from (it guesses by
 * extension the same way); a track whose parsed format is not in the list
 * is refused, so the engine tries the next module for that extension
 * (.m4a: AAC, then ALAC; .ogg: Vorbis, then Opus). The arena is what
 * codec_get_buffer() hands to codeclib's codec_malloc().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef B310E_CODEC_GLUE_H
#define B310E_CODEC_GLUE_H

#include "codecs.h"
#include "metadata_parsers.h"

typedef bool (*glue_parser_t)(int fd, struct mp3entry *id3);

extern struct mp3entry glue_id3;
int glue_probe(struct codec_api *api, int fd, codec_track_t *out,
               const int *formats, glue_parser_t parse);
void glue_start_at(unsigned long ms);

#define CODEC_MODULE(name_, exts_, desc_, arena_, formats_, parser_)        \
    static int module_probe_(struct codec_api *api, int fd,                 \
                             codec_track_t *out)                            \
    {                                                                       \
        return glue_probe(api, fd, out, formats_, parser_);                 \
    }                                                                       \
    const codec_module_t codec_module                                       \
        __attribute__((section(".codec_module"), used)) = {                 \
        CODEC_ABI_MAJOR, CODEC_ABI_MINOR, name_, exts_, desc_, arena_,      \
        module_probe_, glue_start_at, codec_start, codec_run }

#endif
