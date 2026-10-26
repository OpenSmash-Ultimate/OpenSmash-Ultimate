"""Guest registers in C locals: a pass over a lifted function's statements.

The lifter writes every guest register as X(n) - the Cpu context's x[n] in memory - and SP as
c->sp. Guest memory is reached through pointers made from integers, which the C compiler
cannot tell apart from the context, so after every guest store it reloads every register it
needs, and every register write is a store. Here a function keeps the registers it uses in
locals instead (x_N, x_sp), which the compiler holds in host registers, and the context is
brought up to date only where something else reads it:

  calls (direct, indirect, svc)   the registers that may have changed since the context was
                                  last right ("dirty", found by a forward dataflow over the
                                  function's statements) and that the callee may read: the
                                  caller-saved ones (x0-x18, x30), the frame pointer x29 (the
                                  unwinder finds every frame through it) and SP; afterwards
                                  the caller-saved ones are read back. Callee-saved x19-x28
                                  stay in their locals: by the ABI the callee keeps them.
  functions with exception landing pads or setjmp
                                  every dirty register before a call: the unwinder (or the
                                  guest's longjmp) restores the frame's callee-saved registers
                                  from what its callees saved, which is what the context held.
  a landing pad / longjmp return  every register read back from the context (the native
                                  unwinder, or longjmp, put the frame's state there).
  exits (return, tail calls, traps)
                                  every dirty register.
  calls on the exact list (tools/hooks.txt)
                                  every dirty register before the call, every register the
                                  function uses read back after it, so that a hook there sees
                                  and can change all of them: at a direct call to an
                                  exact-callee (marked @E@), at every call of an exact-caller.

The lifter marks the places with tokens this pass replaces: @S@ / @R@ around a call (@E@ before
@S@: an exact one), @F@ / @A@ around setjmp and before a landing pad's goto (@A@: read everything
back), @X@ before an exit.
"""
import re

CALLER_SAVED = set(range(0, 19)) | {30}
SP = 31                       # bit for SP in the register masks
ALL = (1 << 32) - 1
FP_SP = (1 << 29) | (1 << SP)
CALLER_MASK = sum(1 << r for r in CALLER_SAVED)

RE_WRITE = re.compile(r"X\((\d+)\) = (?!=)")
RE_SP_WRITE = re.compile(r"\bSP = (?!=)")
RE_USE = re.compile(r"X\((\d+)\)")
RE_SP_USE = re.compile(r"\bSP\b")
RE_GOTO = re.compile(r"goto L_([0-9a-f]+);")


def mask(regs):
    m = 0
    for r in regs:
        m |= 1 << r
    return m


def regs_of(m):
    return [r for r in range(32) if m >> r & 1]


def store(m):
    return "".join(f"c->x[{r}] = x_{r}; " if r != SP else "c->sp = x_sp; " for r in regs_of(m))


def load(m):
    return "".join(f"x_{r} = c->x[{r}]; " if r != SP else "x_sp = c->sp; " for r in regs_of(m))


def apply(body, labels, careful, exact=False):
    """body: [(pc, stmt, falls)]; labels: pcs that are goto targets. careful: the function has
    landing pads or setjmp (spill everything at calls). exact: an exact-caller (every call as
    exact as one marked @E@). Returns (declarations, new body)."""
    n = len(body)
    writes, used = [], 0
    index = {pc: i for i, (pc, _s, _f) in enumerate(body)}
    succ = []
    for i, (pc, s, falls) in enumerate(body):
        w = mask(int(r) for r in RE_WRITE.findall(s)) | ((1 << SP) if RE_SP_WRITE.search(s) else 0)
        writes.append(w)
        used |= mask(int(r) for r in RE_USE.findall(s)) | ((1 << SP) if RE_SP_USE.search(s) else 0)
        targets = [index[int(t, 16)] for t in RE_GOTO.findall(s) if int(t, 16) in index]
        succ.append(targets)
    spillable = ALL if careful else (CALLER_MASK | FP_SP)

    # forward dataflow: registers whose local may differ from the context at each statement
    dirty = [None] * n
    dirty[0] = 0
    work = [0]
    while work:
        i = work.pop()
        pc, s, falls = body[i]
        d = dirty[i] | writes[i]
        pad = None
        if "@S@" in s:
            d &= ~(ALL if exact or "@E@" in s else (spillable | CALLER_MASK))   # spilled, or read back after the call
            if "@A@" in s:                           # a landing pad: everything read back
                pad = 0
        elif "@F@" in s:
            d = 1 << 0                               # all spilled; X(0) = 0 on the first return
        outs = []
        if falls:
            outs.append((i + 1, d))
        for t in succ[i]:
            outs.append((t, pad if pad is not None and "@A@" in s and t == succ[i][-1] else d))
        for t, v in outs:
            if t >= n:
                continue
            if dirty[t] is None or (dirty[t] | v) != dirty[t]:
                dirty[t] = v if dirty[t] is None else dirty[t] | v
                work.append(t)

    out = []
    for i, (pc, s, falls) in enumerate(body):
        d = dirty[i] if dirty[i] is not None else ALL & used
        s = RE_USE.sub(r"x_\1", s)
        s = RE_SP_USE.sub("x_sp", s)
        dw = (d | writes[i]) & used
        if "@S@" in s and (exact or "@E@" in s):
            s = s.replace("@E@", "").replace("@S@", store(dw)).replace("@R@", load(used))
        elif "@S@" in s:
            s = s.replace("@S@", store(dw & spillable)).replace("@R@", load(used & CALLER_MASK))
        if "@F@" in s:
            s = s.replace("@F@", store(dw))
        if "@A@" in s:
            s = s.replace("@A@", load(used))
        if "@X@" in s:
            s = s.replace("@X@", store(dw))
        out.append((pc, s))
    decl = []
    gprs = [r for r in regs_of(used) if r != SP]
    if gprs:
        decl.append("u64 " + ", ".join(f"x_{r} = c->x[{r}]" for r in gprs) + ";")
    if used >> SP & 1:
        decl.append("u64 x_sp = c->sp;")
    return decl, out
