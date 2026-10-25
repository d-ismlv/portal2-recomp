// setjmp/longjmp for recompiled guest code.
// SHIMS: _setjmp longjmp
//
// glibc i386 jmp_buf: __jmpbuf = {EBX, ESI, EDI, EBP, ESP, PC} at 0..20,
// __mask_was_saved at 24, then the saved signal mask. glibc mangles ESP, EBP
// and PC with the TCB pointer guard; no guest code reads these fields, so they
// are stored in clear. longjmp restores the registers and resumes at the saved
// PC; host frames are unwound by the non-local transfer protocol of rt_call
// (runtime/rt.c), and the lifter makes every statically known setjmp return
// site a resume point of its function (tools/lift.py).
#include "rt.h"

static int nonlocal_shim(CPU *c, const char *name) {
    if (!strcmp(name, "_setjmp")) {
        uint32_t env = R32(c->esp + 4);
        rt_write_span(c, env, 28);
        W32(env, c->ebx); W32(env + 4, c->esi); W32(env + 8, c->edi); W32(env + 12, c->ebp);
        W32(env + 16, c->esp + 4);       // caller's ESP once this call has returned
        W32(env + 20, R32(c->esp));      // return address: where longjmp resumes
        W32(env + 24, 0);                // _setjmp never saves the signal mask
        c->eax = 0;
        return 1;
    }
    if (!strcmp(name, "longjmp")) {
        uint32_t env = R32(c->esp + 4), value = R32(c->esp + 8);
        rt_span(c, env, 28);
        if (R32(env + 24)) rt_trap(c, c->eip, "longjmp: saved signal masks are not implemented");
        uint32_t sp = R32(env + 16), pc = R32(env + 20);
        // The target frame must still be active: it lies above this call.
        if (sp < c->esp + 4) rt_trap(c, c->eip, "longjmp to a frame that has already returned");
        c->ebx = R32(env); c->esi = R32(env + 4); c->edi = R32(env + 8); c->ebp = R32(env + 12);
        c->eax = value ? value : 1;
        // rt_dispatch pops the return address after a shim: make it the saved
        // PC, in the dead stack slot that held setjmp's own return address.
        c->esp = sp - 4;
        W32(c->esp, pc);
        return 1;
    }
    return 0;
}
RT_SHIM_PROVIDER(nonlocal_shim)
