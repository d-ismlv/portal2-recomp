#include "checks.h"

static int fake_gl_calls;
static int fake_gl(CPU *c, const char *name) {
    if (strcmp(name, "glFakeProbe")) return 0;
    c->eax = 0x600d; ++fake_gl_calls; return 1;
}
RT_SHIM_PROVIDER(fake_gl)

RT_CHECK(dynamic_imports_dispatch_by_name) {
    uint32_t a = rt_dynamic_import("glFakeProbe");
    assert(a >= RT_DYNAMIC_IMPORT_BASE && a == rt_dynamic_import("glFakeProbe"));
    uint32_t b = rt_dynamic_import("glOther");
    assert(b && b != a && !strcmp(rt_dynamic_import_name(b), "glOther"));
    assert(!rt_dynamic_import_name(a + 4) && !rt_dynamic_import_name(0x10000000));
    assert(!rt_dynamic_import("") && !rt_dynamic_import(NULL));
    CPU c; rt_cpu_reset(&c); c.esp = 0x20010000;
    rt_call(&c, a, RT_RETURN_SENTINEL);   // même chemin qu'un appel invité indirect
    assert(c.eax == 0x600d && fake_gl_calls == 1 && c.esp == 0x20010000);
}

RT_CHECK(handles_are_typed_and_reusable) {
    int x, y;
    uint32_t h = rt_handle_new(&x, 'WIND'), g = rt_handle_new(&y, 'GLCX');
    assert(h >= RT_HANDLE_BASE && g != h && !(h & 15));
    assert(rt_handle_get(h, 'WIND') == &x && !rt_handle_get(h, 'GLCX'));
    assert(rt_handle_find(&y, 'GLCX') == g && !rt_handle_find(&y, 'WIND'));
    assert(!rt_handle_get(h + 4, 'WIND') && !rt_handle_get(0, 'WIND') && !rt_handle_new(NULL, 'WIND'));
    assert(!rt_handle_free(h, 'GLCX') && rt_handle_free(h, 'WIND') == &x);
    assert(!rt_handle_get(h, 'WIND') && rt_handle_new(&x, 'WIND') == h);  // slot reused
    rt_handle_free(h, 'WIND'); rt_handle_free(g, 'GLCX');
}

RT_CHECK(st0_return_helper) {
    CPU cpu, *c = &cpu; rt_cpu_reset(c);
    rt_return_st0(c, 2.5);
    assert(ST(0) == 2.5);
}

RT_CHECK(handle_range_is_disjoint) {
    // Données importées (stderr, timezone...) à 0xe0000000, imports statiques à 0xf0000000.
    assert(RT_HANDLE_BASE >= 0xe0000000u + 0x1000000u);
    assert((uint64_t)RT_HANDLE_BASE + (uint64_t)RT_HANDLE_LIMIT * RT_HANDLE_STRIDE <= 0xf0000000u);
    assert(rt_standard_stream(2) < RT_HANDLE_BASE);
}
