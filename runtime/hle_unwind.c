/* C++ exceptions across recompiled code.
 *
 * The game's own libunwind does all the work - it walks the guest stack with .eh_frame, asks
 * the personality routines, and finally resumes a frame at a landing pad by loading every
 * register from a context and returning to the pad (Registers_arm64::jumpto). Only that last
 * step is native: a recompiled frame is a host frame, so the pad is reached by unwinding the
 * host stack to the frame that owns it. The registers are loaded into the guest context, then
 * a host exception names the pad and the frame's stack pointer; the recompiler wraps every
 * call an exception may unwind through in a handler that matches exactly that pair
 * (rt_eh_filter), which jumps to the pad. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "host.h"

#define RT_EH_CODE 0xE0E4C5A0u

#ifdef _WIN32
int rt_eh_filter(struct _EXCEPTION_POINTERS *e, u64 sp, u64 lp) {
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode != RT_EH_CODE || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    return r->ExceptionInformation[0] == lp && (r->ExceptionInformation[1] == sp || sp == ~0ull) ? EXCEPTION_EXECUTE_HANDLER
                                                                               : EXCEPTION_CONTINUE_SEARCH;
}
#else
/* Linux: the host exception is a C++ one (linux/eh.cpp throws it; recomp.h's catch asks this) */
int rt_eh_filter(const RtEhSignal *e, u64 sp, u64 lp) { return e->lp == lp && (e->sp == sp || sp == ~0ull); }   /* (~0: a pad that sets its own sp) */
_Noreturn void rt_eh_raise(u64 lp, u64 sp);
#endif

/* libunwind's Registers_arm64: x0-x28, fp, lr at 0, sp at 0xF8, pc at 0x100, d0-d31 at 0x110. */
void rt_unwind_jumpto(Cpu *c) {
    u64 r = c->x[0];
    for (int i = 0; i <= 29; i++) c->x[i] = ld64(r + 8ull * i);
    u64 pc = ld64(r + 0x100);
    c->x[30] = pc;
    c->sp = ld64(r + 0xF8);
    for (int i = 0; i < 32; i++) { c->v[i].d[0] = ld64(r + 0x110 + 8ull * i); c->v[i].d[1] = 0; }
#ifdef _WIN32
    ULONG_PTR info[2] = {(ULONG_PTR)pc, (ULONG_PTR)c->sp};
    RaiseException(RT_EH_CODE, 0, 2, info);
#else
    rt_eh_raise(pc, c->sp);   /* (a frame takes it; if none does, linux/eh.cpp reports that) */
#endif
    char name[96];
    rt_fatal("C++ exception: no recompiled frame resumes landing pad %s (sp %#llx)", rt_describe(pc, name, sizeof name), c->sp);
}
