/* B310E OS — input languages for the text editor: each one's multi-tap
 * letters on 2-9 (the usual phone layout for it, the accented letters after
 * the plain ones), the letters its symbol picker offers, and its letter case.
 *
 * Case goes by the language where it differs: in Turkish the capital of i is
 * İ and the small of I is ı; ß stays ß in capitals (it has no single capital
 * in normal use). The case tables cover what the fonts draw: Latin-1, Latin
 * Extended-A, the Romanian comma letters, Cyrillic U+0400-045F and Ґ ґ.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/db.h"
#include "os/lib.h"
#include "ui/textlang.h"

#define PUNCT    ".,?!'\"-()@/:_;+&%*=<>1"
#define PUNCT_ES ".,?¿!¡'\"-()@/:_;+&%*=<>1"

const input_lang_t input_langs[] = {
    { "en", "EN", "English",
      { NULL, NULL, "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9" },
      "àÀáÁâÂäÄåÅæÆçÇèÈéÉêÊëËíÍîÎïÏñÑóÓôÔöÖøØœŒúÚûÛüÜýÝÿŸ", false },
    { "tr", "TR", "Türkçe",
      { NULL, NULL, "abcç2", "def3", "ghıiğ4", "jkl5", "mnoö6", "pqrsş7", "tuvü8", "wxyz9" },
      "çÇğĞıIiİöÖşŞüÜâÂîÎûÛ", true },
    { "de", "DE", "Deutsch",
      { NULL, NULL, "abcä2", "def3", "ghi4", "jkl5", "mnoö6", "pqrsß7", "tuvü8", "wxyz9" },
      "äÄöÖüÜß", false },
    { "fr", "FR", "Français",
      { NULL, NULL, "abcàâæç2", "deféèêë3", "ghiîï4", "jkl5", "mnoôœ6", "pqrs7", "tuvùûü8",
        "wxyzÿ9" },
      "àÀâÂæÆçÇéÉèÈêÊëËîÎïÏôÔœŒùÙûÛüÜÿŸ", false },
    { "es", "ES", "Español",
      { NULL, PUNCT_ES, "abcá2", "defé3", "ghií4", "jkl5", "mnoñó6", "pqrs7", "tuvúü8", "wxyz9" },
      "áÁéÉíÍñÑóÓúÚüÜ", false },
    { "ru", "RU", "Русский",
      { NULL, NULL, "абвг2", "деёжз3", "ийкл4", "мноп5", "рсту6", "фхцч7", "шщъы8", "ьэюя9" },
      "ёЁъЪьЬ", false },
};
const int input_lang_count = (int)ARRAY_SIZE(input_langs);

/* the picker's rest: punctuation, quotes and dashes, money, maths, arrows */
const char input_symbols[] =
    ".,?!'\"-()@/:_;+&%*=<>#$[]{}\\~^`|¡¿«»‹›“”„‘’‚–—…•·§¶†‡°′″©®™№"
    "€$£¥₺₽₴₹₩¢₿¤"
    "±×÷−≠≈≤≥∞√∑∆πµ‰¹²³¼½¾←↑→↓";

const char *input_key(const input_lang_t *l, int digit)
{
    if (l->keys[digit])
        return l->keys[digit];
    return digit == 0 ? " 0" : PUNCT;
}

int input_lang_find(const char *code)
{
    for (int i = 0; i < input_lang_count; i++)
        if (!strcmp(input_langs[i].code, code))
            return i;
    return -1;
}

int input_langs_on(int *on)
{
    char list[64];
    int n = 0;

    db_get_str(INPUT_LANGS_KEY, list, sizeof(list), INPUT_LANGS_DEF);
    for (int i = 0; i < input_lang_count; i++) {
        const char *p = list;
        size_t k = strlen(input_langs[i].code);

        while (*p) {                    /* "en,tr": whole codes only */
            const char *end = strchr(p, ',');
            size_t len = end ? (size_t)(end - p) : strlen(p);

            if (len == k && !memcmp(p, input_langs[i].code, k)) {
                on[n++] = i;
                break;
            }
            p += len + (end ? 1 : 0);
        }
    }
    if (!n)
        on[n++] = 0;
    return n;
}

/* Latin Extended-A pairs capital, small: Ĺ..ň and Ź..ž start on an odd code,
 * the rest on an even one; ĸ ŉ ſ have no pair. */
static bool ext_a_odd_capital(uint32_t c)
{
    return (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e);
}

uint32_t text_upper(const input_lang_t *l, uint32_t c)
{
    if (c == 'i' && l && l->dotless_i)
        return 0x130;                   /* i: İ */
    if (c == 0x131)
        return 'I';                     /* ı: I */
    if (c >= 'a' && c <= 'z')
        return c - 32;
    if (c >= 0xe0 && c <= 0xfe && c != 0xf7)
        return c - 32;                  /* ß (0xdf) stays */
    if (c == 0xff)
        return 0x178;
    if (c >= 0x100 && c <= 0x17e && c != 0x130 && c != 0x138 && c != 0x149)
        return ext_a_odd_capital(c) ? (c & 1 ? c : c - 1) : (c & 1 ? c - 1 : c);
    if (c >= 0x218 && c <= 0x21b)
        return c & ~1u;
    if (c >= 0x430 && c <= 0x44f)
        return c - 32;
    if (c >= 0x450 && c <= 0x45f)
        return c - 80;
    if (c == 0x491)
        return 0x490;
    return c;
}

uint32_t text_lower(const input_lang_t *l, uint32_t c)
{
    if (c == 'I' && l && l->dotless_i)
        return 0x131;                   /* I: ı */
    if (c == 0x130)
        return 'i';                     /* İ: i */
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    if (c >= 0xc0 && c <= 0xde && c != 0xd7)
        return c + 32;
    if (c == 0x178)
        return 0xff;
    if (c >= 0x100 && c <= 0x17e && c != 0x131 && c != 0x138 && c != 0x149)
        return ext_a_odd_capital(c) ? (c & 1 ? c + 1 : c) : (c & 1 ? c : c + 1);
    if (c >= 0x218 && c <= 0x21b)
        return c | 1;
    if (c >= 0x410 && c <= 0x42f)
        return c + 32;
    if (c >= 0x400 && c <= 0x40f)
        return c + 80;
    if (c == 0x490)
        return 0x491;
    return c;
}

bool text_is_letter(uint32_t c)
{
    return ((c | 32) >= 'a' && (c | 32) <= 'z') ||
           (c >= 0xc0 && c <= 0x24f && c != 0xd7 && c != 0xf7) ||
           (c >= 0x370 && c <= 0x3ff) || (c >= 0x400 && c <= 0x52f);
}
