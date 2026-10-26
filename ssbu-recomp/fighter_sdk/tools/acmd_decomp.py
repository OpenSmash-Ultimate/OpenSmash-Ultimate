"""Mechanical decompiler for the ACMD scripts of a lua2cpp module (AArch64 -> readable C++ calls).

    python acmd_decomp.py mario            writes fighters/<name>/acmd/*.cpp, fighters/<name>/generated/bindings.inc,
                                           fighters/tools_out/<name>_acmd.json (per script: ok / why not)
    python acmd_decomp.py mario --stats    only the numbers
    python acmd_decomp.py all --stats      numbers for all modules (nothing written)

How: a script body is executed symbolically, instruction by instruction, over a small abstract domain (constants,
stack slots holding L2CValue temporaries, the agent, its lua_State and module accessor, results of calls).
Calls to imported functions become call nodes with their arguments; the lib::L2CValue constructors/conversions and
the Lua stack protocol (clear, push..., call, pop) are folded away. Forward conditional branches become `if`.
Anything outside that (loops, unknown instructions, calls to unlisted local functions) makes the script
"not mechanical": it stays lifted and is reported with the reason.
"""
import collections
import json
import os
import re
import struct
import sys
import zlib

import census
import luaconst
import mangle
from nrolib import Nro, STUDY, HERE, demangle_simple


class NotMechanical(Exception):
    pass


UNDEF = ('undef',)
AGENT = ('agent',)
LSTATE = ('L',)
BOMA = ('boma',)
RET = ('ret',)

XREG = {'xzr': None, 'wzr': None}
for _i in range(31):
    XREG['x%d' % _i] = 'x%d' % _i
    XREG['w%d' % _i] = 'x%d' % _i
XREG['x29'] = 'x29'
XREG['fp'] = 'x29'
XREG['x30'] = 'x30'
XREG['lr'] = 'x30'
XREG['sp'] = 'sp'
VREG = {}
for _i in range(32):
    for _p in 'sdq':
        VREG['%s%d' % (_p, _i)] = 'v%d' % _i
    VREG['v%d.16b' % _i] = 'v%d' % _i

LV_CTOR_KIND = {'i': 'i', 'j': 'u', 'l': 'l', 'm': 'm', 'f': 'f', 'b': 'b'}
CMP_BOOL = {'operator_eq': '==', 'operator_lt': '<', 'operator_le': '<='}
ARITH = {'operator_pl': '+', 'operator_mi': '-', 'operator_ml': '*', 'operator_dv': '/'}
NEG_CC = {'eq': '!=', 'ne': '==', 'lt': '>=', 'ge': '<', 'gt': '<=', 'le': '>', 'lo': '>=', 'hs': '<', 'hi': '<=', 'ls': '>',
          'mi': '>=', 'pl': '<'}


def split_ops(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def imm(tok):
    return int(tok.lstrip('#'), 0)


def singleton_name(sym):
    """S8: 'FighterManager' for lib::Singleton<app::FighterManager>::instance_ (a data symbol of main), else None"""
    pre, post = '_ZN3lib9SingletonIN3app', 'EE9instance_E'
    if not (sym.startswith(pre) and sym.endswith(post)):
        return None
    body = sym[len(pre):-len(post)]
    k = 0
    while k < len(body) and body[k].isdigit():
        k += 1
    return body[k:] if k and body[k:].isidentifier() and int(body[:k]) == len(body) - k else None


class Exec:
    """symbolic execution of one function"""

    def __init__(self, mod, off, mode):
        self.m = mod                      # Module
        self.n = mod.nro
        self.off = off
        self.mode = mode                  # 'body' (x0 = result slot, x1 = agent) or 'macro' (x0 = agent, x1.. = arguments)
        self.reg = collections.defaultdict(lambda: UNDEF)
        self.reg['sp'] = ('stk', 0)
        if mode == 'body':
            self.reg['x0'] = RET
            self.reg['x1'] = AGENT
        else:
            self.reg['x0'] = AGENT
            for i in range(1, 8):
                self.reg['x%d' % i] = ('arg', i - 1)
        self.mem = {}                     # stack offset -> value (8-byte slots)
        self.lv = {}                      # stack offset -> L2CValue expression
        self.nodes = []
        self.blocks = []                  # open `if` blocks: end address
        self.lua = None                   # values pushed since clear_lua_stack, or None
        self.flags = None
        self.ret_value = None

    # ---- helpers
    def fail(self, why):
        raise NotMechanical(why)

    def get(self, tok):
        if tok in XREG:
            r = XREG[tok]
            if r is None:
                return ('int', 0)
            v = self.reg[r]
            if tok[0] == 'w' and v[0] == 'int':
                return ('int', v[1] & 0xffffffff)
            return v
        if tok in VREG:
            v = self.reg[VREG[tok]]
            if tok[0] in 'sd' and v[0] == 'vec':         # S10: the scalar in lane 0 of a vector built here
                return v[1]
            if tok[0] in 'sd' and v[0] == 'vecret':      # S10: lane 0 of a vector a game function returned
                return ('lane', v[1], 0)
            return v
        if tok.startswith('#'):
            return ('int', imm(tok))
        self.fail('operand ' + tok)

    # S10: phx::Vector2f / 3f built on the stack. The game's code: `movi vN.2d, #0`, `mov vN.s[i], vM.s[0]` per lane,
    # `str qN, [sp, #off]`, the address as the argument. A register that holds such a vector is
    # ('vec', lane0, lane1, lane2, lane3); a vector a game function returned in v0 is ('vecret', ('ref', node)) and a
    # lane read from it ('lane', ('ref', node), i).
    def lane_move(self, dst, src):
        def split(tok):
            if '.s[' in tok:
                r, _, rest = tok.partition('.s[')
                if r not in ('v%d' % i for i in range(32)):
                    self.fail('operand ' + tok)
                return r, int(rest.rstrip(']'))
            if tok in VREG and tok[0] == 's':
                return VREG[tok], None
            self.fail('operand ' + tok)
        dr, di = split(dst)
        sr, si = split(src)
        v = self.reg[sr]
        if si is None or (si == 0 and v[0] not in ('vec', 'vecret')):
            lane = self.get('s' + sr[1:])                # the scalar the register holds (marks a float result as such)
        elif v[0] == 'vec':
            lane = v[1 + si]
        elif v[0] == 'vecret':
            lane = ('lane', v[1], si)
        elif v[0] == 'ref':
            # the game's code reads a higher lane of what a function returned: that function returns a vector
            # (whatever the public bindings say); its plain uses are lane 0 (printed as .v[0])
            nodes = getattr(self, 'allnodes', None)
            nd = nodes[v[1]] if nodes is not None else self.nodes[v[1]]
            # S20: a script call (lua_State) that returns a vector in v0 (sv_kinetic_energy::get_speed): only where
            # the status sources' lua_vec() exists (status_local.h), only without typed parameters
            lua_vec = nd['kind'] == 'lua' and getattr(self, 'lua_vec', False) and not nd.get('proto')
            if nd['kind'] != 'call' and not lua_vec:
                self.fail('lane %d of the result of a script call' % si)
            nd['vret'] = True
            lane = ('lane', v, si)
        else:
            self.fail('lane %d of %s' % (si, v[0]))
        if lane[0] == 'undef':
            self.fail('lane that was never set')
        if di is None:
            self.put(dst, lane)
            return
        cur = self.reg[dr]
        if cur[0] == 'vecret':
            cur = ('vec',) + tuple(('lane', cur[1], i) for i in range(4))
        elif cur[0] != 'vec':
            cur = ('vec', UNDEF if cur[0] == 'undef' else self.get('s' + dr[1:]), UNDEF, UNDEF, UNDEF)
        self.reg[dr] = cur[:1 + di] + (lane,) + cur[2 + di:]

    def put(self, tok, v):
        if tok in XREG:
            if XREG[tok] is None:
                return
            if tok[0] == 'w' and v[0] == 'int':
                v = ('int', v[1] & 0xffffffff)
            self.reg[XREG[tok]] = v
        elif tok in VREG:
            self.reg[VREG[tok]] = v
        else:
            self.fail('destination ' + tok)

    def addr_of(self, memop):
        """'[base, #imm]' -> abstract address"""
        inner = memop.strip()
        assert inner[0] == '['
        inner = inner[1:inner.index(']')]
        parts = [p.strip() for p in inner.split(',')]
        base = self.get(parts[0])
        off = 0
        if len(parts) > 1:
            o = self.get(parts[1]) if not parts[1].startswith('#') else ('int', imm(parts[1]))
            if o[0] != 'int':
                self.fail('address offset')
            off = o[1]
            if len(parts) > 2:
                self.fail('scaled address')
        return self.add(base, off)

    def add(self, base, off):
        if base[0] == 'stk':
            return ('stk', base[1] + off)
        if base[0] == 'int':
            return ('int', (base[1] + off) & 0xffffffffffffffff)
        if base[0] == 'page' or base[0] == 'img':
            return ('img', base[1] + off)
        if base[0] in ('agent', 'sym', 'arg', 'incoming'):
            return ('off', base, off) if off else base
        if base[0] == 'off':
            return ('off', base[1], base[2] + off)
        self.fail('pointer arithmetic on %s' % (base[0],))

    def load(self, a, size, is_float):
        if a[0] == 'stk':
            if a[1] >= 0 and self.mode == 'macro' and a[1] not in self.mem:
                return ('arg', 7 + a[1] // 8)
            return self.mem.get(a[1], UNDEF)
        if a[0] == 'img':
            o = a[1]
            if o in self.n.got:
                return ('sym', self.n.got[o])
            if o in self.n.rel_ptr:
                return ('img', self.n.rel_ptr[o])
            if self.n.ro_off <= o < self.n.ro_off + self.n.ro_size:
                if is_float:
                    return ('f', self.n.f32(o)) if size == 4 else self.fail('double constant')
                return ('int', self.n.u32(o) if size == 4 else self.n.u64(o))
            self.fail('load from module data +0x%x' % o)
        base, off = (a[1], a[2]) if a[0] == 'off' else (a, 0)
        if base == AGENT:
            if off == 8:
                return LSTATE
            if off == 0x40:
                return BOMA
            return ('agentfield', off)
        if base[0] == 'sym' and 'const_value_table' in base[1] and size == 4:
            return ('luaconst', off // 4)
        if base[0] == 'sym' and off == 0 and size == 8 and singleton_name(base[1]):
            return ('singleton', base[1])                # S8: the instance pointer of one of the game's manager objects
        self.fail('load through %s' % (base[0],))

    def store(self, a, v):
        if a[0] != 'stk':
            self.fail('store outside the stack frame')
        self.mem[a[1]] = v

    def slot(self, v, what):
        if v[0] == 'stk':
            return v[1]
        if v == RET:
            return 'ret'
        self.fail('%s: not a local L2CValue' % what)

    def lvalue(self, v, what):
        """L2CValue expression a pointer refers to"""
        if v[0] == 'stk':
            if v[1] not in self.lv:
                self.fail('%s: uninitialised L2CValue slot' % what)
            return self.lv[v[1]]
        if v[0] == 'sym' and v[1] == '_ZN3lib8L2CValue3NILE':
            return ('nil',)
        if v[0] == 'arg':
            return v
        self.fail('%s: pointer to %s is not an L2CValue we know' % (what, v[0]))

    def node(self, **kw):
        kw['id'] = len(self.nodes)
        self.nodes.append(kw)
        return kw['id']

    # ---- calls
    def call(self, pc, target):
        plt = self.n.plt
        if target in plt:
            self.call_import(plt[target])
        elif target in self.m.macros:
            core, order, pops = self.m.macros[target]
            if self.get('x0') != AGENT:
                self.fail('macro called on something else than the agent')
            pc_ = mangle.parse(core)
            if pc_ and len(pc_[1]) > 1:
                # S21 (found by the offline gate, item metroid_initialize_coroutine): a macro whose script call takes
                # TYPED parameters (app::item::set_hit_as_capsule(L, int, Hash40, float, float): the macro converts
                # some pushed values itself) was written as the untyped wrapper (lua_State only): wrong arguments
                self.fail('macro whose script call takes typed parameters')
            sp = self.reg['sp'][1]
            args = []
            for k in order:
                p = self.get('x%d' % (k + 1)) if k < 7 else self.mem.get(sp + 8 * (k - 7), UNDEF)
                args.append(self.lvalue(p, 'macro argument %d' % k))
            i = self.node(kind='lua', sym=core, args=args, popped=bool(pops), proto=())
            self.lua = None
            if pops:
                self.fail('macro that returns a value')
        else:
            self.fail('call to local function +0x%x' % target)
        for r in range(1, 18):
            self.reg['x%d' % r] = UNDEF
        for r in range(1, 8):
            self.reg['v%d' % r] = UNDEF
        self.flags = None

    def call_import(self, sym):
        p = mangle.parse(sym)
        if not p:
            self.fail('import %s' % sym)
        path, params, const = p
        name = '::'.join(path)
        x0 = self.get('x0')
        if path[:2] == ['lib', 'L2CValue']:
            fn = path[2]
            if fn == 'L2CValue':
                s = self.slot(x0, 'constructor')
                t = mangle.strip(params[0]) if params else None
                if t in LV_CTOR_KIND:
                    v = self.get('w1' if t in 'ijb' else 'x1') if t != 'f' else self.get('s0')
                    e = ('lv', LV_CTOR_KIND[t], v)
                elif t == ('class', 'phx::Hash40'):
                    e = ('lv', 'h', self.get('x1'))
                elif isinstance(t, tuple) and t[0] == 'ref':
                    e = self.lvalue(self.get('x1'), 'copy constructor')
                elif t == ('ptr', 'v'):
                    e = ('lv', 'p', self.get('x1'))
                else:
                    self.fail('L2CValue constructor from %s' % (t,))
                if s == 'ret':
                    self.ret_value = e
                else:
                    self.lv[s] = e
                self.reg['x0'] = UNDEF
                return
            if fn == '~L2CValue':
                self.reg['x0'] = UNDEF
                return
            a = self.lvalue(x0, fn)
            if fn == 'operator b':
                self.reg['x0'] = ('conv', 'bool', a)
                return
            if fn in ('as_integer', 'as_bool', 'as_hash', 'as_pointer'):
                self.reg['x0'] = ('conv', fn, a)
                return
            if fn == 'as_number':
                self.reg['v0'] = ('conv', fn, a)
                return
            if fn in CMP_BOOL:
                self.reg['x0'] = ('lvop', CMP_BOOL[fn], a, self.lvalue(self.get('x1'), fn))
                return
            if fn in ARITH:
                self.lv[self.slot(self.get('x8'), fn)] = ('lvop', ARITH[fn], a, self.lvalue(self.get('x1'), fn))
                return
            if fn == 'operator_aS':
                self.lv[self.slot(x0, fn)] = self.lvalue(self.get('x1'), fn)
                return
            self.fail('L2CValue::%s' % fn)
        if path[:2] == ['lib', 'L2CAgent'] and path[2] in ('clear_lua_stack', 'push_lua_stack', 'pop_lua_stack'):
            if x0 != AGENT:
                self.fail('Lua stack of another agent')
            if path[2] == 'clear_lua_stack':
                if self.lua:
                    self.fail('values pushed and never used')
                self.lua = []
            elif path[2] == 'push_lua_stack':
                if self.lua is None:
                    self.fail('push without clear_lua_stack')
                self.lua.append(self.lvalue(self.get('x1'), 'push_lua_stack'))
            else:
                last = self.nodes[-1] if self.nodes else None
                if not last or last['kind'] != 'lua' or last.get('popped') or self.get('w1') != ('int', 1):
                    self.fail('pop_lua_stack that does not follow a script call')
                last['popped'] = True
                self.lv[self.slot(self.get('x8'), 'pop_lua_stack')] = ('ref', last['id'])
            self.reg['x0'] = UNDEF
            return
        if path[0] == 'app' and len(params) == 1 and mangle.strip(params[0]) == ('ptr', ('class', 'lua_State')):
            if x0 != LSTATE:
                self.fail('%s on another lua_State' % name)
            if self.lua is None:
                self.fail('%s without clear_lua_stack' % name)
            i = self.node(kind='lua', sym=sym, args=self.lua, popped=False, proto=())
            self.lua = None
            self.reg['x0'] = ('luax', ('ref', i)) if self.mode != 'body' else UNDEF      # S21: see the typed case below
            # S8: such a function can return a float directly too (sv_kinetic_energy::get_speed_x). v0 used to keep
            # the value of an EARLIER call here, and a later read of s0 was printed as that earlier result (found by
            # the offline test on the fighters' status scripts; a wrong value, not a refusal).
            self.reg['v0'] = ('ref', i)
            return
        if path[0] == 'app' and len(params) > 1 and mangle.strip(params[0]) == ('ptr', ('class', 'lua_State')) and self.lua:
            if x0 != LSTATE:
                self.fail('%s on another lua_State' % name)
            proto = []
            xi = 1
            fi = 0
            k = 0                                        # (S21: the pushed value a parameter takes, see the vector case)
            consts = []                                  # (S21: constants passed by the game's code itself, see below)
            for t in params[1:]:
                c = mangle.reg_class(t)
                st = mangle.strip(t)
                vn = (isinstance(st, tuple) and st[0] == 'ref' and self.mode != 'body'
                      and {('const', ('class', 'phx::Vector2f')): 2, ('const', ('class', 'phx::Vector3f')): 3}.get(st[1]))
                if vn:
                    # S21 (status sources only): a const phx::Vector2f / 3f made of the pushed values, lane i = pushed
                    # value k + i as_number (kinetic_energy_control::set_speed: push x, push y, {x, y} by address). The
                    # proto says it: 'as_number@<lanes>' for each of those values (l2c_decomp.Printer2: lua_ret)
                    v = self.get('x%d' % xi)
                    xi += 1
                    built = self.mem.get(v[1], UNDEF) if v[0] == 'stk' else UNDEF
                    if built[0] != 'vec':
                        self.fail('%s: argument %d is not a conversion of the value pushed' % (name, k))
                    for i in range(vn):
                        lane = built[1 + i]
                        if k + i >= len(self.lua) or lane[0] != 'conv' or lane[1] != 'as_number' or lane[2] != self.lua[k + i]:
                            self.fail('%s: argument %d is not a conversion of the value pushed' % (name, k))
                        proto.append('as_number@%d' % vn)
                    k += vn
                    continue
                if c == 'f':
                    v = self.get('s%d' % fi)
                    fi += 1
                elif c == 'x':
                    v = self.get('x%d' % xi)
                    xi += 1
                else:
                    self.fail('%s: parameter type' % name)
                if (self.mode != 'body' and (k >= len(self.lua) or v[0] != 'conv' or v[2] != self.lua[k])
                        and ((c == 'f' and v[0] == 'f') or (c == 'x' and v[0] in ('int', 'luaconst')))):
                    # S21 (status sources only): a constant the game's code passes itself, not a pushed value
                    # (target::search(L, kind, 0.0, 0.0)): 'K:f' / 'K:i' in the proto, the value after the pushed ones
                    proto.append('K:f' if c == 'f' else 'K:i')
                    consts.append(v)
                    continue
                if k >= len(self.lua) or v[0] != 'conv' or v[2] != self.lua[k]:
                    self.fail('%s: argument %d is not a conversion of the value pushed' % (name, k))
                proto.append(v[1])
                k += 1
            if k != len(self.lua):                       # (S21: k = the pushed values taken; was len(proto), the same before)
                self.fail('%s: more values pushed than passed' % name)
            i = self.node(kind='lua', sym=sym, args=self.lua + consts, popped=False, proto=tuple(proto))
            self.lua = None
            # S21 (status sources only): such a function can return an integer directly in x0 (app::item::common_param_int):
            # ('luax', node); its width is the register read (l2c_decomp.FExec.get), printed lua_ret<int32_t>(...)
            self.reg['x0'] = ('luax', ('ref', i)) if self.mode != 'body' else UNDEF
            self.reg['v0'] = ('ref', i)                  # S2: such a function can return a float directly (get_value_float)
            return
        # ordinary imported function
        args = []
        xi = fi = 0
        if mangle.is_member(path):
            args.append(self.thisval(x0, name))
            xi = 1
        for t in params:
            c = mangle.reg_class(t)
            st = mangle.strip(t)
            if c == 'f':
                args.append(self.scalar(self.get('s%d' % fi), name))
                fi += 1
                continue
            if c == '?':
                self.fail('%s: parameter type %s' % (name, st))
            if xi >= 8:                                  # ninth and later integer arguments: 8-byte cells at the stack pointer
                v = self.mem.get(self.reg['sp'][1] + 8 * (xi - 8), UNDEF)
            elif c in 'x' and st in ('i', 'j', 'b', 'c', 'h', 's', 't'):
                v = self.get('w%d' % xi)
            else:
                v = self.get('x%d' % xi)
            xi += 1
            vn = isinstance(st, tuple) and st[0] in ('ref', 'ptr') and {('class', 'phx::Vector2f'): 2, ('class', 'phx::Vector3f'): 3, ('class', 'phx::Vector4f'): 4}.get(mangle.strip(st[1]))
            if vn and v[0] == 'stk':                     # S10: a vector built on this function's stack: its lanes are the argument
                built = self.mem.get(v[1], UNDEF)
                if built[0] != 'vec':
                    self.fail('%s: vector argument that is not built from lanes here' % name)
                args.append(('vecarg', vn) + tuple(self.scalar(l, name) for l in built[1:1 + vn]))
                continue
            if c == 'lv' or (isinstance(st, tuple) and st[0] in ('ref', 'ptr') and mangle.strip(st[1]) == ('class', 'lib::L2CValue')):
                args.append(self.lvalue(v, name))
            elif st == ('class', 'phx::Hash40'):
                args.append(('hash', self.scalar(v, name)))
            elif st == 'b':
                args.append(('asbool', self.scalar(v, name)))
            else:
                args.append(self.scalar(v, name))
        if xi > 12 or fi > 8:
            self.fail('%s: arguments on the stack' % name)
        if self.lua:
            self.fail('%s called with values on the Lua stack' % name)
        i = self.node(kind='call', sym=sym, args=args)
        self.reg['x0'] = ('ref', i)
        self.reg['v0'] = ('ref', i)
        if (path[0] == 'lua2cpp' and mangle.is_member(path)) or mangle.is_static_value(path):
            # the script classes' functions return an L2CValue through the buffer in x8
            x8 = self.reg['x8']
            if x8[0] != 'stk':
                self.fail('%s: no result buffer' % name)
            self.lv[x8[1]] = ('ref', i)
            self.reg['x0'] = UNDEF
            self.reg['v0'] = UNDEF

    def thisval(self, v, name):
        if v == AGENT or name == 'lua2cpp::L2CFighterBase::lerp':     # S9: lerp never reads `this`, x0 is whatever the caller left
            return AGENT
        self.fail('%s: member call on something else than the agent' % name)

    def scalar(self, v, name):
        if v[0] in ('int', 'f', 'luaconst', 'ref', 'conv', 'lvop', 'agent', 'L', 'boma', 'agentfield', 'not', 'arg', 'lane'):
            return v
        self.fail('%s: argument is %s' % (name, v[0]))

    # ---- conditions
    def truth(self, v):
        if v[0] in ('conv', 'ref', 'lvop', 'not', 'cmp'):
            return v
        self.fail('condition on %s' % (v[0],))

    def open_if(self, cond, end, pc):
        if end <= pc:
            self.fail('loop')
        self.node(kind='if', cond=cond)
        self.blocks.append(end)

    # ---- one instruction
    def step(self, ins):
        mn = ins.mnemonic
        ops = split_ops(ins.op_str)
        pc = ins.address
        if mn in ('stp', 'ldp'):
            mem = ops[2]
            pre = mem.endswith('!')
            post = len(ops) == 4
            inner = mem.rstrip('!')
            parts = [p.strip() for p in inner[1:-1].split(',')]
            base_tok = parts[0]
            off = imm(parts[1]) if len(parts) > 1 else 0
            base = self.get(base_tok)
            a = self.add(base, 0 if post else off)
            if pre:
                self.put(base_tok, a)
            if mn == 'stp' and ops[0][0] == 'q' and ops[1][0] == 'q' and self.mode != 'body':
                # S21 (status sources only): two 128-bit vectors stored side by side (`stp q0, q0, [sp, #0x10]`: two
                # Vector3f built from the same register), each cell as a `str q` would leave it
                for k in (0, 1):
                    self.store(self.add(a, k * 16), self.get(ops[k]))
                if post:
                    self.put(base_tok, self.add(base, imm(ops[3])))
                return
            for k in (0, 1):
                isf = ops[k] in VREG
                size = 8 if ops[k][0] in 'xd' else 4
                ak = self.add(a, k * size)
                if size != 8:
                    self.fail('pair of 32-bit values')
                if mn == 'stp':
                    self.store(ak, self.get(ops[k]))
                else:
                    self.put(ops[k], self.load(ak, 8, isf))
            if post:
                self.put(base_tok, self.add(base, imm(ops[3])))
            return
        if mn in ('str', 'stur', 'ldr', 'ldur'):
            if ops[1].endswith('!') or len(ops) > 2:
                inner = ops[1].rstrip('!')
                parts = [q.strip() for q in inner[1:-1].split(',')]
                base = self.get(parts[0])
                if ops[1].endswith('!'):
                    a = self.add(base, imm(parts[1]))
                    self.put(parts[0], a)
                else:
                    a = base
                    self.put(parts[0], self.add(base, imm(ops[2])))
            else:
                a = self.addr_of(ops[1])
            isf = ops[0] in VREG
            size = 8 if ops[0][0] in 'xd' else 4
            if mn in ('str', 'stur'):
                self.store(a, self.get(ops[0]))
            else:
                self.put(ops[0], self.load(a, size, isf))
            return
        if mn in ('mov', 'ins') and ('.s[' in ops[0] or '.s[' in ops[1]):     # S10
            self.lane_move(ops[0], ops[1])
            return
        if mn == 'movi':                                 # S10: only "all lanes zero"
            r = VREG.get(ops[0]) or ops[0].split('.')[0]
            try:
                zero = int(ops[1].lstrip('#'), 16) == 0
            except ValueError:
                zero = False
            if not zero or r not in ('v%d' % i for i in range(32)):
                self.fail('instruction movi')
            self.reg[r] = ('vec', ('f', 0.0), ('f', 0.0), ('f', 0.0), ('f', 0.0))
            return
        if mn == 'mov':
            if ops[1].startswith('#'):
                v = imm(ops[1])
                self.put(ops[0], ('int', v & (0xffffffff if ops[0][0] == 'w' else 0xffffffffffffffff)))
            else:
                self.put(ops[0], self.get(ops[1]))
            return
        if mn == 'movk':
            cur = self.get(ops[0])
            if cur[0] != 'int':
                self.fail('movk on a non-constant')
            sh = imm(ops[2].split()[1]) if len(ops) > 2 else 0
            self.put(ops[0], ('int', (cur[1] & ~(0xffff << sh)) | (imm(ops[1]) << sh)))
            return
        if mn in ('add', 'sub'):
            a = self.get(ops[1])
            b = self.get(ops[2])
            if b[0] != 'int' or len(ops) > 3:
                self.fail('%s of non-constants' % mn)
            self.put(ops[0], self.add(a, b[1] if mn == 'add' else -b[1]))
            return
        if mn == 'and' and len(ops) == 3 and ops[2] == '#1':
            v = self.get(ops[1])
            if v[0] not in ('ref', 'conv', 'lvop', 'not'):
                self.fail('and of %s' % (v[0],))
            self.put(ops[0], v)
            return
        if mn == 'adrp':
            self.put(ops[0], ('page', imm(ops[1])))
            return
        if mn == 'fmov':
            if ops[1].startswith('#'):
                self.put(ops[0], ('f', float(ops[1][1:])))
            else:
                v = self.get(ops[1])
                if v[0] != 'int' or ops[0][0] != 's':
                    self.fail('fmov of a non-constant')
                self.put(ops[0], ('f', struct.unpack('<f', struct.pack('<I', v[1] & 0xffffffff))[0]))
            return
        if mn == 'bl':
            self.call(pc, imm(ops[0]))
            return
        if mn in ('tbz', 'tbnz'):
            if imm(ops[1]) != 0:
                self.fail('bit test')
            v = self.truth(self.get(ops[0]))
            self.open_if(v if mn == 'tbz' else ('not', v), imm(ops[2]), pc)
            return
        if mn in ('cbz', 'cbnz'):
            v = self.truth(self.get(ops[0]))
            self.open_if(v if mn == 'cbz' else ('not', v), imm(ops[1]), pc)
            return
        if mn in ('cmp', 'fcmp'):
            a, b = self.get(ops[0]), self.get(ops[1])
            self.flags = (self.scalar(a, 'compare'), self.scalar(b, 'compare'))
            return
        if mn.startswith('b.'):
            cc = mn[2:]
            if not self.flags or cc not in NEG_CC:
                self.fail('conditional branch %s' % cc)
            self.open_if(('cmp', NEG_CC[cc], self.flags[0], self.flags[1]), imm(ops[0]), pc)
            return
        if mn == 'b':
            t = imm(ops[0])
            if self.blocks and pc + 4 == self.blocks[-1] and t > pc + 4:
                self.node(kind='else')
                self.blocks[-1] = t
                return
            if not self.blocks and (t in self.n.plt or t in self.m.macros):
                self.call(pc, t)
                return 'ret'
            self.fail('loop' if t < pc else 'jump')
        if mn == 'nop':
            return
        self.fail('instruction %s' % mn)

    def run(self):
        size = self.n.func_size(self.off)
        if not size:
            self.fail('no unwind entry')
        for ins in self.n.disasm(self.off, size):
            while self.blocks and ins.address == self.blocks[-1]:
                self.blocks.pop()
                self.node(kind='end')
                self.reg['x0'] = UNDEF
            if ins.mnemonic == 'ret':
                if self.blocks:
                    self.fail('return inside a block')
                return self
            if self.step(ins) == 'ret':
                return self
        self.fail('no return')


class Module:
    def __init__(self, name):
        self.r = census.analyse(name)
        self.nro = self.r['nro']
        self.macros = {}
        self.macro_fail = {}
        for a, (core, npush) in self.r['macro_of'].items():
            try:
                e = Exec(self, a, 'macro').run()
                lua = [x for x in e.nodes if x['kind'] == 'lua']
                if len(e.nodes) != 1 or len(lua) != 1:
                    raise NotMechanical('macro is not one script call')
                order = []
                for v in lua[0]['args']:
                    if v[0] != 'arg':
                        raise NotMechanical('macro pushes something that is not a parameter')
                    order.append(v[1])
                self.macros[a] = (core, order, 1 if lua[0]['popped'] else 0)
            except NotMechanical as ex:
                self.macro_fail[a] = str(ex)

    def scripts(self):
        """registered ACMD scripts: (thunk offset, body offset or None, name, kind)"""
        r = self.r
        out = []
        for a, k in sorted(r['kind'].items()):
            if not k.startswith('acmd thunk'):
                continue
            local = [t for pc, t in r['calls'].get(r['idx'][a], []) if t in r['start_set']]
            body = local[0] if len(set(local)) == 1 else None
            out.append((a, body, r['names'].get(a), k.split()[-1]))
        return out


# ---------------------------------------------------------------- C++ text
_hash_labels = None


def hash_text(v):
    global _hash_labels
    if _hash_labels is None:
        _hash_labels = census.script_labels()
    s = _hash_labels.get(v)
    if s is not None and all(32 <= ord(c) < 127 and c not in '"\\' for c in s):
        return 'hash40("%s")' % s
    return 'Hash40{0x%xULL}' % v


def float_text(f):
    if f != f or f in (float('inf'), float('-inf')):
        raise NotMechanical('non-finite float constant')
    s = repr(struct.unpack('<f', struct.pack('<f', f))[0])
    # shortest text that round-trips as a float
    for prec in range(1, 10):
        t = '%.*g' % (prec, f)
        if struct.pack('<f', float(t)) == struct.pack('<f', f):
            s = t
            break
    if 'e' not in s and '.' not in s:
        s += '.0'
    return s + 'f'


def wrapper_name(sym):
    """how the SDK names the wrapper of an imported function"""
    path, params, const = mangle.parse(sym)
    if path[:2] == ['app', 'lua_bind']:
        mod, _, fn = path[2].partition('__')
        if fn.endswith('_impl'):
            fn = fn[:-5]
        return '%s::%s' % (mod, fn)
    if path[:2] == ['app', 'sv_animcmd'] or mangle.is_static_value(path):     # S3: math_abs(x), lerp(a, b, t)
        return path[2]
    if path[0] in ('app', 'lib', 'lua2cpp'):
        return '::'.join(path[1:])
    return '::'.join(path)


class Printer:
    def __init__(self, ex, consts):
        self.ex = ex
        self.consts = consts
        self.nodes = ex.nodes
        self.uses = collections.Counter()
        for nd in self.nodes:
            for v in nd.get('args', []) + ([nd['cond']] if 'cond' in nd else []):
                self.count(v)
        self.named = {}
        self.used_syms = set()

    def count(self, v):
        if not isinstance(v, tuple):
            return
        if v[0] == 'ref':
            self.uses[v[1]] += 1
        for x in v[1:]:
            self.count(x)

    def refs(self, v, out):
        if isinstance(v, tuple):
            if v[0] == 'ref':
                out.append(v[1])
            for x in v[1:]:
                self.refs(x, out)
        return out

    def expr(self, v, want=None):
        k = v[0]
        if k == 'int':
            x = v[1]
            if want == 'b':
                return 'true' if x & 1 else 'false'
            if want == 'i' or want is None:
                x &= 0xffffffff
                if x >= 0x10000 and not x & 0xffff and not x & 0x80000000:
                    return '0x%x' % x
                return str(x - (1 << 32) if x & 0x80000000 else x)
            if want == 'u':
                return '%uu' % (x & 0xffffffff) if (x & 0xffffffff) < 10 else '0x%xu' % (x & 0xffffffff)
            if want == 'l':
                return 'int64_t(%d)' % (x - (1 << 64) if x >> 63 else x)
            if want == 'm':
                return 'uint64_t(0x%x)' % x
            return str(x)
        if k == 'f':
            return float_text(v[1])
        if k == 'luaconst':
            nm = self.consts[v[1]][0] if v[1] < len(self.consts) else None
            return nm if nm else 'lua_const_at(%d)' % v[1]
        if k == 'hash':
            if v[1][0] == 'int':
                return hash_text(v[1][1])
            t = self.expr(v[1])
            return t if t.startswith(('hash40(', 'Hash40{')) else 'Hash40{%s}' % t
        if k == 'asbool':
            return self.expr(v[1], 'b')
        if k == 'nil':
            return 'NIL'
        if k == 'agent':
            return 'agent'
        if k == 'L':
            return 'agent'              # wrappers taking a lua_State* take the agent
        if k == 'boma':
            return 'boma'
        if k == 'ref':
            t = self.named.get(v[1]) or self.call_text(self.nodes[v[1]])
            return t + '.v[0]' if self.nodes[v[1]].get('vret') and want != 'vec' else t      # S10
        if k == 'lv':
            t, x = v[1], v[2]
            if t == 'h':
                return hash_text(x[1]) if x[0] == 'int' else 'Hash40{%s}' % self.expr(x)
            if x[0] == 'int' or (x[0] == 'f' and t == 'f') or (x[0] == 'luaconst' and t == 'i'):
                return self.expr(x, t)
            cast = {'i': 'int32_t', 'u': 'uint32_t', 'l': 'int64_t', 'm': 'uint64_t', 'f': 'float', 'b': 'bool', 'p': 'void *'}[t]
            return 'L2CValue((%s)%s)' % (cast, self.expr(x))
        if k == 'conv':
            inner = self.expr(v[2])
            if v[1] == 'bool':
                return inner if v[2][0] == 'ref' else 'bool(L2CValue(%s))' % inner
            same = {'as_integer': 'iulm', 'as_number': 'f', 'as_bool': 'b', 'as_hash': 'h'}.get(v[1], '')
            if v[2][0] == 'lv' and v[2][1] in same and v[2][2][0] in ('int', 'f', 'luaconst'):
                return inner                # conversion of a constant to its own type
            return 'L2CValue(%s).%s()' % (inner, v[1])
        if k == 'lvop':
            a, b = self.expr(v[2]), self.expr(v[3])
            return '(%s %s %s)' % (a if a.startswith('L2CValue(') else 'L2CValue(%s)' % a, v[1], b if b.startswith('L2CValue(') else 'L2CValue(%s)' % b)
        if k == 'not':
            return '!' + self.expr(v[1])
        if k == 'cmp':
            return '%s %s %s' % (self.expr(v[2]), v[1], self.expr(v[3]))
        if k == 'vecarg':                                # S10: Vector3f{x, y, z}
            return 'Vector%df{%s}' % (v[1], ', '.join('(float)%s' % self.expr(l, 'f') for l in v[2:]))
        if k == 'lane':                                  # S10: one lane of a vector a game function returned
            return '%s.v[%d]' % (self.expr(v[1], 'vec'), v[2])
        raise NotMechanical('cannot print %s' % k)

    def vret_text(self, nd):
        """S10: a call whose result is a vector in v0 (all lanes): not through the wrapper"""
        self.used_syms.add(nd['sym'])
        return 'gcall<Vector4f>(%s)' % ', '.join(['S_' + re.sub(r'\W', '_', wrapper_name(nd['sym']))] + [self.expr(a) for a in nd['args']])

    def call_text(self, nd):
        if nd.get('vret'):
            return self.vret_text(nd)
        self.used_syms.add(nd['sym'])
        name = wrapper_name(nd['sym'])
        if nd['kind'] == 'lua':
            return '%s(%s)' % (name, ', '.join(['agent'] + [self.expr(a) for a in nd['args']]))
        path, params, const = mangle.parse(nd['sym'])
        args = [self.expr(a) for a in nd['args']]
        return '%s(%s)' % (name, ', '.join(args))

    def text(self):
        lines = []
        depth = 1
        nodes = self.nodes
        for i, nd in enumerate(nodes):
            ind = '    ' * depth
            k = nd['kind']
            if k == 'end':
                depth -= 1
                lines.append('    ' * depth + '}')
            elif k == 'else':
                lines.append('    ' * (depth - 1) + '} else {')
            elif k == 'if':
                lines.append(ind + 'if (%s) {' % self.expr(nd['cond']))
                depth += 1
            else:
                u = self.uses[i]
                if nd['kind'] == 'lua' and nd.get('popped') and u == 0:
                    lines.append(ind + self.call_text(nd) + (';' if nd['sym'] in self.ex.m.autopop else '.pop();'))
                elif u == 0:
                    lines.append(ind + self.call_text(nd) + ';')
                else:
                    # inline when the single user is the next node; otherwise give it a name
                    nxt = nodes[i + 1] if i + 1 < len(nodes) else None
                    direct = nxt is not None and u == 1 and i in self.refs(('all',) + tuple(nxt.get('args', [])) + ((nxt['cond'],) if 'cond' in nxt else ()), [])
                    if not direct:
                        self.named[i] = 'r%d' % i
                        lines.append(ind + '%s r%d = %s;' % ('L2CValue' if nd['kind'] == 'lua' else 'auto', i, self.call_text(nd)))
        return lines


def execute_script(mod, body):
    ex = Exec(mod, body, 'body').run()
    if ex.ret_value != ('lv', 'i', ('int', 0)):
        raise NotMechanical('returns something else than 0')
    if ex.lua:
        raise NotMechanical('values left on the Lua stack')
    uses = Printer(ex, []).uses
    for nd in ex.nodes:
        if nd['kind'] == 'lua':
            st = mod.lua_stats.setdefault(nd['sym'], {'protos': set(), 'dropped': 0, 'other': 0})
            st['protos'].add(nd['proto'])
            st['dropped' if nd['popped'] and uses[nd['id']] == 0 else 'other'] += 1
    return ex


def print_script(ex, consts):
    for nd in ex.nodes:
        if nd['kind'] == 'lua' and len(ex.m.lua_stats[nd['sym']]['protos']) != 1:
            raise NotMechanical('%s is called in more than one way' % demangle_simple(nd['sym']))
    p = Printer(ex, consts)
    lines = p.text()
    return lines, p.used_syms


def run_module(name, write):
    mod = Module(name)
    consts = luaconst.table_names()
    res = []
    reasons = collections.Counter()
    bytes_ok = bytes_all = 0
    out = collections.defaultdict(list)
    syms = set()
    bodies_done = {}
    mod.lua_stats = {}
    mod.autopop = set()
    execs = {}
    scripts = mod.scripts()
    for thunk, body, sname, kind in scripts:
        if body and body not in execs:
            try:
                execs[body] = execute_script(mod, body)
            except NotMechanical as ex:
                execs[body] = str(ex)
    mod.autopop = set(s for s, st in mod.lua_stats.items() if st['dropped'] and not st['other'])
    for thunk, body, sname, kind in scripts:
        size = (mod.nro.func_size(body) or 0) if body else 0
        entry = {'thunk': thunk, 'body': body, 'name': sname, 'kind': kind, 'size': size}
        try:
            if body is None:
                raise NotMechanical('no body of its own (forwards to a shared function)')
            if not sname or sname.startswith('hash_'):
                raise NotMechanical('script name not in the label lists')
            if body in bodies_done:
                lines, used = bodies_done[body]
            else:
                if isinstance(execs[body], str):
                    raise NotMechanical(execs[body])
                lines, used = print_script(execs[body], consts)
                bodies_done[body] = (lines, used)
                bytes_ok += size
            entry['ok'] = True
            syms |= used
            out[kind].append((sname, thunk, lines))
        except NotMechanical as ex:
            entry['ok'] = False
            entry['why'] = str(ex)
            reasons[str(ex).split('+0x')[0][:70]] += 1
        res.append(entry)
    seen = set()
    for e in res:
        if e['body'] and e['body'] not in seen:
            seen.add(e['body'])
            bytes_all += e['size']
    return mod, res, reasons, out, syms, bytes_ok, bytes_all


def write_module(short, mod, res, out, syms):
    fdir = os.path.join(STUDY, 'fighters', short)
    os.makedirs(os.path.join(fdir, 'acmd'), exist_ok=True)
    os.makedirs(os.path.join(fdir, 'generated'), exist_ok=True)
    os.makedirs(os.path.join(STUDY, 'fighters', 'tools_out'), exist_ok=True)
    binds = []
    for kind, scripts in sorted(out.items()):
        path = os.path.join(fdir, 'acmd', kind + '.cpp')
        text = ['// %s: ACMD scripts "%s_*". Decompiled once by fighter_sdk/tools/acmd_decomp.py from the game\'s own module;' % (short, kind),
                '// from here on this file is source: edit it, then `python fighter_sdk/tools/build_fighter.py %s`.' % short,
                '#include "fighter_sdk.h"', '']
        seen = {}
        for sname, thunk, lines in sorted(scripts):
            ident = sname
            if ident in seen:                      # the same script name registered for several agents (mario, mario_fireball ...)
                seen[ident] += 1
                ident = '%s__%d' % (sname, seen[ident])
            else:
                seen[ident] = 0
            text.append('ACMD(%s, %s) {' % (short, ident))
            text += lines
            text.append('}')
            text.append('')
            binds.append((thunk, ident, sname))
        with open(path, 'w', newline='\n') as f:
            f.write('\n'.join(text))
    with open(os.path.join(fdir, 'generated', 'bindings.inc'), 'w', newline='\n') as f:
        f.write('// GENERATED by acmd_decomp.py: where each script\'s original lives in lua2cpp_%s (offset of the registered function).\n' % short)
        f.write('// The only place with addresses; the move logic never names one.\n')
        for thunk, ident, sname in sorted(binds):
            f.write('ACMD_BIND(%s, %s, 0x%x)\n' % (short, ident, thunk))
    with open(os.path.join(STUDY, 'fighters', 'tools_out', short + '_acmd.json'), 'w') as f:
        protos = {s: {'proto': list(sorted(st['protos'])[0]), 'autopop': s in mod.autopop} for s, st in mod.lua_stats.items() if len(st['protos']) == 1}
        json.dump({'scripts': res, 'imports_used': sorted(syms), 'lua_calls': protos}, f, indent=1)


def main():
    what = sys.argv[1]
    stats_only = '--stats' in sys.argv
    mods = [e['name'] for e in census.nro_map()] if what == 'all' else [what]
    tot = collections.Counter()
    all_reasons = collections.Counter()
    for m in mods:
        mod, res, reasons, out, syms, bytes_ok, bytes_all = run_module(m, not stats_only)
        ok = sum(1 for e in res if e['ok'])
        own = sum(1 for e in res if e['body'])
        tot['scripts'] += len(res)
        tot['own'] += own
        tot['ok'] += ok
        tot['bytes_ok'] += bytes_ok
        tot['bytes_all'] += bytes_all
        tot['macros'] += len(mod.macros)
        tot['macro_fail'] += len(mod.macro_fail)
        all_reasons.update(reasons)
        print('%-22s scripts %5d  with own body %5d  decompiled %5d (%.1f%% of own; body bytes %.1f%%)  macros %d ok / %d not' %
              (m, len(res), own, ok, 100.0 * ok / max(own, 1), 100.0 * bytes_ok / max(bytes_all, 1), len(mod.macros), len(mod.macro_fail)), flush=True)
        if not stats_only:
            write_module(mod.nro.short, mod, res, out, syms)
        if len(mods) == 1:
            for a, w in mod.macro_fail.items():
                print('  macro +0x%x not understood: %s' % (a, w))
    if len(mods) > 1:
        print('TOTAL scripts %d, with own body %d, decompiled %d (%.1f%%), body bytes %.1f%%' %
              (tot['scripts'], tot['own'], tot['ok'], 100.0 * tot['ok'] / max(tot['own'], 1), 100.0 * tot['bytes_ok'] / max(tot['bytes_all'], 1)))
    print('reasons a script is not mechanical:')
    for w, c in all_reasons.most_common(25):
        print('  %6d  %s' % (c, w))


if __name__ == '__main__':
    main()
