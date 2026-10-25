// Banc du code généré : vraies fonctions de libtier0/libvstdlib, boucles
// gourmandes en mémoire et en appels. Mesure seulement ; le résultat de
// chaque passe (somme de contrôle) doit être identique d'un mode à l'autre.
#include "rt.h"
#include "modules.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define ARENA 0x20000000u
#define ARENA_SIZE 0x01000000u
#define STACK (ARENA + 0x00100000u)
#define INPUT (ARENA + 0x00200000u)
#define ENCODED (ARENA + 0x00400000u)
#define OUTPUT (ARENA + 0x00800000u)
#define WIDE (ARENA + 0x00a00000u)

static CPU cpu;

static uint32_t call(const char *name, const uint32_t *args, size_t count) {
    uint32_t entry = rt_export(name);
    if (!entry) { fprintf(stderr, "bench: export %s absent\n", name); exit(2); }
    cpu.esp = STACK;
    return rt_invoke(&cpu, entry, args, count);
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static uint32_t checksum(uint32_t address, uint32_t size) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < size; ++i) h = (h ^ M[address + i]) * 16777619u;
    return h;
}

// Encodage hexadécimal maison de Source : boucle octet par octet.
static uint32_t codec(unsigned rounds) {
    const uint32_t n = 0x10000;
    uint32_t sum = 0;
    for (unsigned r = 0; r < rounds; ++r) {
        uint32_t enc[] = {INPUT, n, ENCODED, 0x200000};
        uint32_t size = call("EncodeBinaryToString", enc, 4);
        uint32_t dec[] = {ENCODED, OUTPUT, n, 0};
        uint32_t back = call("DecodeBinaryFromString", dec, 4);
        sum = sum * 31 + size + back;
    }
    return sum ^ checksum(ENCODED, 0x20000) ^ checksum(OUTPUT, 0x10000);
}

// UTF-8 <-> UTF-32 (wchar_t Linux) : appels de fonctions par caractère.
static uint32_t unicode(unsigned rounds) {
    uint32_t sum = 0;
    for (unsigned r = 0; r < rounds; ++r) {
        uint32_t to_wide[] = {INPUT, WIDE, 0x100000};
        uint32_t wide = call("V_UTF8ToUnicode", to_wide, 3);
        uint32_t to_utf8[] = {WIDE, OUTPUT, 0x100000};
        uint32_t utf8 = call("V_UnicodeToUTF8", to_utf8, 3);
        sum = sum * 31 + wide + utf8;
    }
    return sum ^ checksum(WIDE, 0x40000) ^ checksum(OUTPUT, 0x18000);
}

// Table de symboles de KeyValuesSystem (vstdlib) : hachage insensible à la
// casse, arbre/table de symboles, appels directs et virtuels.
#define NAMES (ARENA + 0x00c00000u)
#define NAME_COUNT 20000u
static uint32_t kv_object, kv_vtable;
static uint32_t method(unsigned index) { return R32(kv_vtable + 4 * index); }
static uint32_t kv_call(unsigned index, const uint32_t *args, size_t count) {
    uint32_t full[4] = {kv_object};
    for (size_t i = 0; i < count; ++i) full[i + 1] = args[i];
    cpu.esp = STACK;
    return rt_invoke(&cpu, method(index), full, count + 1);
}
static uint32_t keyvalues(unsigned rounds) {
    uint32_t sum = 0;
    for (unsigned r = 0; r < rounds; ++r)
        for (uint32_t i = 0; i < NAME_COUNT; ++i) {
            uint32_t get[] = {NAMES + i * 32, 1};
            uint32_t symbol = kv_call(3, get, 2);              // GetSymbolForString
            uint32_t back[] = {symbol};
            uint32_t text = kv_call(4, back, 1);               // GetStringForSymbol
            if (strcasecmp((const char *)M + text, (const char *)M + NAMES + i * 32)) {
                fprintf(stderr, "bench: symbole %u incohérent\n", i);
                exit(1);
            }
            sum = sum * 31 + symbol;
        }
    return sum;
}

static void run_constructors(const char *module, unsigned count) {
    char spec[96];
    snprintf(spec, sizeof(spec), "%s:init:dt", module);
    uint32_t args[3] = {0};
    if (rt_export(spec)) { cpu.esp = STACK; rt_invoke(&cpu, rt_export(spec), args, 3); }
    for (unsigned i = 0; i < count; ++i) {
        snprintf(spec, sizeof(spec), "%s:init:%u", module, i);
        if (rt_export(spec)) { cpu.esp = STACK; rt_invoke(&cpu, rt_export(spec), args, 3); }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s image-directory [rounds]\n", argv[0]); return 2; }
    unsigned rounds = argc > 2 ? (unsigned)atoi(argv[2]) : 40;
    if (rt_init() || rt_load_images(argv[1]) || rt_map(ARENA, ARENA_SIZE)) return 1;
    rt_cpu_reset(&cpu);
    // Données d'entrée déterministes : octets aléatoires, puis texte UTF-8
    // mêlant ASCII, 2, 3 et 4 octets.
    uint32_t seed = 0x12345;
    for (uint32_t i = 0; i < 0x10000; ++i) { seed = seed * 1664525u + 1013904223u; M[INPUT + i] = (uint8_t)(seed >> 24); }
    static const char *pieces[] = {"Portal ", "\xc3\xa9t\xc3\xa9 ", "\xe2\x82\xac", "\xf0\x9f\x8e\x82", "GLaDOS "};
    uint32_t text = 0;
    while (text < 0x14000) {
        seed = seed * 1664525u + 1013904223u;
        const char *piece = pieces[(seed >> 24) % 5];
        size_t len = strlen(piece);
        memcpy(M + INPUT + 0x20000 + text, piece, len);
        text += (uint32_t)len;
    }
    M[INPUT + 0x20000 + text] = 0;

    run_constructors("libtier0.so", 64);
    run_constructors("libvstdlib.so", 64);
    kv_object = call("KeyValuesSystem", NULL, 0);
    kv_vtable = R32(kv_object);
    for (uint32_t i = 0; i < NAME_COUNT; ++i)
        snprintf((char *)M + NAMES + i * 32, 32, "%s_Key%u_%x", i & 1 ? "Model" : "Sound", i, i * 2654435761u);

    double t0 = now();
    uint32_t a = codec(rounds);
    double t1 = now();
    // V_UTF8ToUnicode lit depuis INPUT : y placer le texte.
    memmove(M + INPUT, M + INPUT + 0x20000, text + 1);
    double t2 = now();
    uint32_t b = unicode(rounds);
    double t3 = now();
    double t4 = now();
    uint32_t k = keyvalues(rounds / 8 ? rounds / 8 : 1);
    double t5 = now();
    printf("codec    %8.1f ms  checksum %08x\n", (t1 - t0) * 1e3, a);
    printf("unicode  %8.1f ms  checksum %08x\n", (t3 - t2) * 1e3, b);
    printf("keyvals  %8.1f ms  checksum %08x\n", (t5 - t4) * 1e3, k);
    return 0;
}
