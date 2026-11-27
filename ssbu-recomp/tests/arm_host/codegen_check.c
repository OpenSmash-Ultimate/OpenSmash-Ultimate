/* One function per ordering macro of runtime.h; run_frame.sh compiles this with the build's flags and checks the
 * instructions in each function with objdump (LDAR / STLR / DMB ISH / DMB ISHLD / DMB ISHST, acquire vs. sequentially
 * consistent compare-and-swap for STXR / STLXR and STXP / STLXP, nothing for ISB). */
#include "runtime.h"

uint64_t t_rdacq64(uint64_t a) { return rdacq64(a); }
uint32_t t_rdacq32(uint64_t a) { return rdacq32(a); }
uint16_t t_rdacq16(uint64_t a) { return rdacq16(a); }
uint8_t t_rdacq8(uint64_t a) { return rdacq8(a); }
void t_wrrel64(uint64_t a, uint64_t v) { wrrel64(a, v); }
void t_wrrel32(uint64_t a, uint32_t v) { wrrel32(a, v); }
void t_wrrel16(uint64_t a, uint16_t v) { wrrel16(a, v); }
void t_wrrel8(uint64_t a, uint8_t v) { wrrel8(a, v); }
void t_fence_full(void) { RT_FENCE_FULL(); }
void t_fence_acq(void) { RT_FENCE_ACQ(); }
void t_fence_st(void) { RT_FENCE_ST(); }
void t_compiler_barrier(void) { RT_COMPILER_BARRIER(); }
uint32_t t_stx64(Ctx *c, uint64_t a, uint64_t v) { return stx64(c, a, v); }
uint32_t t_stlx64(Ctx *c, uint64_t a, uint64_t v) { return stlx64(c, a, v); }
uint32_t t_stx32(Ctx *c, uint64_t a, uint32_t v) { return stx32(c, a, v); }
uint32_t t_stlx32(Ctx *c, uint64_t a, uint32_t v) { return stlx32(c, a, v); }
uint32_t t_stxp64(Ctx *c, uint64_t a, uint64_t lo, uint64_t hi) { return stxp64(c, a, lo, hi); }
uint32_t t_stlxp64(Ctx *c, uint64_t a, uint64_t lo, uint64_t hi) { return stlxp64(c, a, lo, hi); }
void t_clrex(Ctx *c) { RT_CLREX(c); }
/* the plain accessors stay single plain LDR / STR (single-copy atomic when aligned) */
uint64_t t_rd64(uint64_t a) { return rd64(a); }
void t_wr64(uint64_t a, uint64_t v) { wr64(a, v); }
