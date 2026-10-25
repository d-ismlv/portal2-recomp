#include "rt.h"
#include <stdlib.h>
#include <stdio.h>

#define HEAP_BEGIN 0x80000000u
#define HEAP_END 0xc0000000u

typedef struct Block {
    uint32_t addr, size;
    int used;
    struct Block *next;
} Block;
static Block *blocks;
static uint32_t heap_top = HEAP_BEGIN;

typedef struct { uint32_t function, arg, dso; } ExitHandler;
static ExitHandler *handlers;
static size_t handler_count, handler_capacity;

static Block *find_block(uint32_t ptr) {
    for (Block *b = blocks; b; b = b->next)
        if (b->addr == ptr && b->used) return b;
    return NULL;
}

uint32_t rt_alloc(uint32_t size) {
    uint64_t aligned = ((uint64_t)(size ? size : 1) + 15) & ~UINT64_C(15);
    if (aligned > HEAP_END - HEAP_BEGIN) return 0;
    Block **tail = &blocks;
    for (Block *b = blocks; b; b = b->next) {
        if (!b->used && b->size >= aligned) {
            if (b->size > aligned) {
                Block *rest = calloc(1, sizeof(*rest));
                if (!rest) return 0;
                rest->addr = b->addr + (uint32_t)aligned;
                rest->size = b->size - (uint32_t)aligned;
                rest->next = b->next;
                b->next = rest;
                b->size = (uint32_t)aligned;
            }
            b->used = 1;
            return b->addr;
        }
        tail = &b->next;
    }
    if (aligned > HEAP_END - heap_top) return 0;
    Block *b = calloc(1, sizeof(*b));
    if (!b) return 0;
    if (rt_map(heap_top, aligned)) { free(b); return 0; }
    b->addr = heap_top; b->size = (uint32_t)aligned; b->used = 1;
    heap_top += b->size;
    *tail = b;
    return b->addr;
}

int rt_free(uint32_t ptr) {
    if (!ptr) return 0;
    Block *b = find_block(ptr);
    if (!b) return -1;
    b->used = 0;
    for (b = blocks; b && b->next;) {
        if (!b->used && !b->next->used) {
            Block *next = b->next;
            b->size += next->size;
            b->next = next->next;
            free(next);
        } else b = b->next;
    }
    return 0;
}

uint32_t rt_realloc(uint32_t ptr, uint32_t size) {
    if (!ptr) return rt_alloc(size);
    Block *b = find_block(ptr);
    if (!b) return 0;
    if (!size) { rt_free(ptr); return 0; }
    if (size <= b->size) return ptr;
    uint32_t result = rt_alloc(size);
    if (!result) return 0; // preserve the original allocation on failure
    memcpy(GPTR(result), GPTR(ptr), b->size);
    rt_free(ptr);
    return result;
}

void rt_shims_destroy(void) {
    rt_pthreads_destroy();
    rt_futex_destroy();
    rt_libc_destroy();
    rt_tls_destroy();
    rt_modules_destroy();
    rt_vm_destroy();
    while (blocks) { Block *next = blocks->next; free(blocks); blocks = next; }
    heap_top = HEAP_BEGIN;
    free(handlers); handlers = NULL; handler_count = handler_capacity = 0;
}

// Statistiques du tas invité pour mallinfo (runtime/process.c).
void rt_heap_info(uint32_t *arena, uint32_t *used, uint32_t *free_bytes, uint32_t *free_blocks) {
    *arena = heap_top - HEAP_BEGIN; *used = *free_bytes = *free_blocks = 0;
    for (Block *b = blocks; b; b = b->next) {
        if (b->used) *used += b->size;
        else { *free_bytes += b->size; ++*free_blocks; }
    }
}
size_t rt_atexit_count(void) { return handler_count; }
int rt_atexit_get(size_t index, uint32_t *function, uint32_t *arg, uint32_t *dso) {
    if (index >= handler_count) return -1;
    *function = handlers[index].function;
    *arg = handlers[index].arg;
    *dso = handlers[index].dso;
    return 0;
}

static uint32_t arg(CPU *c, unsigned index) { return R32(c->esp + 4 + 4 * index); }
void *rt_span(CPU *c, uint32_t address, uint64_t length) {
    if (length > (UINT64_C(1) << 32) - address || (length && !address))
        rt_trap(c, c->eip, "invalid libc guest span");
    rt_check_span(address, length, 1);
    return GPTR(address);
}
void *rt_write_span(CPU *c, uint32_t address, uint64_t length) {
    void *result = rt_span(c, address, length); rt_check_span(address, length, 2); return result;
}
uint32_t rt_string_length(CPU *c, uint32_t address) {
    if (!address) rt_trap(c, c->eip, "null guest string");
    uint64_t pos = address;
    while (pos < (UINT64_C(1) << 32)) {
        if (!R8((uint32_t)pos)) return (uint32_t)(pos - address);
        ++pos;
    }
    rt_trap(c, c->eip, "unterminated guest string");
}

static rt_shim_provider providers[64];
static size_t provider_count;
void rt_register_shim_provider(rt_shim_provider provider) {
    if (provider_count == sizeof(providers) / sizeof(*providers)) abort();
    providers[provider_count++] = provider;
}
static int registered_shim(CPU *c, const char *name) {
    for (size_t i = 0; i < provider_count; ++i) if (providers[i](c, name)) return 1;
    return 0;
}

// Return 0 for an unsupported name. The dispatcher traps, with no fake result.
// Argument layouts here are i386 cdecl, never the host's variadic/pointer ABI.
int rt_shim(CPU *c, const char *name) {
    uint32_t a, b, n;
    if (!strcmp(name, "___tls_get_addr") || !strcmp(name, "__tls_get_addr")) {
        a = c->eax; rt_span(c, a, 8); // i386 TLS resolver uses regparm(1), not cdecl
        c->eax = rt_tls_address(c, R32(a), R32(a + 4));
    } else if (!strcmp(name, "memset")) {
        a = arg(c, 0); b = arg(c, 1); n = arg(c, 2);
        memset(rt_write_span(c, a, n), (int)b, n); c->eax = a;
    } else if (!strcmp(name, "memcpy") || !strcmp(name, "memmove")) {
        a = arg(c, 0); b = arg(c, 1); n = arg(c, 2);
        void *dst = rt_write_span(c, a, n); void *src = rt_span(c, b, n);
        // memmove also gives a deterministic result for overlapping memcpy;
        // conforming callers must not rely on that extension.
        memmove(dst, src, n); c->eax = a;
    } else if (!strcmp(name, "memcmp")) {
        a = arg(c, 0); b = arg(c, 1); n = arg(c, 2);
        void *lhs = rt_span(c, a, n); void *rhs = rt_span(c, b, n);
        c->eax = (uint32_t)memcmp(lhs, rhs, n);
    } else if (!strcmp(name, "strlen")) {
        c->eax = rt_string_length(c, arg(c, 0));
    } else if (!strcmp(name, "strcpy") || !strcmp(name, "stpcpy")) {
        a = arg(c, 0); b = arg(c, 1); n = rt_string_length(c, b);
        memmove(rt_write_span(c, a, (uint64_t)n + 1), GPTR(b), (size_t)n + 1);
        c->eax = !strcmp(name, "stpcpy") ? a + n : a;
    } else if (!strcmp(name, "strncpy")) {
        a = arg(c, 0); b = arg(c, 1); n = arg(c, 2);
        rt_write_span(c, a, n);
        uint32_t i = 0;
        while (i < n) {
            rt_span(c, b, (uint64_t)i + 1);
            uint8_t value = R8(b + i);
            W8(a + i, value);
            if (!value) break;
            ++i;
        }
        if (i < n) memset(GPTR(a + i), 0, n - i);
        c->eax = a;
    } else if (!strcmp(name, "strcmp")) {
        a = arg(c, 0); b = arg(c, 1);
        rt_string_length(c, a); rt_string_length(c, b);
        c->eax = (uint32_t)strcmp(GPTR(a), GPTR(b));
    } else if (!strcmp(name, "malloc")) {
        c->eax = rt_alloc(arg(c, 0));
    } else if (!strcmp(name, "calloc")) {
        uint64_t size = (uint64_t)arg(c, 0) * arg(c, 1);
        c->eax = size > UINT32_MAX ? 0 : rt_alloc((uint32_t)size);
        if (c->eax) memset(GPTR(c->eax), 0, (size_t)size);
    } else if (!strcmp(name, "realloc")) {
        a = arg(c, 0); b = arg(c, 1);
        if (a && !find_block(a)) rt_trap(c, c->eip, "invalid guest realloc");
        c->eax = rt_realloc(a, b);
    } else if (!strcmp(name, "free")) {
        if (rt_free(arg(c, 0))) rt_trap(c, c->eip, "invalid guest free");
    } else if (!strcmp(name, "malloc_usable_size")) {
        a = arg(c, 0); Block *block = find_block(a);
        if (a && !block) rt_trap(c, c->eip, "invalid guest malloc_usable_size");
        c->eax = block ? block->size : 0;
    } else if (!strcmp(name, "__cxa_atexit")) {
        if (handler_count == handler_capacity) {
            size_t capacity = handler_capacity ? handler_capacity * 2 : 32;
            ExitHandler *grown = realloc(handlers, capacity * sizeof(*grown));
            if (!grown) { c->eax = UINT32_MAX; return 1; }
            handlers = grown; handler_capacity = capacity;
        }
        handlers[handler_count++] = (ExitHandler){arg(c, 0), arg(c, 1), arg(c, 2)};
        c->eax = 0;
    } else if (!strcmp(name, "puts")) {
        a = arg(c, 0); rt_string_length(c, a);
        c->eax = (uint32_t)puts(GPTR(a));
    } else if (!strcmp(name, "abort")) {
        rt_trap(c, c->eip, "guest libc abort");
    } else return rt_format_shim(c, name) || rt_pthread_shim(c, name) || rt_libc_shim(c, name) || rt_scan_shim(c, name) || rt_loader_shim(c, name) || rt_stat_shim(c, name) || registered_shim(c, name);
    return 1;
}
