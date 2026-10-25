// Greffon PE traduit d'avance (runtime/pe.c, lift.generate_pe) : la DLL de
// test (build/pe-fixture.c) est chargée comme le fait libMiles, à une base
// choisie à l'exécution, puis appelée par son adresse réelle.
#include "checks.h"
#include "modules.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern const uint8_t testpe_file[];
extern const uint32_t testpe_file_size;
uint32_t testpe_symbol(const char *name);

static uint32_t sym(const char *name) { uint32_t rva = testpe_symbol(name); assert(rva); return rva; }
static uint32_t le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t le16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

typedef struct { uint32_t mapping, data_mapping, size, base, data_base; } Loaded;

static uint32_t map_image(uint32_t size) {
    uint32_t mapping = rt_mmap(NULL, 0, size, 7, 0x22, -1, 0);
    assert(mapping != UINT32_MAX);
    return mapping;
}

// Comme load_rib_dll de libMiles : une page d'en-tête, l'image derrière,
// sections recopiées à leur RVA, puis .reloc appliquée. Avec `dual`, comme
// sa projection double d'un fichier : une valeur qui désigne .text vise la
// vue exécutable, les autres la vue écrivable (ici deux copies distinctes).
// Le chargeur de test est indépendant du générateur.
static Loaded load_like_miles(int dual, int corrupt_code, int skip_first_relocation) {
    const uint8_t *f = testpe_file;
    uint32_t pe = le32(f + 0x3c), opt = pe + 24;
    uint16_t sections = le16(f + pe + 6), optional_size = le16(f + pe + 20);
    uint32_t image_base = le32(f + opt + 28), image_size = le32(f + opt + 56), headers = le32(f + opt + 60);
    uint32_t reloc_rva = le32(f + opt + 96 + 5 * 8), reloc_size = le32(f + opt + 96 + 5 * 8 + 4);
    Loaded l = {0};
    l.size = image_size + 0x1000;
    l.mapping = map_image(l.size);
    l.data_mapping = dual ? map_image(l.size) : l.mapping;
    l.base = l.mapping + 0x1000;
    l.data_base = l.data_mapping + 0x1000;
    uint32_t text_rva = 0, text_size = 0;
    uint32_t views[2] = {l.base, l.data_base};
    for (int v = 0; v < 1 + dual; ++v) {
        memcpy(GPTR(views[v]), f, headers);
        for (uint16_t i = 0; i < sections; ++i) {
            const uint8_t *s = f + opt + optional_size + 40 * i;
            uint32_t size = le32(s + 8), rva = le32(s + 12), raw_size = le32(s + 16), raw = le32(s + 20);
            if (!memcmp(s, ".text", 6)) { text_rva = rva; text_size = size; }
            memcpy(GPTR(views[v] + rva), f + raw, size < raw_size ? size : raw_size);
        }
    }
    int first = 1;
    for (uint32_t o = reloc_rva; o < reloc_rva + reloc_size;) {
        uint32_t page = R32(l.base + o), block = R32(l.base + o + 4);
        for (uint32_t i = 8; i < block; i += 2) {
            uint16_t entry = R16(l.base + o + i);
            if (entry >> 12 != 3) continue;
            uint32_t rva = page + (entry & 0xfff), value = R32(l.base + rva);
            uint32_t target = value - image_base >= text_rva && value - image_base < text_rva + text_size ? l.base : l.data_base;
            if (!(skip_first_relocation && first))
                for (int v = 0; v < 1 + dual; ++v) W32(views[v] + rva, value + target - image_base);
            first = 0;
        }
        o += block;
    }
    if (corrupt_code) W8(l.base + sym("helper") + 2, 2);   // add eax, 2 au lieu de 1
    return l;
}

static void unload(Loaded l) {
    assert(rt_munmap(l.mapping, l.size) == 0);
    if (l.data_mapping != l.mapping) assert(rt_munmap(l.data_mapping, l.size) == 0);
}

static uint32_t call(uint32_t address, const uint32_t *args, size_t count) {
    CPU c; rt_cpu_reset(&c); c.esp = 0x20010000;
    return rt_invoke(&c, address, args, count);
}

static void run_plugin(Loaded a) {
    // Immédiat relocalisé : l'adresse réelle de `value` (vue écrivable).
    assert(call(a.base + sym("pe_address"), NULL, 0) == a.data_base + sym("value"));
    assert(call(a.base + sym("pe_load"), NULL, 0) == 42);           // déplacement relocalisé + appel interne
    for (uint32_t i = 0; i < 3; ++i)
        assert(call(a.base + sym("pe_switch"), &i, 1) == 10 * (i + 1));   // table relocalisée dans .text
    uint32_t outside = 7;
    assert(call(a.base + sym("pe_switch"), &outside, 1) == UINT32_MAX);
    assert(call(a.base + sym("pe_indirect"), NULL, 0) == 6);        // pointeur de code relocalisé
    CPU c; rt_cpu_reset(&c); c.esp = 0x20010000;                    // stdcall atteint par .data
    PUSH(&c, 9);
    rt_call(&c, R32(a.data_base + sym("callbacks")), RT_RETURN_SENTINEL);
    assert(c.eax == 77 && c.esp == 0x20010000);
}

RT_CHECK(pe_plugin_runs_translated_at_a_runtime_base) {
    assert(rt_pe_image_named("TESTPE.ASI") && !rt_pe_image_named("other.asi"));
    Loaded a = load_like_miles(0, 0, 0);
    assert(a.base != 0x10000000u);
    run_plugin(a);
    // Pas des débuts de fonctions traduites : table de switch, milieu d'instruction.
    assert(!rt_pe_lookup(a.base + sym("table")) && !rt_pe_lookup(a.base + sym("pe_load") + 1));

    // Une seconde copie vivante est refusée (un seul delta par image traduite)...
    Loaded b = load_like_miles(0, 0, 0);
    assert(!rt_pe_lookup(b.base + sym("pe_address")));
    // ...et acceptée une fois la première démappée.
    unload(a);
    assert(rt_pe_lookup(b.base + sym("pe_address")));
    run_plugin(b);
    unload(b);

    // Projection double de libMiles : données vers la vue écrivable, .text
    // vers la vue exécutable ; deux deltas distincts.
    Loaded dual = load_like_miles(1, 0, 0);
    assert(dual.data_base != dual.base);
    run_plugin(dual);
    unload(dual);

    // Image différente du fichier traduit : refusée, le transfert piégerait.
    Loaded changed = load_like_miles(0, 1, 0);
    assert(!rt_pe_lookup(changed.base + sym("pe_load")) && !rt_pe_lookup(changed.base + sym("pe_address")));
    unload(changed);
    Loaded unrelocated = load_like_miles(0, 0, 1);
    assert(!rt_pe_lookup(unrelocated.base + sym("pe_load")));
    unload(unrelocated);
}

RT_CHECK(pe_plugin_file_identity) {
    const RtPeImage *image = rt_pe_image_named("testpe.asi");
    assert(image && image->file_size == testpe_file_size);
    assert(rt_pe_hash(testpe_file, testpe_file_size) == image->file_hash);
    char path[] = "/tmp/rt-pe-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "wb");
    assert(fwrite(testpe_file, 1, testpe_file_size, file) == testpe_file_size && !fclose(file));
    assert(rt_pe_file_matches(image, path));
    // Même taille, un octet différent (nouveau fichier) : refusé.
    assert(!unlink(path));
    file = fopen(path, "wb");
    uint8_t *copy = malloc(testpe_file_size);
    memcpy(copy, testpe_file, testpe_file_size);
    copy[testpe_file_size / 2] ^= 0x40;
    assert(fwrite(copy, 1, testpe_file_size, file) == testpe_file_size && !fclose(file));
    free(copy);
    assert(!rt_pe_file_matches(image, path));
    assert(!unlink(path));
    assert(!rt_pe_file_matches(image, path));
}
