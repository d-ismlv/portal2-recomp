#include "rt.h"
#include <assert.h>
#include <stdio.h>
#include "cpu_checks.h"
#include "vm_checks.h"
#include "filesystem_checks.h"
#include "thread_checks.h"

static CPU run(uint32_t target) {
    CPU c; rt_cpu_reset(&c); c.esp = 0x20010000;
    rt_call(&c, target, RT_RETURN_SENTINEL);
    assert(c.esp == 0x20010000);
    assert(c.eip == RT_RETURN_SENTINEL);
    return c;
}
int main(void) {
    assert(rt_init() == 0);
    assert(rt_map(0x20000000, 0x20000) == 0);
    CPU c = run(0x1000);
    assert(c.eax == 1 && c.edx == 0x1003); // carry survives get_pc_thunk
    assert(run(0x3000).eax == 5); // indirect CALL, RET 8 cleans arguments
    assert(run(0x5000).eax == 99); // indirect tail call
    assert(run(0x7000).eax == 99); // direct tail call
    assert(run(0x8000).eax == 1); // caller sees callee's flags
    assert(run(0xa000).eax == 10); // recursive calls preserve guest stack
    W32(0x20001000, 7);
    c = run(0xc000);
    assert(c.ecx == 7 && c.eax == 9 && c.ebx == 1 && c.flags.zf);
    assert(R32(0x20001000) == 11);
    c = run(0xd000);
    assert(c.eax == 11 && c.ebx == 0 && !c.flags.zf && c.flags.cf);
    assert(R32(0x20001000) == 11);
    memcpy(GPTR(0x20002000), "abcz", 4);
    c = run(0xe000);
    assert(!memcmp(GPTR(0x20003000), "abcz", 4) && !c.ecx);
    memset(GPTR(0x20003000), 0, 4);
    c = run(0xf000);
    assert(!memcmp(GPTR(0x20003000), "abcz", 4) && !c.df && !c.ecx);
    c = run(0x10000);
    for (unsigned i = 0; i < 3; ++i) assert(R32(0x20003000 + 4 * i) == 0x12345678);
    assert(c.edi == 0x2000300c && !c.ecx);
    c = run(0x11000);
    assert(c.edi == 0x20002002 && c.ecx == 2 && c.flags.zf);
    run(0x12000); // zero repetitions must not access unmapped address zero
    memcpy(GPTR(0x20003000), "abxy", 4);
    c = run(0x13000);
    assert(c.ecx == 1 && !c.flags.zf && c.esi == 0x20002003 && c.edi == 0x20003003);
    assert(run(0x14000).eax == 0x5678);
    WD(0x20004000, 1.5); W32(0x20004008, (uint32_t)-4);
    W64(0x20004010, 0x1111111111111111); W64(0x20004018, 0x2222222222222222);
    c = run(0x15000);
    assert(c.eax == 3 && RD(0x20005000) == 3.5);
    c = run(0x16000);
    assert(R64(0x20005008) == 0x11223344);
    assert(RD(0x20005010) == 1.5 && R64(0x20005018) == 0x2222222222222222);
    assert(c.flags.cf && !c.flags.zf && !parity(c.flags.pf));
    c = run(0x17000);
    assert(!c.ftop && RD(0x20005020) == 2.5 && RF(0x20005028) == 4.0f);
    W32(0x20007000,1); c=run(0x19000);
    assert(R32(0x20007000)==0 && c.eax==1 && c.ecx==1 && c.edx==1);
    W32(0x20007000,10); c=run(0x1a000);
    assert(R32(0x20007000)==15 && c.eax==10 && !c.flags.cf);
    W64(0x20007008,UINT64_C(0x1122334455667788)); c=run(0x1b000);
    assert(c.flags.zf && R64(0x20007008)==UINT64_C(0xaabbccddeeff0011));
    c=run(0x1c000);
    assert(!c.flags.zf && c.flags.cf && c.eax==0xeeff0011 && c.edx==0xaabbccdd);
    c=run(0x1d000); assert(c.eax==c.gs_base && c.edx==R32(c.gs_base+20));
    assert(run(0x1e000).eax==104 && run(0x1f000).eax==0x1f001);
    WD(0x20008000, NAN); WD(0x20008008, -2.5);
    WD(0x20008010, 1); WD(0x20008018, -2.5);
    c=run(0x20000); assert(!c.xmm[0].q[0] && c.xmm[0].q[1]==UINT64_MAX);
    c=run(0x21000); assert(c.xmm[0].q[0]==UINT64_MAX && c.xmm[0].q[1]==UINT64_MAX);
    c=run(0x22000); assert(c.xmm[0].d[0]==0x80000000u && c.xmm[0].i[1]==-2 && !c.xmm[0].q[1]);
    c=run(0x23000); assert(!c.xmm[0].q[0] && !c.xmm[0].q[1]);
    WD(0x20008000, 3.5);
    c=run(0x24000); assert(c.xmm[0].i[0]==4 && c.xmm[0].i[1]==-2 && !c.xmm[0].q[1]);
    check_rotations();
    check_arithmetic_flags();
    check_cpu_runtime();
    check_virtual_memory();
    check_filesystem();
    check_threads();
    rt_destroy();
    puts("PASS: nested/recursive CALL, direct/indirect tail calls, flags, PIC, RET 8, atomics, REP/DF, SSE2 and x87");
    return 0;
}
