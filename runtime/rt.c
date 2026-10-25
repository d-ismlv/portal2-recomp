#include "rt.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

uint8_t *M;
static const uint64_t address_space = UINT64_C(1) << 32;
typedef struct { uint32_t target, caller; } GuestFrame;
static _Thread_local GuestFrame frames[256];
static _Thread_local unsigned frame_depth;
static void guest_backtrace(void) {
    unsigned count=frame_depth<256?frame_depth:256;
    unsigned first=count>16?count-16:0;
    for(unsigned i=count;i>first;--i)
        fprintf(stderr,"  guest call %#x, return %#x\n",frames[i-1].target,frames[i-1].caller);
}

int rt_init(void) {
    if (M) return -1;
    void *p = mmap(NULL, (size_t)address_space, PROT_NONE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("guest address space"); return -1; }
    M = p;
    // RT_RETURN_SENTINEL is the return address of every outermost guest
    // frame. Unwinders (libgcc's fallback for frames without FDE) read the
    // code at a return address to detect signal trampolines: back it with a
    // readable page of HLT bytes, which matches no trampoline and has no
    // unwind data, so stack walks end there as at the bottom of a process.
    if (rt_map(RT_RETURN_SENTINEL & ~UINT32_C(0xfff), 0x1000)) { perror("guest sentinel page"); return -1; }
    memset(M + (RT_RETURN_SENTINEL & ~UINT32_C(0xfff)), 0xf4, 0x1000);
    return 0;
}

void rt_destroy(void) {
    rt_shims_destroy();
    if (M) munmap(M, (size_t)address_space);
    M = NULL;
}

int rt_map(uint32_t addr, uint64_t size) {
    if (!M || !size || size > address_space - addr) return -1;
    uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    uint64_t lo = addr & ~(page - 1);
    uint64_t hi = ((uint64_t)addr + size + page - 1) & ~(page - 1);
    return mprotect(M + lo, (size_t)(hi - lo), PROT_READ | PROT_WRITE);
}

int rt_load_segment(const char *directory, const char *name, uint32_t offset,
                    uint32_t addr, uint32_t filesz, uint32_t memsz) {
    if (filesz > memsz || !memsz) return -1;
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return -1; }
    int failed = fseek(file, (long)offset, SEEK_SET) || rt_map(addr, memsz);
    if (!failed) {
        failed = fread(M + addr, 1, filesz, file) != filesz;
        if (!failed) memset(M + (uint64_t)addr + filesz, 0, memsz - filesz);
    }
    fclose(file);
    return failed ? -1 : 0;
}

__attribute__((noreturn)) void rt_trap(CPU *c, uint32_t pc, const char *why) {
    fprintf(stderr, "guest trap at %#x (esp=%#x): %s\n", pc, c->esp, why);
    guest_backtrace();
    abort();
}

// CPU invité du shim en cours sur ce thread hôte : un rappel natif (par ex.
// un SDL_EventWatch déclenché pendant un appel SDL) s'en sert pour exécuter
// une fonction invitée sur le même thread et la même pile.
static _Thread_local CPU *shim_cpu;
CPU *rt_shim_cpu(void) { return shim_cpu; }

// Cache d'aiguillage des imports (statiques 0xf0000000+, dynamiques
// 0xf8000000+, pas de 16) : pour chaque adresse, le nom et la route du
// fournisseur qui l'a accepté (rt_shim_route). Les entrées sont immuables,
// publiées par échange atomique et jamais libérées (les noms sont statiques
// ou conservés par bridge.c). Un nom inconnu n'est jamais mis en cache.
#define ROUTE_BASE 0xf0000000u
#define ROUTE_CHUNK_BITS 16
typedef struct { const char *name; RtShimRoute route; } ImportRoute;
typedef ImportRoute *_Atomic RouteSlot;
static RouteSlot *_Atomic route_chunks[((UINT64_C(1) << 32) - ROUTE_BASE) >> (4 + ROUTE_CHUNK_BITS)];

static RouteSlot *route_slot(uint32_t target, int create) {
    if (target < ROUTE_BASE || (target & 15)) return NULL;
    uint32_t index = (target - ROUTE_BASE) >> 4;
    RouteSlot *_Atomic *chunk = &route_chunks[index >> ROUTE_CHUNK_BITS];
    RouteSlot *slots = atomic_load_explicit(chunk, memory_order_acquire);
    if (!slots && create) {
        RouteSlot *fresh = calloc((size_t)1 << ROUTE_CHUNK_BITS, sizeof(*fresh));
        if (!fresh) return NULL;
        if (atomic_compare_exchange_strong_explicit(chunk, &slots, fresh, memory_order_acq_rel, memory_order_acquire))
            slots = fresh;
        else free(fresh);  // un autre thread l'a installé : slots le désigne
    }
    return slots ? &slots[index & ((1u << ROUTE_CHUNK_BITS) - 1)] : NULL;
}

static int run_shim(CPU *c, const char *name, const RtShimRoute *route, RtShimRoute *learned) {
    CPU *outer = shim_cpu;
    shim_cpu = c;
    int handled = route ? rt_route_call(route, c, name) : rt_shim_route(c, name, learned);
    shim_cpu = outer;
    return handled;
}

void rt_dispatch(CPU *c, uint32_t target) {
    rt_guest_poll();
    c->eip = target;
    gfn fn = rt_lookup(target);
    if (fn) { fn(c); return; }
    RouteSlot *slot = route_slot(target, 0);
    ImportRoute *cached = slot ? atomic_load_explicit(slot, memory_order_acquire) : NULL;
    // Route connue : même fournisseur que la chaîne complète (contrat dans
    // rt.h) ; s'il refuse malgré tout, la chaîne est reprise ci-dessous.
    if (cached && run_shim(c, cached->name, &cached->route, NULL)) { rt_return(c); return; }
    const char *name = cached ? cached->name : rt_import_name(target);
    if (!name) name = rt_dynamic_import_name(target);
    if (name) {
        RtShimRoute route;
        if (run_shim(c, name, NULL, &route)) {
            if ((slot = route_slot(target, 1))) {
                ImportRoute *entry = malloc(sizeof(*entry));
                if (entry) {
                    *entry = (ImportRoute){name, route};
                    ImportRoute *old = atomic_exchange_explicit(slot, entry, memory_order_acq_rel);
                    (void)old;  // éventuel lecteur concurrent : l'ancienne entrée reste valide, jamais libérée
                }
            }
            rt_return(c);
            return;
        }
    }
    rt_trap(c, target, name ? name : "target not recompiled");
}

void rt_return(CPU *c) { c->eip = POP(c); }

// Non-local control flow (longjmp, an unwinder installing a landing pad):
// the guest resumes at an address other than the return site of the call in
// progress. Each lifted call site compares c->eip with its own return site;
// on mismatch it either resumes at a known point of its function (setjmp
// return sites, see tools/lift.py) or returns, so host C frames are dropped
// one by one until a frame expects that address. Every C frame can run any
// guest code because all guest state lives in CPU and guest memory. Calls
// made by native code use RT_RETURN_SENTINEL and never let a transfer escape.
static _Thread_local struct { uint32_t from_target, expected, actual; unsigned depth; } nonlocal;

void rt_call(CPU *c, uint32_t target, uint32_t expected_return) {
    rt_guest_enter();
    if(frame_depth<256)frames[frame_depth]=(GuestFrame){target,expected_return};
    ++frame_depth;
    PUSH(c, expected_return);
    rt_dispatch(c, target);
    if (c->eip != expected_return) {
        if (!nonlocal.actual || nonlocal.actual != c->eip)
            nonlocal = (typeof(nonlocal)){target, expected_return, c->eip, frame_depth};
        if (expected_return == RT_RETURN_SENTINEL) {
            fprintf(stderr, "non-local transfer to %#x (first seen returning from %#x, expected %#x, depth %u) "
                    "is not resumable by any recompiled frame\n", nonlocal.actual, nonlocal.from_target,
                    nonlocal.expected, nonlocal.depth);
            rt_trap(c, c->eip, "guest control left a native call boundary");
        }
    } else nonlocal.actual = 0;
    --frame_depth;
    rt_guest_leave();
}

uint32_t rt_exchange32(CPU *c, uint32_t address, uint32_t value) {
    if (address & 3) rt_trap(c, c->eip, "unaligned atomic exchange is unsupported");
    return __atomic_exchange_n((uint32_t *)(void *)(M + address), value, __ATOMIC_SEQ_CST);
}
uint32_t rt_compare_exchange32(CPU *c, uint32_t address, uint32_t expected, uint32_t value) {
    if (address & 3) rt_trap(c, c->eip, "unaligned atomic compare-exchange is unsupported");
    __atomic_compare_exchange_n((uint32_t *)(void *)(M + address), &expected, value,
                                0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

uint32_t rt_atomic32(CPU *c, uint32_t address, uint32_t value, int operation) {
    if (address & 3) rt_trap(c, c->eip, "unaligned atomic arithmetic is unsupported");
    uint32_t *pointer = (uint32_t *)(void *)(M + address);
    switch (operation) {
    case 0: return __atomic_fetch_add(pointer, value, __ATOMIC_SEQ_CST);
    case 1: return __atomic_fetch_sub(pointer, value, __ATOMIC_SEQ_CST);
    case 2: return __atomic_fetch_and(pointer, value, __ATOMIC_SEQ_CST);
    case 3: return __atomic_fetch_or(pointer, value, __ATOMIC_SEQ_CST);
    case 4: return __atomic_fetch_xor(pointer, value, __ATOMIC_SEQ_CST);
    default: rt_trap(c, c->eip, "unknown atomic arithmetic operation");
    }
}

uint64_t rt_compare_exchange64(CPU *c, uint32_t address, uint64_t expected, uint64_t value) {
    if (address & 7) rt_trap(c, c->eip, "unaligned 64-bit atomic compare-exchange is unsupported");
    __atomic_compare_exchange_n((uint64_t *)(void *)(M + address), &expected, value,
                                0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

__attribute__((noreturn)) void rt_memory_trap(uint32_t address, uint64_t length, int mode) {
    fprintf(stderr, "guest memory fault at %#x length=%llu access=%s\n", address,
            (unsigned long long)length, mode == 2 ? "write" : "read");
    guest_backtrace();
    abort();
}
void rt_check_span(uint32_t address, uint64_t length, int mode) {
    if (!length) return;
    uint64_t end = (uint64_t)address + length;
    if (end > (UINT64_C(1) << 32)) rt_memory_trap(address, length, mode);
    uint64_t lo = address < RT_VM_BEGIN ? RT_VM_BEGIN : address;
    uint64_t hi = end > RT_VM_END ? RT_VM_END : end;
    for (uint64_t page = lo & ~UINT64_C(4095); page < hi; page += 4096)
        if ((rt_vm_permissions[(page - RT_VM_BEGIN) >> 12] & mode) != mode)
            rt_memory_trap(address, length, mode);
}
