// État de locale invité partagé entre ctype_locale.c, wchar.c et time.c.
//
// Seules les locales glibc "C" (alias "POSIX") et "C.UTF-8" (alias "C.utf8")
// sont disponibles. Elles ne diffèrent que par le jeu de caractères de
// LC_CTYPE : ASCII strict (ANSI_X3.4-1968) ou UTF-8. Leurs tables à un octet,
// leurs chaînes LC_TIME/LC_NUMERIC/LC_MONETARY et leur collation (ordre des
// octets/points de code) sont identiques.
#pragma once
#include "rt.h"

#define RT_LC_GLOBAL_LOCALE 0xffffffffu

// 1 si la locale LC_CTYPE effective du thread (uselocale, sinon globale) est UTF-8.
int rt_locale_ctype_utf8(void);
// Idem pour un locale_t invité (RT_LC_GLOBAL_LOCALE accepté) ; trap si inconnu.
int rt_locale_object_utf8(CPU *c, uint32_t locale);
// Classes larges glibc : 1 alnum, 2 alpha, 3 blank, 4 cntrl, 5 digit, 6 graph,
// 7 lower, 8 print, 9 punct, 10 space, 11 upper, 12 xdigit.
enum { RT_WC_ALNUM = 1, RT_WC_ALPHA, RT_WC_BLANK, RT_WC_CNTRL, RT_WC_DIGIT, RT_WC_GRAPH,
       RT_WC_LOWER, RT_WC_PRINT, RT_WC_PUNCT, RT_WC_SPACE, RT_WC_UPPER, RT_WC_XDIGIT };
int rt_wclass(uint32_t wc, int cls, int utf8);
uint32_t rt_wcase(uint32_t wc, int upper, int utf8);
// Formatage strftime en locale C (unités : octets ou wchar_t de 4 octets).
// Renvoie le nombre d'unités écrites sans le NUL, 0 si `max` est insuffisant.
uint32_t rt_strftime(CPU *c, uint32_t out, uint32_t max, uint32_t format, uint32_t tm, int wide);
