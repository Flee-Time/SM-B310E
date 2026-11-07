/* B310E OS shim: <ctype.h> without newlib's locale tables (ASCII only).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef B310E_RB_CTYPE_H
#define B310E_RB_CTYPE_H
static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int isalpha(int c) { return isupper(c) || islower(c); }
static inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
static inline int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
static inline int isxdigit(int c) { return isdigit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f'); }
static inline int isprint(int c) { return c >= 0x20 && c < 0x7f; }
static inline int toupper(int c) { return islower(c) ? c - 0x20 : c; }
static inline int tolower(int c) { return isupper(c) ? c + 0x20 : c; }
#endif
