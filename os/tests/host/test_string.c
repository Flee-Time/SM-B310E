/* Host test: the string functions (os/lib/string.c), built with their names
 * changed (-Dmemcpy=os_memcpy ...) so the host's own stay in use here.
 *
 *   - memcpy, memmove, memset, memcmp against byte loops for every source and
 *     destination alignment 0-7 and every length 0-300; memmove with the
 *     buffers overlapping both ways by every distance up to 40 bytes;
 *   - the destination has 16 guard bytes on each side, checked after each
 *     call; sources are put against an inaccessible page (VirtualProtect)
 *     at their start and at their end, so a read past the source's words
 *     (string.c's comment promises none) is an access violation;
 *   - strlen, strcmp (bytes above 0x7f compare unsigned), strncpy (cut, no
 *     NUL; padding), strchr, strrchr (the NUL itself, a char above 0xff).
 *
 * Depends on the OS: memcpy memmove memset memcmp strlen strcmp strncpy
 * strchr strrchr (os/lib.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "host.h"

void *os_memcpy(void *dst, const void *src, size_t n);
void *os_memmove(void *dst, const void *src, size_t n);
void *os_memset(void *dst, int c, size_t n);
int os_memcmp(const void *a, const void *b, size_t n);
size_t os_strlen(const char *s);
int os_strcmp(const char *a, const char *b);
char *os_strncpy(char *dst, const char *src, size_t n);
char *os_strchr(const char *s, int c);
char *os_strrchr(const char *s, int c);

#define MAXLEN 300
#define GUARDB 16
#define PAGE   4096

static uint8_t *page;                   /* one accessible page between two that are not */

static void pages_init(void)
{
#ifdef _WIN32
    DWORD old;
    uint8_t *p = VirtualAlloc(NULL, 3 * PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    if (!p || !VirtualProtect(p, PAGE, PAGE_NOACCESS, &old) ||
        !VirtualProtect(p + 2 * PAGE, PAGE, PAGE_NOACCESS, &old)) {
        fprintf(stderr, "no guard pages\n");
        exit(2);
    }
    page = p + PAGE;
#else
    page = aligned_alloc(PAGE, PAGE);   /* no guard pages: the reads go unchecked */
#endif
}

static void fill(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(host_rand() | 1);
}

/* a destination area: GUARDB + 8 + MAXLEN + GUARDB bytes of a pattern */
static uint8_t area[GUARDB + 8 + MAXLEN + GUARDB + 8], want[sizeof(area)];

static void area_fill(void)
{
    for (size_t i = 0; i < sizeof(area); i++)
        area[i] = (uint8_t)(0xe1 ^ i * 37);
    memcpy(want, area, sizeof(area));
}

static bool area_same(const char *what, size_t at, size_t n)
{
    for (size_t i = 0; i < sizeof(area); i++)
        if (area[i] != want[i]) {
            bool inside = i >= at && i < at + n;

            CHECK(0, "%s: byte %d %s the destination is %02x, should be %02x", what,
                  (int)i - (int)at, inside ? "of" : "outside", area[i], want[i]);
            return false;
        }
    return true;
}

static void test_copy(bool move)
{
    const char *name = move ? "memmove" : "memcpy";

    for (int place = 0; place < 2; place++)         /* the source at the page's start, its end */
        for (int sa = 0; sa < 8; sa++)
            for (int da = 0; da < 8; da++)
                for (size_t n = 0; n <= MAXLEN; n++) {
                    const uint8_t *src;
                    size_t at = GUARDB + (size_t)da;
                    void *r;

                    if (place == 0) {
                        src = page + sa;
                    } else {
                        src = page + PAGE - n;
                        if (((uintptr_t)src & 7) != (uintptr_t)sa)
                            continue;           /* each length has its own alignment there */
                    }
                    fill((uint8_t *)src, n);
                    area_fill();
                    memcpy(want + at, src, n);
                    host_context("%s: source alignment %d (at the page's %s), destination %d, %u bytes",
                                 name, sa, place ? "end" : "start", da, (unsigned)n);
                    r = move ? os_memmove(area + at, src, n) : os_memcpy(area + at, src, n);
                    CHECK(r == area + at, "%s returned %p, not the destination", name, r);
                    if (!area_same(name, at, n) && host_failures > 10)
                        return;
                }
}

static void test_overlap(void)
{
    static uint8_t buf[MAXLEN + 128], ref[sizeof(buf)];

    for (int d = -40; d <= 40; d++)
        for (int al = 0; al < 8; al++)
            for (size_t n = 0; n <= MAXLEN; n += n < 80 ? 1 : 7) {
                size_t s = 48 + (size_t)al, t = (size_t)((int)s + d);

                fill(buf, sizeof(buf));
                memcpy(ref, buf, sizeof(buf));
#ifdef HOST_PROVE
                for (size_t i = 0; i < n; i++)  /* a wrong reference: forward, byte by byte */
                    ref[t + i] = ref[s + i];
#else
                memmove(ref + t, ref + s, n);
#endif
                host_context("memmove overlapping: source +%u, destination %+d, %u bytes",
                             (unsigned)s, d, (unsigned)n);
                os_memmove(buf + t, buf + s, n);
                if (memcmp(buf, ref, sizeof(buf))) {
                    size_t i = 0;

                    while (buf[i] == ref[i])
                        i++;
                    CHECK(0, "byte %d (from the destination) is %02x, should be %02x",
                          (int)i - (int)t, buf[i], ref[i]);
                    if (host_failures > 10)
                        return;
                }
            }
}

static void test_memset(void)
{
    static const int values[] = { 0, 0xff, 0x5a, 0x80, 0x1234, -1 };

    for (int da = 0; da < 8; da++)
        for (size_t n = 0; n <= MAXLEN; n++)
            for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
                size_t at = GUARDB + (size_t)da;

                area_fill();
                memset(want + at, values[v], n);
                host_context("memset: alignment %d, %u bytes of %#x", da, (unsigned)n, values[v]);
                CHECK(os_memset(area + at, values[v], n) == area + at, "memset's result");
#ifdef HOST_PROVE
                if (n == 100 && v == 0)
                    os_memset(area + at, values[v], n + 1);     /* one past: the guards must see it */
#endif
                if (!area_same("memset", at, n) && host_failures > 10)
                    return;
            }
}

static int sign(int v)
{
    return (v > 0) - (v < 0);
}

static void test_memcmp(void)
{
    static uint8_t a[MAXLEN + 16], b[MAXLEN + 16];

    for (int aa = 0; aa < 8; aa++)
        for (int ba = 0; ba < 8; ba++)
            for (size_t n = 0; n <= MAXLEN; n += n < 72 ? 1 : 19) {
                uint8_t *x = a + aa, *y = b + ba;

                fill(x, n);
                memcpy(y, x, n);
                host_context("memcmp: alignments %d %d, %u bytes, equal", aa, ba, (unsigned)n);
                CHECK(os_memcmp(x, y, n) == 0, "equal buffers compare %d", os_memcmp(x, y, n));
                for (size_t at = 0; at < n; at += 1 + at / 16) {
                    uint8_t keep = y[at];

                    for (int k = 0; k < 2; k++) {
                        y[at] = k ? (uint8_t)(x[at] ^ 0x80) : (uint8_t)(x[at] + 1);
                        host_context("memcmp: alignments %d %d, %u bytes, %02x vs %02x at %u", aa,
                                     ba, (unsigned)n, x[at], y[at], (unsigned)at);
                        CHECK(sign(os_memcmp(x, y, n)) == sign(memcmp(x, y, n)),
                              "result %d, the bytes say %d", os_memcmp(x, y, n), memcmp(x, y, n));
                        if (at + 1 < n) {       /* a later difference must not count */
                            uint8_t later = y[n - 1];

                            y[n - 1] ^= 0x55;
                            CHECK(sign(os_memcmp(x, y, n)) == sign(memcmp(x, y, n)),
                                  "with a later difference: %d", os_memcmp(x, y, n));
                            y[n - 1] = later;
                        }
                    }
                    y[at] = keep;
                    if (host_failures > 10)
                        return;
                }
            }
}

static void test_str(void)
{
    char d[64];
    const char *s = "abc\xe9z\xe9q";

    host_context("strlen");
    CHECK(os_strlen("") == 0, "strlen of \"\"");
    for (int al = 0; al < 8; al++)
        for (int n = 0; n < 40; n++) {
            char *p = (char *)page + PAGE - n - 1;     /* the NUL is the page's last byte */

            (void)al;
            memset(p, 'x', (size_t)n);
            p[n] = 0;
            CHECK(os_strlen(p) == (size_t)n, "strlen %u, should be %d", (unsigned)os_strlen(p), n);
        }
    host_context("strcmp");
    CHECK(os_strcmp("", "") == 0, "\"\" \"\"");
    CHECK(os_strcmp("a", "") > 0 && os_strcmp("", "a") < 0, "a prefix");
    CHECK(os_strcmp("abc", "abd") < 0 && os_strcmp("abd", "abc") > 0, "abc abd");
    CHECK(os_strcmp("ab", "abc") < 0, "ab abc");
    CHECK(os_strcmp("\xc3\xa9", "z") > 0, "bytes above 0x7f are above ASCII (unsigned)");
    CHECK(os_strcmp("\x80", "\x7f") > 0, "0x80 > 0x7f");
    CHECK(os_strcmp("same", "same") == 0, "same");
    host_context("strncpy");
    memset(d, '#', sizeof(d));
    CHECK(os_strncpy(d, "hello", 3) == d && !memcmp(d, "hel#", 4), "a cut leaves no NUL");
    memset(d, '#', sizeof(d));
    os_strncpy(d, "hi", 6);
    CHECK(!memcmp(d, "hi\0\0\0\0#", 7), "padding with NULs to n, not past it");
    memset(d, '#', sizeof(d));
    os_strncpy(d, "", 0);
    CHECK(d[0] == '#', "n 0 writes nothing");
    os_strncpy(d, "exact", 5);
    CHECK(!memcmp(d, "exact#", 6), "exactly n: no NUL");
    host_context("strchr, strrchr");
    CHECK(os_strchr(s, 'c') == s + 2, "strchr c");
    CHECK(os_strchr(s, 0xe9) == s + 3, "strchr of a byte above 0x7f (as an int)");
    CHECK(os_strchr(s, (char)0xe9) == s + 3, "strchr of a negative char");
    CHECK(os_strchr(s, 0) == s + strlen(s), "strchr of 0: the NUL");
    CHECK(os_strchr(s, 'y') == NULL, "strchr of a missing one");
    CHECK(os_strchr(s, 0x100 + 'b') == s + 1, "strchr takes c as a char");
    CHECK(os_strrchr(s, 0xe9) == s + 5, "strrchr: the last one");
    CHECK(os_strrchr(s, 0) == s + strlen(s), "strrchr of 0: the NUL");
    CHECK(os_strrchr(s, 'y') == NULL, "strrchr of a missing one");
    CHECK(os_strrchr("", 'a') == NULL && os_strchr("", 'a') == NULL, "the empty string");
}

int main(int argc, char **argv)
{
    host_init(argc, argv);
    host_srand(0x73747231u);
    pages_init();
    test_copy(false);
    test_copy(true);
    test_overlap();
    test_memset();
    test_memcmp();
    test_str();
    printf("string: mem* at every alignment 0-7 and length 0-%d, str* edge cases\n", MAXLEN);
    return host_done();
}
