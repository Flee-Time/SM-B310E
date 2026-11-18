/* Host test: vsnprintf (os/lib/printf.c), built as os_vsnprintf and
 * os_snprintf, against the host's snprintf.
 *
 *   - every conversion it supports (d i u x X o p s c %) with the flags
 *     - 0 + and widths (also from '*'), precision for strings ("%.3s",
 *     "%.*s"), the l, z and ll modifiers, on edge values (0, -1, INT_MIN,
 *     LLONG_MIN, ULLONG_MAX ...); %p as the OS prints it, "0x" and at
 *     least 8 hex digits;
 *   - "%.*s" on a buffer with no NUL that ends where an unreadable page
 *     begins: a read past the precision is an access violation;
 *   - buffer sizes 0 .. the full length + 2: the return value is the full
 *     length, nothing is written past size, the text is the full text cut
 *     and NUL-ended, and a cut never ends inside a UTF-8 character (the
 *     longest whole-character prefix that fits).
 *
 * Depends on the OS: vsnprintf, snprintf (os/lib/printf.c), utf8_cut
 * (os/lib/utf8.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "os/kernel.h"

int os_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int os_snprintf(char *buf, size_t size, const char *fmt, ...);

static unsigned compared;

/* the longest prefix of s (whole characters, s valid UTF-8) of at most n bytes */
static size_t whole_prefix(const char *s, size_t len, size_t n)
{
    if (n >= len)
        return len;
    while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80)
        n--;
    return n;
}

/* fmt with the arguments, at every buffer size, against the expected text */
static void check_sizes(const char *expect, int elen, const char *fmt, va_list ap0)
{
    char buf[600];

    for (size_t size = 0; size <= (size_t)elen + 2 && size < sizeof(buf) - 8; size++) {
        va_list ap;
        int r;

        memset(buf, '#', sizeof(buf));
        va_copy(ap, ap0);
        r = os_vsnprintf(size ? buf : NULL, size, fmt, ap);
        va_end(ap);
        CHECK(r == elen, "\"%s\" size %u: returned %d, should be %d", fmt, (unsigned)size, r,
              elen);
        for (size_t i = size; i < size + 8; i++)
            if (buf[i] != '#') {
                CHECK(0, "\"%s\" size %u: wrote byte %u", fmt, (unsigned)size, (unsigned)i);
                break;
            }
        if (size) {
            size_t keep = whole_prefix(expect, (size_t)elen, size - 1);

            CHECK(!memcmp(buf, expect, keep) && buf[keep] == 0,
                  "\"%s\" size %u: \"%.*s\", should be \"%.*s\"", fmt, (unsigned)size,
                  (int)size, buf, (int)keep, expect);
        }
        compared++;
        if (host_failures > 10)
            return;
    }
}

/* the OS's output must be the host's (expect NULL) or expect */
static void same(const char *expect, const char *fmt, ...)
{
    char host[600], os[600];
    va_list ap;
    int hn, on;

    va_start(ap, fmt);
    if (!expect) {
        va_list aq;

        va_copy(aq, ap);
        hn = vsnprintf(host, sizeof(host), fmt, aq);
        va_end(aq);
#ifdef HOST_PROVE
        if (!strcmp(fmt, "%5d"))
            host[0] = '*';              /* a wrong reference */
#endif
    } else {
        hn = (int)strlen(expect);
        memcpy(host, expect, (size_t)hn + 1);
    }
    host_context("format \"%s\"", fmt);
    {
        va_list aq;

        va_copy(aq, ap);
        on = os_vsnprintf(os, sizeof(os), fmt, aq);
        va_end(aq);
    }
    CHECK(on == hn && !memcmp(os, host, (size_t)hn + 1), "\"%s\": \"%s\" (%d), should be \"%s\" (%d)",
          fmt, os, on, host, hn);
    compared++;
    check_sizes(host, hn, fmt, ap);
    va_end(ap);
}

static void note_same(const char *fmt, ...)
{
    char host[200], os[200];
    va_list ap, aq;

    va_start(ap, fmt);
    va_copy(aq, ap);
    vsnprintf(host, sizeof(host), fmt, ap);
    os_vsnprintf(os, sizeof(os), fmt, aq);
    va_end(aq);
    va_end(ap);
    if (strcmp(host, os))
        printf("note: \"%s\" gives \"%s\", the C library \"%s\" (not checked)\n", fmt, os, host);
}

int main(int argc, char **argv)
{
    static const int ints[] = { 0, 1, -1, 7, -42, 12345, -99999, INT_MAX, INT_MIN };
    static const unsigned uints[] = { 0, 1, 9, 10, 255, 4096, 0xdeadbeefu, UINT_MAX };
    static const char *const int_fmts[] = {
        "%d", "%i", "%5d", "%-5d|", "%05d", "%+d", "%+5d", "%-+6d|", "%0+7d", "%1d", "%12d",
        "%-12i|", "%012d", "[%d]",
    };
    static const char *const uint_fmts[] = {
        "%u", "%x", "%X", "%o", "%8x", "%-8X|", "%08o", "%010u", "%+u", "%2x", "%lx", "%lu",
    };
    static const char *const str_fmts[] = {
        "%s", "%10s", "%-10s|", "%.3s", "%5.2s", "%-6.4s|", "%.0s", "%.40s", "<%s>",
    };
    static const char *const strs[] = {
        "", "a", "hello", "Merhaba dünya", "İstanbul ığüşöç", "日本語テキスト", "€uro 🙂 face",
    };
    char buf[64];

    host_init(argc, argv);
    for (size_t f = 0; f < sizeof(int_fmts) / sizeof(int_fmts[0]); f++)
        for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++)
            same(NULL, int_fmts[f], ints[i]);
    for (size_t f = 0; f < sizeof(uint_fmts) / sizeof(uint_fmts[0]); f++)
        for (size_t i = 0; i < sizeof(uints) / sizeof(uints[0]); i++)
            same(NULL, uint_fmts[f], uints[i]);
    for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
        same(NULL, "%ld", (long)ints[i]);
        same(NULL, "%li", (long)ints[i]);
        same(NULL, "%zu", (size_t)(unsigned)ints[i]);
        same(NULL, "%zx", (size_t)(unsigned)ints[i]);
        same(NULL, "%*d|", 6, ints[i]);
        same(NULL, "%-*d|", 7, ints[i]);
    }
    {
        static const long long lls[] = { 0, -1, 1LL << 32, -(1LL << 40), 123456789012345LL,
                                         LLONG_MAX, LLONG_MIN };
        static const unsigned long long ulls[] = { 0, 1ULL << 32, 0xfedcba9876543210ULL,
                                                   ULLONG_MAX };

        for (size_t i = 0; i < sizeof(lls) / sizeof(lls[0]); i++) {
            same(NULL, "%lld", lls[i]);
            same(NULL, "%+lli", lls[i]);
            same(NULL, "%25lld|", lls[i]);
            same(NULL, "%-25lld|", lls[i]);
            same(NULL, "%025lld", lls[i]);
        }
        for (size_t i = 0; i < sizeof(ulls) / sizeof(ulls[0]); i++) {
            same(NULL, "%llu", ulls[i]);
            same(NULL, "%llx", ulls[i]);
            same(NULL, "%llX", ulls[i]);
            same(NULL, "%llo", ulls[i]);
            same(NULL, "%20llx|", ulls[i]);
        }
    }
    for (size_t f = 0; f < sizeof(str_fmts) / sizeof(str_fmts[0]); f++)
        for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) {
            /* the host counts the precision in bytes and may cut a character;
             * the OS cuts on a character: the expected text is the host's, cut back */
            char host[200], fmt2[40];
            const char *fmt = str_fmts[f], *dot = strchr(fmt, '.');

            snprintf(fmt2, sizeof(fmt2), "%s", fmt);
            if (dot) {
                int prec = atoi(dot + 1), slen = (int)strlen(strs[i]);
                size_t keep = whole_prefix(strs[i], (size_t)slen,
                                           (size_t)(prec < slen ? prec : slen));
                const char *after = dot + 1;

                while (*after >= '0' && *after <= '9')
                    after++;
                snprintf(fmt2, sizeof(fmt2), "%.*s%u%s", (int)(dot + 1 - fmt), fmt,
                         (unsigned)keep, after);
            }
            snprintf(host, sizeof(host), fmt2, strs[i]);
            same(host, fmt, strs[i]);
        }
    same(NULL, "%c%c%c", 'a', 'B', '0');
    /* outside what printf.c's header promises, and no OS code uses them:
     * shown as notes, not failures */
    note_same("%5c|%-3c|", 'x', 'y');
    note_same("%*d|", -6, 42);
    same(NULL, "100%%");
    same(NULL, "%% %d %%", 3);
    same(NULL, "plain text, no conversions");
    same("(null)", "%s", (char *)NULL);
    same("", "");
    {
        void *ptrs[] = { NULL, (void *)(uintptr_t)0x1234, (void *)(uintptr_t)0xdeadbeefu, &buf };

        for (size_t i = 0; i < 4; i++) {
            char e[40];

            snprintf(e, sizeof(e), "0x%08llx", (unsigned long long)(uintptr_t)ptrs[i]);
            same(e, "%p", ptrs[i]);
        }
    }
    /* "%.*s" on bytes with no NUL, against an unreadable page */
    {
        static const char text[] = "abcdeüfgh";
        size_t n = sizeof(text) - 1;
        char *p = host_edge(n);

        memcpy(p, text, n);
        for (int prec = 0; prec <= (int)n; prec++) {
            char e[40], o[40];
            int keep = (int)whole_prefix(text, n, (size_t)prec);

            snprintf(e, sizeof(e), "%.*s", keep, text);
            host_context("\"%%.*s\" with precision %d on %u bytes and no NUL", prec, (unsigned)n);
            os_snprintf(o, sizeof(o), "%.*s", prec, p);
            CHECK(!strcmp(o, e), "\"%s\", should be \"%s\"", o, e);
            compared++;
        }
        os_snprintf(buf, sizeof(buf), "%.*s|", -1, "neg");
        CHECK(!strcmp(buf, "neg|"), "a negative precision is none: \"%s\"", buf);
    }
    /* the cut on a character: sizes inside a 2-, 3- and 4-byte character */
    {
        static const char *const texts[] = { "aé", "a€b", "x🙂y", "ğğğğ", "🙂🙂" };

        for (size_t t = 0; t < 5; t++)
            same(texts[t], "%s", texts[t]);
    }
    printf("printf: %u comparisons\n", compared);
    return host_done();
}
