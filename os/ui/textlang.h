/* B310E OS — the text editor's input languages, letter case and word
 * completion (ui/textlang.c, ui/words.c). Inside the OS only; apps reach
 * them through ui_text_edit.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef UI_TEXTLANG_H
#define UI_TEXTLANG_H

#include <stdbool.h>
#include <stdint.h>

/* An input language: adding one is adding an entry to input_langs[]. */
typedef struct {
    const char *code;           /* "tr": in ui.input_langs, and the word list's name */
    const char *tag;            /* "TR": shown in the editor */
    const char *name;           /* shown in Settings */
    const char *keys[10];       /* multi-tap letters of 0-9, small, UTF-8; NULL: the
                                 * shared ones (0: space, 1: punctuation) */
    const char *extra;          /* letters the symbol picker offers first */
    bool dotless_i;             /* Turkish and Azeri: i/İ and ı/I */
} input_lang_t;

extern const input_lang_t input_langs[];
extern const int input_lang_count;

#define INPUT_LANG_KEY   "ui.input_lang"     /* str: the code of the current one */
#define INPUT_LANGS_KEY  "ui.input_langs"    /* str: the codes switched on, "en,tr" */
#define INPUT_LANGS_DEF  "en,tr"

int input_lang_find(const char *code);      /* its index, -1 none */
/* The switched-on languages' indexes into on (at most input_lang_count), in
 * input_langs[] order; never none (English then). How many. */
int input_langs_on(int *on);
const char *input_key(const input_lang_t *l, int digit);
extern const char input_symbols[];          /* the picker's punctuation, money, maths */

uint32_t text_upper(const input_lang_t *l, uint32_t c);
uint32_t text_lower(const input_lang_t *l, uint32_t c);
bool text_is_letter(uint32_t c);            /* part of a word, for completion */

/* ---- words (ui/words.c) ------------------------------------------------- */
#define WORD_MAX     32                     /* a word's bytes, the NUL included */
#define WORD_MIN     3                      /* characters: shorter ones are not learned */

/* Words are small letters (text_lower), in UTF-8. */
void words_learn(const char *lang, const char *word);  /* one more use, in RAM */
void words_flush(void);                     /* the learned words into the store */
void words_load(void);                      /* from the store, once until words_unload */
void words_unload(void);                    /* flushed, then out of RAM (the last editor closed) */
/* Completions of prefix (small letters, longer than it): the user's words by
 * use, then the card's word list for lang (/b310e-os/words/<lang>.wl) by
 * frequency. How many (at most max). */
int words_find(const char *lang, const char *prefix, char (*out)[WORD_MAX], int max);
void words_close(void);                     /* the word list's file and caches */

#endif
