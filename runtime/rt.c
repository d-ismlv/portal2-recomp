#include "rt.h"
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

void rt_dispatch(CPU *c, uint32_t target) {
    rt_guest_poll();
    c->eip = target;
    gfn fn = rt_lookup(target);
    if (fn) { fn(c); return; }
    const char *name = rt_import_name(target);
    if (!name) name = rt_dynamic_import_name(target);
    if (name && rt_shim(c, name)) { rt_return(c); return; }
    rt_trap(c, target, name ? name : "target not recompiled");
}

void rt_return(CPU *c) { c->eip = POP(c); }

void rt_call(CPU *c, uint32_t target, uint32_t expected_return) {
    rt_guest_enter();
    if(frame_depth<256)frames[frame_depth]=(GuestFrame){target,expected_return};
    ++frame_depth;
    PUSH(c, expected_return);
    rt_dispatch(c, target);
    if (c->eip != expected_return)
        rt_trap(c, c->eip, "guest callee returned to unexpected address");
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
