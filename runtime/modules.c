#include "modules.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned char *state;
static uint32_t boot_argc, boot_argv, boot_envp;
static uint32_t error_string;

const GuestModule *rt_find_module(const char *name) {
    const char *base = strrchr(name, '/'); base = base ? base + 1 : name;
    for (size_t i = 0; i < rt_module_count; ++i)
        if (!strcmp(rt_modules[i].name, base)) return &rt_modules[i];
    return NULL;
}
uint32_t rt_module_symbol(const GuestModule *module, const char *name) {
    if (!module) {
        for (size_t i = 0; i < rt_module_count; ++i) {
            uint32_t value = rt_module_symbol(&rt_modules[i], name);
            if (value) return value;
        }
        return 0;
    }
    size_t lo = 0, hi = module->symbol_count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        int order = strcmp(name, module->symbols[middle].name);
        if (!order) return module->symbols[middle].address;
        if (order < 0) hi = middle; else lo = middle + 1;
    }
    return 0;
}
uint32_t rt_invoke(CPU *c, uint32_t address, const uint32_t *args, size_t count) {
    uint32_t original = c->esp;
    c->esp -= (c->esp - (uint32_t)count * 4) & 15;
    for (size_t i = count; i; --i) PUSH(c, args[i - 1]);
    uint32_t expected = c->esp;
    rt_call(c, address, RT_RETURN_SENTINEL);
    if (c->esp != expected) rt_trap(c, address, "cdecl stack imbalance");
    c->esp = original;
    return c->eax;
}
void rt_loader_set_arguments(uint32_t argc, uint32_t argv, uint32_t envp) {
    boot_argc = argc; boot_argv = argv; boot_envp = envp;
}
int rt_initialize_module(CPU *c, const GuestModule *module) {
    if (!state) {
        state = calloc(rt_module_count, 1);
        if (!state) return -1;
    }
    size_t index = (size_t)(module - rt_modules);
    if (index >= rt_module_count) return -1;
    if (state[index]) return state[index] == 3 ? -1 : 0;
    state[index] = 1;
    for (size_t i = 0; i < module->needed_count; ++i) {
        const GuestModule *dependency = rt_find_module(module->needed[i]);
        if (dependency && rt_initialize_module(c, dependency)) { state[index] = 3; return -1; }
    }
    fprintf(stderr, "initializing %s (%zu constructors)\n", module->name, module->constructor_count);
    uint32_t args[] = {boot_argc, boot_argv, boot_envp};
    if (module->init) rt_invoke(c, module->init, args, 3);
    for (size_t i = 0; i < module->constructor_count; ++i)
        if (module->constructors[i] && module->constructors[i] != UINT32_MAX)
            rt_invoke(c, module->constructors[i], args, 3);
    state[index] = 2;
    return 0;
}
static void loader_error(const char *prefix, const char *name) {
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), "%s: %s", prefix, name);
    size_t size = strlen(buffer) + 1;
    error_string = rt_alloc((uint32_t)size);
    if (error_string) memcpy(GPTR(error_string), buffer, size);
}
int rt_loader_shim(CPU *c, const char *name) {
    if (!strcmp(name, "dlopen")) {
        uint32_t path = R32(c->esp + 4);
        if (!path) { c->eax = 0xccfff000; return 1; }
        rt_string_length(c, path);
        const GuestModule *module = rt_find_module(GPTR(path));
        if (!module) { loader_error("guest module unavailable", GPTR(path)); c->eax = 0; }
        else if (rt_initialize_module(c, module)) { loader_error("initialization failed", module->name); c->eax = 0; }
        else c->eax = 0xcc000000u + module->id * 0x100;
    } else if (!strcmp(name, "dlsym")) {
        uint32_t handle = R32(c->esp + 4), symbol = R32(c->esp + 8);
        rt_string_length(c, symbol);
        const GuestModule *module = NULL;
        if (handle == UINT32_MAX) rt_trap(c, c->eip, "RTLD_NEXT lookup not implemented");
        if (handle && handle != 0xccfff000 && handle != UINT32_MAX) {
            for (size_t i = 0; i < rt_module_count; ++i)
                if (handle == 0xcc000000u + rt_modules[i].id * 0x100) module = &rt_modules[i];
            if (!module) { loader_error("invalid guest module handle", GPTR(symbol)); c->eax = 0; return 1; }
        }
        c->eax = rt_module_symbol(module, GPTR(symbol));
        if (!c->eax) loader_error("guest symbol unavailable", GPTR(symbol));
    } else if (!strcmp(name, "dlerror")) {
        c->eax = error_string; error_string = 0;
    } else if (!strcmp(name, "dlclose")) {
        rt_trap(c, c->eip, "guest module finalization is not implemented");
    } else return 0;
    return 1;
}

void rt_modules_destroy(void) {
    free(state); state = NULL; boot_argc = boot_argv = boot_envp = error_string = 0;
}
