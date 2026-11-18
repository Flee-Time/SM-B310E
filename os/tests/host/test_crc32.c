/* Host test: CRC-32 (os/lib/crc32.c) against a bit-at-a-time reference.
 *
 *   - crc32_update at every start alignment 0-3 and every length 0-70 (its
 *     byte head, the 16-byte word loop, the 4-byte loop and the tail), on
 *     random bytes in a guarded buffer;
 *   - chained calls: a buffer cut at every point gives the CRC of the whole;
 *   - long random buffers (and the zlib check value of "123456789");
 *   - crc32_part against the CRC of every split of a buffer.
 *
 * Depends on the OS: crc32_update, crc32_part (os/lib.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "os/kernel.h"
#include "os/lib.h"

static uint32_t ref_update(uint32_t c, const uint8_t *p, size_t n)
{
#ifdef HOST_PROVE
    if (n > 37)
        n--;                            /* a wrong reference: the checks must see it */
#endif
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? (c >> 1) ^ 0xedb88320u : c >> 1;
    }
    return c;
}

static uint32_t ref_crc(const uint8_t *p, size_t n)
{
    return ~ref_update(0xffffffffu, p, n);
}

int main(int argc, char **argv)
{
    uint8_t *buf;
    unsigned checks = 0;

    host_init(argc, argv);
    host_srand(0x63726333u);
    CHECK(~crc32_update(0xffffffffu, "123456789", 9) == 0xcbf43926u, "the check value: %08x",
          (unsigned)~crc32_update(0xffffffffu, "123456789", 9));
    /* every alignment and length; the buffer is kmalloc'ed, so a read or
     * write outside it shows in its guards (reads: the poison does not, but
     * the CRC would then differ from the reference) */
    buf = kmalloc(80);
    for (int al = 0; al < 4; al++)
        for (int len = 0; len <= 70; len++) {
            uint8_t *p;

            for (int i = 0; i < 80; i++)
                buf[i] = (uint8_t)host_rand();
            p = buf + ((4 - ((uintptr_t)buf & 3)) & 3) + al;   /* exactly al past a word */
            host_context("alignment %d, length %d", al, len);
            for (uint32_t seed = 0; seed < 3; seed++) {
                uint32_t start = seed == 0 ? 0xffffffffu : seed == 1 ? 0 : host_rand();

                CHECK(crc32_update(start, p, (size_t)len) == ref_update(start, p, (size_t)len),
                      "start %08x: %08x, the reference %08x", (unsigned)start,
                      (unsigned)crc32_update(start, p, (size_t)len),
                      (unsigned)ref_update(start, p, (size_t)len));
                checks++;
            }
            ga_check();
        }
    kfree(buf);
    /* long buffers, in one call and cut at every point of the first 300 bytes */
    for (int round = 0; round < 6; round++) {
        size_t n = 1000 + host_rand() % 70000;
        uint8_t *b = kmalloc(n);
        uint32_t whole;

        for (size_t i = 0; i < n; i++)
            b[i] = (uint8_t)host_rand();
        host_context("a buffer of %u bytes", (unsigned)n);
        whole = ref_crc(b, n);
        CHECK(~crc32_update(0xffffffffu, b, n) == whole, "%u bytes: %08x, the reference %08x",
              (unsigned)n, (unsigned)~crc32_update(0xffffffffu, b, n), (unsigned)whole);
        for (size_t cut = 0; cut < 300; cut++) {
            uint32_t c = crc32_update(0xffffffffu, b, cut);

            c = crc32_update(c, b + cut, n - cut);
            CHECK(~c == whole, "cut at %u: %08x", (unsigned)cut, (unsigned)~c);
            checks++;
        }
        kfree(b);
    }
    /* crc32_part: the CRC of the bytes between two running values */
    {
        uint8_t b[600];

        for (size_t i = 0; i < sizeof(b); i++)
            b[i] = (uint8_t)host_rand();
        for (size_t a = 0; a < sizeof(b); a += 1 + a / 40)
            for (size_t e = a; e <= sizeof(b); e += 1 + (e - a) / 30) {
                uint32_t before = crc32_update(0xffffffffu, b, a);
                uint32_t after = crc32_update(before, b + a, e - a);

                host_context("crc32_part of bytes %u..%u", (unsigned)a, (unsigned)e);
                CHECK(crc32_part(before, after, (uint32_t)(e - a)) == ref_crc(b + a, e - a),
                      "%08x, the reference %08x",
                      (unsigned)crc32_part(before, after, (uint32_t)(e - a)),
                      (unsigned)ref_crc(b + a, e - a));
                checks++;
            }
    }
    printf("crc32: %u comparisons\n", checks);
    return host_done();
}
