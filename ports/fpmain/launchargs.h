/* Packed argv for the fpdoom loader, without text-config variables. */
#ifndef FPMAIN_LAUNCHARGS_H
#define FPMAIN_LAUNCHARGS_H
typedef struct { int argc, skip; char *end; } extract_args_t;
static char *get_first_arg(char *s)
{
    return ((unsigned char)s[0] | (unsigned char)s[1] << 8) ? s + 2 : NULL;
}
static char *extract_args(extract_args_t *x, char *s, char *d)
{
    unsigned n = (unsigned char)s[0] | (unsigned char)s[1] << 8;
    s += 2;
    for (unsigned i = 0; i < n; i++) {
        unsigned len = strlen(s) + 1;
        if (x->skip) x->skip--;
        else {
            if ((unsigned)(x->end - d) < len) return NULL;
            memcpy(d, s, len); d += len; x->argc++;
        }
        s += len;
    }
    return d;
}
#endif
