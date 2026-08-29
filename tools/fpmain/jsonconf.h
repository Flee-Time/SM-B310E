/* B310E-OS - tools/fpmain/jsonconf.h
 *
 * Minimal JSON parser for the fpmain game menu config.
 *
 * WHY: the stock fpdoom "readconf.h" tokenizer (|Name| args + $vars) is
 * fragile - a single unexpected byte (e.g. the longer SNES/NES sections)
 * makes parse_config() fail the WHOLE file, and the menu shows nothing.
 * JSON has explicit structure and is trivially robust to parse.
 *
 * SCHEMA (config.json):
 * {
 *   "system": ["--bright", "50", "--charge", "2", "--rotate", "2,0"],
 *   "categories": [
 *     { "name": "Ports", "items": [
 *         { "name": "Doom 1",
 *           "bin":  "fpbin/chocolate-doom.bin",
 *           "args": ["--dir", "games/doom1", "doom", "-iwad", "doom1.wad"] },
 *         ...
 *     ]},
 *     ...
 *   ]
 * }
 *
 * - "system" is PREPENDED to every item's args (replaces the legacy $@
 *   variable - no more variable indirection).
 * - Each item's full argv becomes: [bin, ...system, ...args].
 * - A category "name" is emitted as a "=== Name ===" marker item (argc 0),
 *   exactly like the legacy |=== Name ===| headers, so games_init()'s
 *   is_section_header()/cat_name_from_header() detection works unchanged.
 *
 * OUTPUT: the exact same packed item chain as readconf.h's parse_config():
 *   buf[0..3]   = offset from buf to the first item's [next] slot
 *   item        = [next:u32][name\0][argc:u16][argv[0]\0][argv[1]\0]...
 *   next        = offset from this item's [next] slot to the next item's
 *                 [next] slot; 0 on the last item.
 * games_init() / launch_item() / extract_args() are 100% compatible.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p, *end;    /* input scan cursor */
    char *out, *oend;       /* output chain buffer */
    char *prev_next;        /* [next] slot of the previous item (NULL = none) */
    const char **sys;       /* "system" arg list (prepended to items) */
    int nsys;
} jp_t;

static void jp_ws(jp_t *j) {
    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->p++;
        else break;
    }
}

/* Consume a literal char (after ws). Returns 1 on match. */
static int jp_ch(jp_t *j, int ch) {
    jp_ws(j);
    if (j->p < j->end && *j->p == ch) { j->p++; return 1; }
    return 0;
}

/* Parse a JSON string (or a bare token for numbers) into dst. Returns len. */
static unsigned jp_str(jp_t *j, char *dst, unsigned cap) {
    unsigned n = 0;
    jp_ws(j);
    if (j->p >= j->end) return 0;
    if (*j->p == '"') {
        j->p++;
        while (j->p < j->end && *j->p != '"') {
            int c = (unsigned char)*j->p++;
            if (c == '\\' && j->p < j->end) {
                c = (unsigned char)*j->p++;
                switch (c) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '/': c = '/'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case 'u': {          /* \uXXXX -> BMP low 8 bits */
                    int v = 0, k;
                    for (k = 0; k < 4 && j->p < j->end; k++) {
                        int d = (unsigned char)*j->p++;
                        v <<= 4;
                        if (d >= '0' && d <= '9') v |= d - '0';
                        else if (d >= 'a' && d <= 'f') v |= d - 'a' + 10;
                        else if (d >= 'A' && d <= 'F') v |= d - 'A' + 10;
                        else { v = '?'; break; }
                    }
                    c = v & 0xff;
                    break;
                }
                default: c = '?'; break;
                }
            }
            if (n + 1 < cap) dst[n++] = (char)c;
        }
        if (j->p < j->end) j->p++;  /* closing quote */
    } else {
        /* bare token (numbers / true / false / null) */
        while (j->p < j->end) {
            int c = (unsigned char)*j->p;
            if (c == ',' || c == ']' || c == '}' || c == ' ' ||
                c == '\t' || c == '\n' || c == '\r') break;
            if (n + 1 < cap) dst[n++] = (char)*j->p;
            j->p++;
        }
    }
    if (n < cap) dst[n] = 0;
    return n;
}

/* Skip an arbitrary JSON value (for unknown keys). */
static void jp_skip(jp_t *j) {
    jp_ws(j);
    if (j->p >= j->end) return;
    if (*j->p == '{') {
        j->p++;
        if (!jp_ch(j, '}')) {
            do {
                char key[64];
                jp_str(j, key, sizeof(key));
                jp_ch(j, ':');
                jp_skip(j);
            } while (jp_ch(j, ','));
            jp_ch(j, '}');
        }
    } else if (*j->p == '[') {
        j->p++;
        if (!jp_ch(j, ']')) {
            do jp_skip(j); while (jp_ch(j, ','));
            jp_ch(j, ']');
        }
    } else {
        char tmp[128];
        jp_str(j, tmp, sizeof(tmp));
    }
}

/* Begin a new item in the output chain. Returns 0 on success. */
static int jp_item_begin(jp_t *j, char *head) {
    char *p = j->out;
    p = (char *)(((uintptr_t)p + 3) & ~3);      /* 4-align */
    if (p + 4 >= j->oend) return -1;
    if (j->prev_next) *(uint32_t *)j->prev_next = (uint32_t)(p - j->prev_next);
    else *(uint32_t *)head = (uint32_t)(p - head);
    j->prev_next = p;
    j->out = p + 4;
    return 0;
}

/* Append a NUL-terminated string to the current item's argv. */
static int jp_arg(jp_t *j, const char *s) {
    unsigned n = (unsigned)strlen(s) + 1;
    if (j->out + n >= j->oend) return -1;
    memcpy(j->out, s, n);
    j->out += n;
    return 0;
}

/* Finish an item: patch the argc:u16 at argv_start. */
static void jp_item_end(jp_t *j, unsigned argc, char *argv_start) {
    (void)j;
    argv_start[0] = (char)(argc & 0xff);
    argv_start[1] = (char)(argc >> 8);
}

/* ---- schema walkers ------------------------------------------------------ */

static int jp_parse_item(jp_t *j, char *head, const char *bin_default);

/* Emit a category header item: name "=== Cat ===", argc 0. */
static int jp_cat_header(jp_t *j, char *head, const char *catname) {
    char hdr[48], *av;
    unsigned nl = (unsigned)strlen(catname);
    if (nl + 8 > sizeof(hdr)) return -1;    /* "=== " + name + " ===" */
    memcpy(hdr, "=== ", 4);
    memcpy(hdr + 4, catname, nl);
    memcpy(hdr + 4 + nl, " ===", 5);        /* includes NUL */
    if (jp_item_begin(j, head)) return -1;
    if (jp_arg(j, hdr)) return -1;
    av = j->out;
    j->out += 2;                            /* argc:u16 = 0 */
    jp_item_end(j, 0, av);
    return 0;
}

/* Parse a category object: { "name": ..., "items": [...] }. */
static int jp_parse_category(jp_t *j, char *head) {
    char key[64], catname[24] = "";
    if (!jp_ch(j, '{')) return -1;
    if (jp_ch(j, '}')) return 0;
    for (;;) {
        jp_str(j, key, sizeof(key));
        if (jp_ch(j, ':')) {
            if (!strcmp(key, "name")) {
                jp_str(j, catname, sizeof(catname));
            } else if (!strcmp(key, "items")) {
                if (jp_ch(j, '[')) {
                    if (jp_cat_header(j, head, catname)) return -1;
                    if (!jp_ch(j, ']')) {
                        do {
                            jp_parse_item(j, head, NULL);
                        } while (jp_ch(j, ','));
                        jp_ch(j, ']');
                    }
                } else jp_skip(j);
            } else {
                jp_skip(j);
            }
        } else break;
        if (!jp_ch(j, ',')) break;
    }
    jp_ch(j, '}');
    return 0;
}

/* Parse one game item: { "name": ..., "bin": ..., "args": [...] }.
 * Item args are copied into the output pool FIRST (jp_str's tmp is
 * stack-scoped), then the item is emitted after the pool. */
static int jp_parse_item(jp_t *j, char *head, const char *bin_default) {
    char name[40] = "", bin[64] = "", tmp[64], *av;
    const char *argv[32];
    int nargv = 0, i;
    char key[64];

    if (!jp_ch(j, '{')) return -1;
    if (jp_ch(j, '}')) return 0;
    for (;;) {
        jp_str(j, key, sizeof(key));
        if (jp_ch(j, ':')) {
            if (!strcmp(key, "name")) {
                jp_str(j, name, sizeof(name));
            } else if (!strcmp(key, "bin")) {
                jp_str(j, bin, sizeof(bin));
            } else if (!strcmp(key, "args")) {
                if (jp_ch(j, '[')) {
                    while (!jp_ch(j, ']')) {
                        if (jp_str(j, tmp, sizeof(tmp))) {
                            unsigned n = (unsigned)strlen(tmp) + 1;
                            if (nargv >= 32 || j->out + n >= j->oend) break;
                            memcpy(j->out, tmp, n);
                            argv[nargv++] = j->out;
                            j->out += n;
                        }
                        if (!jp_ch(j, ',')) { jp_ch(j, ']'); break; }
                    }
                } else jp_skip(j);
            } else {
                jp_skip(j);
            }
        } else break;
        if (!jp_ch(j, ',')) break;
    }
    jp_ch(j, '}');

    if (!bin[0] && bin_default) strcpy(bin, bin_default);
    if (!name[0] || !bin[0]) return 0;      /* skip malformed item */

    if (jp_item_begin(j, head)) return -1;
    if (jp_arg(j, name)) return -1;
    av = j->out;
    j->out += 2;                            /* argc:u16 placeholder */
    if (jp_arg(j, bin)) return -1;          /* argv[0] = bin path */
    for (i = 0; i < j->nsys; i++)           /* system args */
        if (jp_arg(j, j->sys[i])) return -1;
    for (i = 0; i < nargv; i++)             /* item args (from the pool) */
        if (jp_arg(j, argv[i])) return -1;
    jp_item_end(j, 1 + (unsigned)j->nsys + (unsigned)nargv, av);
    return 0;
}

/* ---- entry point --------------------------------------------------------- */

/* Parse config.json from fi. Returns a malloc'd buffer holding the item
 * chain (same layout as readconf's parse_config()), or NULL on any error.
 * The caller owns the buffer (never freed - lives for the menu's lifetime).
 * The file is read with fgetc - the ONLY stdio call the fpdoom fatfile
 * layer is proven to support on the B310E (parse_config uses it too). */
static char *json_parse(FILE *fi, unsigned size) {
    jp_t j;
    char *src, *buf, key[64];
    const char *sys[16];
    int nsys = 0;
    unsigned fsize = 0, cap = 0x8000;
    int c;

    /* read the whole file into a scratch buffer, fgetc at a time */
    src = malloc(cap);
    if (!src) return NULL;
    while ((c = fgetc(fi)) != EOF) {
        if (fsize + 1 >= cap) {
            char *n = malloc(cap + 0x4000);
            if (!n) { free(src); return NULL; }
            memcpy(n, src, fsize);
            free(src);
            src = n;
            cap += 0x4000;
            if (cap > 0x10000) { free(src); return NULL; }
        }
        src[fsize++] = (char)c;
    }
    if (!fsize) { free(src); return NULL; }
    src[fsize] = 0;

    buf = malloc(size);
    if (!buf) {
        free(src);
        return NULL;
    }

    memset(&j, 0, sizeof(j));
    j.p = src; j.end = src + fsize;
    j.out = buf + 8; j.oend = buf + size;
    j.prev_next = NULL;

    /* root object */
    if (!jp_ch(&j, '{')) goto err;
    if (!jp_ch(&j, '}')) {
        for (;;) {
            jp_str(&j, key, sizeof(key));
            if (jp_ch(&j, ':')) {
                if (!strcmp(key, "system")) {
                    /* store system args in the output buffer itself so the
                     * pointers survive (jp_str's tmp is stack-scoped) */
                    char tmp[64];
                    if (jp_ch(&j, '[')) {
                        while (!jp_ch(&j, ']')) {
                            if (jp_str(&j, tmp, sizeof(tmp)) && nsys < 16) {
                                unsigned n = (unsigned)strlen(tmp) + 1;
                                if (j.out + n >= j.oend) break;
                                memcpy(j.out, tmp, n);
                                sys[nsys++] = j.out;
                                j.out += n;
                            }
                            if (!jp_ch(&j, ',')) { jp_ch(&j, ']'); break; }
                        }
                    } else jp_skip(&j);
                    j.sys = sys;
                    j.nsys = nsys;
                } else if (!strcmp(key, "categories")) {
                    if (jp_ch(&j, '[')) {
                        if (!jp_ch(&j, ']')) {
                            do {
                                jp_parse_category(&j, buf);
                            } while (jp_ch(&j, ','));
                            jp_ch(&j, ']');
                        }
                    } else jp_skip(&j);
                } else {
                    jp_skip(&j);
                }
            } else break;
            if (!jp_ch(&j, ',')) break;
        }
        jp_ch(&j, '}');
    }

    /* close the chain */
    if (j.prev_next) *(uint32_t *)j.prev_next = 0;

    free(src);
    return buf;
err:
    free(src);
    free(buf);
    return NULL;
}
