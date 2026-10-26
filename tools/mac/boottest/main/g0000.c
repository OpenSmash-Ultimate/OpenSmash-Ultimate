#include "recomp.h"
#include "hookable.h"

/* The boot test's main (tools/mac/boottest): hand-written as tools/recomp.py writes a module's code,
 * at main's address. nnMain checks what the loader and the kernel did and exercises system calls:
 * memory, a second guest thread, waits, sleeps, atomics, a guest setjmp the runtime's longjmp comes
 * back to, a guest C++ exception thrown through a hook slot (f_7100001500) by libunwind's jumpto
 * (the runtime's, rt_unwind_jumpto) and caught at its landing pad (with build_mac.py --game-dll,
 * thrown in the program and caught in the game's library: tools/game_dll.py); each check's line is
 * in x20/x21 as it runs (said once it passes; a failure says it and stops with svcBreak), then
 * svcExitProcess. A string is at the address before it ("..."): tools/mac/boottest/boottest.py
 * puts it in the image there.
 *   heap + 0x4000  the thread's argument: its event's handle; + 0x8 its result, + 0x10 its handle,
 *                  + 0x18 the argument as it saw it
 *   heap + 0x5000  the counter both threads increment
 *   heap + 0x7000  the exception's context, as libunwind's Registers_arm64 has it (jumpto loads it)
 *   0x7701000000   the thread's stack (64 KiB), mapped from heap + 0x1000000 */

GuestFn f_7100000100;
GuestFn f_7100000200;
GuestFn f_7100000300;
GuestFn f_7100001000;
GuestFn f_7100001100;
GuestFn f_7100001200;
GuestFn f_7100001300;
GuestFn f_7100001400;
GuestFn f_7100001500;
GuestFn f_7100001580;
GuestFn f_7100007f00;
GuestFn f_7100007f10;
extern RtHook rt_hook_7100001500;

void f_7100000100(Cpu *c) {
    u32 nf = 0, zf = 0, cf = 0, vf = 0; u64 ex_a = 0, ex_v = 0;
    X(8) = 0x710000A000ull;
    X(8) = ld64(X(8) + 0x10ull);
    X(9) = ld32(X(8) + 0x0ull);
    X(10) = X(8) + X(9);
    X(11) = 0x4Dull;
    st8(X(10) + 0x4ull, (u8)X(11));
    X(9) = (u32)((u32)X(9) + 0x1u);
    st32(X(8) + 0x0ull, (u32)X(9));
    return;
}

void f_7100000200(Cpu *c) {
    u32 nf = 0, zf = 0, cf = 0, vf = 0; u64 ex_a = 0, ex_v = 0;
    X(8) = 0x7100009000ull;
    X(9) = 0x1ull;
    st32(X(8) + 0x0ull, (u32)X(9));
    return;
}

void f_7100000300(Cpu *c) {
    u32 nf = 0, zf = 0, cf = 0, vf = 0; u64 ex_a = 0, ex_v = 0;
    { u64 a_ = SP + 0xFFFFFFFFFFFFFFC0ull; st64(a_, (u64)X(29)); st64(a_ + 8, (u64)X(30)); SP = a_; }
    { u64 a_ = SP + 0x10ull; st64(a_, (u64)X(19)); st64(a_ + 8, (u64)X(20)); }
    { u64 a_ = SP + 0x20ull; st64(a_, (u64)X(21)); st64(a_ + 8, (u64)X(22)); }
    st64(SP + 0x30ull, (u64)X(23));
    X(29) = SP + 0x0ull;
    X(20) = 0x7100008100ull; X(21) = 46;   /* "boottest: nnMain runs on the guest main thread" */
    X(30) = 0x710000032Cull; f_7100001200(c);

    X(20) = 0x7100008180ull; X(21) = 64;   /* "boottest: the three modules' images are at their guest addresses" */
    X(8) = 0x7100008000ull;
    X(9) = ld64(X(8) + 0x0ull);
    X(10) = 0x6E69616Dull;
    { u64 a_ = X(9), b_ = X(10), r_ = a_ - b_; nf = r_ >> 63; zf = r_ == 0; cf = a_ >= b_; vf = ((a_ ^ b_) & (a_ ^ r_)) >> 63; (void)(r_); }
    if (!zf) goto L_ff0;
    X(8) = 0x7108008000ull;
    X(9) = ld64(X(8) + 0x0ull);
    X(10) = 0x306B6473627573ull;
    X(9) = X(9) ^ X(10);
    if (X(9) != 0) goto L_ff0;
    X(8) = 0x7109008000ull;
    X(9) = ld64(X(8) + 0x0ull);
    X(10) = 0x6B6473ull;
    X(9) = X(9) ^ X(10);
    if (X(9) != 0) goto L_ff0;
    X(30) = 0x7100000378ull; f_7100001200(c);

    X(20) = 0x7100008200ull; X(21) = 41;   /* "boottest: nninitStartup ran before nnMain" */
    X(8) = 0x7100009000ull;
    X(9) = ld32(X(8) + 0x0ull);
    X(9) = (u32)((u32)X(9) ^ 0x1u);
    if ((u32)X(9) != 0) goto L_ff0;
    X(30) = 0x7100000394ull; f_7100001200(c);

    X(20) = 0x7100008280ull; X(21) = 72;   /* "boottest: the modules' initialisers ran, the sdk's first and main's last" */
    X(8) = 0x710000A000ull;
    X(8) = ld64(X(8) + 0x10ull);
    X(10) = 0x7109009000ull;
    X(9) = X(8) ^ X(10);
    if (X(9) != 0) goto L_ff0;
    X(9) = ld32(X(8) + 0x0ull);
    { u32 a_ = (u32)X(9), b_ = 0x3u, r_ = a_ - b_; nf = r_ >> 31; zf = r_ == 0; cf = a_ >= b_; vf = ((a_ ^ b_) & (a_ ^ r_)) >> 31; (void)(r_); }
    if (!zf) goto L_ff0;
    X(9) = ld32(X(8) + 0x4ull);
    X(10) = 0x4D5553ull;
    X(9) = (u32)((u32)X(9) ^ (u32)X(10));
    if ((u32)X(9) != 0) goto L_ff0;
    X(30) = 0x71000003D4ull; f_7100001200(c);

    X(20) = 0x7100008300ull; X(21) = 44;   /* "boottest: a weak import nothing defines is 0" */
    X(8) = 0x710000A000ull;
    X(9) = ld64(X(8) + 0x18ull);
    if (X(9) != 0) goto L_ff0;
    X(30) = 0x71000003ECull; f_7100001200(c);

    X(20) = 0x7100008380ull; X(21) = 60;   /* "boottest: a call into subsdk0 through an import: 40 + 2 = 42" */
    X(0) = 0x28ull;
    X(1) = 0x2ull;
    X(30) = 0x7100000404ull; f_7100007f10(c);
    { u64 a_ = X(0), b_ = 0x2Aull, r_ = a_ - b_; nf = r_ >> 63; zf = r_ == 0; cf = a_ >= b_; vf = ((a_ ^ b_) & (a_ ^ r_)) >> 63; (void)(r_); }
    if (!zf) goto L_ff0;
    X(30) = 0x7100000410ull; f_7100001200(c);

    X(20) = 0x7100008400ull; X(21) = 68;   /* "boottest: svcGetInfo: the alias area, the heap and the address space" */
    X(1) = 0x2ull;
    X(2) = 0x0ull;
    X(3) = 0x0ull;
    rt_svc(c, 0x29);
    if ((u32)X(0) != 0) goto L_ff0;
    X(10) = 0x6000000000ull;
    X(9) = X(1) ^ X(10);
    if (X(9) != 0) goto L_ff0;
    X(1) = 0x4ull;
    X(2) = 0x0ull;
    X(3) = 0x0ull;
    rt_svc(c, 0x29);
    if ((u32)X(0) != 0) goto L_ff0;
    X(10) = 0x7400000000ull;
    X(9) = X(1) ^ X(10);
    if (X(9) != 0) goto L_ff0;
    X(1) = 0xDull;
    X(2) = 0x0ull;
    X(3) = 0x0ull;
    rt_svc(c, 0x29);
    if ((u32)X(0) != 0) goto L_ff0;
    X(10) = 0x7FF8000000ull;
    X(9) = X(1) ^ X(10);
    if (X(9) != 0) goto L_ff0;
    X(30) = 0x7100000478ull; f_7100001200(c);

    X(20) = 0x7100008480ull; X(21) = 71;   /* "boottest: svcSetHeapSize: 32 MiB of heap, zeroed, written and read back" */
    X(1) = 0x2000000ull;
    rt_svc(c, 0x1);
    if ((u32)X(0) != 0) goto L_ff0;
    X(22) = X(1);
    X(10) = 0x7400000000ull;
    X(9) = X(22) ^ X(10);
    if (X(9) != 0) goto L_ff0;
    X(9) = ld64(X(22) + 0x100000ull);
    if (X(9) != 0) goto L_ff0;
    X(10) = 0x1122334455667788ull;
    st64(X(22) + 0x0ull, (u64)X(10));
    X(11) = X(22) + 0x1FF0000ull;
    st64(X(11) + 0xFFF8ull, (u64)X(10));
    X(9) = ld64(X(22) + 0x0ull);
    X(9) = X(9) ^ X(10);
    if (X(9) != 0) goto L_ff0;
    X(9) = ld64(X(11) + 0xFFF8ull);
    X(9) = X(9) ^ X(10);
    if (X(9) != 0) goto L_ff0;
    X(30) = 0x71000004CCull; f_7100001200(c);

    X(20) = 0x7100008500ull; X(21) = 43;   /* "boottest: svcQueryMemory: the heap's region" */
    X(0) = X(22) + 0x6000ull;
    X(2) = X(22) + 0x10ull;
    rt_svc(c, 0x6);
    if ((u32)X(0) != 0) goto L_ff0;
    X(9) = ld64(X(22) + 0x6000ull);
    X(9) = X(9) ^ X(22);
    if (X(9) != 0) goto L_ff0;
    X(9) = ld64(X(22) + 0x6008ull);
    X(10) = 0x2000000ull;
    X(9) = X(9) ^ X(10);
    if (X(9) != 0) goto L_ff0;
    X(9) = ld32(X(22) + 0x6010ull);
    { u32 a_ = (u32)X(9), b_ = 0x5u, r_ = a_ - b_; nf = r_ >> 31; zf = r_ == 0; cf = a_ >= b_; vf = ((a_ ^ b_) & (a_ ^ r_)) >> 31; (void)(r_); }
    if (!zf) goto L_ff0;
    X(9) = ld32(X(22) + 0x6018ull);
    { u32 a_ = (u32)X(9), b_ = 0x3u, r_ = a_ - b_; nf = r_ >> 31; zf = r_ == 0; cf = a_ >= b_; vf = ((a_ ^ b_) & (a_ ^ r_)) >> 31; (void)(r_); }
    if (!zf) goto L_ff0;
    X(30) = 0x7100000530ull; f_7100001200(c);

    X(20) = 0x7100008580ull; X(21) = 60;   /* "boottest: dc zva zeroes its 64 bytes and nothing around them" */
    X(8) = X(22) + 0x2000ull;
    X(9) = 0xFFFFFFFFFFFFFFFFull;
    { u64 a_ = X(8) + 0xFFFFFFFFFFFFFFF0ull; st64(a_, (u64)X(9)); st64(a_ + 8, (u64)X(9)); }
    { u64 a_ = X(8) + 0x0ull; st64(a_, (u64)X(9)); st64(a_ + 8, (u64)X(9)); }
    { u64 a_ = X(8) + 0x10ull; st64(a_, (u64)X(9)); st64(a_ + 8, (u64)X(9)); }
    { u64 a_ = X(8) + 0x20ull; st64(a_, (u64)X(9)); st64(a_ + 8, (u64)X(9)); }
    { u64 a_ = X(8) + 0x30ull; st64(a_, (u64)X(9)); st64(a_ + 8, (u64)X(9)); }
    { u64 a_ = X(8) + 0x40ull; st64(a_, (u64)X(9)); st64(a_ + 8, (u64)X(9)); }
    X(10) = X(8) + 0x28ull;
    rt_zva(X(10));
    { u64 a_ = X(8) + 0x0ull; X(11) = ld64(a_); X(12) = ld64(a_ + 8); }
    X(11) = X(11) | X(12);
    if (X(11) != 0) goto L_ff0;
    { u64 a_ = X(8) + 0x30ull; X(11) = ld64(a_); X(12) = ld64(a_ + 8); }
    X(11) = X(11) | X(12);
    if (X(11) != 0) goto L_ff0;
    X(11) = ld64(X(8) + 0xFFFFFFFFFFFFFFF8ull);
    X(12) = ld64(X(8) + 0x40ull);
    X(11) = X(11) & X(12);
    { u64 a_ = X(11), b_ = X(9), r_ = a_ - b_; nf = r_ >> 63; zf = r_ == 0; cf = a_ >= b_; vf = ((a_ ^ b_) & (a_ ^ r_)) >> 63; (void)(r_); }
    if (!zf) goto L_ff0;
    X(30) = 0x71000005A0ull; f_7100001200(c);

    X(20) = 0x7100008600ull; X(21) = 24;   /* "boottest: stlr then ldar" */
    X(8) = X(22) + 0x3000ull;
    X(9) = 0x5ull;
    sta32(X(8), (u32)X(9));
    X(10) = lda32(X(8));
    X(10) = (u32)((u32)X(10) ^ 0x5u);
    if ((u32)X(10) != 0) goto L_ff0;
    X(9) = 0xF00DF00DF00Dull;
    sta64(X(8), (u64)X(9));
    X(10) = lda64(X(8));
    X(10) = X(10) ^ X(9);
    if (X(10) != 0) goto L_ff0;
    X(30) = 0x71000005DCull; f_7100001200(c);

    X(20) = 0x7100008680ull; X(21) = 67;   /* "boottest: svcSleepThread(20 ms) took at least 20 ms of system ticks" */
    rt_svc(c, 0x1e);
    X(23) = X(0);
    X(0) = 0x1312D00ull;
    rt_svc(c, 0xb);
    rt_svc(c, 0x1e);
    X(9) = X(0) - X(23);
    X(10) = 0x5DC00ull;
    { u64 a_ = X(9), b_ = X(10), r_ = a_ - b_; nf = r_ >> 63; zf = r_ == 0; cf = a_ >= b_; vf = ((a_ ^ b_) & (a_ ^ r_)) >> 63; (void)(r_); }
    if (!cf) goto L_ff0;
    X(10) = 0x5B8D800ull;
    { u64 a_ = X(9), b_ = X(10), r_ = a_ - b_; nf = r_ >> 63; zf = r_ == 0; cf = a_ >= b_; vf = ((a_ ^ b_) & (a_ ^ r_)) >> 63; (void)(r_); }
    if (cf) goto L_ff0;
    X(30) = 0x7100000614ull; f_7100001200(c);

    X(20) = 0x7100008700ull; X(21) = 55;   /* "boottest: svcMapMemory: a thread's stack, from the heap" */
    X(0) = 0x7701000000ull;
    X(1) = X(22) + 0x1000000ull;
    X(2) = 0x10000ull;
    rt_svc(c, 0x4);
    if ((u32)X(0) != 0) goto L_ff0;
    X(30) = 0x7100000630ull; f_7100001200(c);

    X(20) = 0x7100008780ull; X(21) = 44;   /* "boottest: svcCreateThread and svcStartThread" */
    rt_svc(c, 0x45);
    if ((u32)X(0) != 0) goto L_ff0;
    st32(X(22) + 0x4000ull, (u32)X(1));
    X(1) = 0x7100001000ull;
    X(2) = X(22) + 0x4000ull;
    X(3) = 0x7701010000ull;
    X(4) = 0x2Cull;
    X(5) = 0x0ull;
    rt_svc(c, 0x8);
    if ((u32)X(0) != 0) goto L_ff0;
    st32(X(22) + 0x4010ull, (u32)X(1));
    X(0) = (u32)X(1);
    rt_svc(c, 0x9);
    if ((u32)X(0) != 0) goto L_ff0;
    X(30) = 0x7100000678ull; f_7100001200(c);
    X(0) = X(22) + 0x5000ull;
    X(30) = 0x7100000680ull; f_7100001400(c);

    X(20) = 0x7100008800ull; X(21) = 71;   /* "boottest: the guest thread signalled its event (svcWaitSynchronization)" */
    X(1) = X(22) + 0x4000ull;
    X(2) = 0x1ull;
    X(3) = 0x12A05F200ull;
    rt_svc(c, 0x18);
    if ((u32)X(0) != 0) goto L_ff0;
    X(30) = 0x710000069Cull; f_7100001200(c);

    X(20) = 0x7100008880ull; X(21) = 65;   /* "boottest: the guest thread left through svcExitThread, waited for" */
    X(1) = X(22) + 0x4010ull;
    X(2) = 0x1ull;
    X(3) = 0x12A05F200ull;
    rt_svc(c, 0x18);
    if ((u32)X(0) != 0) goto L_ff0;
    X(30) = 0x71000006B8ull; f_7100001200(c);

    X(20) = 0x7100008900ull; X(21) = 62;   /* "boottest: 1000 ldaxr/stlxr increments on each thread make 2000" */
    X(8) = X(22) + 0x5000ull;
    X(9) = lda32(X(8));
    { u32 a_ = (u32)X(9), b_ = 0x7D0u, r_ = a_ - b_; nf = r_ >> 31; zf = r_ == 0; cf = a_ >= b_; vf = ((a_ ^ b_) & (a_ ^ r_)) >> 31; (void)(r_); }
    if (!zf) goto L_ff0;
    X(30) = 0x71000006CCull; f_7100001200(c);

    X(20) = 0x7100008980ull; X(21) = 62;   /* "boottest: the guest thread's argument, its write and its stack" */
    X(9) = ld64(X(22) + 0x4018ull);
    X(10) = X(22) + 0x4000ull;
    X(9) = X(9) ^ X(10);
    if (X(9) != 0) goto L_ff0;
    X(9) = ld32(X(22) + 0x4008ull);
    X(10) = 0xC0FFEEull;
    X(9) = (u32)((u32)X(9) ^ (u32)X(10));
    if ((u32)X(9) != 0) goto L_ff0;
    X(8) = 0x7701000000ull;
    X(9) = ld64(X(8) + 0xFFF8ull);
    X(10) = 0x6B63617473ull;
    X(9) = X(9) ^ X(10);
    if (X(9) != 0) goto L_ff0;
    X(30) = 0x710000070Cull; f_7100001200(c);

    X(20) = 0x7100008A00ull; X(21) = 60;   /* "boottest: svcUnmapMemory copied the stack back into the heap" */
    X(0) = 0x7701000000ull;
    X(1) = X(22) + 0x1000000ull;
    X(2) = 0x10000ull;
    rt_svc(c, 0x5);
    if ((u32)X(0) != 0) goto L_ff0;
    X(8) = X(22) + 0x1000000ull;
    X(9) = ld64(X(8) + 0xFFF8ull);
    X(10) = 0x6B63617473ull;
    X(9) = X(9) ^ X(10);
    if (X(9) != 0) goto L_ff0;
    X(30) = 0x7100000744ull; f_7100001200(c);

    X(20) = 0x7100008A80ull; X(21) = 71;   /* "boottest: the runtime's longjmp back to a guest setjmp: 7, x19 restored" */
    X(0) = 0x7100009100ull;
    X(19) = 0x1919ull;
    if (!RT_SETJMP(*rt_setjmp(c))) X(0) = 0;
    if (X(0) != 0) goto L_770;
    X(19) = 0xDEADull;
    X(30) = 0x7100000768ull; f_7100001100(c);
    goto L_ff0;
L_770:;
    X(9) = (u32)((u32)X(0) ^ 0x7u);
    if ((u32)X(9) != 0) goto L_ff0;
    X(10) = 0x1919ull;
    X(9) = X(19) ^ X(10);
    if (X(9) != 0) goto L_ff0;
    X(30) = 0x7100000788ull; f_7100001200(c);

    X(20) = 0x7100008C00ull; X(21) = 77;   /* "boottest: a guest C++ exception resumed its landing pad, x19 from its context" */
    X(8) = X(22) + 0x7000ull;
    X(9) = 0x1A1Aull;
    st64(X(8) + 0x98ull, (u64)X(9));
    st64(X(8) + 0xA0ull, (u64)X(20));
    st64(X(8) + 0xA8ull, (u64)X(21));
    st64(X(8) + 0xB0ull, (u64)X(22));
    st64(X(8) + 0xE8ull, (u64)X(29));
    X(9) = SP + 0x0ull;
    st64(X(8) + 0xF8ull, (u64)X(9));
    X(9) = 0x7100000E00ull;
    st64(X(8) + 0x100ull, (u64)X(9));
    X(0) = X(8);
    X(19) = 0xDEADull;
    { u64 sp_ = SP; int lp_ = 0; __try { X(30) = 0x7100000D3Cull; RT_HOOK(rt_hook_7100001500)(c); } __except (rt_eh_filter(GetExceptionInformation(), sp_, 0x7100000E00ull)) { lp_ = 1; } if (lp_) goto L_e00; }
    goto L_ff0;
L_e00:;
    X(10) = 0x1A1Aull;
    X(9) = X(19) ^ X(10);
    if (X(9) != 0) goto L_ff0;
    X(30) = 0x7100000E10ull; f_7100001200(c);

    X(0) = 0x7100008B00ull; X(1) = 43;   /* "boottest: all checks passed: svcExitProcess" */
    rt_svc(c, 0x27);
    rt_svc(c, 0x7);
    rt_trap(c, 0x7100000798ull, "udf");
L_ff0:;
    X(30) = 0x7100000FF4ull; f_7100001300(c);
    rt_trap(c, 0x7100000FF4ull, "udf");
}

void f_7100001000(Cpu *c) {
    u32 nf = 0, zf = 0, cf = 0, vf = 0; u64 ex_a = 0, ex_v = 0;
    { u64 a_ = SP + 0xFFFFFFFFFFFFFFE0ull; st64(a_, (u64)X(29)); st64(a_ + 8, (u64)X(30)); SP = a_; }
    st64(SP + 0x10ull, (u64)X(19));
    X(29) = SP + 0x0ull;
    X(19) = X(0);
    X(8) = 0x6B63617473ull;
    st64(SP + 0x18ull, (u64)X(8));
    st64(X(19) + 0x18ull, (u64)X(19));
    X(0) = X(19) + 0x1000ull;
    X(30) = 0x7100001030ull; f_7100001400(c);
    X(8) = 0xC0FFEEull;
    st32(X(19) + 0x8ull, (u32)X(8));
    X(0) = ld32(X(19) + 0x0ull);
    rt_svc(c, 0x11);
    X(0) = 0xF4240ull;
    rt_svc(c, 0xb);
    rt_svc(c, 0xa);
    rt_trap(c, 0x710000104Cull, "udf");
}

void f_7100001100(Cpu *c) {
    u32 nf = 0, zf = 0, cf = 0, vf = 0; u64 ex_a = 0, ex_v = 0;
    { u64 a_ = SP + 0xFFFFFFFFFFFFFFF0ull; st64(a_, (u64)X(29)); st64(a_ + 8, (u64)X(30)); SP = a_; }
    X(29) = SP + 0x0ull;
    X(0) = 0x7100009100ull;
    X(1) = 0x7ull;
    X(30) = 0x7100001114ull; f_7100007f00(c);
    rt_trap(c, 0x7100001114ull, "udf");
}

void f_7100001200(Cpu *c) {
    u32 nf = 0, zf = 0, cf = 0, vf = 0; u64 ex_a = 0, ex_v = 0;
    X(0) = X(20);
    X(1) = X(21);
    rt_svc(c, 0x27);
    return;
}

void f_7100001300(Cpu *c) {
    u32 nf = 0, zf = 0, cf = 0, vf = 0; u64 ex_a = 0, ex_v = 0;
    X(0) = 0x7100008B80ull; X(1) = 17;   /* "boottest: FAILED:" */
    rt_svc(c, 0x27);
    X(0) = X(20);
    X(1) = X(21);
    rt_svc(c, 0x27);
    X(0) = 0x1ull;
    X(1) = X(20);
    X(2) = 0x0ull;
    rt_svc(c, 0x26);
    rt_trap(c, 0x7100001328ull, "udf");
}

void f_7100001400(Cpu *c) {
    u32 nf = 0, zf = 0, cf = 0, vf = 0; u64 ex_a = 0, ex_v = 0;
    X(9) = 0x3E8ull;
L_1404:;
    { ex_a = X(0); ex_v = lda32(ex_a); X(10) = ex_v; }
    X(10) = (u32)((u32)X(10) + 0x1u);
    { u32 ok_ = (X(0) == ex_a && cas32(ex_a, ex_v, X(10))); ex_a = 0; X(11) = (u32)(!ok_); }
    if ((u32)X(11) != 0) goto L_1404;
    { u64 a_ = X(9), b_ = 0x1ull, r_ = a_ - b_; nf = r_ >> 63; zf = r_ == 0; cf = a_ >= b_; vf = ((a_ ^ b_) & (a_ ^ r_)) >> 63; X(9) = r_; }
    if (!zf) goto L_1404;
    return;
}

void f_7100007f00(Cpu *c) {
    u32 nf = 0, zf = 0, cf = 0, vf = 0; u64 ex_a = 0, ex_v = 0;
    X(16) = 0x710000A000ull;
    X(17) = ld64(X(16) + 0x0ull);
    X(16) = X(16) + 0x0ull;
    { static RtCache k_; GuestFn *t_ = rt_target(X(17), &k_); __attribute__((musttail)) return t_(c); }
}

void f_7100007f10(Cpu *c) {
    u32 nf = 0, zf = 0, cf = 0, vf = 0; u64 ex_a = 0, ex_v = 0;
    X(16) = 0x710000A000ull;
    X(17) = ld64(X(16) + 0x8ull);
    X(16) = X(16) + 0x8ull;
    { static RtCache k_; GuestFn *t_ = rt_target(X(17), &k_); __attribute__((musttail)) return t_(c); }
}

GuestFn f_7100001500_body;
RtHook rt_hook_7100001500 = {0x7100001500ull, f_7100001500_body, f_7100001500_body};
RT_HOOK_ADD(rt_hook_7100001500)
void f_7100001500(Cpu *c) { TAIL(RT_HOOK(rt_hook_7100001500)) }
RT_HOOKABLE void f_7100001500_body(Cpu *c) {
    u32 nf = 0, zf = 0, cf = 0, vf = 0; u64 ex_a = 0, ex_v = 0;
    TAIL(f_7100001580)
}

GuestFn rt_unwind_jumpto;
void f_7100001580(Cpu *c) { rt_unwind_jumpto(c); }
