/* B310E OS shim: Rockbox rbunicode.h; ISO-8859-1 is the only code page
 * (os/media/codecs/rt.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef _RBUNICODE_H_
#define _RBUNICODE_H_
#include "platform.h"

typedef unsigned long ucschar_t;
enum codepages { ISO_8859_1 = 0, UTF_8 = 14, NUM_CODEPAGES, INIT_CODEPAGE = ISO_8859_1 };
#define BOM_UTF_8 "\xef\xbb\xbf"
#define BOM_UTF_8_SIZE 3
#define BOM_UTF_16_SIZE 2
#define MASK 0xC0
#define COMP 0x80

unsigned char *utf8encode(unsigned long ucs, unsigned char *utf8);
unsigned char *iso_decode(const unsigned char *latin1, unsigned char *utf8, int cp, int count);
unsigned char *iso_decode_ex(const unsigned char *iso, unsigned char *utf8, int cp, int count, int utf8_size);
unsigned char *utf16LEdecode(const unsigned char *utf16, unsigned char *utf8, int count);
unsigned char *utf16BEdecode(const unsigned char *utf16, unsigned char *utf8, int count);
unsigned char *utf16decode(const unsigned char *utf16, unsigned char *utf8, int count, int utf8_size, bool le);
bool utf16_has_bom(const unsigned char *utf16, bool *le);
unsigned long utf8length(const unsigned char *utf8);
const unsigned char *utf8decode(const unsigned char *utf8, ucschar_t *ucs);
int get_codepage(void);
#endif
