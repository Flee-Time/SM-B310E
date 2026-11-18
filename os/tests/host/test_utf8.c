/* Host test: the UTF-8 helpers (os/lib/utf8.c, and utf8_valid /
 * utf8_from_latin1 in os/fs/db.c) and the letter case tables of the text
 * editor (os/ui/textlang.c).
 *
 *   - utf8_put / utf8_get / utf8_len round trip every code point
 *     (U+0000..U+10FFFF but the surrogates) against an independent encoder;
 *   - broken input (stray continuation bytes, C0/C1, F5..FF, a sequence cut
 *     short or by a NUL) reads as one byte, U+FFFD, and never past the NUL;
 *     overlong forms and encoded surrogates are reported as notes (utf8.c
 *     takes them; db.c's utf8_valid refuses them);
 *   - utf8_valid against a reference validator on every 1-3 byte string from
 *     a set of interesting bytes, and on random strings;
 *   - utf8_cut never ends inside a character and keeps every whole one, for
 *     every cut of mixed text; utf8_trim likewise; utf8_from_latin1;
 *   - text_upper / text_lower: Turkish i/İ and ı/I both ways, English
 *     i/I, ß, ÿ/Ÿ, and for every code point up to U+0600 in every language:
 *     a changed letter changes back (but for the dotted/dotless i pairs),
 *     upper and lower are idempotent, a letter keeps being a letter.
 *   - input_langs_on with no store: English and Turkish (the default).
 *
 * Depends on the OS: utf8_len utf8_get utf8_put utf8_cut utf8_trim
 * (os/lib/utf8.c); utf8_valid utf8_from_latin1 (os/fs/db.c, linked whole,
 * with the store never started); text_upper text_lower text_is_letter
 * input_langs input_lang_count input_lang_find input_langs_on
 * (os/ui/textlang.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "os/db.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "ui/textlang.h"

static int ref_put(uint32_t c, uint8_t *o)
{
    if (c < 0x80) { o[0] = (uint8_t)c; return 1; }
    if (c < 0x800) { o[0] = (uint8_t)(0xc0 | c >> 6); o[1] = (uint8_t)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) {
        o[0] = (uint8_t)(0xe0 | c >> 12); o[1] = (uint8_t)(0x80 | (c >> 6 & 63));
        o[2] = (uint8_t)(0x80 | (c & 63));
        return 3;
    }
    o[0] = (uint8_t)(0xf0 | c >> 18); o[1] = (uint8_t)(0x80 | (c >> 12 & 63));
    o[2] = (uint8_t)(0x80 | (c >> 6 & 63)); o[3] = (uint8_t)(0x80 | (c & 63));
    return 4;
}

/* the length of a well-formed sequence at p (n bytes), else 0 (RFC 3629) */
static int ref_seq(const uint8_t *p, size_t n)
{
    int len;
    uint32_t c, min;

    if (!n)
        return 0;
    if (p[0] < 0x80)
        return 1;
    if (p[0] >= 0xc2 && p[0] <= 0xdf) len = 2, c = p[0] & 0x1f, min = 0x80;
    else if (p[0] >= 0xe0 && p[0] <= 0xef) len = 3, c = p[0] & 0x0f, min = 0x800;
    else if (p[0] >= 0xf0 && p[0] <= 0xf4) len = 4, c = p[0] & 0x07, min = 0x10000;
    else return 0;
    if (n < (size_t)len)
        return 0;
    for (int i = 1; i < len; i++) {
        if ((p[i] & 0xc0) != 0x80)
            return 0;
        c = c << 6 | (p[i] & 0x3f);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
        return 0;
    return len;
}

static bool ref_valid(const uint8_t *p, size_t n)
{
    while (n) {
        int k = ref_seq(p, n);

        if (!k)
            return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static void test_round_trip(void)
{
    unsigned n = 0;

    for (uint32_t c = 0; c <= 0x10ffff; c++) {
        char out[8] = { 0 }, ref[8] = { 0 };
        const char *p = out;
        int k, kr;

        if (c >= 0xd800 && c <= 0xdfff)
            continue;
        k = utf8_put(c, out);
        kr = ref_put(c, (uint8_t *)ref);
        if (k != kr || memcmp(out, ref, (size_t)k)) {
            CHECK(0, "utf8_put(U+%04X): %d bytes, should be %d", (unsigned)c, k, kr);
            if (host_failures > 10)
                return;
            continue;
        }
        if (c) {
            uint32_t g;

            CHECK(utf8_len(out) == k, "utf8_len of U+%04X: %d", (unsigned)c, utf8_len(out));
            g = utf8_get(&p);
            CHECK(g == c && p == out + k, "utf8_get of U+%04X: U+%04X, %d bytes", (unsigned)c,
                  (unsigned)g, (int)(p - out));
            CHECK(utf8_valid(out, (size_t)k), "utf8_valid refuses U+%04X", (unsigned)c);
        }
        n++;
        if (host_failures > 10)
            return;
    }
    printf("utf8: %u code points round trip\n", n);
}

static void test_broken(void)
{
    static const char *const broken[] = {
        "\x80", "\xbf", "\xc0\x80", "\xc1\xbf", "\xc3", "\xc3x", "\xe2\x82", "\xe2\x82x",
        "\xe2x\x82", "\xf0\x9f\x99", "\xf0\x9f\x99x", "\xf5\x80\x80\x80", "\xf8\x88\x80\x80\x80",
        "\xfe", "\xff", "\xc3\xc3\xa9",
    };
    static const char *const lenient[] = {
        "\xe0\x80\x80", "\xe0\x9f\xbf", "\xed\xa0\x80", "\xed\xbf\xbf", "\xf0\x80\x80\x80",
        "\xf0\x8f\xbf\xbf", "\xf4\x90\x80\x80",
    };

    for (unsigned i = 0; i < sizeof(broken) / sizeof(broken[0]); i++) {
        const char *s = broken[i], *p = s;
        uint32_t c;

        host_context("broken sequence %u", (unsigned)i);
        CHECK(utf8_len(s) == 1, "utf8_len of broken %u: %d", i, utf8_len(s));
        c = utf8_get(&p);
        CHECK(c == 0xfffd && p == s + 1, "utf8_get of broken %u: U+%04X, %d bytes", i,
              (unsigned)c, (int)(p - s));
        CHECK(!utf8_valid(s, strlen(s)), "utf8_valid takes broken %u", (unsigned)i);
    }
    for (unsigned i = 0; i < sizeof(lenient) / sizeof(lenient[0]); i++) {
        const char *s = lenient[i], *p = s;
        uint32_t c = utf8_get(&p);

        CHECK(!utf8_valid(s, strlen(s)), "utf8_valid takes an overlong or surrogate (%u)", i);
        if (c != 0xfffd)
            printf("note: utf8_get reads the overlong/surrogate/out-of-range form %u (%u bytes) as "
                   "U+%04X, not U+FFFD (utf8_valid refuses it)\n", i, (unsigned)strlen(s),
                   (unsigned)c);
    }
    /* never past a NUL: a sequence cut by the end of the string */
    {
        char *b = host_edge(2);

        b[0] = (char)0xe2;
        b[1] = 0;                       /* the NUL ends the page: a read past it faults */
        CHECK(utf8_len(b) == 1, "a 3-byte lead before the NUL: %d", utf8_len(b));
    }
}

static void test_valid(void)
{
    static const uint8_t bytes[] = {
        0x00, 0x41, 0x7f, 0x80, 0x8f, 0x90, 0x9f, 0xa0, 0xbf, 0xc0, 0xc1, 0xc2, 0xdf, 0xe0,
        0xe1, 0xec, 0xed, 0xee, 0xef, 0xf0, 0xf1, 0xf3, 0xf4, 0xf5, 0xfe, 0xff,
    };
    const size_t nb = sizeof(bytes);
    unsigned n = 0;

    for (size_t a = 0; a < nb; a++)
        for (size_t b = 0; b <= nb; b++)
            for (size_t c = 0; c <= nb; c++) {
                uint8_t s[4];
                size_t len = 1;

                s[0] = bytes[a];
                if (b < nb) {
                    s[len++] = bytes[b];
                    if (c < nb)
                        s[len++] = bytes[c];
                } else if (c < nb) {
                    continue;
                }
                CHECK(utf8_valid((const char *)s, len) == ref_valid(s, len),
                      "utf8_valid(%02x %02x %02x, %u) is %d", s[0], len > 1 ? s[1] : 0,
                      len > 2 ? s[2] : 0, (unsigned)len, utf8_valid((const char *)s, len));
                n++;
                if (host_failures > 10)
                    return;
            }
    for (int r = 0; r < 20000; r++) {
        uint8_t s[16];
        size_t len = host_rand() % 16;

        for (size_t i = 0; i < len; i++)
            s[i] = (uint8_t)(host_rand() % 3 ? bytes[host_rand() % nb] : host_rand());
        CHECK(utf8_valid((const char *)s, len) == ref_valid(s, len), "utf8_valid on random bytes");
        n++;
        if (host_failures > 10)
            return;
    }
    printf("utf8: utf8_valid on %u strings\n", n);
}

static void test_cut(void)
{
    static const char text[] = "aé€🙂bİığ日本🙂z";
    size_t len = strlen(text);
    char buf[64], out[64];

    for (size_t n = 0; n <= len; n++) {
        size_t c = utf8_cut(text, n), want = n;

        while (want > 0 && want < len && ((uint8_t)text[want] & 0xc0) == 0x80)
            want--;
        host_context("utf8_cut at %u", (unsigned)n);
        CHECK(c == want, "utf8_cut(%u) is %u, should be %u", (unsigned)n, (unsigned)c,
              (unsigned)want);
        memcpy(buf, text, n);
        buf[n] = 0;
        CHECK(utf8_trim(buf) == want && strlen(buf) == want, "utf8_trim of %u bytes", (unsigned)n);
    }
    /* broken text: a cut keeps what is there (no sequence to cut) */
    CHECK(utf8_cut("ab\x80\x80\x80\x80", 6) == 6, "four stray continuation bytes are kept");
    CHECK(utf8_cut("", 0) == 0, "utf8_cut of nothing");
    /* Latin-1 to UTF-8 */
    {
        size_t n = utf8_from_latin1("caf\xe9 d\xfcnya", 10, out, sizeof(out));

        CHECK(n == 12 && !strcmp(out, "café dünya"), "utf8_from_latin1: %u \"%s\"", (unsigned)n,
              out);
        n = utf8_from_latin1("café", 5, out, sizeof(out));
        CHECK(n == 5 && !strcmp(out, "café"), "utf8_from_latin1 keeps UTF-8: \"%s\"", out);
        n = utf8_from_latin1("\xe9\xe9\xe9", 3, out, 4);
        CHECK(n == 2 && !strcmp(out, "é"), "utf8_from_latin1 cut to 4 bytes: %u \"%s\"",
              (unsigned)n, out);
        for (size_t size = 0; size < 8; size++) {
            char *o = kmalloc(size ? size : 1);

            utf8_from_latin1("\xe9x\xe9y", 4, o, size);   /* a write past size is a heap error */
            if (size)
                CHECK(ref_valid((uint8_t *)o, strlen(o)), "utf8_from_latin1 into %u: broken",
                      (unsigned)size);
            kfree(o);
        }
    }
}

static void test_case(void)
{
    const input_lang_t *en = &input_langs[input_lang_find("en")];
    const input_lang_t *tr = &input_langs[input_lang_find("tr")];
    unsigned n = 0;

    CHECK(input_lang_find("en") >= 0 && input_lang_find("tr") >= 0 && input_lang_find("xx") < 0,
          "input_lang_find");
    host_context("Turkish and English letter case");
#ifdef HOST_PROVE
    CHECK(text_upper(tr, 'i') == 'I', "a wrong reference: Turkish capital i is I");
#else
    CHECK(text_upper(tr, 'i') == 0x130, "Turkish i -> İ: U+%04X", (unsigned)text_upper(tr, 'i'));
#endif
    CHECK(text_lower(tr, 'I') == 0x131, "Turkish I -> ı: U+%04X", (unsigned)text_lower(tr, 'I'));
    CHECK(text_upper(tr, 0x131) == 'I', "Turkish ı -> I");
    CHECK(text_lower(tr, 0x130) == 'i', "Turkish İ -> i");
    CHECK(text_upper(en, 'i') == 'I' && text_lower(en, 'I') == 'i', "English i/I");
    CHECK(text_upper(en, 0x131) == 'I' && text_lower(en, 0x130) == 'i', "ı and İ in English");
    CHECK(text_upper(NULL, 'i') == 'I', "no language: i -> I");
    CHECK(text_upper(tr, 0xdf) == 0xdf && text_lower(tr, 0xdf) == 0xdf, "ß stays");
    CHECK(text_upper(en, 0xff) == 0x178 && text_lower(en, 0x178) == 0xff, "ÿ/Ÿ");
    CHECK(text_upper(en, 0xf7) == 0xf7 && text_lower(en, 0xd7) == 0xd7, "÷ and × are not letters");
    CHECK(text_upper(en, 0x11f) == 0x11e && text_lower(en, 0x15e) == 0x15f, "ğ/Ğ ş/Ş");
    CHECK(text_upper(en, 0x44f) == 0x42f && text_lower(en, 0x401) == 0x451, "я/Я Ё/ё");
    CHECK(text_upper(en, 0x491) == 0x490, "ґ/Ґ");
    for (int l = 0; l < input_lang_count; l++) {
        const input_lang_t *lang = &input_langs[l];

        for (uint32_t c = 0; c < 0x600; c++) {
            uint32_t u = text_upper(lang, c), w = text_lower(lang, c);
            bool i_pair = c == 'i' || c == 'I' || c == 0x130 || c == 0x131;

            host_context("%s, U+%04X", lang->code, (unsigned)c);
            CHECK(text_upper(lang, u) == u, "upper is not idempotent: U+%04X -> U+%04X -> U+%04X",
                  (unsigned)c, (unsigned)u, (unsigned)text_upper(lang, u));
            CHECK(text_lower(lang, w) == w, "lower is not idempotent: U+%04X -> U+%04X -> U+%04X",
                  (unsigned)c, (unsigned)w, (unsigned)text_lower(lang, w));
            if (!i_pair && u != c)
                CHECK(text_lower(lang, u) == c, "U+%04X -> upper U+%04X -> lower U+%04X",
                      (unsigned)c, (unsigned)u, (unsigned)text_lower(lang, u));
            if (!i_pair && w != c)
                CHECK(text_upper(lang, w) == c, "U+%04X -> lower U+%04X -> upper U+%04X",
                      (unsigned)c, (unsigned)w, (unsigned)text_upper(lang, w));
            if (u != c || w != c)
                CHECK(text_is_letter(c) && text_is_letter(u) && text_is_letter(w),
                      "U+%04X changes case but is not a letter", (unsigned)c);
            n++;
            if (host_failures > 10)
                return;
        }
        /* the Turkish pairs round trip in the dotless languages */
        if (lang->dotless_i) {
            CHECK(text_lower(lang, text_upper(lang, 'i')) == 'i' &&
                  text_lower(lang, text_upper(lang, 0x131)) == 0x131 &&
                  text_upper(lang, text_lower(lang, 'I')) == 'I' &&
                  text_upper(lang, text_lower(lang, 0x130)) == 0x130,
                  "%s: the i pairs do not round trip", lang->code);
        }
    }
    {
        int on[16], k = input_langs_on(on);

        host_context("input_langs_on");
        CHECK(k == 2 && on[0] == input_lang_find("en") && on[1] == input_lang_find("tr"),
              "the default languages: %d", k);
    }
    printf("textlang: %u case mappings\n", n);
}

int main(int argc, char **argv)
{
    host_init(argc, argv);
    host_srand(0x75746638u);
    test_round_trip();
    test_broken();
    test_valid();
    test_cut();
    test_case();
#ifdef HOST_PROVE
    {
        /* and the UBSan traps work: a signed overflow must stop the program */
        volatile int big = 0x7fffffff, one = 1;

        host_context("a deliberate signed overflow (the UBSan trap)");
        printf("overflow: %d\n", big + one);
    }
#endif
    return host_done();
}
