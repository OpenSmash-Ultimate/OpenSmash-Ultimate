#!/usr/bin/env python3
"""Lifter checks for memory ordering on an ARM host. No game binary is used or needed.

    python3 test_lifter_atomics.py                 decoder test: synthetic instruction words -> expected C,
                                                   for tools/lifter/recomp.py and tools/nro/recomp_nro.py
    python3 test_lifter_atomics.py --gen OUT.h     lift the litmus guest programs below into OUT.h
                                                   (included by litmus.c; done by run_frame.sh)
    python3 test_lifter_atomics.py --asm           print the litmus guest programs as assembly (run_frame.sh
                                                   assembles them and checks the instruction words below)

The litmus programs are guest AArch64 code; their words were produced by GNU as on the target and are checked
against it again by run_frame.sh, so the C in OUT.h is exactly what the lifter emits for that guest code.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
os.environ['RECOMP_ELF'] = os.path.join(HERE, 'no-game-binary-here.elf')   # never read a game binary
os.environ['RECOMP_NO_IMAGE'] = '1'                                         # recomp.py imports without one
os.environ.pop('RECOMP_PATCHES', None)
os.environ.pop('RECOMP_NRO', None)
sys.path[:0] = [os.path.join(ROOT, 'tools', 'lifter'), os.path.join(ROOT, 'tools', 'nro')]
import recomp          # noqa: E402
import recomp_nro      # noqa: E402

PC = 0x7100001000

# (assembly, word, C the lifter must emit)  -- words from GNU as on the Frame
CASES = [
    ('dmb ish',             0xd5033bbf, 'RT_FENCE_FULL();'),
    ('dmb ishld',           0xd50339bf, 'RT_FENCE_ACQ();'),
    ('dmb ishst',           0xd5033abf, 'RT_FENCE_ST();'),
    ('dmb sy',              0xd5033fbf, 'RT_FENCE_FULL();'),
    ('dmb ld',              0xd5033dbf, 'RT_FENCE_ACQ();'),
    ('dmb st',              0xd5033ebf, 'RT_FENCE_ST();'),
    ('dmb osh',             0xd50333bf, 'RT_FENCE_FULL();'),
    ('dmb oshst',           0xd50332bf, 'RT_FENCE_ST();'),
    ('dmb nsh',             0xd50337bf, 'RT_FENCE_FULL();'),
    ('dmb nshld',           0xd50335bf, 'RT_FENCE_ACQ();'),
    ('dmb #0 (reserved)',   0xd50330bf, 'RT_FENCE_FULL();'),
    ('dsb ish',             0xd5033b9f, 'RT_FENCE_FULL();'),
    ('dsb sy',              0xd5033f9f, 'RT_FENCE_FULL();'),
    ('dsb ishst',           0xd5033a9f, 'RT_FENCE_FULL();'),
    ('isb',                 0xd5033fdf, 'RT_COMPILER_BARRIER();'),
    ('clrex',               0xd5033f5f, 'RT_CLREX(c);'),
    ('ldar x0, [x1]',       0xc8dffc20, 'c->x[0] = (uint64_t)(rdacq64(c->x[1]));'),
    ('ldar w0, [x1]',       0x88dffc20, 'c->x[0] = (uint64_t)(rdacq32(c->x[1]));'),
    ('ldarb w0, [x1]',      0x08dffc20, 'c->x[0] = (uint64_t)(rdacq8(c->x[1]));'),
    ('ldarh w0, [x1]',      0x48dffc20, 'c->x[0] = (uint64_t)(rdacq16(c->x[1]));'),
    ('ldar x0, [sp]',       0xc8dfffe0, 'c->x[0] = (uint64_t)(rdacq64(c->x[31]));'),
    ('stlr x0, [x1]',       0xc89ffc20, 'wrrel64(c->x[1], c->x[0]);'),
    ('stlr w0, [x1]',       0x889ffc20, 'wrrel32(c->x[1], (uint32_t)c->x[0]);'),
    ('stlrb w0, [x1]',      0x089ffc20, 'wrrel8(c->x[1], (uint32_t)c->x[0]);'),
    ('stlrh w0, [x1]',      0x489ffc20, 'wrrel16(c->x[1], (uint32_t)c->x[0]);'),
    ('stlr wzr, [x0]',      0x889ffc1f, 'wrrel32(c->x[0], 0U);'),
    ('ldaxr x0, [x1]',      0xc85ffc20, '{ uint64_t a = c->x[1]; c->ex_addr = a; c->ex_val = rdacq64(a); c->x[0] = (uint64_t)(c->ex_val); }'),
    ('ldaxr w0, [x1]',      0x885ffc20, '{ uint64_t a = c->x[1]; c->ex_addr = a; c->ex_val = rdacq32(a); c->x[0] = (uint64_t)(c->ex_val); }'),
    ('ldxr x0, [x1]',       0xc85f7c20, '{ uint64_t a = c->x[1]; c->ex_addr = a; c->ex_val = rd64(a); c->x[0] = (uint64_t)(c->ex_val); }'),
    ('ldxr w0, [x1]',       0x885f7c20, '{ uint64_t a = c->x[1]; c->ex_addr = a; c->ex_val = rd32(a); c->x[0] = (uint64_t)(c->ex_val); }'),
    ('ldxrb w0, [x1]',      0x085f7c20, '{ uint64_t a = c->x[1]; c->ex_addr = a; c->ex_val = rd8(a); c->x[0] = (uint64_t)(c->ex_val); }'),
    ('stxr w2, x0, [x1]',   0xc8027c20, 'c->x[2] = (uint32_t)(stx64(c, c->x[1], c->x[0]));'),
    ('stxr w2, w0, [x1]',   0x88027c20, 'c->x[2] = (uint32_t)(stx32(c, c->x[1], (uint32_t)c->x[0]));'),
    ('stlxr w2, x0, [x1]',  0xc802fc20, 'c->x[2] = (uint32_t)(stlx64(c, c->x[1], c->x[0]));'),
    ('stlxr w2, w0, [x1]',  0x8802fc20, 'c->x[2] = (uint32_t)(stlx32(c, c->x[1], (uint32_t)c->x[0]));'),
    ('stlxrh w2, w0, [x1]', 0x4802fc20, 'c->x[2] = (uint32_t)(stlx16(c, c->x[1], (uint32_t)c->x[0]));'),
    ('stlxr w2, wzr, [x0]', 0x8802fc1f, 'c->x[2] = (uint32_t)(stlx32(c, c->x[0], 0U));'),
    ('prfm pldl1keep, [x0]',       0xf9800000, ';'),       # PRFM stays a no-op
    ('prfm pldl1keep, [x0, x1]',   0xf8a16800, ';'),
    ('prfum pldl1keep, [x0, #1]',  0xf8801000, ';'),
    ('ldlar x0, [x1]',      0xc8df7c20, 'c->x[0] = (uint64_t)(rdacq64(c->x[1]));'),   # LORegions: as ldar / stlr
    ('stllr x0, [x1]',      0xc89f7c20, 'wrrel64(c->x[1], c->x[0]);'),
    # exclusive pairs (upstream f4cc28c): o0 (bit 15) picks the ordered accessors exactly as for ldaxr / stlxr
    ('ldxp x0, x1, [x2]',   0xc87f0440, '{ uint64_t a = c->x[2]; uint64_t lo = rd64(a), hi = rd64(a + 8); c->ex_addr = a; '
                                        'c->ex_val = lo; c->ex_val2 = hi; c->x[0] = (uint64_t)(lo); c->x[1] = (uint64_t)(hi); }'),
    ('ldaxp x0, x1, [x2]',  0xc87f8440, '{ uint64_t a = c->x[2]; uint64_t lo = rdacq64(a), hi = rdacq64(a + 8); c->ex_addr = a; '
                                        'c->ex_val = lo; c->ex_val2 = hi; c->x[0] = (uint64_t)(lo); c->x[1] = (uint64_t)(hi); }'),
    ('ldaxp x0, x1, [sp]',  0xc87f87e0, '{ uint64_t a = c->x[31]; uint64_t lo = rdacq64(a), hi = rdacq64(a + 8); c->ex_addr = a; '
                                        'c->ex_val = lo; c->ex_val2 = hi; c->x[0] = (uint64_t)(lo); c->x[1] = (uint64_t)(hi); }'),
    ('ldxp w0, w1, [x2]',   0x887f0440, '{ uint64_t a = c->x[2]; c->ex_addr = a; c->ex_val = rd64(a); c->x[0] = (uint32_t)(c->ex_val); '
                                        'c->x[1] = (uint32_t)(c->ex_val >> 32); }'),
    ('ldaxp w0, w1, [x2]',  0x887f8440, '{ uint64_t a = c->x[2]; c->ex_addr = a; c->ex_val = rdacq64(a); c->x[0] = (uint32_t)(c->ex_val); '
                                        'c->x[1] = (uint32_t)(c->ex_val >> 32); }'),
    ('stxp w3, x0, x1, [x2]',  0xc8230440, 'c->x[3] = (uint32_t)(stxp64(c, c->x[2], c->x[0], c->x[1]));'),
    ('stlxp w3, x0, x1, [x2]', 0xc8238440, 'c->x[3] = (uint32_t)(stlxp64(c, c->x[2], c->x[0], c->x[1]));'),
    ('stlxp w3, xzr, x1, [x2]', 0xc823845f, 'c->x[3] = (uint32_t)(stlxp64(c, c->x[2], 0ULL, c->x[1]));'),
    ('stxp w3, w0, w1, [x2]',  0x88230440, 'c->x[3] = (uint32_t)(stx64(c, c->x[2], ((uint64_t)(uint32_t)c->x[1] << 32) | (uint32_t)c->x[0]));'),
    ('stlxp w3, w0, w1, [x2]', 0x88238440, 'c->x[3] = (uint32_t)(stlx64(c, c->x[2], ((uint64_t)(uint32_t)c->x[1] << 32) | (uint32_t)c->x[0]));'),
    # CRC32 (upstream f4cc28c): unchanged by the ordering work
    ('crc32b w0, w1, w2',   0x1ac24020, 'c->x[0] = (uint32_t)(crc32_arm((uint32_t)c->x[1], (uint32_t)c->x[2], 1, 0xedb88320U));'),
    ('crc32cx w0, w1, x2',  0x9ac25c20, 'c->x[0] = (uint32_t)(crc32_arm((uint32_t)c->x[1], c->x[2], 8, 0x82f63b78U));'),
]

# Not translated: ARMv8.1 LSE atomics (cas*, casp*, ld<op>*, st<op>*, swp*) and ARMv8.3 / 8.4 RCpc loads and stores
# (ldapr*, ldapur*, stlur*). The Switch's Cortex-A57 (ARMv8.0-A) has none of them, so code built for it cannot execute
# them; they must become guest_unimpl (a trap if ever reached), never a plain access or a no-op. Words: llvm-mc 19.
UNIMPL = [
    ('casp x0, x1, x2, x3, [x4]',   0x48207c82),
    ('caspal x0, x1, x2, x3, [x4]', 0x4860fc82),
    ('caspa w0, w1, w2, w3, [x4]',  0x08607c82),
    ('caspl w0, w1, w2, w3, [x4]',  0x0820fc82),
    ('cas w0, w1, [x2]',       0x88a07c41),
    ('casa x0, x1, [x2]',      0xc8e07c41),
    ('casl x0, x1, [x2]',      0xc8a0fc41),
    ('casal x0, x1, [x2]',     0xc8e0fc41),
    ('casb w0, w1, [x2]',      0x08a07c41),
    ('casalh w0, w1, [x2]',    0x48e0fc41),
    ('ldapr x0, [x1]',         0xf8bfc020),
    ('ldapr w0, [x1]',         0xb8bfc020),
    ('ldaprb w0, [x1]',        0x38bfc020),
    ('ldaprh w0, [x1]',        0x78bfc020),
    ('ldadd x0, x1, [x2]',     0xf8200041),
    ('ldadda x0, x1, [x2]',    0xf8a00041),   # was lifted as PRFM (a no-op) by upstream's lifter
    ('ldaddl w0, w1, [x2]',    0xb8600041),
    ('ldaddal w0, w1, [x2]',   0xb8e00041),
    ('ldaddb w0, w1, [x2]',    0x38200041),
    ('ldaddalh w0, w1, [x2]',  0x78e00041),
    ('stadd w0, [x2]',         0xb820005f),
    ('staddl x0, [x2]',        0xf860005f),
    ('ldclr x0, x1, [x2]',     0xf8201041),
    ('ldclral w0, w1, [x2]',   0xb8e01041),
    ('ldeor x0, x1, [x2]',     0xf8202041),
    ('ldeora w0, w1, [x2]',    0xb8a02041),
    ('ldset x0, x1, [x2]',     0xf8203041),
    ('ldsetl w0, w1, [x2]',    0xb8603041),
    ('ldsmax x0, x1, [x2]',    0xf8204041),
    ('ldsmin w0, w1, [x2]',    0xb8205041),
    ('ldumax x0, x1, [x2]',    0xf8206041),
    ('lduminalb w0, w1, [x2]', 0x38e07041),
    ('swp x0, x1, [x2]',       0xf8208041),
    ('swpa w0, w1, [x2]',      0xb8a08041),
    ('swpl x0, x1, [x2]',      0xf8608041),
    ('swpal x0, x1, [x2]',     0xf8e08041),
    ('swpb w0, w1, [x2]',      0x38208041),
    ('swpalh w0, w1, [x2]',    0x78e08041),
    ('ldapur x0, [x1, #8]',    0xd9408020),
    ('ldapur w0, [x1, #-4]',   0x995fc020),
    ('ldapurb w0, [x1]',       0x19400020),
    ('ldapursw x0, [x1]',      0x99800020),
    ('stlur x0, [x1, #8]',     0xd9008020),
    ('stlur w0, [x1]',         0x99000020),
    ('stlurb w0, [x1]',        0x19000020),
    ('stlurh w0, [x1]',        0x59000020),
]

LITMUS_ASM = '''
mp_rel_writer:
  str x2, [x0]
  stlr x2, [x1]
  ret
mp_acq_reader:
1: ldar x3, [x1]
  cbz x3, 1b
  ldr x4, [x0]
  ret
mp_fence_writer:
  str x2, [x0]
  dmb ishst
  str x2, [x1]
  ret
mp_fence_reader:
1: ldr x3, [x1]
  cbz x3, 1b
  dmb ishld
  ldr x4, [x0]
  ret
mp_plain_writer:
  str x2, [x0]
  str x2, [x1]
  ret
mp_plain_reader:
1: ldr x3, [x1]
  cbz x3, 1b
  ldr x4, [x0]
  ret
sb_fence:
  mov x2, #1
  str x2, [x0]
  dmb ish
  ldr x3, [x1]
  ret
sb_dsb:
  mov x2, #1
  str x2, [x0]
  dsb sy
  ldr x3, [x1]
  ret
sb_rel_acq:
  mov x2, #1
  stlr x2, [x0]
  ldar x3, [x1]
  ret
sb_plain:
  mov x2, #1
  str x2, [x0]
  ldr x3, [x1]
  ret
lock_inc_a:
  mov w3, #1
1: ldaxr w1, [x0]
  cbnz w1, 1b
  stxr w2, w3, [x0]
  cbnz w2, 1b
  ldr x4, [x5]
  add x4, x4, #1
  str x4, [x5]
  stlr wzr, [x0]
  subs x6, x6, #1
  b.ne 1b
  ret
lock_inc_b:
  mov w3, #1
1: ldxr w1, [x0]
  cbnz w1, 1b
  stxr w2, w3, [x0]
  cbnz w2, 1b
  dmb ish
  ldr x4, [x5]
  add x4, x4, #1
  str x4, [x5]
2: ldxr w1, [x0]
  stlxr w2, wzr, [x0]
  cbnz w2, 2b
  subs x6, x6, #1
  b.ne 1b
  ret
lock_inc_byte:
  mov w3, #1
1: ldaxrb w1, [x0]
  cbnz w1, 1b
  stxrb w2, w3, [x0]
  cbnz w2, 1b
  ldr x4, [x5]
  add x4, x4, #1
  str x4, [x5]
  stlrb wzr, [x0]
  subs x6, x6, #1
  b.ne 1b
  ret
atomic_inc_sc:
1: ldaxr x4, [x5]
  add x4, x4, #1
  stlxr w2, x4, [x5]
  cbnz w2, 1b
  subs x6, x6, #1
  b.ne 1b
  ret
atomic_inc_relaxed:
1: ldxr w4, [x5]
  add w4, w4, #1
  stxr w2, w4, [x5]
  cbnz w2, 1b
  subs x6, x6, #1
  b.ne 1b
  ret
clrex_check:
  ldxr x1, [x0]
  clrex
  stxr w2, x1, [x0]
  ldxr x1, [x0]
  stxr w3, x1, [x0]
  ret
mp_ldaxp_reader:
1: ldaxp x3, x8, [x1]
  cbz x3, 1b
  ldr x4, [x0]
  ret
mp_ldaxp_w_reader:
1: ldaxp w3, w8, [x1]
  cbz w3, 1b
  ldr x4, [x0]
  ret
mp_stlxp_writer:
  str x2, [x0]
1: ldxp x3, x8, [x1]
  stlxp w9, x2, x8, [x1]
  cbnz w9, 1b
  ret
mp_stlxp_w_writer:
  str x2, [x0]
1: ldxp w3, w8, [x1]
  stlxp w9, w2, w8, [x1]
  cbnz w9, 1b
  ret
lock_pair_x:
  mov x3, #1
1: ldaxp x1, x7, [x0]
  cbnz x1, 1b
  stxp w2, x3, x7, [x0]
  cbnz w2, 1b
  ldr x4, [x5]
  add x4, x4, #1
  str x4, [x5]
2: ldxp x1, x7, [x0]
  stlxp w2, xzr, x7, [x0]
  cbnz w2, 2b
  subs x6, x6, #1
  b.ne 1b
  ret
lock_pair_w:
  mov w3, #1
1: ldaxp w1, w7, [x0]
  cbnz w1, 1b
  stxp w2, w3, w7, [x0]
  cbnz w2, 1b
  ldr x4, [x5]
  add x4, x4, #1
  str x4, [x5]
2: ldxp w1, w7, [x0]
  stlxp w2, wzr, w7, [x0]
  cbnz w2, 2b
  subs x6, x6, #1
  b.ne 1b
  ret
fetch_add_pair:
1: ldaxp x4, x7, [x5]
  add x4, x4, #1
  add x7, x7, #3
  stlxp w2, x4, x7, [x5]
  cbnz w2, 1b
  subs x6, x6, #1
  b.ne 1b
  ret
fetch_add_pair_relaxed:
1: ldxp x4, x7, [x5]
  add x4, x4, #1
  add x7, x7, #3
  stxp w2, x4, x7, [x5]
  cbnz w2, 1b
  subs x6, x6, #1
  b.ne 1b
  ret
'''

# GNU as output for LITMUS_ASM, in order; the functions follow each other as in the listing (the exclusive-pair
# programs from mp_ldaxp_reader on: llvm-mc 19; frame_tests.sh checks all of them against GNU as on the target)
LITMUS_WORDS = [
    ('mp_rel_writer', [0xf9000002, 0xc89ffc22, 0xd65f03c0]),
    ('mp_acq_reader', [0xc8dffc23, 0xb4ffffe3, 0xf9400004, 0xd65f03c0]),
    ('mp_fence_writer', [0xf9000002, 0xd5033abf, 0xf9000022, 0xd65f03c0]),
    ('mp_fence_reader', [0xf9400023, 0xb4ffffe3, 0xd50339bf, 0xf9400004, 0xd65f03c0]),
    ('mp_plain_writer', [0xf9000002, 0xf9000022, 0xd65f03c0]),
    ('mp_plain_reader', [0xf9400023, 0xb4ffffe3, 0xf9400004, 0xd65f03c0]),
    ('sb_fence', [0xd2800022, 0xf9000002, 0xd5033bbf, 0xf9400023, 0xd65f03c0]),
    ('sb_dsb', [0xd2800022, 0xf9000002, 0xd5033f9f, 0xf9400023, 0xd65f03c0]),
    ('sb_rel_acq', [0xd2800022, 0xc89ffc02, 0xc8dffc23, 0xd65f03c0]),
    ('sb_plain', [0xd2800022, 0xf9000002, 0xf9400023, 0xd65f03c0]),
    ('lock_inc_a', [0x52800023, 0x885ffc01, 0x35ffffe1, 0x88027c03, 0x35ffffa2, 0xf94000a4, 0x91000484, 0xf90000a4,
                    0x889ffc1f, 0xf10004c6, 0x54fffee1, 0xd65f03c0]),
    ('lock_inc_b', [0x52800023, 0x885f7c01, 0x35ffffe1, 0x88027c03, 0x35ffffa2, 0xd5033bbf, 0xf94000a4, 0x91000484,
                    0xf90000a4, 0x885f7c01, 0x8802fc1f, 0x35ffffc2, 0xf10004c6, 0x54fffe81, 0xd65f03c0]),
    ('lock_inc_byte', [0x52800023, 0x085ffc01, 0x35ffffe1, 0x08027c03, 0x35ffffa2, 0xf94000a4, 0x91000484, 0xf90000a4,
                       0x089ffc1f, 0xf10004c6, 0x54fffee1, 0xd65f03c0]),
    ('atomic_inc_sc', [0xc85ffca4, 0x91000484, 0xc802fca4, 0x35ffffa2, 0xf10004c6, 0x54ffff61, 0xd65f03c0]),
    ('atomic_inc_relaxed', [0x885f7ca4, 0x11000484, 0x88027ca4, 0x35ffffa2, 0xf10004c6, 0x54ffff61, 0xd65f03c0]),
    ('clrex_check', [0xc85f7c01, 0xd5033f5f, 0xc8027c01, 0xc85f7c01, 0xc8037c01, 0xd65f03c0]),
    ('mp_ldaxp_reader', [0xc87fa023, 0xb4ffffe3, 0xf9400004, 0xd65f03c0]),
    ('mp_ldaxp_w_reader', [0x887fa023, 0x34ffffe3, 0xf9400004, 0xd65f03c0]),
    ('mp_stlxp_writer', [0xf9000002, 0xc87f2023, 0xc829a022, 0x35ffffc9, 0xd65f03c0]),
    ('mp_stlxp_w_writer', [0xf9000002, 0x887f2023, 0x8829a022, 0x35ffffc9, 0xd65f03c0]),
    ('lock_pair_x', [0xd2800023, 0xc87f9c01, 0xb5ffffe1, 0xc8221c03, 0x35ffffa2, 0xf94000a4, 0x91000484, 0xf90000a4, 0xc87f1c01, 0xc8229c1f, 0x35ffffc2, 0xf10004c6, 0x54fffea1, 0xd65f03c0]),
    ('lock_pair_w', [0x52800023, 0x887f9c01, 0x35ffffe1, 0x88221c03, 0x35ffffa2, 0xf94000a4, 0x91000484, 0xf90000a4, 0x887f1c01, 0x88229c1f, 0x35ffffc2, 0xf10004c6, 0x54fffea1, 0xd65f03c0]),
    ('fetch_add_pair', [0xc87f9ca4, 0x91000484, 0x91000ce7, 0xc8229ca4, 0x35ffff82, 0xf10004c6, 0x54ffff41, 0xd65f03c0]),
    ('fetch_add_pair_relaxed', [0xc87f1ca4, 0x91000484, 0x91000ce7, 0xc8221ca4, 0x35ffff82, 0xf10004c6, 0x54ffff41, 0xd65f03c0]),
]


class NoCalls:
    """Env stand-in: the test programs never call out of themselves."""
    full = False

    def call(self, target):
        raise AssertionError('unexpected call to %x' % target)


def lift_one(mod, w):
    return mod.Fn(PC, PC + 4, NoCalls()).lift(w, PC)


def test_decoder():
    fails = passes = 0
    for mod in (recomp, recomp_nro):
        for asm, w, want in CASES:
            got = lift_one(mod, w)
            if got != want:
                fails += 1
                print('FAIL %-11s %-22s %08x\n     got:  %s\n     want: %s' % (mod.__name__, asm, w, got, want))
            else:
                passes += 1
        for asm, w in UNIMPL:
            try:
                got = lift_one(mod, w)
            except mod.Unimpl:
                passes += 1
            else:
                fails += 1
                print('FAIL %-11s %-22s %08x expected Unimpl, got %s' % (mod.__name__, asm, w, got))
    # the two lifters must agree on everything above
    print('decoder: %d passed, %d failed' % (passes, fails))
    return fails == 0


def gen_litmus(out_path):
    """Each program becomes `void g_<name>(Ctx *c)` built the way lift_function builds a function."""
    out = ['/* generated by tests/arm_host/test_lifter_atomics.py --gen from tools/lifter/recomp.py -- do not edit */']
    addr = PC
    for name, words in LITMUS_WORDS:
        end = addr + 4 * len(words)
        fn = recomp.Fn(addr, end, NoCalls())
        out.append('static void g_%s(Ctx *c) {' % name)
        for i, w in enumerate(words):
            out.append('L_%x: %s' % (addr + 4 * i, fn.lift(w, addr + 4 * i)))
        out.append('  return;')
        out.append('}')
        addr = end
    with open(out_path, 'w') as f:
        f.write('\n'.join(out) + '\n')
    print('wrote %s (%d functions)' % (out_path, len(LITMUS_WORDS)))


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == '--gen':
        gen_litmus(sys.argv[2])
    elif len(sys.argv) > 1 and sys.argv[1] == '--asm':
        sys.stdout.write(LITMUS_ASM.lstrip())
    elif len(sys.argv) > 1 and sys.argv[1] == '--words':
        for name, words in LITMUS_WORDS:
            for w in words:
                print('%08x' % w)
    else:
        sys.exit(0 if test_decoder() else 1)
