// ctype, locales, caractères larges, multioctets, iconv et gettext glibc i386.
#include "checks.h"
#include "rt_locale.h"
#include <stdarg.h>
#include <stdlib.h>

#define STACK 0x2001f000u
#define G 0x20012000u   // zone de données des tests (jusqu'à 0x2001e000)
static CPU cpu;
static uint32_t call(const char *name, int count, ...) {
    rt_cpu_reset(&cpu); cpu.esp = STACK;
    va_list list; va_start(list, count);
    for (int i = 0; i < count; ++i) W32(STACK + 4 + 4 * (uint32_t)i, va_arg(list, uint32_t));
    va_end(list);
    assert(rt_shim(&cpu, name));
    return cpu.eax;
}
static uint32_t guest_errno(void) { return R32(call("__errno_location", 0)); }
static uint32_t put_string(uint32_t at, const char *s) { memcpy(GPTR(at), s, strlen(s) + 1); return at; }
static uint32_t put_wide(uint32_t at, const uint32_t *s) {
    uint32_t i = 0; do W32(at + 4 * i, s[i]); while (s[i++]);
    return at;
}
static int guest_equals(uint32_t at, const char *s) { return at && !strcmp(GPTR(at), s); }
static void global(const char *name) { assert(guest_equals(call("setlocale", 2, 6, put_string(G + 0x3f00, name)), name)); }

RT_CHECK(ctype_tables_use_glibc_layout) {
    uint32_t b = R32(call("__ctype_b_loc", 0));
    assert(b && b == R32(call("__ctype_b_loc", 0)));
    assert(R16(b + 2 * 'A') == 0xd508 && R16(b + 2 * 'a') == 0xd608 && R16(b + 2 * '0') == 0xd808);
    assert(R16(b + 2 * ' ') == 0x6001 && R16(b + 2 * '\t') == 0x2003 && R16(b + 2 * '!') == 0xc004);
    assert(R16(b + 2 * 127) == 0x0002 && R16(b - 2) == 0 && R16(b + 2 * 0xe9) == 0 && R16(b - 256) == 0);
    uint32_t lower = R32(call("__ctype_tolower_loc", 0)), upper = R32(call("__ctype_toupper_loc", 0));
    assert(R32(lower + 4 * 'A') == 'a' && R32(lower + 4 * 'a') == 'a' && R32(upper + 4 * 'z') == 'Z');
    assert(R32(upper + 4 * 0xe9) == 0xe9 && (int32_t)R32(lower - 4 * 128) == -128 && (int32_t)R32(lower - 4) == -1);
    assert(call("isalpha", 1, 'q') && !call("isalpha", 1, 0xe9) && !call("isalpha", 1, (uint32_t)-1));
    assert(call("isxdigit", 1, 'F') && !call("isxdigit", 1, 'g') && call("ispunct", 1, '~'));
    assert(call("isspace", 1, '\v') && !call("isprint", 1, '\n') && call("isgraph", 1, '#') && !call("isgraph", 1, ' '));
    assert(call("iscntrl", 1, 0) && call("isupper", 1, 'Q') && call("islower", 1, 'q') && call("isalnum", 1, '7'));
    assert(call("__ctype_get_mb_cur_max", 0) == 1);
}

RT_CHECK(setlocale_offers_only_c_and_c_utf8) {
    assert(guest_equals(call("setlocale", 2, 6, 0), "C"));
    uint32_t utf8 = put_string(G, "C.UTF-8");
    assert(guest_equals(call("setlocale", 2, 0, utf8), "C.UTF-8"));
    assert(call("__ctype_get_mb_cur_max", 0) == 6);
    uint32_t all = call("setlocale", 2, 6, 0);
    assert(guest_equals(all, "LC_CTYPE=C.UTF-8;LC_NUMERIC=C;LC_TIME=C;LC_COLLATE=C;LC_MONETARY=C;LC_MESSAGES=C;"
                             "LC_PAPER=C;LC_NAME=C;LC_ADDRESS=C;LC_TELEPHONE=C;LC_MEASUREMENT=C;LC_IDENTIFICATION=C"));
    // Locale non installée : NULL, état inchangé.
    assert(!call("setlocale", 2, 6, put_string(G + 0x100, "fr_FR.UTF-8")) && guest_errno() == 2);
    assert(guest_equals(call("setlocale", 2, 0, 0), "C.UTF-8"));
    assert(!call("setlocale", 2, 13, utf8) && guest_errno() == 22);
    // Nom composite renvoyé par une requête : réutilisable pour restaurer.
    uint32_t saved = put_string(G + 0x200, GPTR(all));
    assert(guest_equals(call("setlocale", 2, 6, put_string(G + 0x100, "POSIX")), "C"));
    assert(guest_equals(call("setlocale", 2, 6, saved), GPTR(saved)) && call("__ctype_get_mb_cur_max", 0) == 6);
    // "" : LC_ALL, puis LC_<catégorie>, puis LANG.
    char *old_all = getenv("LC_ALL") ? strdup(getenv("LC_ALL")) : NULL, *old_lang = getenv("LANG") ? strdup(getenv("LANG")) : NULL;
    char *old_ctype = getenv("LC_CTYPE") ? strdup(getenv("LC_CTYPE")) : NULL;
    unsetenv("LC_ALL"); unsetenv("LC_CTYPE"); setenv("LANG", "C", 1);
    uint32_t empty = put_string(G + 0x100, "");
    assert(guest_equals(call("setlocale", 2, 6, empty), "C") && call("__ctype_get_mb_cur_max", 0) == 1);
    setenv("LC_CTYPE", "C.utf8", 1);
    assert(guest_equals(call("setlocale", 2, 0, empty), "C.utf8") && call("__ctype_get_mb_cur_max", 0) == 6);
    setenv("LC_ALL", "de_DE.UTF-8", 1);
    assert(!call("setlocale", 2, 6, empty) && guest_equals(call("setlocale", 2, 0, 0), "C.utf8"));
    if (old_all) setenv("LC_ALL", old_all, 1); else unsetenv("LC_ALL");
    if (old_ctype) setenv("LC_CTYPE", old_ctype, 1); else unsetenv("LC_CTYPE");
    if (old_lang) setenv("LANG", old_lang, 1); else unsetenv("LANG");
    free(old_all); free(old_ctype); free(old_lang);
    global("C");
    uint32_t lconv = call("localeconv", 0);
    assert(guest_equals(R32(lconv), ".") && guest_equals(R32(lconv + 4), "") && guest_equals(R32(lconv + 36), ""));
    assert(R8(lconv + 40) == 127 && R8(lconv + 53) == 127);
}

RT_CHECK(locale_objects_follow_glibc_struct) {
    global("C");
    uint32_t c_name = put_string(G, "C"), utf8 = put_string(G + 0x10, "C.UTF-8");
    uint32_t loc = call("__newlocale", 3, 1u << 6, c_name, 0);   // 1 << LC_ALL = LC_ALL_MASK
    assert(loc && loc < RT_HANDLE_BASE);
    uint32_t b = R32(call("__ctype_b_loc", 0)), lower = R32(call("__ctype_tolower_loc", 0));
    assert(R32(loc + 52) == b && R32(loc + 56) == lower && R32(loc + 60) == R32(call("__ctype_toupper_loc", 0)));
    assert(guest_equals(R32(loc + 64), "C") && guest_equals(R32(loc + 64 + 4 * 12), "C"));
    assert(call("__newlocale", 3, 1, utf8, loc) == loc && guest_equals(R32(loc + 64), "C.UTF-8") && guest_equals(R32(loc + 68), "C"));
    assert(guest_equals(call("__nl_langinfo_l", 2, 14, loc), "UTF-8"));
    assert(guest_equals(call("__nl_langinfo_l", 2, 14, RT_LC_GLOBAL_LOCALE), "ANSI_X3.4-1968"));
    assert(guest_equals(call("__nl_langinfo_l", 2, 0x20007, loc), "Sunday") && guest_equals(call("__nl_langinfo_l", 2, 0x2000e, loc), "Jan"));
    assert(guest_equals(call("__nl_langinfo_l", 2, 0x20028, loc), "%a %b %e %H:%M:%S %Y"));
    uint32_t wide = call("__nl_langinfo_l", 2, 0x2005c, loc);   // _NL_WD_T_FMT
    assert(R32(wide) == '%' && R32(wide + 4) == 'a' && R32(wide + 4 * 20) == 0 && R32(wide + 4 * 19) == 'Y');
    assert(call("__nl_langinfo_l", 2, 0x10003, loc) == '.' && call("__nl_langinfo_l", 2, 0x10004, loc) == 0);
    assert(guest_equals(call("__nl_langinfo_l", 2, 0x40007, loc), "\377") && guest_equals(call("__nl_langinfo_l", 2, 0x4000f, loc), "-"));
    assert(call("__nl_langinfo_l", 2, 13, loc) == 6);
    // uselocale : par thread, n'affecte pas la locale globale.
    assert(call("__uselocale", 1, loc) == RT_LC_GLOBAL_LOCALE && call("__ctype_get_mb_cur_max", 0) == 6);
    assert(call("__uselocale", 1, 0) == loc);
    assert(guest_equals(call("setlocale", 2, 0, 0), "C"));
    assert(call("__uselocale", 1, RT_LC_GLOBAL_LOCALE) == loc && call("__ctype_get_mb_cur_max", 0) == 1);
    uint32_t copy = call("__duplocale", 1, loc);
    assert(copy && copy != loc && guest_equals(R32(copy + 64), "C.UTF-8"));
    uint32_t global_copy = call("__duplocale", 1, RT_LC_GLOBAL_LOCALE);
    assert(guest_equals(R32(global_copy + 64), "C"));
    // Comparaisons et classes larges.
    uint32_t alpha = call("__wctype_l", 2, put_string(G + 0x20, "alpha"), loc);
    assert(alpha && !call("__wctype_l", 2, put_string(G + 0x20, "bogus"), loc));
    assert(call("__iswctype_l", 3, 'x', alpha, global_copy) && !call("__iswctype_l", 3, 0xe9, alpha, global_copy));
    assert(call("__iswctype_l", 3, 0xe9, alpha, loc) && !call("__iswctype_l", 3, '1', alpha, loc));
    assert(call("__towupper_l", 2, 0xe9, loc) == 0xc9 && call("__towupper_l", 2, 0xe9, global_copy) == 0xe9);
    assert(call("__towlower_l", 2, 'Q', global_copy) == 'q');
    uint32_t abc = put_string(G + 0x30, "abc"), abd = put_string(G + 0x40, "abd");
    assert((int32_t)call("strcoll", 2, abc, abd) == -1 && call("__strcoll_l", 3, abd, abd, loc) == 0);
    memset(GPTR(G + 0x50), 'X', 8);
    assert(call("__strxfrm_l", 4, G + 0x50, put_string(G + 0x60, "hello"), 3, loc) == 5 && !memcmp(GPTR(G + 0x50), "helX", 4));
    assert(call("__strxfrm_l", 4, G + 0x50, G + 0x60, 9, loc) == 5 && guest_equals(G + 0x50, "hello"));
    // Erreurs glibc.
    assert(!call("__newlocale", 3, 1, put_string(G + 0x20, "de_DE"), 0) && guest_errno() == 2);
    assert(!call("__newlocale", 3, 1u << 13, c_name, 0) && guest_errno() == 22);
    call("__freelocale", 1, copy); call("__freelocale", 1, global_copy); call("__freelocale", 1, loc);
}

RT_CHECK(gettext_without_catalogs_returns_msgid) {
    uint32_t message = put_string(G, "New Game"), domain = put_string(G + 0x20, "portal2");
    assert(call("gettext", 1, message) == message && call("dgettext", 2, domain, message) == message);
    assert(call("dcgettext", 3, domain, message, 5) == message);
    W32(call("__errno_location", 0), 4);
    assert(call("gettext", 1, message) == message && guest_errno() == 4);
    assert(guest_equals(call("bindtextdomain", 2, domain, 0), "/usr/share/locale"));
    uint32_t bound = call("bindtextdomain", 2, domain, put_string(G + 0x40, "/game/locale"));
    assert(guest_equals(bound, "/game/locale") && bound != G + 0x40 && call("bindtextdomain", 2, domain, 0) == bound);
    assert(!call("bind_textdomain_codeset", 2, domain, 0));
    assert(guest_equals(call("bind_textdomain_codeset", 2, domain, put_string(G + 0x60, "UTF-8")), "UTF-8"));
    assert(guest_equals(call("textdomain", 1, 0), "messages") && guest_equals(call("textdomain", 1, domain), "portal2"));
    assert(guest_equals(call("textdomain", 1, put_string(G + 0x80, "")), "messages"));
}

RT_CHECK(wide_strings_are_signed_utf32) {
    static const uint32_t hello[] = {'h', 'e', 'l', 'l', 'o', 0}, lo[] = {'l', 'o', 0}, neg[] = {0x80000000u, 0}, one[] = {1, 0};
    uint32_t s = put_wide(G, hello), t = put_wide(G + 0x100, lo);
    assert(call("wcslen", 1, s) == 5 && call("wcslen", 1, put_wide(G + 0x200, (uint32_t[]){0})) == 0);
    assert(call("wcschr", 2, s, 'l') == s + 8 && call("wcsrchr", 2, s, 'l') == s + 12 && call("wcschr", 2, s, 0) == s + 20);
    assert(!call("wcschr", 2, s, 'z') && call("wcsstr", 2, s, t) == s + 12 && call("wcsstr", 2, s, G + 0x200) == s);
    assert((int32_t)call("wcscmp", 2, put_wide(G + 0x300, neg), put_wide(G + 0x340, one)) == -1);
    assert(call("wcscmp", 2, s, s) == 0 && call("wcsncmp", 3, s, t, 0) == 0 && (int32_t)call("wcsncmp", 3, t, s, 1) == 1);
    assert((int32_t)call("wcscoll", 2, G + 0x300, G + 0x340) == -1);
    assert(call("wcscpy", 2, G + 0x400, s) == G + 0x400 && R32(G + 0x400 + 20) == 0 && R32(G + 0x400 + 4) == 'e');
    for (uint32_t i = 0; i < 8; ++i) W32(G + 0x500 + 4 * i, 0x55);
    call("wcsncpy", 3, G + 0x500, t, 5);
    assert(R32(G + 0x500) == 'l' && R32(G + 0x504) == 'o' && R32(G + 0x510) == 0 && R32(G + 0x514) == 0x55);
    call("wcscat", 2, G + 0x400, t); assert(call("wcslen", 1, G + 0x400) == 7 && R32(G + 0x400 + 24) == 'o');
    call("wcsncat", 3, G + 0x400, s, 2); assert(call("wcslen", 1, G + 0x400) == 9 && R32(G + 0x400 + 32) == 'e');
    assert(call("wmemchr", 3, s, 'o', 5) == s + 16 && !call("wmemchr", 3, s, 'o', 4));
    assert((int32_t)call("wmemcmp", 3, G + 0x300, G + 0x340, 1) == -1 && call("wmemcmp", 3, s, s, 5) == 0);
    call("wmemset", 3, G + 0x600, 0x1f600, 3); assert(R32(G + 0x608) == 0x1f600);
    call("wmemmove", 3, G + 0x604, G + 0x600, 2); assert(R32(G + 0x604) == 0x1f600);
    call("wmemcpy", 3, G + 0x700, s, 2); assert(R32(G + 0x704) == 'e');
    assert(call("towupper", 1, 'a') == 'A' && call("towlower", 1, 0xc9) == 0xc9 && !call("iswalpha", 1, 0xe9));
    assert(call("iswdigit", 1, '7') && call("iswspace", 1, '\n') && call("iswcntrl", 1, 0x7f) && call("iswalnum", 1, 'Z'));
    global("C.UTF-8");
    assert(call("iswalpha", 1, 0xe9) && call("towupper", 1, 0xe9) == 0xc9 && !call("iswdigit", 1, 0x663));
    assert(call("iswspace", 1, 0x2003) && !call("iswalpha", 1, 0xd800));
    global("C");
}

RT_CHECK(wide_number_parsing) {
    static const uint32_t text[] = {' ', '\t', '-', '4', '2', 'x', 0}, big[] = {'9', '9', '9', '9', '9', '9', '9', '9', '9', '9', '9', 0};
    static const uint32_t none[] = {' ', 'z', 0}, real[] = {' ', '2', '.', '5', 'e', '1', 0xe9, 0}, hex[] = {'0', 'x', '1', 'f', 0};
    uint32_t end = G + 0x800;
    assert((int32_t)call("wcstol", 3, put_wide(G, text), end, 10) == -42 && R32(end) == G + 20);
    assert(call("wcstol", 3, put_wide(G + 0x100, hex), end, 0) == 31 && R32(end) == G + 0x110);
    W32(call("__errno_location", 0), 0);
    assert(call("wcstol", 3, put_wide(G + 0x200, big), end, 10) == INT32_MAX && guest_errno() == 34);
    assert(call("wcstol", 3, put_wide(G + 0x300, none), end, 10) == 0 && R32(end) == G + 0x300);
    assert(call("wcstol", 3, G + 0x300, end, 1) == 0 && guest_errno() == 22);
    call("wcstod", 2, put_wide(G + 0x400, real), end);
    assert(cpu.st[cpu.ftop] == 25.0 && R32(end) == G + 0x418);
    call("wcstof", 2, G + 0x400, 0);
    assert(cpu.st[cpu.ftop] == 25.0);
    assert(call("wcstod", 2, G + 0x300, end) == call("wcstod", 2, G + 0x300, end) && R32(end) == G + 0x300);
}

RT_CHECK(multibyte_conversions_follow_lc_ctype) {
    uint32_t ps = G + 0xf00, out = G + 0x400, end = G + 0xf10;
    // "C" = ASCII strict.
    assert(call("mbstowcs", 3, out, put_string(G, "abc"), 8) == 3 && R32(out + 8) == 'c' && R32(out + 12) == 0);
    assert(call("mbstowcs", 3, 0, put_string(G, "h\xc3\xa9"), 0) == UINT32_MAX && guest_errno() == 84);
    assert(call("wcrtomb", 3, G + 0x100, 0xe9, 0) == UINT32_MAX && guest_errno() == 84);
    assert(call("btowc", 1, 'A') == 'A' && call("btowc", 1, 0xe9) == UINT32_MAX && call("btowc", 1, UINT32_MAX) == UINT32_MAX);
    assert(call("wctob", 1, 'A') == 'A' && call("wctob", 1, 0xe9) == UINT32_MAX);
    global("C.UTF-8");
    assert(call("mbstowcs", 3, out, put_string(G, "h\xc3\xa9\xf0\x9f\x98\x80"), 8) == 3);
    assert(R32(out) == 'h' && R32(out + 4) == 0xe9 && R32(out + 8) == 0x1f600 && R32(out + 12) == 0);
    assert(call("mbstowcs", 3, out, G, 2) == 2 && R32(out + 12) == 0);   // n atteint : pas de NUL écrit
    static const uint32_t text[] = {0xe9, 0x20ac, 0x1f600, 0};
    uint32_t wide = put_wide(G + 0x200, text);
    assert(call("wcstombs", 3, 0, wide, 0) == 9);
    memset(GPTR(G + 0x300), 'X', 16);
    assert(call("wcstombs", 3, G + 0x300, wide, 16) == 9 && !memcmp(GPTR(G + 0x300), "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", 10));
    memset(GPTR(G + 0x300), 'X', 16);
    assert(call("wcstombs", 3, G + 0x300, wide, 4) == 2 && R8(G + 0x302) == 'X');   // € ne tient pas
    // mbrtowc avec état : séquence coupée en deux appels.
    W32(ps, 0); W32(ps + 4, 0);
    assert(call("mbrtowc", 4, out, put_string(G + 0x100, "\xe2\x82\xac"), 2, ps) == (uint32_t)-2);
    assert(call("mbrtowc", 4, out, G + 0x102, 1, ps) == 1 && R32(out) == 0x20ac && R32(ps) == 0);
    assert(call("mbrtowc", 4, out, put_string(G + 0x100, "\xc0\x80"), 2, ps) == UINT32_MAX && guest_errno() == 84);
    W32(ps, 0);
    assert(call("mbrtowc", 4, out, put_string(G + 0x100, "\xed\xa0\x80"), 3, ps) == UINT32_MAX);
    W32(ps, 0);
    assert(call("mbrtowc", 4, out, put_string(G + 0x100, ""), 1, ps) == 0 && R32(out) == 0);
    assert(call("mbrtowc", 4, 0, 0, 0, 0) == 0);
    assert(call("wcrtomb", 3, G + 0x100, 0x1f600, ps) == 4 && !memcmp(GPTR(G + 0x100), "\xf0\x9f\x98\x80", 4));
    assert(call("wcrtomb", 3, G + 0x100, 0xd800, ps) == UINT32_MAX && call("wcrtomb", 3, 0, 'x', ps) == 1);
    // mbsrtowcs / mbsnrtowcs / wcsnrtombs mettent à jour *src.
    W32(end, put_string(G + 0x100, "a\xc3\xa9z")); W32(ps, 0);
    assert(call("mbsrtowcs", 4, out, end, 2, ps) == 2 && R32(end) == G + 0x103 && R32(out + 4) == 0xe9);
    assert(call("mbsrtowcs", 4, out, end, 8, ps) == 1 && R32(end) == 0 && R32(out) == 'z' && R32(out + 4) == 0);
    W32(end, G + 0x100);
    assert(call("mbsrtowcs", 4, 0, end, 0, 0) == 3 && R32(end) == G + 0x100);
    W32(end, G + 0x100); W32(ps, 0);
    assert(call("mbsnrtowcs", 5, out, end, 2, 8, ps) == 1 && R32(end) == G + 0x102 && R32(ps) != 0);
    assert(call("mbsnrtowcs", 5, out, end, 8, 8, ps) == 2 && R32(out) == 0xe9 && R32(end) == 0);
    W32(end, wide);
    assert(call("wcsnrtombs", 5, G + 0x300, end, 2, 16, 0) == 5 && R32(end) == wide + 8);
    assert(call("wcsnrtombs", 5, G + 0x300, end, 8, 16, 0) == 4 && R32(end) == 0);
    W32(end, put_wide(G + 0x500, (uint32_t[]){'a', 0xd800, 0}));
    assert(call("wcsnrtombs", 5, G + 0x300, end, 8, 16, 0) == UINT32_MAX && R32(end) == G + 0x504 && guest_errno() == 84);
    global("C");
}

RT_CHECK(iconv_bridges_host_with_linux_errno) {
    uint32_t cd = call("iconv_open", 2, put_string(G, "UTF-32LE"), put_string(G + 0x20, "UTF-8"));
    assert(cd != UINT32_MAX);
    uint32_t in = G + 0x100, inp = G + 0x40, inleft = G + 0x44, outp = G + 0x48, outleft = G + 0x4c, out = G + 0x200;
    put_string(in, "h\xc3\xa9");
    W32(inp, in); W32(inleft, 3); W32(outp, out); W32(outleft, 64);
    assert(call("iconv", 5, cd, inp, inleft, outp, outleft) == 0);
    assert(R32(inp) == in + 3 && R32(inleft) == 0 && R32(outp) == out + 8 && R32(outleft) == 56);
    assert(R32(out) == 'h' && R32(out + 4) == 0xe9);
    W32(inp, in); W32(inleft, 3); W32(outp, out); W32(outleft, 4);
    assert(call("iconv", 5, cd, inp, inleft, outp, outleft) == UINT32_MAX && guest_errno() == 7);
    assert(R32(inp) == in + 1 && R32(inleft) == 2 && R32(outleft) == 0);
    W32(inp, put_string(in, "\xff")); W32(inleft, 1); W32(outp, out); W32(outleft, 64);
    assert(call("iconv", 5, cd, inp, inleft, outp, outleft) == UINT32_MAX && guest_errno() == 84 && R32(inp) == in);
    W32(inp, put_string(in, "\xc3")); W32(inleft, 1);
    assert(call("iconv", 5, cd, inp, inleft, outp, outleft) == UINT32_MAX && guest_errno() == 22);
    assert(call("iconv", 5, cd, 0, 0, 0, 0) == 0);
    assert(call("iconv_close", 1, cd) == 0);
    cd = call("iconv_open", 2, put_string(G, "UTF-8"), put_string(G + 0x20, "WCHAR_T"));
    assert(cd != UINT32_MAX);
    W32(in, 0x20ac); W32(inp, in); W32(inleft, 4); W32(outp, out); W32(outleft, 8);
    assert(call("iconv", 5, cd, inp, inleft, outp, outleft) == 0 && !memcmp(GPTR(out), "\xe2\x82\xac", 3) && R32(outleft) == 5);
    assert(call("iconv_close", 1, cd) == 0);
    assert(call("iconv_open", 2, put_string(G, "NO-SUCH-CODESET"), G + 0x20) == UINT32_MAX && guest_errno() == 22);
    assert(call("iconv_close", 1, UINT32_MAX) == UINT32_MAX && guest_errno() == 9);
}
