/* JSON-only menu configuration. Bounded input, nesting and output.
 * A malformed file never produces a partial menu. */
#ifndef FPMAIN_JSONCONF_H
#define FPMAIN_JSONCONF_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JC_INPUT_MAX 65536u
#define JC_TOKENS 2048u
#define JC_ARGS 48u
#define JC_CATEGORIES 8u
#define JC_ITEMS 64u
#define JC_TOTAL 256u
typedef struct { char *text; unsigned next; char type; } jc_token;
typedef struct { char *p, *end; jc_token *tok; unsigned n; } jc_parser;
/* Enumerate regular filenames, stopping when emit returns 1. Missing
 * directories are empty; negative returns indicate an enumeration error. */
typedef int (*json_scan_fn)(void *, const char *, int (*)(void *, const char *), void *);

static void jc_ws(jc_parser *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' ||
           *j->p == '\r' || *j->p == '\n')) j->p++;
}
static int jc_hex(jc_parser *j)
{
    unsigned value = 0;
    if (j->end - j->p < 4) return -1;
    for (unsigned i = 0; i < 4; i++) {
        unsigned c = (unsigned char)*j->p++, digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return -1;
        value = value * 16 + digit;
    }
    return (int)value;
}
static int jc_string(jc_parser *j, char **out)
{
    char *d;
    if (j->p == j->end || *j->p++ != '"') return -1;
    *out = d = j->p;
    while (j->p < j->end) {
        unsigned c = (unsigned char)*j->p++;
        if (c == '"') { *d = 0; return 0; }
        if (c < 0x20) return -1;
        if (c == '\\') {
            if (j->p == j->end) return -1;
            c = (unsigned char)*j->p++;
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                int code = jc_hex(j);
                if (code <= 0) return -1; /* NUL cannot be packed into argv */
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (j->end - j->p < 6 || j->p[0] != '\\' || j->p[1] != 'u') return -1;
                    j->p += 2;
                    int low = jc_hex(j);
                    if (low < 0xdc00 || low > 0xdfff) return -1;
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) return -1;
                if (code < 0x80) c = code;
                else {
                    if (code >= 0x10000) *d++ = 0xf0 | (code >> 18);
                    if (code >= 0x800) *d++ = (code < 0x10000 ? 0xe0 : 0x80) | ((code >> 12) & 0x3f);
                    *d++ = (code < 0x800 ? 0xc0 : 0x80) | ((code >> 6) & 0x3f);
                    c = 0x80 | (code & 0x3f);
                }
                break;
            }
            default: return -1;
            }
        }
        *d++ = (char)c;
        if (d - *out > 255) return -1;
    }
    return -1;
}
static int jc_value(jc_parser *j, unsigned depth)
{
    jc_ws(j);
    if (depth > 16 || j->n == JC_TOKENS || j->p == j->end) return -1;
    unsigned index = j->n++;
    jc_token *t = &j->tok[index];
    t->type = *j->p;
    if (t->type == '"') {
        if (jc_string(j, &t->text)) return -1;
    } else if (t->type == '{' || t->type == '[') {
        char close = t->type == '{' ? '}' : ']';
        j->p++; jc_ws(j);
        if (j->p < j->end && *j->p == close) j->p++;
        else for (;;) {
            if (t->type == '{') {
                unsigned key = j->n;
                if (jc_value(j, depth + 1) || j->tok[key].type != '"') return -1;
                for (unsigned prev = index + 1; prev < key; prev = j->tok[prev + 1].next)
                    if (!strcmp(j->tok[prev].text, j->tok[key].text)) return -1;
                jc_ws(j);
                if (j->p == j->end || *j->p++ != ':') return -1;
            }
            if (jc_value(j, depth + 1)) return -1;
            jc_ws(j);
            if (j->p == j->end) return -1;
            if (*j->p == close) { j->p++; break; }
            if (*j->p++ != ',') return -1;
        }
    } else {
        char *start = j->p;
        if (*j->p == '-' || (*j->p >= '0' && *j->p <= '9')) {
            if (*j->p == '-') j->p++;
            if (j->p == j->end) return -1;
            if (*j->p == '0') j->p++;
            else {
                if (*j->p < '1' || *j->p > '9') return -1;
                do j->p++; while (j->p < j->end && *j->p >= '0' && *j->p <= '9');
            }
            if (j->p < j->end && *j->p == '.') {
                j->p++;
                if (j->p == j->end || *j->p < '0' || *j->p > '9') return -1;
                do j->p++; while (j->p < j->end && *j->p >= '0' && *j->p <= '9');
            }
            if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
                j->p++;
                if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
                if (j->p == j->end || *j->p < '0' || *j->p > '9') return -1;
                do j->p++; while (j->p < j->end && *j->p >= '0' && *j->p <= '9');
            }
        } else {
            const char *literal = *start == 't' ? "true" : *start == 'f' ? "false" : "null";
            unsigned length = strlen(literal);
            if ((unsigned)(j->end - start) < length || memcmp(start, literal, length)) return -1;
            j->p += length;
        }
    }
    t->next = j->n;
    return 0;
}
static int jc_get(jc_token *t, unsigned obj, const char *key)
{
    if (t[obj].type != '{') return -1;
    for (unsigned i = obj + 1; i < t[obj].next; i = t[i + 1].next)
        if (!strcmp(t[i].text, key)) return (int)i + 1;
    return -1;
}
static const char *jc_text(jc_token *t, int index)
{
    return index >= 0 && t[index].type == '"' ? t[index].text : NULL;
}
static int jc_args(jc_token *t, int index, const char **args, unsigned *n)
{
    if (index < 0) return 0;
    if (t[index].type != '[') return -1;
    for (unsigned i = index + 1; i < t[index].next; i = t[i].next) {
        if (t[i].type != '"' || *n == JC_ARGS) return -1;
        args[(*n)++] = t[i].text;
    }
    return 0;
}
typedef struct {
    jc_token *tok;
    char *buf, *out, *end, *prev;
    const char *sys[JC_ARGS];
    unsigned nsys, total;
} jc_menu;

/* entry.c needs system options before sys_init/main. Missing or invalid
 * JSON falls back to framework defaults; main later reports CONFIG ERROR. */
static inline int json_boot_args(FILE *fi, char *dest, unsigned capacity)
{
    char *src = malloc(JC_INPUT_MAX + 1);
    jc_token *tokens = calloc(JC_TOKENS, sizeof(*tokens));
    unsigned length = 0, count = 0, used = 0;
    const char *args[JC_ARGS];
    int c, result = -1;
    if (!src || !tokens) goto done;
    while ((c = fgetc(fi)) != EOF) {
        if (length == JC_INPUT_MAX || !c) goto done;
        src[length++] = (char)c;
    }
    src[length] = 0;
    jc_parser parser = { src, src + length, tokens, 0 };
    if (length >= 3 && !memcmp(src, "\xef\xbb\xbf", 3)) parser.p += 3;
    if (jc_value(&parser, 0) || tokens[0].type != '{') goto done;
    jc_ws(&parser);
    if (parser.p != parser.end ||
        jc_args(tokens, jc_get(tokens, 0, "system"), args, &count)) goto done;
    for (unsigned i = 0; i < count; i++) used += strlen(args[i]) + 1;
    if (used > capacity) goto done;
    for (unsigned i = 0; i < count; i++) {
        unsigned n = strlen(args[i]) + 1;
        memcpy(dest, args[i], n); dest += n;
    }
    result = count;
done:
    free(tokens); free(src);
    return result;
}
static int jc_emit(jc_menu *m, const char *name, const char **args, unsigned n)
{
    char *p = (char *)(((uintptr_t)m->out + 3) & ~(uintptr_t)3);
    unsigned length = 4 + strlen(name) + 1 + 2;
    for (unsigned i = 0; i < n; i++) length += strlen(args[i]) + 1;
    if (p > m->end || (unsigned)(m->end - p) < length) return -1;
    *(uint32_t *)m->prev = (uint32_t)(p - m->prev);
    m->prev = p; *(uint32_t *)p = 0; p += 4;
    strcpy(p, name); p += strlen(name) + 1;
    *p++ = (char)n; *p++ = (char)(n >> 8);
    for (unsigned i = 0; i < n; i++) { strcpy(p, args[i]); p += strlen(args[i]) + 1; }
    m->out = p;
    return 0;
}
static int jc_header(jc_menu *m, const char *name)
{
    char header[48];
    if (!name || !*name || strlen(name) > 39) return -1;
    strcpy(header, "=== "); strcat(header, name); strcat(header, " ===");
    return jc_emit(m, header, NULL, 0);
}
static int jc_launch_args(jc_menu *m, unsigned spec, const char **args, unsigned *n)
{
    const char *bin = jc_text(m->tok, jc_get(m->tok, spec, "bin"));
    if (!bin || !*bin || m->nsys >= JC_ARGS) return -1;
    args[(*n)++] = bin;
    for (unsigned i = 0; i < m->nsys; i++) args[(*n)++] = m->sys[i];
    return jc_args(m->tok, jc_get(m->tok, spec, "args"), args, n);
}
static int jc_equal_extension(const char *a, const char *b)
{
    if (*b == '.') b++;
    for (;;) {
        unsigned x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
        if (!x) return 1;
    }
}
typedef struct { jc_menu *menu; unsigned spec, count; int extensions, error; } jc_scan;
static int jc_rom(void *opaque, const char *name)
{
    jc_scan *s = opaque;
    jc_menu *m = s->menu;
    const char *dot = strrchr(name, '.');
    const char *args[JC_ARGS];
    unsigned n = 0;
    int match = 0;
    if (!dot || dot == name || !dot[1]) return 0;
    for (unsigned i = s->extensions + 1; i < m->tok[s->extensions].next; i++)
        if (jc_equal_extension(dot + 1, m->tok[i].text)) match = 1;
    if (!match) return 0;
    if (s->count == JC_ITEMS || m->total == JC_TOTAL) return 1;
    if (jc_launch_args(m, s->spec, args, &n) || n == JC_ARGS) goto error;
    args[n++] = name; /* Keep the complete filename, including spaces. */
    if (jc_emit(m, name, args, n)) goto error;
    s->count++; m->total++;
    return 0;
error:
    s->error = 1;
    return 1;
}
static inline char *json_parse(FILE *fi, unsigned size, json_scan_fn scan, void *opaque)
{
    char *src = malloc(JC_INPUT_MAX + 1), *buf = NULL;
    jc_token *tokens = calloc(JC_TOKENS, sizeof(*tokens));
    unsigned length = 0, categories = 0;
    int c;
    if (!src || !tokens || size < 8) goto error;
    while ((c = fgetc(fi)) != EOF) {
        if (length == JC_INPUT_MAX || !c) goto error;
        src[length++] = (char)c;
    }
    src[length] = 0;
    jc_parser parser = { src, src + length, tokens, 0 };
    if (length >= 3 && !memcmp(src, "\xef\xbb\xbf", 3)) parser.p += 3;
    if (jc_value(&parser, 0) || tokens[0].type != '{') goto error;
    jc_ws(&parser);
    if (parser.p != parser.end) goto error;
    buf = calloc(1, size);
    if (!buf) goto error;
    jc_menu menu = { tokens, buf, buf + 8, buf + size, buf, {0}, 0, 0 };
    if (jc_args(tokens, jc_get(tokens, 0, "system"), menu.sys, &menu.nsys)) goto error;
    const char *lists[] = { "categories", "emulators" };
    for (unsigned list = 0; list < 2; list++) {
        int array = jc_get(tokens, 0, lists[list]);
        if (array < 0) continue;
        if (tokens[array].type != '[') goto error;
        for (unsigned cat = array + 1; cat < tokens[array].next; cat = tokens[cat].next) {
            if (++categories > JC_CATEGORIES ||
                jc_header(&menu, jc_text(tokens, jc_get(tokens, cat, "name")))) goto error;
            if (!list) {
                int items = jc_get(tokens, cat, "items");
                unsigned count = 0;
                if (items < 0 || tokens[items].type != '[') goto error;
                for (unsigned item = items + 1; item < tokens[items].next; item = tokens[item].next) {
                    const char *args[JC_ARGS];
                    unsigned n = 0;
                    const char *name = jc_text(tokens, jc_get(tokens, item, "name"));
                    if (!name || !*name || ++count > JC_ITEMS || menu.total == JC_TOTAL ||
                        jc_launch_args(&menu, item, args, &n) || jc_emit(&menu, name, args, n)) goto error;
                    menu.total++;
                }
            } else {
                const char *dir = jc_text(tokens, jc_get(tokens, cat, "directory"));
                int exts = jc_get(tokens, cat, "extensions");
                const char *args[JC_ARGS]; unsigned n = 0;
                if (!dir || !*dir || exts < 0 || tokens[exts].type != '[' ||
                    tokens[exts].next == (unsigned)exts + 1 || jc_launch_args(&menu, cat, args, &n)) goto error;
                for (unsigned i = exts + 1; i < tokens[exts].next; i++)
                    if (tokens[i].type != '"' || !*tokens[i].text) goto error;
                jc_scan state = { &menu, cat, 0, exts, 0 };
                if (scan && (scan(opaque, dir, jc_rom, &state) < 0 || state.error)) goto error;
            }
        }
    }
    free(tokens); free(src);
    return buf;
error:
    free(buf); free(tokens); free(src);
    return NULL;
}
#endif
