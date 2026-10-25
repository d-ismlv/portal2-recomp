#include "rt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define ARENA 0x20000000u
#define INPUT (ARENA + 0x1000u)
#define ENCODED (ARENA + 0x4000u)
#define OUTPUT (ARENA + 0x8000u)
#define NEXT (ARENA + 0xc000u)
#define STACK (ARENA + 0x20000u)

static uint32_t invoke(const char *name, const uint32_t *args, size_t count) {
    CPU c; rt_cpu_reset(&c);
    c.esp = STACK;
    c.ebx = 0x12345678; c.esi = 0x87654321; c.edi = 0xabcdef01; c.ebp = 0x55aa55aa;
    for (size_t i = count; i; --i) PUSH(&c, args[i - 1]);
    PUSH(&c, RT_RETURN_SENTINEL);
    uint32_t entry = rt_export(name);
    assert(entry);
    rt_dispatch(&c, entry);
    assert(c.eip == RT_RETURN_SENTINEL);
    assert(c.esp == STACK - count * 4);
    assert(c.ebx == 0x12345678 && c.esi == 0x87654321);
    assert(c.edi == 0xabcdef01 && c.ebp == 0x55aa55aa);
    return c.eax;
}

static uint32_t shim(const char *name, const uint32_t *args, size_t count) {
    CPU c; rt_cpu_reset(&c); c.esp = STACK;
    c.flags.cf = 1; c.flags.pf = 7;
    for (size_t i = count; i; --i) PUSH(&c, args[i - 1]);
    PUSH(&c, RT_RETURN_SENTINEL);
    uint32_t original_sp = c.esp;
    assert(rt_shim(&c, name));
    assert(c.esp == original_sp); // dispatcher owns the return, not the shim
    assert(c.flags.cf == 1 && c.flags.pf == 7);
    return c.eax;
}

static void check_shims(void) {
    uint32_t size = 31;
    uint32_t p = shim("malloc", &size, 1);
    assert(p && !(p & 15));
    memset(GPTR(p), 0x42, size);
    uint32_t grown[] = {p, 300};
    uint32_t q = shim("realloc", grown, 2);
    assert(q);
    for (unsigned i = 0; i < size; ++i) assert(R8(q + i) == 0x42);
    uint32_t failure[] = {q, UINT32_MAX};
    assert(shim("realloc", failure, 2) == 0);
    assert(R8(q) == 0x42); // failed realloc keeps the old block alive
    assert(shim("malloc_usable_size", &q, 1) >= 300);
    shim("free", &q, 1);
    uint32_t reuse = shim("malloc", &size, 1);
    assert(reuse == p); // freed adjacent blocks coalesce
    shim("free", &reuse, 1);
    uint32_t zero = 0;
    shim("free", &zero, 1);
    uint32_t dimensions[] = {8, 4};
    p = shim("calloc", dimensions, 2);
    assert(p);
    for (unsigned i = 0; i < 32; ++i) assert(R8(p + i) == 0);
    dimensions[0] = UINT32_MAX;
    assert(shim("calloc", dimensions, 2) == 0);
    shim("free", &p, 1);

    memcpy(GPTR(INPUT), "abcdef", 7);
    uint32_t copy[] = {OUTPUT, INPUT, 7};
    assert(shim("memcpy", copy, 3) == OUTPUT);
    assert(!memcmp(GPTR(OUTPUT), "abcdef", 7));
    copy[0] = OUTPUT + 1; copy[1] = OUTPUT; copy[2] = 6;
    assert(shim("memmove", copy, 3) == OUTPUT + 1);
    assert(!memcmp(GPTR(OUTPUT), "aabcdef", 7));
    uint32_t input = INPUT;
    assert(shim("strlen", &input, 1) == 6);
    uint32_t strings[] = {OUTPUT, INPUT, 10};
    assert(shim("strncpy", strings, 3) == OUTPUT);
    assert(!memcmp(GPTR(OUTPUT), "abcdef\0\0\0\0", 10));
    assert(shim("strcmp", strings, 2) == 0);
    assert(shim("strcpy", strings, 2) == OUTPUT);
    uint32_t cmp[] = {OUTPUT, INPUT, 7};
    assert(shim("memcmp", cmp, 3) == 0);
    W8(OUTPUT, 'z');
    assert((int32_t)shim("memcmp", cmp, 3) > 0);
    uint32_t fill[] = {OUTPUT + 1, 37};
    memset(GPTR(OUTPUT), 0x5a, 40);
    invoke("ZeroMemory", fill, 2); // real guest CALL -> generated thunk -> host memset
    assert(R8(OUTPUT) == 0x5a && R8(OUTPUT + 38) == 0x5a);
    for (unsigned i = 1; i < 38; ++i) assert(R8(OUTPUT + i) == 0);
}

static void check_pthreads(void) {
    uint32_t attr_address = NEXT + 0x100, mutex_address = NEXT + 0x200;
    uint32_t type[] = {attr_address, 1}; // Linux recursive == 1, Darwin uses another constant
    assert(shim("pthread_mutexattr_init", &attr_address, 1) == 0);
    assert(shim("pthread_mutexattr_settype", type, 2) == 0);
    uint32_t init[] = {mutex_address, attr_address};
    assert(shim("pthread_mutex_init", init, 2) == 0);
    assert(shim("pthread_mutex_lock", &mutex_address, 1) == 0);
    assert(shim("pthread_mutex_trylock", &mutex_address, 1) == 0);
    assert(shim("pthread_mutex_unlock", &mutex_address, 1) == 0);
    assert(shim("pthread_mutex_unlock", &mutex_address, 1) == 0);
    assert(shim("pthread_mutex_destroy", &mutex_address, 1) == 0);
    assert(shim("pthread_mutexattr_destroy", &attr_address, 1) == 0);
    assert(shim("pthread_mutex_trylock", &mutex_address, 1) == 22);
    uint32_t self = shim("pthread_self", NULL, 0);
    assert(self && shim("pthread_self", NULL, 0) == self);
    // Static default mutex: repeated trylock reports Linux EBUSY.
    memset(GPTR(mutex_address), 0, 24);
    assert(shim("pthread_mutex_trylock", &mutex_address, 1) == 0);
    assert(shim("pthread_mutex_trylock", &mutex_address, 1) == 16);
    assert(shim("pthread_mutex_unlock", &mutex_address, 1) == 0);
    assert(shim("pthread_mutex_destroy", &mutex_address, 1) == 0);
}

static void check_constructors(void) {
    assert(rt_atexit_count() == 0);
    invoke("libtier0.so:init:dt", NULL, 0);
    const char *constructors[] = {"libtier0.so:init:0", "libtier0.so:init:1",
        "libtier0.so:init:2", "libtier0.so:init:3", "libtier0.so:init:4", "libtier0.so:init:5", "libtier0.so:init:6", "libtier0.so:init:7", "libtier0.so:init:8",
        "libtier0.so:init:9", "libtier0.so:init:10", "libtier0.so:init:11", "libtier0.so:init:12", "libtier0.so:init:13"};
    for (size_t i = 0; i < sizeof(constructors) / sizeof(*constructors); ++i)
        invoke(constructors[i], NULL, 0);
    assert(rt_atexit_count() == 11);
    assert(invoke("LoggingSystem_GetChannelCount", NULL, 0) == 7);
    uint32_t function, object, dso;
    for (size_t i = 0; i < 2; ++i) {
        assert(rt_atexit_get(i, &function, &object, &dso) == 0);
        assert(function && object && dso);
        assert(R32(object)); // original constructor installed a real relocated vtable
    }
}

static void check_cpu_information(void) {
    uint32_t info = invoke("GetCPUInformation", NULL, 0);
    assert(R32(info) == 20 && R64(info + 8) == UINT64_C(1000000000));
    assert(R8(info + 5) == 1 && R8(info + 6) == 1);
    assert(!strcmp(GPTR(R32(info + 16)), "GenuineIntel"));
    CPU c; rt_cpu_reset(&c); c.esp = STACK;
    rt_call(&c, rt_export("libtier0.so:init:13"), RT_RETURN_SENTINEL);
    assert(c.esp == STACK && c.mxcsr == 0x9fc0);
}

static void check_source_allocator(void) {
    uint32_t bytes = 31;
    uint32_t p = invoke("Plat_Alloc", &bytes, 1);
    assert(p);
    memset(GPTR(p), 0x31, bytes);
    uint32_t grow[] = {p, 300};
    uint32_t q = invoke("Plat_Realloc", grow, 2);
    assert(q);
    for (unsigned i = 0; i < bytes; ++i) assert(R8(q + i) == 0x31);
    invoke("Plat_Free", &q, 1);
    assert(invoke("UnitTestCount", NULL, 0) == 0);
    for (unsigned i = 0; i < 40; ++i) {
        uint32_t object = INPUT + i * 16;
        invoke("UnitTestInstallTestCase", &object, 1);
        assert(invoke("UnitTestCount", NULL, 0) == i + 1);
        assert(invoke("GetUnitTest", &i, 1) == object);
    }
    for (uint32_t i = 0; i < 40; ++i)
        assert(invoke("GetUnitTest", &i, 1) == INPUT + i * 16);
}

static void check_formats(void) {
    const char *expected = "-1/4294967295/1122334455667788/-1/abc/1.25";
    strcpy(GPTR(INPUT), "%ld/%zu/%llx/%hhd/%.*s/%.2f%n");
    strcpy(GPTR(INPUT + 256), "abcdef");
    double value = 1.25; uint64_t bits; memcpy(&bits, &value, 8);
    uint32_t args[] = {OUTPUT, 256, INPUT, UINT32_MAX, UINT32_MAX,
        0x55667788, 0x11223344, 0xff, 3, INPUT + 256,
        (uint32_t)bits, (uint32_t)(bits >> 32), NEXT};
    assert(shim("snprintf", args, sizeof(args) / sizeof(*args)) == strlen(expected));
    assert(!strcmp(GPTR(OUTPUT), expected));
    assert(R32(NEXT) == strlen(expected));
    args[1] = 8; W8(OUTPUT + 8, 0x5a);
    assert(shim("snprintf", args, sizeof(args) / sizeof(*args)) == strlen(expected));
    assert(!memcmp(GPTR(OUTPUT), expected, 7) && !R8(OUTPUT + 7));
    assert(R8(OUTPUT + 8) == 0x5a);
    args[0] = 0; args[1] = 0;
    assert(shim("snprintf", args, sizeof(args) / sizeof(*args)) == strlen(expected));
    strcpy(GPTR(INPUT), "%*.*s %d");
    uint32_t varargs[] = {(uint32_t)-6, 3, INPUT + 256, 42};
    for (unsigned i = 0; i < 4; ++i) W32(NEXT + 64 + 4 * i, varargs[i]);
    uint32_t va[] = {OUTPUT, 256, INPUT, NEXT + 64};
    assert(shim("vsnprintf", va, 4) == 9);
    assert(!strcmp(GPTR(OUTPUT), "abc    42"));
}

// Independent format oracle: four 7-bit size digits, then groups of seven
// bytes with a bitmap recording their inverted high bits. Every encoded byte
// has its high bit set, so the string contains no embedded NUL.
static size_t reference_encode(const uint8_t *input, size_t size, uint8_t *out) {
    for (int i = 0; i < 4; ++i) out[i] = 0x80 | ((size >> (7 * (3 - i))) & 0x7f);
    size_t pos = 4;
    for (size_t i = 0; i < size;) {
        size_t mask = pos++;
        out[mask] = 0x80;
        for (unsigned bit = 0; bit < 7 && i < size; ++bit, ++i) {
            out[mask] |= ((input[i] & 0x80) ? 0 : 1) << (6 - bit);
            out[pos++] = input[i] | 0x80;
        }
    }
    out[pos] = 0;
    return pos;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s image-directory\n", argv[0]); return 2; }
    assert(rt_init() == 0);
    assert(rt_load_images(argv[1]) == 0);
    assert(rt_map(ARENA, 0x30000) == 0);
    assert(rt_map(0xfffff000u, 0x2000) == -1); // cannot wrap past 4 GiB
    assert(rt_lookup(0xdeadbeef) == NULL);
    assert(rt_export("not_an_export") == 0);
    assert(invoke("UnitTestCount", NULL, 0) == 0); // constructors not run
    assert(invoke("Plat_GetMemPageSize", NULL, 0) == 4); // original guest result
    for (uint32_t value = 0; value < 2; ++value) {
        invoke("Plat_SetBenchmarkMode", &value, 1);
        assert(invoke("Plat_IsInBenchmarkMode", NULL, 0) == value);
        invoke("SetAllAssertsDisabled", &value, 1);
        assert(invoke("AreAllAssertsDisabled", NULL, 0) == value);
    }
    uint8_t expected[2048];
    uint32_t random = 0x12345;
    for (unsigned n = 0; n <= 512; ++n) {
        for (unsigned i = 0; i < n; ++i) {
            random = random * 1664525u + 1013904223u;
            W8(INPUT + i, random >> 24);
        }
        size_t size = reference_encode(GPTR(INPUT), n, expected);
        memset(GPTR(ENCODED), 0x5a, 2048);
        uint32_t enc[] = {INPUT, n, ENCODED, (uint32_t)size + 1};
        assert(invoke("EncodeBinaryToString", enc, 4) == size);
        assert(!memcmp(GPTR(ENCODED), expected, size + 1));
        assert(R8(ENCODED + (uint32_t)size + 1) == 0x5a);
        memset(GPTR(OUTPUT), 0x5a, 1024);
        uint32_t dec[] = {ENCODED, OUTPUT, n, NEXT};
        assert(invoke("DecodeBinaryFromString", dec, 4) == n);
        assert(!memcmp(GPTR(INPUT), GPTR(OUTPUT), n));
        assert(R8(OUTPUT + n) == 0x5a);
        assert(R32(NEXT) == ENCODED + size);
        dec[3] = 0; // optional next pointer
        assert(invoke("DecodeBinaryFromString", dec, 4) == n);
        enc[3] = (uint32_t)size;
        W8(ENCODED, 0x5a);
        assert((int32_t)invoke("EncodeBinaryToString", enc, 4) == -(int32_t)(size + 1));
        assert(R8(ENCODED) == 0x5a);
        memcpy(GPTR(ENCODED), expected, size + 1);
        if (n) {
            dec[2] = n - 1; dec[3] = NEXT;
            W8(OUTPUT, 0x5a);
            assert((int32_t)invoke("DecodeBinaryFromString", dec, 4) == -(int32_t)n);
            assert(R8(OUTPUT) == 0x5a && R32(NEXT) == ENCODED + 4);
        }
        // Truncate a header or payload byte: guest reports invalid input.
        W8(ENCODED + (uint32_t)(size / 2), 0);
        dec[2] = n; dec[3] = NEXT;
        assert((int32_t)invoke("DecodeBinaryFromString", dec, 4) == INT32_MIN);
        assert(R32(NEXT) == ENCODED);
    }
    check_shims();
    check_formats();
    check_pthreads();
    check_constructors();
    check_source_allocator();
    check_cpu_information();
    rt_destroy();
    puts("PASS: native tier0; 513 codec cases, variadics, Source allocator/registry, DT_INIT and 14 constructors");
    return 0;
}
