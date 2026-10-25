// Temps glibc i386 : time_t 32 bits, struct tm de 44 octets, strftime/strptime C.
#include "checks.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STACK 0x2001f000u
#define G 0x20012000u
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
static int guest_equals(uint32_t at, const char *s) { return at && !strcmp(GPTR(at), s); }
static int32_t field(uint32_t tm, int i) { return (int32_t)R32(tm + 4 * (uint32_t)i); }
static void set_tm(uint32_t tm, int sec, int min, int hour, int mday, int mon, int year, int wday, int yday, int isdst) {
    int v[9] = {sec, min, hour, mday, mon, year, wday, yday, isdst};
    for (int i = 0; i < 9; ++i) W32(tm + 4 * (uint32_t)i, (uint32_t)v[i]);
    W32(tm + 36, 0); W32(tm + 40, 0);
}
static char *saved_tz;
static void use_tz(const char *zone) { setenv("TZ", zone, 1); call("tzset", 0); }
static void restore_tz(void) {
    if (saved_tz) setenv("TZ", saved_tz, 1); else unsetenv("TZ");
    free(saved_tz); saved_tz = NULL; tzset();
}

RT_CHECK(time_conversions_use_i386_tm) {
    saved_tz = getenv("TZ") ? strdup(getenv("TZ")) : NULL;
    use_tz("Europe/Paris");
    uint32_t t = G, tm = G + 0x40;
    uint32_t now = call("time", 1, t);
    assert(now == R32(t) && (int64_t)(int32_t)now - time(NULL) <= 1 && (int64_t)(int32_t)now - time(NULL) >= -1);
    assert(call("time", 1, 0) - now <= 1);
    W32(t, 0);
    uint32_t shared = call("gmtime", 1, t);
    assert(shared && field(shared, 5) == 70 && field(shared, 3) == 1 && field(shared, 6) == 4 && field(shared, 7) == 0);
    assert(R32(shared + 36) == 0 && guest_equals(R32(shared + 40), "GMT"));
    W32(t, 1700000000);
    assert(call("localtime", 1, t) == shared);   // tampon statique commun (glibc _tmbuf)
    assert(field(shared, 2) == 23 && field(shared, 1) == 13 && field(shared, 3) == 14 && field(shared, 4) == 10);
    assert(field(shared, 5) == 123 && field(shared, 6) == 2 && field(shared, 7) == 317 && field(shared, 8) == 0);
    assert(R32(shared + 36) == 3600 && guest_equals(R32(shared + 40), "CET"));
    W32(t, 1690000000);
    assert(call("localtime_r", 2, t, tm) == tm && field(tm, 2) == 6 && field(tm, 8) == 1 && R32(tm + 36) == 7200);
    assert(guest_equals(R32(tm + 40), "CEST") && field(shared, 2) == 23);
    assert(call("gmtime_r", 2, t, tm) == tm && field(tm, 2) == 4 && field(tm, 1) == 26 && field(tm, 0) == 40);
    // mktime normalise, renseigne wday/yday/isdst, gmtoff et tm_zone.
    set_tm(tm, 20, 13, 23, 14, 10, 123, 0, 0, -1);
    assert(call("mktime", 1, tm) == 1700000000 && field(tm, 6) == 2 && field(tm, 7) == 317 && field(tm, 8) == 0);
    assert(R32(tm + 36) == 3600 && guest_equals(R32(tm + 40), "CET"));
    set_tm(tm, 0, 0, 0, 32, 0, 124, 0, 0, -1);   // 32 janvier -> 1er février
    assert(call("mktime", 1, tm) != UINT32_MAX && field(tm, 3) == 1 && field(tm, 4) == 1);
    set_tm(tm, 0, 0, 0, 1, 0, 200, 9, 9, 0);     // 2100 : hors time_t 32 bits
    assert(call("mktime", 1, tm) == UINT32_MAX && guest_errno() == 75 && field(tm, 6) == 9 && field(tm, 5) == 200);
    set_tm(tm, 0, 0, 0, 2, 0, 70, 0, 0, 0);
    assert(call("timegm", 1, tm) == 86400 && field(tm, 6) == 5 && guest_equals(R32(tm + 40), "GMT"));
    call("difftime", 2, 10, (uint32_t)-4);
    assert(cpu.st[cpu.ftop] == 14.0);
    uint32_t c0 = call("clock", 0), c1 = call("clock", 0);
    assert(c1 >= c0 && c1 - c0 < 1000000);
    restore_tz();
}

RT_CHECK(asctime_and_ctime_share_glibc_buffers) {
    saved_tz = getenv("TZ") ? strdup(getenv("TZ")) : NULL;
    use_tz("Europe/Paris");
    uint32_t t = G, tm = G + 0x40, buf = G + 0x100;
    W32(t, 1700000000);
    uint32_t text = call("ctime", 1, t);
    assert(guest_equals(text, "Tue Nov 14 23:13:20 2023\n"));
    set_tm(tm, 5, 4, 3, 2, 0, 100, 0, 1, 0);
    assert(call("asctime", 1, tm) == text && guest_equals(text, "Sun Jan  2 03:04:05 2000\n"));
    assert(call("ctime_r", 2, t, buf) == buf && guest_equals(buf, "Tue Nov 14 23:13:20 2023\n"));
    set_tm(tm, 5, 4, 3, 2, 12, 100, 9, 1, 0);
    assert(call("asctime_r", 2, tm, buf) == buf && guest_equals(buf, "??? ???  2 03:04:05 2000\n"));
    set_tm(tm, 5, 4, 3, 2, 0, 99999, 0, 1, 0);   // 27 caractères : dépasse les 26 d'asctime_r
    assert(!call("asctime_r", 2, tm, buf) && guest_errno() == 75);
    assert(guest_equals(call("asctime", 1, tm), "Sun Jan  2 03:04:05 101899\n"));
    restore_tz();
}

RT_CHECK(strftime_formats_c_locale) {
    uint32_t tm = G, out = G + 0x100, fmt = G + 0x300;
    set_tm(tm, 9, 8, 7, 5, 2, 124, 2, 64, 0);   // mardi 5 mars 2024 07:08:09
    W32(tm + 36, (uint32_t)-16200); W32(tm + 40, put_string(G + 0x80, "XST"));
    put_string(fmt, "%Y-%m-%d %H:%M:%S %Z %z|%a %A %b %B %h|%j %U %W %u %w|%e %C %y %p %I %l %k|%G %g %V|%D %F %T %R %r|%%%n%t%Q");
    uint32_t n = call("strftime", 4, out, 256, fmt, tm);
    const char *expected = "2024-03-05 07:08:09 XST -0430|Tue Tuesday Mar March Mar|065 09 10 2 2| 5 20 24 AM 07  7  7|"
                           "2024 24 10|03/05/24 2024-03-05 07:08:09 07:08 07:08:09 AM|%\n\t%Q";
    if (!guest_equals(out, expected)) fprintf(stderr, "got [%s]\n", (char *)GPTR(out));
    assert(n == strlen(expected) && guest_equals(out, expected));
    assert(call("strftime", 4, out, n, fmt, tm) == 0 && call("strftime", 4, out, n + 1, fmt, tm) == n);
    put_string(fmt, "[%10A][%^a][%#p][%-d][%_m][%5Y][%c][%x %X][%P]");
    n = call("strftime", 4, out, 256, fmt, tm);
    assert(guest_equals(out, "[   Tuesday][TUE][am][5][ 3][ 2024][Tue Mar  5 07:08:09 2024][03/05/24 07:08:09][am]"));
    W32(tm + 32, (uint32_t)-1);
    put_string(fmt, "<%z>"); call("strftime", 4, out, 256, fmt, tm);
    assert(guest_equals(out, "<>"));
    // Semaines ISO aux bornes d'année.
    set_tm(tm, 0, 0, 0, 1, 0, 123, 0, 0, 0);    // dimanche 1er janvier 2023
    put_string(fmt, "%G-W%V-%u %U %W"); call("strftime", 4, out, 256, fmt, tm);
    assert(guest_equals(out, "2022-W52-7 01 00"));
    set_tm(tm, 0, 0, 0, 30, 11, 124, 1, 364, 0); // lundi 30 décembre 2024
    call("strftime", 4, out, 256, fmt, tm);
    assert(guest_equals(out, "2025-W01-1 52 53"));
    // Variantes _l et large (wchar_t de 4 octets).
    uint32_t loc = call("__newlocale", 3, 1u << 6, put_string(G + 0x90, "C"), 0);
    put_string(fmt, "%d.%m"); assert(call("__strftime_l", 5, out, 16, fmt, tm, loc) == 5 && guest_equals(out, "30.12"));
    const uint32_t wide_format[] = {0x263a, '%', 'b', ' ', '%', 'Y', 0};
    for (uint32_t i = 0; i < 7; ++i) W32(fmt + 4 * i, wide_format[i]);
    assert(call("__wcsftime_l", 5, out, 16, fmt, tm, loc) == 9);
    assert(R32(out) == 0x263a && R32(out + 4) == 'D' && R32(out + 20) == '2' && R32(out + 32) == '4' && R32(out + 36) == 0);
    assert(call("__wcsftime_l", 5, out, 9, fmt, tm, loc) == 0);
    call("__freelocale", 1, loc);
}

RT_CHECK(strptime_parses_c_locale) {
    uint32_t tm = G, input = G + 0x100, fmt = G + 0x200;
    set_tm(tm, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    put_string(input, "2023-11-14 23:13:20 rest");
    put_string(fmt, "%Y-%m-%d %H:%M:%S");
    assert(call("strptime", 3, input, fmt, tm) == input + 19);
    assert(field(tm, 5) == 123 && field(tm, 4) == 10 && field(tm, 3) == 14 && field(tm, 2) == 23 && field(tm, 0) == 20);
    assert(field(tm, 6) == 2 && field(tm, 7) == 317);
    set_tm(tm, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    put_string(input, "tuesday, 14 NOV 2023 11:13:20 pm +01:30");
    put_string(fmt, "%a, %d %b %Y %I:%M:%S %p %z");
    assert(call("strptime", 3, input, fmt, tm) == input + strlen("tuesday, 14 NOV 2023 11:13:20 pm +01:30"));
    assert(field(tm, 2) == 23 && field(tm, 6) == 2 && (int32_t)R32(tm + 36) == 5400);
    put_string(input, "68 69"); put_string(fmt, "%y");
    assert(call("strptime", 3, input, fmt, tm) == input + 2 && field(tm, 5) == 168);
    assert(call("strptime", 3, input + 2, fmt, tm) == input + 5 && field(tm, 5) == 69);
    set_tm(tm, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    put_string(input, "2023 318"); put_string(fmt, "%Y %j");
    assert(call("strptime", 3, input, fmt, tm) == input + 8);
    assert(field(tm, 4) == 10 && field(tm, 3) == 14 && field(tm, 6) == 2 && field(tm, 7) == 317);
    put_string(input, "20 24/03/05"); put_string(fmt, "%C %y/%m/%d");
    assert(call("strptime", 3, input, fmt, tm) && field(tm, 5) == 124 && field(tm, 6) == 2);
    put_string(input, "x"); put_string(fmt, "%Y");
    assert(!call("strptime", 3, input, fmt, tm));
    put_string(input, "13"); put_string(fmt, "%m");
    assert(!call("strptime", 3, input, fmt, tm));
    put_string(input, "CET   7"); put_string(fmt, "%Z %H");
    assert(call("strptime", 3, input, fmt, tm) == input + 7 && field(tm, 2) == 7);
}

RT_CHECK(tzset_refreshes_imported_timezone_data) {
    saved_tz = getenv("TZ") ? strdup(getenv("TZ")) : NULL;
    uint32_t data = rt_alloc(32);   // dédié : tzset réécrira ces adresses ensuite
    assert(data && !rt_time_data_import("timezone", data) && !rt_time_data_import("daylight", data + 16));
    assert(rt_time_data_import("tzname", data) == -1);
    use_tz("UTC");
    assert(R32(data) == 0 && R32(data + 16) == 0);
    use_tz("America/New_York");
    assert(R32(data) == 18000 && R32(data + 16) == 1);
    use_tz("Asia/Tokyo");
    assert((int32_t)R32(data) == -32400);
    restore_tz();
}

RT_CHECK(utime_sets_linux_utimbuf) {
    char path[] = "/tmp/portal2-utime-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd);
    uint32_t guest_path = put_string(G, path), times = G + 0x100;
    W32(times, 1000000); W32(times + 4, 2000000);
    assert(call("utime", 2, guest_path, times) == 0);
    struct stat info; assert(!stat(path, &info) && info.st_mtime == 2000000 && info.st_atime == 1000000);
    assert(call("utime", 2, guest_path, 0) == 0 && !stat(path, &info) && info.st_mtime > 2000000);
    unlink(path);
    assert(call("utime", 2, guest_path, 0) == UINT32_MAX && guest_errno() == 2);
}
