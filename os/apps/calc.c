/* B310E OS — Calculator.
 *
 * Phone keys: the digits, # the decimal point, * the sign; the D-pad
 * Up + Down - Left x Right /, Center =. LSoft deletes a digit (with none
 * left it clears all), RSoft goes back. Each operator works on the result
 * so far (a pocket calculator, no precedence). Soft-float doubles
 * (libgcc); shown to 9 significant digits.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/drivers.h"
#include "os/lib.h"
#include "os/ui.h"

#define ENTRY_MAX 12                    /* characters typed, sign and point too */
#define LIMIT     1e12                  /* beyond: "Overflow" */

static double calc_acc;                      /* the result so far */
static char op;                         /* pending: + - x /, or 0 */
static char calc_entry[ENTRY_MAX + 1];       /* the number being typed */
static bool have_acc, calc_error;

/* v with up to 9 significant digits, no trailing zeros; false if too big */
static bool format(double v, char *out, int n)
{
    uint64_t scaled, ip, scale = 1;
    int intdigits = 1, dec;
    bool neg = v < 0;
    char frac[12];
    int fl;

    if (neg)
        v = -v;
    if (v >= LIMIT || v != v)
        return false;
    for (double t = v; t >= 10.0 && intdigits < 12; t /= 10.0)
        intdigits++;
    dec = 9 - intdigits;
    if (dec < 0)
        dec = 0;
    for (int i = 0; i < dec; i++)
        scale *= 10;
    scaled = (uint64_t)(v * (double)scale + 0.5);
    ip = scaled / scale;
    fl = 0;
    if (dec) {
        uint64_t f = scaled % scale;

        for (int i = dec - 1; i >= 0; i--) {
            frac[i] = (char)('0' + f % 10);
            f /= 10;
        }
        fl = dec;
        while (fl && frac[fl - 1] == '0')
            fl--;
    }
    frac[fl] = 0;
    if (ip == 0 && fl == 0)
        neg = false;                    /* no "-0" */
    snprintf(out, (size_t)n, "%s%lu%s%s", neg ? "-" : "", (unsigned long)ip,
             fl ? "." : "", frac);
    return true;
}

/* the typed calc_entry as a number (strtod is not in our lib) */
static double entry_value(void)
{
    double v = 0, scale = 1;
    bool neg = calc_entry[0] == '-', point = false;

    for (const char *p = calc_entry + neg; *p; p++) {
        if (*p == '.') {
            point = true;
        } else {
            v = v * 10 + (*p - '0');
            if (point)
                scale *= 10;
        }
    }
    v /= scale;
    return neg ? -v : v;
}

static void apply(void)
{
    double b = entry_value();

    if (!have_acc || !op) {
        calc_acc = b;
    } else {
        switch (op) {
        case '+':
            calc_acc += b;
            break;
        case '-':
            calc_acc -= b;
            break;
        case 'x':
            calc_acc *= b;
            break;
        case '/':
            if (b == 0) {
                calc_error = true;
                return;
            }
            calc_acc /= b;
            break;
        }
    }
    have_acc = true;
    if (calc_acc >= LIMIT || calc_acc <= -LIMIT)
        calc_error = true;
}

static void clear_all(void)
{
    calc_acc = 0;
    op = 0;
    calc_entry[0] = 0;
    have_acc = calc_error = false;
}

static void draw(screen_t *s)
{
    char line[24], big[24];
    const int x = UI_PAD, w = LCD_WIDTH - 2 * UI_PAD;
    int font = FONT_HUGE, y;

    /* the display: one card, the pending operation small above the number */
    gfx_round(x, UI_TOP + 4, w, 50, UI_RADIUS, C_CARD);
    line[0] = 0;
    if (have_acc && op && format(calc_acc, big, sizeof(big)))
        snprintf(line, sizeof(line), "%s %c", big, op);
    gfx_text(LCD_WIDTH - UI_PAD - 4 - gfx_text_width(line, FONT_SMALL, 1), UI_TOP + 9,
             line, C_DIM, FONT_SMALL, 1, 0);

    if (calc_error) {
        snprintf(big, sizeof(big), "Error");
        font = FONT_LARGE;
    } else if (calc_entry[0]) {
        snprintf(big, sizeof(big), "%s", calc_entry);
    } else if (!have_acc || !format(calc_acc, big, sizeof(big))) {
        snprintf(big, sizeof(big), "0");
    }
    if (font == FONT_HUGE && gfx_text_width(big, FONT_HUGE, 1) > w - 8)
        font = FONT_LARGE;
    y = UI_TOP + 4 + 50 - 8 - gfx_text_height(font, 1);
    gfx_text(LCD_WIDTH - UI_PAD - 4 - gfx_text_width(big, font, 1), y, big,
             calc_error ? C_BAD : C_TEXT, font, 1, 0);

    /* the key map */
    y = UI_TOP + 62;
    gfx_text(x + 2, y, "Up +   Down -", C_DIM, FONT_SMALL, 1, 0);
    gfx_text(x + 2, y + 12, "Left x   Right /", C_DIM, FONT_SMALL, 1, 0);
    gfx_text(x + 2, y + 24, "# point   * sign", C_DIM, FONT_SMALL, 1, 0);
    gfx_text(x + 2, y + 36, "OK =", C_DIM, FONT_SMALL, 1, 0);
}

static void key(screen_t *s, const key_event_t *ev)
{
    int n = (int)strlen(calc_entry);
    char next_op = 0;

    if (ev->code == KEY_RSOFT) {
        ui_pop();
        return;
    }
    if (calc_error && ev->code != KEY_LSOFT) {
        if (ev->code < KEY_0 || ev->code > KEY_9)
            return;
        clear_all();                    /* a digit starts over */
        n = 0;
    }
    switch (ev->code) {
    case KEY_0: case KEY_1: case KEY_2: case KEY_3: case KEY_4:
    case KEY_5: case KEY_6: case KEY_7: case KEY_8: case KEY_9:
        if (have_acc && !op)            /* after =, a digit starts anew */
            clear_all(), n = 0;
        if (n == 1 && calc_entry[0] == '0')  /* no leading zeros */
            n = 0;
        else if (n == 2 && calc_entry[0] == '-' && calc_entry[1] == '0')
            n = 1;
        if (n < ENTRY_MAX) {
            calc_entry[n] = (char)ev->code;
            calc_entry[n + 1] = 0;
        }
        break;
    case KEY_HASH:
        if (have_acc && !op)
            clear_all(), n = 0;
        if (!strchr(calc_entry, '.') && n < ENTRY_MAX - 1) {
            if (n == 0 || (n == 1 && calc_entry[0] == '-'))
                calc_entry[n++] = '0';
            calc_entry[n] = '.';
            calc_entry[n + 1] = 0;
        }
        break;
    case KEY_STAR:
        if (n) {
            if (calc_entry[0] == '-')
                memmove(calc_entry, calc_entry + 1, (size_t)n);
            else if (n < ENTRY_MAX) {
                memmove(calc_entry + 1, calc_entry, (size_t)n + 1);
                calc_entry[0] = '-';
            }
        } else if (have_acc && !op) {
            calc_acc = -calc_acc;                 /* the result's sign */
        } else {
            snprintf(calc_entry, sizeof(calc_entry), "-");
        }
        break;
    case KEY_UP:
        next_op = '+';
        break;
    case KEY_DOWN:
        next_op = '-';
        break;
    case KEY_LEFT:
        next_op = 'x';
        break;
    case KEY_RIGHT:
        next_op = '/';
        break;
    case KEY_CENTER:
        if (calc_entry[0] && strcmp(calc_entry, "-")) {
            apply();
            calc_entry[0] = 0;
        }
        op = 0;
        break;
    case KEY_LSOFT:
        if (calc_error)
            clear_all();
        else if (n)
            calc_entry[n - 1] = 0;
        else
            clear_all();
        break;
    default:
        return;
    }
    if (next_op) {
        if (calc_entry[0] && strcmp(calc_entry, "-")) {
            apply();
            calc_entry[0] = 0;
        } else if (!have_acc) {
            have_acc = true;            /* an operator first: on 0 */
            calc_acc = 0;
        }
        if (!calc_error)
            op = next_op;
    }
    ui_redraw();
}

static screen_t screen = {
    .title = "Calculator", .left = "Del", .center = "=", .right = "Back",
    .draw = draw, .key = key,
};

screen_t *app_calc(void)
{
    clear_all();
    return &screen;
}
