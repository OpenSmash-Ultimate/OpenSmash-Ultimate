"""A fighter's OWN status scripts (session 8): census, registration, decompilation, build, offline test.

Every fighter module (lua2cpp_<name>) holds, besides the ACMD scripts, the fighter's own state functions: for each
of its agents (the fighter and its weapons) an INSTALL function that binds (status kind, line) -> function with
lua2cpp::L2CAgentBase::sv_set_status_func, the registered functions (pre / main / end / init / exec / ...), the
main loops they hand to the shared state machine by address, and helpers. None of them has an exported name, so
this driver names them from the registration (public constant names) and gives them to the general decompiler
(status_decomp.MExec + l2c_decomp), exactly as lua2cpp_common is done.

    python fighter_status.py census [name ...]        table per fighter: agents, functions, bytes, registrations
    python fighter_status.py list mario               the registration table of one fighter (readable)
    python fighter_status.py collect [name ...]       how the status scripts use script calls no ACMD script makes
    python fighter_status.py decompile [name ...]     fighters/<name>/status/*.cpp, generated/status_local.h,
                                                      generated/status_bindings.inc, tools_out/status_s8/<name>_status.json
    python fighter_status.py stats [name ...]         the same without writing anything
    python fighter_status.py show mario <regex>       print the functions whose name matches
    python fighter_status.py sdk                      SDK headers for this work -> fighters/sdk_generated_s8 (never the shared one)
    python fighter_status.py build [name ...]         build_fighter.py with SSBU_FIGHTER_STATUS=1 into SSBU_FIGHTER_BIN
                                                      (default fighters_bin_s8); what does not compile is refused
    python fighter_status.py test [name ...]          offline synthetic differential (S2_EXE, default ssbu_s4.exe)
    python fighter_status.py cycle [name ...]         decompile, sdk, build, test, refuse, until stable
    python fighter_status.py table                    tools_out/status_s8/status_table.txt

Names: a registered function is <agent>_<status kind>_<line> (mario_special_n_main); a function whose address is
taken inside F is F_loop (F_loop2 ...); any other helper is <fighter>_sub_<offset>. Environment: S2_JOBS (6).
Nothing here starts the game; nothing is written to fighters_bin\\ or fighters\\common\\ or the shared SDK folder.
"""
import bisect
import collections
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import acmd_decomp as A          # noqa: E402
import census                    # noqa: E402
import l2c_decomp as D           # noqa: E402
import luaconst                  # noqa: E402
import mangle                    # noqa: E402
import status_decomp as SD       # noqa: E402
from acmd_decomp import NotMechanical   # noqa: E402
import sdk_paths                 # noqa: E402
from nrolib import STUDY, demangle_simple   # noqa: E402

OUT = os.path.join(STUDY, 'fighters', 'tools_out', 'status_s8')
SDK_DIR = 'sdk_generated_s8'
# S12: SSBU_FIGHTERS_ROOT=<folder under fighters\>: a WORK run. Sources / generated files go to fighters\<root>\<name>\
# (not fighters\<name>\), tool output to fighters\<root>\tools_out\status, the SDK to fighters\<root>\sdk_generated, and a
# build first copies the fighter's acmd\ and generated\bindings.inc there. Nothing outside <root> is written.
ROOT = os.environ.get('SSBU_FIGHTERS_ROOT') or ''
if ROOT:
    OUT = os.path.join(STUDY, 'fighters', ROOT, 'tools_out', 'status')
    SDK_DIR = ROOT + '/sdk_generated'
FROOT = os.path.join(STUDY, 'fighters', ROOT)
BIN = os.environ.get('SSBU_FIGHTER_BIN') or 'fighters_bin_s8'
EXE = os.environ.get('S2_EXE', 'ssbu_s4.exe')
JOBS = int(os.environ.get('S2_JOBS', '6'))
MAX_ARGS = 6


def all_modules():
    return [e['name'][8:] for e in census.nro_map() if e['name'] != 'lua2cpp_common']


# ---------------------------------------------------------------- what a module registers
def camel_strip(name, marker):
    i = name.find(marker)
    return name[i + len(marker):] if i >= 0 else name


def agent_classes(mod):
    """[(agent name or hash text, vtable offset)] from create_agent_fighter_status_script_<name>: the function
    compares the agent's hash40 and stores the vtable of the class it creates"""
    n = mod.nro
    labels = census.script_labels()
    out = []
    for off, sym in sorted(n.exports.items()):
        if 'status_script' not in sym or not n.func_size(off):
            continue
        ins = n.disasm(off, n.func_size(off))
        consts = {}
        last_cmp = None
        label = {}
        for k, i in enumerate(ins):
            ops = A.split_ops(i.op_str)
            if i.mnemonic == 'mov' and len(ops) == 2 and ops[1].startswith('#'):
                consts[ops[0]] = A.imm(ops[1])
            elif i.mnemonic == 'movk' and ops[0] in consts:
                sh = A.imm(ops[2].split()[1]) if len(ops) > 2 else 0
                consts[ops[0]] = (consts[ops[0]] & ~(0xffff << sh)) | (A.imm(ops[1]) << sh)
            elif i.mnemonic == 'cmp' and len(ops) == 2:
                last_cmp = consts.get(ops[1])
            elif i.mnemonic == 'b.eq' and last_cmp is not None:
                label[A.imm(ops[0])] = last_cmp
            elif i.mnemonic == 'b.ne' and last_cmp is not None:
                label[i.address + 4] = last_cmp
        by_addr = {i.address: k for k, i in enumerate(ins)}
        for a, h in sorted(label.items()):
            k = by_addr.get(a)
            page = {}
            while k is not None and k < len(ins):
                i = ins[k]
                ops = A.split_ops(i.op_str)
                if i.mnemonic == 'adrp':
                    page[ops[0]] = A.imm(ops[1])
                elif i.mnemonic == 'add' and len(ops) == 3 and ops[1] in page and ops[2].startswith('#'):
                    out.append((labels.get(h) or 'hash_%010x' % h, page[ops[1]] + A.imm(ops[2])))
                    break
                elif i.mnemonic in ('ret', 'b.eq', 'b.ne'):
                    break
                elif i.mnemonic == 'b':
                    k = by_addr.get(A.imm(ops[0]))
                    continue
                k += 1
    return out


# S21: names of the item scripts by item. create_agent_item_status_script builds, per item class, the object (vtable
# stored at +0), one TABLE per status at a member (+0x3f8, +0x408 ...) and puts each status function into its table
# under a key (hash40 "setting", "init", "update", "coroutine", "exit", ...): `operator[](member, key) =
# L2CValue(function)`. The class's install (vtable slot 9) registers each member under a status kind and a NAME
# ("INITIALIZE", "FALL" ...): `item_sub_8fcf50(agent, kind, "NAME", agent->member)`. Both are read here by a linear
# scan of the two functions (registers, their spills to the frame, constants); a function found so is named
# <item>_<name>_<key> (darzpierce_initialize_init), its file is the item's. Anything not matched keeps its old name.
ITEM_KEYS = {0x79f74b898: 'setting', 0x4c674e474: 'init', 0x698253578: 'update', 0x95cb61c70: 'coroutine', 0x49409840e: 'exit'}


def item_names(mod):
    n = mod.nro
    r = mod.r
    classes = agent_classes(mod)
    vt_name = {v: nm for nm, v in classes}
    helper_install = {}
    for nm, v in classes:
        t = n.rel_ptr.get(v + 8 * 9)
        if t is not None:
            helper_install[v] = t
    lv_ctor_fn = set(a for a, s in n.plt.items() if s in ('_ZN3lib8L2CValueC1EPv', '_ZN3lib8L2CValueC1EPKv'))
    ix = set(a for a, s in n.plt.items() if s == '_ZN3lib8L2CValueixEN3phx6Hash40E' or s == '_ZNK3lib8L2CValueixEN3phx6Hash40E')
    def scan(off, on_ins):
        st = {'reg': {}, 'const': {}, 'page': {}, 'spill': {}}
        for i in n.disasm(off, n.func_size(off)):
            ops = A.split_ops(i.op_str)
            reg, const, page, spill = st['reg'], st['const'], st['page'], st['spill']
            mn = i.mnemonic
            if mn == 'mov' and len(ops) == 2 and ops[1].startswith('#'):
                const[A.XREG.get(ops[0])] = A.imm(ops[1]) & (0xffffffff if ops[0][0] == 'w' else 0xffffffffffffffff)
                reg.pop(A.XREG.get(ops[0]), None)
            elif mn == 'movk' and A.XREG.get(ops[0]) in const:
                sh = A.imm(ops[2].split()[1]) if len(ops) > 2 else 0
                k = A.XREG[ops[0]]
                const[k] = (const[k] & ~(0xffff << sh)) | (A.imm(ops[1]) << sh)
            elif mn == 'adrp':
                page[A.XREG.get(ops[0])] = A.imm(ops[1])
                reg.pop(A.XREG.get(ops[0]), None)
            elif mn == 'add' and len(ops) == 3 and ops[2].startswith('#') and A.XREG.get(ops[1]) in page and ops[0] == ops[1]:
                k = A.XREG[ops[0]]
                const[k] = page.pop(k) + A.imm(ops[2])
                reg.pop(k, None)
            elif mn == 'add' and len(ops) == 3 and ops[2].startswith('#') and A.XREG.get(ops[1]) == st.get('obj'):
                reg[A.XREG[ops[0]]] = A.imm(ops[2])
                const.pop(A.XREG[ops[0]], None)
            elif mn == 'mov' and len(ops) == 2 and ops[1] in A.XREG and A.XREG[ops[1]]:
                k, s = A.XREG[ops[0]], A.XREG[ops[1]]
                for d in (reg, const):
                    if s in d:
                        d[k] = d[s]
                    else:
                        d.pop(k, None)
                if s == st.get('obj'):
                    reg[k] = 0
            elif mn in ('str', 'stur') and len(ops) == 2 and A.XREG.get(ops[0]) in reg:
                spill[ops[1]] = reg[A.XREG[ops[0]]]
            elif mn in ('ldr', 'ldur') and len(ops) == 2 and ops[1] in spill:
                reg[A.XREG.get(ops[0])] = spill[ops[1]]
            on_ins(i, ops, st)
            if mn == 'bl':
                for x in range(0, 18):
                    reg.pop('x%d' % x, None)
                    const.pop('x%d' % x, None)
        return st

    # 1. create_agent_item_status_script: function -> (class, member, key)
    found = {}
    create = [o for o, s in n.exports.items() if 'status_script' in s and n.func_size(o)]

    def on_create(i, ops, st):
        if i.mnemonic == 'add' and ops[0] in A.XREG and st['const'].get(A.XREG[ops[0]]) in vt_name:
            st['cls'] = vt_name[st['const'][A.XREG[ops[0]]]]
            st['clsv'] = st['const'][A.XREG[ops[0]]]
        if i.mnemonic == 'str' and len(ops) == 2 and ops[1].endswith(']') and st.get('clsv') is not None \
                and st['const'].get(A.XREG.get(ops[0])) == st['clsv'] and ',' not in ops[1]:
            st['obj'] = A.XREG.get(ops[1].strip('[]'))      # `str x8, [x19]`: x19 is the new object
        if i.mnemonic == 'bl' and A.imm(ops[0]) in ix:
            st['pend'] = (st['reg'].get('x0'), st['const'].get('x1'))
        if i.mnemonic == 'bl' and A.imm(ops[0]) in lv_ctor_fn:
            fn = st['const'].get('x1')
            p = st.pop('pend', None)
            if fn in r['idx'] and p and p[0] is not None and p[1] is not None and st.get('cls'):
                found.setdefault(fn, set()).add((st['cls'], p[0], p[1]))

    for off in create:
        scan(off, on_create)
    # 2. each class's install: member -> status name
    names_of = {}
    for v, inst in helper_install.items():
        cls = vt_name[v]
        mem = {}

        # the NAME's value is built first (`adrp x1; add x1; bl L2CValue(char const*)`), then the member's copy
        # (`add x1, x19, #0x3f8; bl L2CValue(const L2CValue &)`): the member gets the last text
        def on_inst2(i, ops, st, mem=mem):
            if i.mnemonic == 'bl' and A.imm(ops[0]) in n.plt and n.plt[A.imm(ops[0])] == '_ZN3lib8L2CValueC1EPKc':
                t = st['const'].get('x1')
                st['lasttext'] = n.cstr(t) if t is not None and n.ro_off <= t < n.ro_off + n.ro_size else None
            if i.mnemonic == 'add' and len(ops) == 3 and ops[0] == 'x1' and ops[2].startswith('#') and ops[1] in ('x19', 'x20', 'x21', 'x22') \
                    and st.get('lasttext'):
                mem.setdefault(A.imm(ops[2]), st['lasttext'])
                st['lasttext'] = None
        scan(inst, on_inst2)
        names_of[cls] = mem
    out = {}
    used = collections.Counter()
    for fn in sorted(found):
        if len(found[fn]) != 1:
            continue                                     # the same function in two places: no single name
        cls, m, key = next(iter(found[fn]))
        st = names_of.get(cls, {}).get(m)
        if not st:
            continue
        k = ITEM_KEYS.get(key, 'k%x' % key)
        ident = re.sub(r'\W', '_', '%s_%s_%s' % (cls, st.lower(), k))
        out[fn] = (ident, cls, st, k)
        used[ident] += 1
    return {fn: v for fn, v in out.items() if used[v[0]] == 1}


def closure(mod, roots):
    """functions reached from `roots` by direct calls and by addresses formed in their code (not ACMD, not macros)"""
    r = mod.r
    fdes = r['fdes']
    starts = [a for a, s in fdes]
    ptr_in = collections.defaultdict(list)
    for pc, (a, reg) in r['code_ptr'].items():
        i = bisect.bisect_right(starts, pc) - 1
        if i >= 0 and pc < fdes[i][0] + fdes[i][1]:
            ptr_in[fdes[i][0]].append((pc, a))
    seen = []
    done = set()
    work = list(roots)
    taken_by = collections.defaultdict(list)
    while work:
        f = work.pop(0)
        if f in done or f not in r['idx']:
            continue
        k = r['kind'].get(f, '')
        # S20: a function census calls a macro (one script call between Lua-stack plumbing) that the macro model
        # refused (mod.macro_fail) is an ordinary helper: it was left out, and every call to it was refused as
        # "call to local function"
        if k.startswith('acmd') or (k == 'macro' and f not in getattr(mod, 'macro_fail', {})) or f in mod.macros:
            continue
        done.add(f)
        seen.append(f)
        for pc, t in sorted(r['calls'].get(r['idx'][f], [])):
            if t in r['start_set'] and t not in mod.nro.plt:
                work.append(t)
        for pc, a in sorted(ptr_in.get(f, [])):
            taken_by[a].append(f)
            work.append(a)
    return seen, taken_by


def analyse(mod):
    """-> dict: agents [{name, vtable, install, entries [(kind text, line text, fn offset)]}], functions {off: ident},
    role {off: 'install' | 'registered' | 'loop' | 'helper'}, group {off: file name}"""
    n = mod.nro
    r = mod.r
    short = n.short
    consts = luaconst.table_names()
    installs = sorted(r['fdes'][i][0] for i, cl in r['calls'].items() if any(n.plt.get(t) == census.SET_STATUS for pc, t in cl))
    vt = {}
    for nm, v in agent_classes(mod):
        for k in range(96):
            t = n.rel_ptr.get(v + 8 * k)
            if t in installs:
                vt.setdefault(t, (nm, v))
                break
    mod.locals = {}
    names = {}
    role = {}
    group = {}
    used = set()

    def claim(off, ident, rl, grp):
        if off in names:
            return
        base, k = ident, 1
        while ident in used:
            k += 1
            ident = '%s_%d' % (base, k)
        used.add(ident)
        names[off] = ident
        role[off] = rl
        group[off] = grp

    agents = []
    for f in installs:
        nm, v = vt.get(f, ('agent_%x' % f, 0))
        agents.append({'name': nm, 'vtable': v, 'install': f, 'entries': [], 'why': None})
    # lua2cpp_item: the install function of a class (same vtable slot, 9) does not call sv_set_status_func itself but
    # a shared helper of the module that does; the functions it registers are the addresses formed in its code
    seen_install = set(installs)
    for nm, v in agent_classes(mod):
        t = n.rel_ptr.get(v + 8 * 9)
        if t in r['idx'] and t not in seen_install and not any(a['vtable'] == v for a in agents):
            seen_install.add(t)
            agents.append({'name': nm, 'vtable': v, 'install': t, 'entries': [], 'why': None, 'helper': True})
    # the install functions are executed with every function of the module nameable, then the names are made
    mod.locals = {a: 'fn_%x' % a for a, s in r['fdes']}

    def cname(e, strip):
        if e[0] == 'lv' and e[2][0] == 'luaconst':
            c = consts[e[2][1]][0] if e[2][1] < len(consts) else None
            return (c or 'lua_const_at(%d)' % e[2][1]), (camel_strip(c, strip).lower() if c else 'const%d' % e[2][1])
        if e[0] == 'lv' and e[2][0] == 'int':
            return str(e[2][1]), 'k%d' % e[2][1]
        return '?', 'x'

    for ag in agents:
        claim(ag['install'], '%s_install' % ag['name'], 'install', 'install')
        if ag.get('helper'):
            lo, hi = ag['install'], ag['install'] + (n.func_size(ag['install']) or 0)
            for pc in sorted(p for p in r['code_ptr'] if lo <= p < hi):
                fn = r['code_ptr'][pc][0]
                ag['entries'].append(('?', '?', fn))
                claim(fn, '%s_status_%x' % (ag['name'], fn), 'registered', ag['name'])
            continue
        try:
            ex = SD.execute(mod, ag['install'])
            # S18: an install function that registers in a loop (the three Mii fighters) executes since the counted
            # loops are understood, but its status kinds are a variable, not constants: there is no name to take from
            # them. It is named as before (<agent>_status_<offset>): the names are what the exclude files, the
            # bindings and every earlier tree use, they must not move.
            for nd in ex.allnodes.values():
                if nd['kind'] == 'call' and nd.get('sym') == census.SET_STATUS and cname(nd['args'][1], 'STATUS_KIND_')[0] == '?':
                    raise NotMechanical('status kind that is not a constant')
        except (NotMechanical, RecursionError) as x:
            ag['why'] = str(x) or 'too deeply nested'
            # the registered functions are still found (the pointer formed before each call), their kinds are not
            for pc, t in r['calls'].get(r['idx'][ag['install']], []):
                if n.plt.get(t) != census.SET_STATUS:
                    continue
                for p in range(pc - 4, max(pc - 64, ag['install']) - 4, -4):
                    if p in r['code_ptr'] and r['code_ptr'][p][1] == 3:
                        fn = r['code_ptr'][p][0]
                        ag['entries'].append(('?', '?', fn))
                        claim(fn, '%s_status_%x' % (ag['name'], fn), 'registered', ag['name'])
                        break
            continue
        ag['conditional'] = len(ex.B) > 1
        if ag['name'].startswith('hash_'):               # a weapon: its name is in its constants (WEAPON_MARIO_FIREBALL_STATUS_KIND_*), checked against the hash
            import zlib
            for nd in ex.allnodes.values():
                if nd['kind'] == 'call' and nd.get('sym') == census.SET_STATUS:
                    m = re.match(r'[A-Z]+_(\w+)_STATUS_KIND_', cname(nd['args'][1], 'STATUS_KIND_')[0])
                    parts = m.group(1).lower().split('_') if m else []
                    cand = ''
                    for bits in range(1 << max(len(parts) - 1, 0)) if 0 < len(parts) <= 8 else ():      # MARIO_HUGE_FLAME is "mario_hugeflame"
                        t = parts[0] + ''.join(('_' if bits >> k & 1 else '') + p for k, p in enumerate(parts[1:]))
                        if 'hash_%010x' % (zlib.crc32(t.encode()) | (len(t) << 32)) == ag['name']:
                            cand = t
                            break
                    if cand:
                        used.discard(names[ag['install']])
                        ag['name'] = cand
                        names[ag['install']] = cand + '_install'
                        used.add(cand + '_install')
                        break
        for i in sorted(ex.allnodes):
            nd = ex.allnodes[i]
            if nd['kind'] == 'call' and nd.get('sym') == census.SET_STATUS:
                a = nd['args']
                kt, ks = cname(a[1], 'STATUS_KIND_')
                lt, ls = cname(a[2], 'STATUS_FUNC_')
                if ls.startswith('status_') and ls != 'status_coroutine' and ls != 'status_setting':
                    ls = ls[7:]
                fn = a[3][1] if a[3][0] == 'img' else None
                ag['entries'].append((kt, lt, fn))
                if fn is not None:
                    claim(fn, '%s_%s_%s' % (ag['name'], ks, ls), 'registered', '%s_%s' % (ag['name'], ks))
    roots = [ag['install'] for ag in agents] + [e[2] for ag in agents for e in ag['entries'] if e[2] is not None]
    if any(ag.get('helper') for ag in agents):
        # lua2cpp_item: the status functions of a class are function values built by its constructor, which is inlined
        # in create_agent_item_status_script; the install function binds them by status kind and a NAME STRING through
        # a helper. They are found here as the addresses formed in that function; which item each belongs to is not
        # worked out yet (names item_status_<offset>).
        # S21: by item where the class's tables and install say so (item_names; SSBU_ITEM_NAMES=0: the old names)
        by_item = item_names(mod) if os.environ.get('SSBU_ITEM_NAMES', '1') != '0' else {}
        ag_by = {ag['name']: ag for ag in agents}
        for off, sym in sorted(n.exports.items()):
            if 'status_script' in sym and n.func_size(off):
                lo, hi = off, off + n.func_size(off)
                for pc in sorted(p for p in r['code_ptr'] if lo <= p < hi):
                    fn = r['code_ptr'][pc][0]
                    if fn not in names:
                        if fn in by_item:
                            ident, cls, stname, key = by_item[fn]
                            claim(fn, ident, 'registered', cls)
                            if cls in ag_by:
                                ag_by[cls]['entries'].append(('status "%s"' % stname, 'table key %s' % key, fn))
                        else:
                            claim(fn, '%s_status_%x' % (short, fn), 'registered', '%s_%02x' % (short, fn >> 16))
                        roots.append(fn)
    fns, taken_by = closure(mod, roots)
    called = set(t for x in fns for pc, t in r['calls'].get(r['idx'][x], []))
    for f in fns:
        if f in names:
            continue
        by = [b for b in taken_by.get(f, []) if b in names and b != f]
        if by and f not in called:
            claim(f, names[by[0]] + '_loop', 'loop', group[by[0]])
        else:
            claim(f, '%s_sub_%x' % (short, f), 'helper', short + '_helpers')
    for f in fns:                                        # names of loops depend on the order: a second sweep for the late ones
        if f not in names:
            claim(f, '%s_sub_%x' % (short, f), 'helper', short + '_helpers')
    mod.locals = {f: names[f] for f in fns}
    return {'agents': agents, 'functions': mod.locals, 'role': role, 'group': group, 'order': fns}


def new_module(name):
    mod = A.Module(name)
    mod.lua_stats, mod.autopop = {}, set()
    mod.float_syms, mod.void_syms, mod.value_syms = set(), set(), set()
    mod.glob = None
    return mod


# ---------------------------------------------------------------- census
def census_row(name):
    mod = new_module(name)
    info = analyse(mod)
    n = mod.nro
    c = collections.Counter()
    for f in info['order']:
        s = n.func_size(f) or 0
        rl = info['role'][f]
        c['fn'] += 1
        c['bytes'] += s
        c['fn_' + rl] += 1
        c['bytes_' + rl] += s
    # fighter agent / weapon agents (projectiles, articles) / item agents: a function belongs to the first agent that reaches it
    owner = {}
    for ag in info['agents']:
        kind = 'item' if n.short == 'item' else 'fighter' if ag['name'] == n.short or len(info['agents']) == 1 else 'weapon'
        c['agents_' + kind] += 1
        fns, _ = closure(mod, [ag['install']] + [e[2] for e in ag['entries'] if e[2] is not None])
        for f in fns:
            if f not in owner:
                owner[f] = kind
                c['fn_' + kind] += 1
                c['bytes_' + kind] += n.func_size(f) or 0
    c['agents'] = len(info['agents'])
    c['entries'] = sum(len(a['entries']) for a in info['agents'])
    c['statuses'] = len(set((a['name'], e[0]) for a in info['agents'] for e in a['entries']))
    c['install_unread'] = sum(1 for a in info['agents'] if a['why'])
    c['text'] = n.text_size
    return name, dict(c), [(a['name'], len(a['entries']), a['why']) for a in info['agents']]


def cmd_census(mods):
    os.makedirs(OUT, exist_ok=True)
    rows = []
    with concurrent.futures.ProcessPoolExecutor(JOBS) as pool:
        for name, c, ags in pool.map(census_row, mods):
            rows.append((name, collections.Counter(c), ags))
    tot = collections.Counter()
    lines = ['%-12s %6s %8s %9s %9s %8s %8s %8s %8s' % ('fighter', 'agents', 'statuses', 'bindings', 'functions', 'bytes', 'install', 'regist.', 'loop+help')]
    for name, c, ags in rows:
        tot.update(c)
        lines.append('%-12s %6d %8d %9d %9d %8d %8d %8d %8d' % (name, c['agents'], c['statuses'], c['entries'], c['fn'], c['bytes'], c['fn_install'],
                                                             c['fn_registered'], c['fn_loop'] + c['fn_helper']))
    lines.append('%-12s %6d %8d %9d %9d %8d %8d %8d %8d' % ('TOTAL', tot['agents'], tot['statuses'], tot['entries'], tot['fn'], tot['bytes'], tot['fn_install'],
                                                         tot['fn_registered'], tot['fn_loop'] + tot['fn_helper']))
    lines.append('')
    lines.append('bytes by role: install %d, registered %d, loops %d, helpers %d; install functions not read: %d of %d' %
                 (tot['bytes_install'], tot['bytes_registered'], tot['bytes_loop'], tot['bytes_helper'], tot['install_unread'], tot['agents']))
    lines.append('')
    lines.append('%-12s %8s %8s %9s | %8s %8s %9s | %8s %8s %9s' % ('by agent', 'fighter', 'fns', 'bytes', 'weapons', 'fns', 'bytes', 'items', 'fns', 'bytes'))
    for name, c, ags in rows + [('TOTAL', tot, None)]:
        lines.append('%-12s %8d %8d %9d | %8d %8d %9d | %8d %8d %9d' % (name, c['agents_fighter'], c['fn_fighter'], c['bytes_fighter'], c['agents_weapon'], c['fn_weapon'],
                                                                      c['bytes_weapon'], c['agents_item'], c['fn_item'], c['bytes_item']))
    text = '\n'.join(lines)
    if len(mods) > 3:
        with open(os.path.join(OUT, 'census.txt'), 'w') as f:
            f.write(text + '\n')
        with open(os.path.join(OUT, 'census.json'), 'w') as f:
            json.dump({name: {'counts': dict(c), 'agents': ags} for name, c, ags in rows}, f, indent=1)
    print(text)


def cmd_list(name):
    mod = new_module(name)
    info = analyse(mod)
    for ag in info['agents']:
        print('agent %s: install function +0x%x (%s), vtable +0x%x%s' % (ag['name'], ag['install'], info['functions'].get(ag['install']), ag['vtable'],
                                                                      '  NOT READ: ' + ag['why'] if ag['why'] else ''))
        for kt, lt, fn in ag['entries']:
            print('  %-52s %-42s %-44s +0x%x %5d bytes' % (kt, lt, info['functions'].get(fn, '?'), fn or 0, mod.nro.func_size(fn or 0) or 0))


# ---------------------------------------------------------------- decompile
def tree_args(v, out):
    if isinstance(v, tuple):
        if v and v[0] == 'arg' and len(v) == 2 and isinstance(v[1], int):
            out.add(v[1])
            return
        if v and v[0] == 'argel':                        # S18: an element of a row argument
            out.add(v[1])
            return
        for x in v[1:]:
            tree_args(x, out)


def args_read(ex, arity):
    """indices of the function's own L2CValue arguments that its expressions read (local calls already truncated)"""
    out = set()
    for nd in ex.allnodes.values():
        args = nd.get('args', [])
        if nd['kind'] == 'local':
            args = args[:arity.get(nd['target'], 0)]
        for a in args:
            tree_args(a, out)
    for b in ex.B.values():
        for v in (b.cond, b.ret):
            if v is not None:
                tree_args(v, out)
        for s, e in getattr(b, 'assigns', []):
            tree_args(e, out)
    return out


def load_glob():
    g = D.load_global() or {}
    for p in (os.path.join(STUDY, 'fighters', 'tools_out', 'status_lua_calls.json'), os.path.join(OUT, 'lua_calls.json')):
        if os.path.exists(p):
            for s, e in json.load(open(p)).items():
                g.setdefault(s, e)
    return g


def run_module(name, collect_only=False):
    mod = new_module(name)
    info = analyse(mod)
    n = mod.nro
    short = n.short
    consts = luaconst.table_names()
    xp = os.path.join(OUT, short + '_exclude.json')
    exclude = json.load(open(xp)) if os.path.exists(xp) and not os.environ.get('FS_NOEXCLUDE') else {}     # FS_NOEXCLUDE=1: look at a refused function (show)
    mod.glob = None if collect_only else load_glob()
    if mod.glob:
        mod.autopop = set(s for s, st in mod.glob.items() if st['autopop'])
    execs = {}
    old_functions = None                                 # S18: the functions whose text must stay as it is (see the res loop)
    op = os.path.join(OUT, '_old_functions.json')
    if os.path.exists(op) and not os.environ.get('FS_NO_COMPOUND'):
        old_functions = set(json.load(open(op)).get(short, []))
    mod.static_fns = set()
    mod.plain_fns = set()                                # S18, see settle_plain below

    def run_one(off):
        try:
            execs[off] = SD.execute(mod, off)
        except NotMechanical as x:
            execs[off] = str(x)
        except RecursionError:
            execs[off] = 'too deeply nested'

    for off in info['order']:
        run_one(off)
    again = set()                                        # helpers found to be called as (result, agent, arguments...): run as such

    def settle():
        while mod.static_fns - again:
            for off in sorted(mod.static_fns - again):
                again.add(off)
                if off in execs:
                    run_one(off)

    settle()
    # S18: helpers called as (result, L2CValue arguments...) with no agent (status_decomp.call_local marks the call
    # `plain`). A callee becomes plain when a function that executed calls it so, nothing that executed calls it any
    # other way, and it does not execute as a member; it is then executed as plain, and kept so even if that fails
    # (its callers are then refused through the cascade, as before).
    def settle_plain():
        while True:
            sites = collections.defaultdict(set)
            for ex in execs.values():
                if not isinstance(ex, str):
                    for nd in ex.allnodes.values():
                        if nd['kind'] == 'local':
                            sites[nd['target']].add('plain' if nd.get('plain') else 'agent')
            new = sorted(t for t, how in sites.items() if how == {'plain'} and t in execs and isinstance(execs[t], str)
                         and t not in mod.static_fns and t not in mod.plain_fns and info['role'][t] in ('helper', 'loop'))
            if not new:
                return
            for off in new:
                mod.plain_fns.add(off)
                run_one(off)

    settle_plain()
    # a function that does not execute as a member and that nothing calls as a member: try the plain convention. It is
    # kept only if it then executes. Such a function is never bound: it runs only when a source caller calls it.
    member_called = set(nd['target'] for ex in execs.values() if not isinstance(ex, str) for nd in ex.allnodes.values()
                        if nd['kind'] == 'local' and not nd.get('static'))
    for off in info['order']:
        if off in mod.plain_fns:
            continue
        if isinstance(execs[off], str) and off not in mod.static_fns and off not in member_called and info['role'][off] in ('helper', 'loop'):
            old = execs[off]
            mod.static_fns.add(off)
            again.add(off)
            run_one(off)
            if isinstance(execs[off], str):
                mod.static_fns.discard(off)
                execs[off] = old
            else:
                settle()
                settle_plain()
    # S20: helpers that WRITE one of their L2CValue arguments (`operator=` through x1..: the caller's value changes,
    # printed const_cast<L2CValue &>(a2) = ...). Their callers are run again knowing it (status_decomp.call_local
    # forgets the written slot after the call); a caller that passes something else than a slot of its own is refused.
    def writes(ex):
        return set(nd['args'][0][1] for nd in ex.allnodes.values() if nd['kind'] == 'store' and nd['args'][0][0] == 'arg')

    mod.arg_written = {}
    for rounds in range(4):
        w = {off: writes(ex) for off, ex in execs.items() if not isinstance(ex, str) and writes(ex)}
        if w == mod.arg_written:
            break
        mod.arg_written = w
        for off, ex in list(execs.items()):
            if not isinstance(ex, str) and any(nd['kind'] == 'local' and nd['target'] in w for nd in ex.allnodes.values()):
                run_one(off)
    if collect_only:
        known = load_glob()
        extra = {}
        for off, ex in execs.items():
            if isinstance(ex, str):
                continue
            p = D.Printer2(ex, [], {})
            p.count_stmts(ex.stmts)
            for nd in ex.allnodes.values():
                if nd['kind'] == 'lua' and nd['sym'] not in known:
                    s = extra.setdefault(nd['sym'], {'protos': set(), 'dropped': 0, 'other': 0})
                    s['protos'].add(nd['proto'])
                    s['dropped' if nd['popped'] and p.uses[nd['id']] == 0 else 'other'] += 1
        return {s: {'protos': sorted(e['protos']), 'dropped': e['dropped'], 'other': e['other']} for s, e in extra.items()}
    # arity: what each function reads of x1..; a local call passes as many arguments as its callee reads
    arity = {off: 0 for off in info['order']}
    for rounds in range(12):
        changed = False
        for off, ex in execs.items():
            if isinstance(ex, str):
                continue
            used = args_read(ex, arity)
            a = max(used) + 1 if used else 0
            if a != arity[off]:
                arity[off] = a
                changed = True
        if not changed:
            break
    res = {}
    for off in info['order']:
        ident = info['functions'][off]
        size = n.func_size(off) or 0
        e = {'off': off, 'name': ident, 'ident': ident, 'size': size, 'role': info['role'][off], 'group': info['group'][off]}
        res[off] = e
        ex = execs[off]
        try:
            if ident in exclude:
                raise NotMechanical(exclude[ident])
            if isinstance(ex, str):
                raise NotMechanical(ex)
            # S21: lua2cpp_item's registration helper (+0x8fd140: kind, then six functions; it calls sv_set_status_func, so
            # analyse lists it as an install of its own) is called by the other functions of the module, never by the
            # game through a class: a helper like the others (not bound, see 'unbound')
            helper_like = info['role'][off] == 'helper' or (info['role'][off] == 'install' and off in member_called
                                                            and not any(a['install'] == off and a['vtable'] for a in info['agents']))
            if arity[off] > MAX_ARGS and (arity[off] > 16 or not (off in mod.static_fns or off in mod.plain_fns or helper_like)):
                # S20: a helper (plain C++, or of the member convention and not bound, see write_module) may take more
                raise NotMechanical('more than 6 arguments')
            calls = set()
            for nd in ex.allnodes.values():
                if nd['kind'] == 'local':
                    if bool(nd.get('plain')) != (nd['target'] in mod.plain_fns):      # S18: the call and the callee must agree on who is in x0 / x1
                        raise NotMechanical('local function called in two ways')
                    want = arity.get(nd['target'], 0)
                    if len(nd['args']) < want:
                        raise NotMechanical('local call with fewer arguments than the function reads')
                    nd['args'] = nd['args'][:want]
                    for k in mod.arg_written.get(nd['target'], ()):      # S20: see arg_written above
                        if k < want and not nd.get('argsrc', [])[k:k + 1] == [True]:
                            raise NotMechanical('the callee writes an argument that is not a value of this frame')
                    # S18: a callee that reads an argument as a row of values: the caller must have built that many
                    # values in a row there, none of them a variable of its own (those are not followed for a row)
                    tex = execs.get(nd['target'])
                    for ai, nrow in sorted(getattr(tex, 'agg', {}).items()) if not isinstance(tex, str) else ():
                        if ai >= len(nd['args']):
                            continue
                        if nd['args'][ai][0] == 'row':
                            continue                     # (this node was completed on an earlier pass)
                        more = nd.get('rows', {}).get(ai, [])[:nrow]
                        if len(more) < nrow or any(D.has_var(x) for x in more):
                            raise NotMechanical('row argument that is not built here')
                        nd['args'][ai] = ('row', nd['args'][ai]) + tuple(more)
                    calls.add(nd['target'])
                if nd['kind'] == 'lua' and mod.glob.get(nd['sym'], {}).get('protos', 1) != 1:
                    # S21: such a call is written lua_ret(...) with THIS call's own conversions (l2c_decomp.Printer2), not
                    # through the SDK wrapper that one way made (only typed calls: an untyped one has no conversions)
                    if not nd['proto']:
                        raise NotMechanical('script call made in more than one way')
                    nd['own_proto'] = True
            if old_functions is not None and ident not in old_functions:
                # S18: readable structure (compound conditions) for functions that are NOT in the list of already-covered
                # ones (<tree>\tools_out\status\_old_functions.json; without that file nothing changes for anyone)
                st = D.Structurer(ex, compound=True)
                ex.stmts = st.tree(ex.off, None, [])
                ex.labels, ex.gotos = st.labels, st.gotos
                ex.folded = st.folded
            p = D.Printer2(ex, consts, D.var_names(ex.variables))
            rets = [b.ret for b in ex.B.values() if b.term[0] in ('ret', 'tail')]
            p.void_fn = all(x is None or x == ('tail',) for x in rets)
            lines = p.function_text(ex.stmts, ex.labels, 'fn', ex.variables)
            for nd in ex.allnodes.values():
                if (nd['kind'] == 'lua' and nd['sym'] in mod.autopop and p.uses[nd['id']] and (nd['proto'] or nd.get('fret'))
                        and not nd.get('vret')):         # (S20: a vector result is lua_vec(...), not the wrapper)
                    # (S18: without typed parameters it is written lua_result(...), see l2c_decomp.Printer2.call_text)
                    raise NotMechanical('uses the result of a script call the SDK declares without one')
                if nd['kind'] == 'lua' and nd['sym'] not in mod.glob:
                    raise NotMechanical('script call no ACMD script uses')
            plain = off in mod.plain_fns
            static = off in mod.static_fns or plain
            if static and p.void_fn:
                raise NotMechanical('static helper without a result')
            if getattr(ex, 'agg', None) and not static:
                raise NotMechanical('row argument of a function the game calls')
            e.update(ok=True, lines=lines, used=sorted(p.used_syms), gotos=ex.gotos, void=p.void_fn, arity=arity[off], calls=sorted(calls), static=static)
            if plain:
                e['plain'] = True
            if not static and arity[off] > MAX_ARGS:
                e['unbound'] = True                      # S20: see write_module (FS_L2C_N with stack arguments)
            if getattr(ex, 'agg', None):
                e['agg'] = sorted(ex.agg)                # S18: arguments that are rows of values (const L2CValue *)
        except NotMechanical as x:
            e.update(ok=False, why=str(x))
        except RecursionError:
            e.update(ok=False, why='too deeply nested')
    # a source function calls a local function directly as source; one that stays original cannot be called so that
    # the offline test still sees the calls: its callers stay original too (counted under their own reason)
    changed = True
    while changed:
        changed = False
        for e in res.values():
            if e['ok'] and any(not res[t]['ok'] for t in e['calls'] if t in res):
                bad = next(t for t in e['calls'] if t in res and not res[t]['ok'])
                e.update(ok=False, why='calls a local function that stays original (%s)' % res[bad]['why'].split('+0x')[0][:40])
                e.pop('lines', None)
                changed = True
    reached = set(t for e in res.values() if e['ok'] for t in e['calls'])
    for off, e in res.items():
        if e['ok'] and e.get('static') and off not in reached:
            e['unreached'] = True                        # readable, but no source function calls it: never run by any test
    return mod, info, [res[o] for o in info['order']]


def reason_key(why):
    why = why.split('+0x')[0]
    for k in ('differential test', 'does not compile', 'the offline test stops', 'calls a local function that stays original'):
        if why.startswith(k):
            return k
    return re.sub(r'\b(v|x|w|s|d|q)\d+\b', r'\1N', why)[:64]


LOCAL_H = r'''// GENERATED by fighter_sdk/tools/fighter_status.py from lua2cpp_%(short)s - do not edit.
// The fighter's own functions have no exported names: this file gives each one a name (from the registration:
// <agent>_<status kind>_<line>), its place in the game's module, and a way to call it.
//   status_function(name)        the address of the function (what sv_set_status_func and the main-loop shifts take)
//   local::name(agent, args...)  call it: as source if it is source in this DLL, otherwise the game's original
#pragma once
#include "fighter_sdk.h"

inline void *fs_module_function(uint32_t off) { return (void *)(uintptr_t)(fs_addr[%(anchor)s] - 0x%(anchor_off)x + off); }
#define status_function(name) fs_module_function(FS_OFF_##name)
template <class... A> inline L2CValue fs_local_source(void (*fn)(Ctx *), L2CAgent *agent, const A &... a) {
    L2CValue args[sizeof...(a) + 1] = {L2CValue(a)...};
    L2CValue out;
    Ctx *c = fs_ctx();
    c->x[0] = (uint64_t)(uintptr_t)agent;
    for (int i = 0; i < (int)sizeof...(a) && i < 7; i++) c->x[1 + i] = (uint64_t)(uintptr_t)&args[i];
    c->x[8] = (uint64_t)(uintptr_t)&out;
    if (sizeof...(a) <= 7) { fn(c); return out; }
    // S20: the eighth and later arguments are pointers in 8-byte cells at the stack pointer (AArch64), see FS_SARG
    uint64_t sp = c->x[31];
    int more = (int)sizeof...(a) - 7;
    uint64_t *cell = (uint64_t *)(uintptr_t)(sp - 8 * (uint64_t)((more + 1) & ~1));
    for (int i = 0; i < more; i++) cell[i] = (uint64_t)(uintptr_t)&args[7 + i];
    c->x[31] = (uint64_t)(uintptr_t)cell;
    fn(c);
    c->x[31] = sp;
    return out;
}
// S20: argument 7 + j of a function the game calls (x0 = agent): the pointer in the j-th cell at the stack pointer
#define FS_SARG(c, j) (*(const L2CValue *)(uintptr_t)*(const uint64_t *)(uintptr_t)((c)->x[31] + 8 * (j)))
// a helper that is not a member function (result buffer in x0, agent in x1, arguments from x2): the game's original
template <class... A> inline L2CValue fs_local_static(uint32_t off, L2CAgent *agent, const A &... a) {
    L2CValue args[sizeof...(a) + 1] = {L2CValue(a)...};
    L2CValue out;
    Ctx *c = fs_ctx();
    c->x[0] = (uint64_t)(uintptr_t)&out;
    c->x[1] = (uint64_t)(uintptr_t)agent;
    for (int i = 0; i < (int)sizeof...(a); i++) c->x[2 + i] = (uint64_t)(uintptr_t)&args[i];
    fs_host.call_addr(c, (uint64_t)(uintptr_t)fs_module_function(off));
    return out;
}
// S18: a value read through a pointer a game function returned, `off` bytes in:
// field<float>(PostureModule::pos(boma), 4) is the y of the position (the game's code reads it in place, too)
template <class T> inline T field(uint64_t pointer, uint32_t off) { return *(const T *)((uintptr_t)pointer + off); }
// S19: an L2CValue member of the agent other than global_table (item agents keep several), by its place in the agent
inline L2CValue &agent_value(L2CAgent *agent, uint32_t off) { return *(L2CValue *)((char *)agent + off); }
// S18: a script call whose result is used here although the SDK wrapper of that call drops it: call, then take the result
template <class... A> inline L2CValue lua_result(L2CAgent *agent, int sym, A... v) {
    L2CValue args[sizeof...(v) + 1] = {L2CValue(v)...};
    return fs_lua_call(agent, sym, args, (int)sizeof...(v)).pop();
}
// S20: a constant L2CValue of the game's script runtime (lib::L2CAgent::math_pi ...), read where it is
inline const L2CValue &fs_gvalue(int slot) { return *(const L2CValue *)(uintptr_t)fs_addr[slot]; }
// S20: a script call that returns a vector in v0 (sv_kinetic_energy::get_speed: the game's code reads lane 1 too)
template <class... A> inline Vector4f lua_vec(L2CAgent *agent, int sym, A... v) {
    L2CValue args[sizeof...(v) + 1] = {L2CValue(v)...};
    fs_lua_clear(agent);
    for (int i = 0; i < (int)sizeof...(v); i++) fs_lua_push(agent, args[i]);
    return gcall<Vector4f>(sym, agent->lua_state);
}
// S21: lua2cpp::L2CAgentBase::coroutine_yield() through the agent's vtable (slot 2), as the game's code calls it
inline void fs_coroutine_yield(L2CAgent *agent) {
    Ctx *c = fs_ctx();
    c->x[0] = (uint64_t)(uintptr_t)agent;
    c->x[8] = 0;
    fs_host.call_addr(c, ((const uint64_t *)agent->vtable)[2]);
}
// S21: a script call written with its conversions spelt out, one letter per parameter after the lua_State: i as_integer,
// f as_number, b as_bool, h as_hash of the next pushed value; 2 / 3 = a phx::Vector2f / 3f made of the next 2 / 3 pushed
// values (as_number), passed by address (kinetic_energy_control::set_speed); I / F = an integer / float CONSTANT the
// game's code passes itself (not pushed), given after the pushed values (target::search(L, kind, 0.0, 0.0)). "" = the
// function takes the lua_State only. R = what the game function leaves in x0 (void: nothing of it is used).
template <class R, class... A> inline R lua_ret(L2CAgent *agent, int sym, const char *conv, A... v) {
    L2CValue args[sizeof...(v) + 1] = {L2CValue(v)...};
    int npush = (int)sizeof...(v);
    for (int p = 0; conv[p]; p++) if (conv[p] == 'I' || conv[p] == 'F') npush--;
    fs_lua_clear(agent);
    for (int i = 0; i < npush; i++) fs_lua_push(agent, args[i]);
    uint64_t w[8] = {0}; float fl[8] = {0}; Vector4f vec[4] = {}; char kind[8] = {0};
    int a = 0, b = npush, n = 0, nv = 0;
    for (int p = 0; conv[p] && n < 8; p++, n++) {          // the conversions first: they may call the game themselves
        char c = conv[p]; kind[n] = c;
        if (c == 'i') w[n] = (uint64_t)args[a++].as_integer();
        else if (c == 'f') fl[n] = args[a++].as_number();
        else if (c == 'b') w[n] = args[a++].as_bool() ? 1 : 0;
        else if (c == 'h') w[n] = args[a++].as_hash().v;
        else if (c == 'I') w[n] = (uint64_t)args[b++].as_integer();
        else if (c == 'F') fl[n] = args[b++].as_number();
        else { int lanes = c - '0'; for (int l = 0; l < lanes; l++) vec[nv].v[l] = args[a++].as_number(); w[n] = (uint64_t)(uintptr_t)&vec[nv++]; }
    }
    GCall g;
    g.put((uint64_t)(uintptr_t)agent->lua_state);
    for (int k = 0; k < n; k++) { if (kind[k] == 'f' || kind[k] == 'F') g.put(fl[k]); else g.put(w[k]); }
    FS_SCRIBBLE_NOW();
    g.call(sym);
    return GRet<R>::get(g.c);
}
// S18: values in a row, for a helper whose parameter is `const L2CValue *` (it reads a0[0], a0[1], a0[2]): local::f(agent, row(x, y, z))
struct row {
    L2CValue v[4];
    row(const L2CValue &a, const L2CValue &b) { v[0] = a; v[1] = b; }
    row(const L2CValue &a, const L2CValue &b, const L2CValue &c) { v[0] = a; v[1] = b; v[2] = c; }
    row(const L2CValue &a, const L2CValue &b, const L2CValue &c, const L2CValue &d) { v[0] = a; v[1] = b; v[2] = c; v[3] = d; }
    operator const L2CValue *() const { return v; }
};

enum : uint32_t {
%(offsets)s
};

extern "C" {
%(externs)s
}
namespace local {
%(wrappers)s
}
'''


def params(e):
    """the C++ parameters of a helper: an L2CValue each, or (S18) a row of them: `const L2CValue *a0`, read a0[0], a0[1] ..."""
    return ['const L2CValue %sa%d' % ('*' if k in e.get('agg', ()) else '&', k) for k in range(e['arity'])]


def write_module(mod, info, res):
    n = mod.nro
    short = n.short
    fdir = os.path.join(FROOT, short)
    sdir = os.path.join(fdir, 'status')
    os.makedirs(sdir, exist_ok=True)
    os.makedirs(os.path.join(fdir, 'generated'), exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    anchor_off, anchor_sym = next((o, s) for o, s in sorted(n.exports.items()) if n.func_size(o) and mangle.parse(s))
    anchor = 'S_' + re.sub(r'\W', '_', A.wrapper_name(anchor_sym))
    offsets, externs, wrappers = [], [], []
    for e in res:
        offsets.append('    FS_OFF_%s = 0x%x,' % (e['ident'], e['off']))
        if e['ok'] and e.get('plain'):                   # S18: a helper that takes no agent
            wrappers.append('L2CValue %s(%s);' % (e['ident'], ', '.join(params(e))))
        elif e['ok'] and e.get('static'):
            wrappers.append('L2CValue %s(L2CAgent *agent%s);' % (e['ident'], ''.join(', ' + x for x in params(e))))
        elif e['off'] in mod.static_fns:
            wrappers.append('template <class... A> inline L2CValue %s(L2CAgent *agent, const A &... a) { return fs_local_static(FS_OFF_%s, agent, a...); }'
                            % (e['ident'], e['ident']))
        elif e['ok']:
            externs.append('void fs_fn_%s(Ctx *c);' % e['ident'])
            wrappers.append('template <class... A> inline L2CValue %s(L2CAgent *agent, const A &... a) { return fs_local_source(fs_fn_%s, agent, a...); }'
                            % (e['ident'], e['ident']))
        else:
            wrappers.append('template <class... A> inline L2CValue %s(L2CAgent *agent, const A &... a) { return call_function(agent, L2CValue(status_function(%s)), a...); }'
                            % (e['ident'], e['ident']))
    with open(os.path.join(fdir, 'generated', 'status_local.h'), 'w', newline='\n') as f:
        f.write(LOCAL_H % {'short': short, 'anchor': anchor, 'anchor_off': anchor_off, 'offsets': '\n'.join(offsets),
                           'externs': '\n'.join(externs), 'wrappers': '\n'.join(wrappers)})
    for nm in os.listdir(sdir):
        if nm.endswith('.cpp'):
            os.remove(os.path.join(sdir, nm))
    files = collections.OrderedDict()
    binds = []
    used = {anchor_sym}
    reg_of = collections.defaultdict(list)
    for ag in info['agents']:
        for kt, lt, fn in ag['entries']:
            reg_of[fn].append('%s: %s, %s' % (ag['name'], kt, lt))
    for e in res:
        if not e['ok']:
            continue
        grp = re.sub(r'\W', '_', e['group'])
        if grp not in files:
            files[grp] = ['// %s: the fighter\'s own status scripts (%s), decompiled by fighter_sdk/tools/fighter_status.py' % (short, grp),
                          '// from the game\'s own module. From here on this file is source: edit it, then',
                          '// `set SSBU_FIGHTER_STATUS=1` and `python fighter_sdk/tools/build_fighter.py %s`.' % short,
                          '#include "fighter_sdk.h"', '#include "status_local.h"', '']
        text = files[grp]
        for line in reg_of.get(e['off'], []):
            text.append('// registered for %s' % line)
        args = ''.join(', a%d' % k for k in range(e['arity']))
        if e.get('plain'):                               # S18: a helper called as (arguments...): it has no agent, it computes on values only
            text.append('L2CValue local::%s(%s) {' % (e['ident'], ', '.join(params(e))))
        elif e.get('static'):                            # a helper called as (agent, arguments...): plain C++, reached from its callers only
            text.append('L2CValue local::%s(L2CAgent *agent%s) {' % (e['ident'], ''.join(', ' + x for x in params(e))))
            text.append('    BattleObjectModuleAccessor *boma = agent->boma;')
        elif e.get('unbound'):
            # S20: a helper of the member convention with more than six arguments: the eighth and later come in stack
            # cells (FS_SARG, status_local.h). Not bound: the game's own callers keep the original; source callers call
            # it through local::name (fs_local_source puts the cells), so it is tested inside them
            decl = ''.join(' FS_COMMA FS_V a%d' % k for k in range(e['arity']))
            pas = ''.join(' FS_COMMA FS_ARG(c, %d)' % (k + 1) if k < 7 else ' FS_COMMA FS_SARG(c, %d)' % (k - 7) for k in range(e['arity']))
            text.append('FS_L2C_%sN(%s,%s,%s) {' % ('VOID_' if e['void'] else '', e['ident'], decl, pas))
        else:
            text.append('L2C_%sFUNCTION%s(%s, %s%s) {' % ('VOID_' if e['void'] else '', str(e['arity']) if e['arity'] else '', short, e['ident'], args))
            binds.append((e['off'], e['ident']))
        text += e.pop('lines')
        text += ['}', '']
        used |= set(e['used'])
    for k, text in files.items():
        with open(os.path.join(sdir, k + '.cpp'), 'w', newline='\n') as f:
            f.write('\n'.join(text))
    with open(os.path.join(fdir, 'generated', 'status_bindings.inc'), 'w', newline='\n') as f:
        f.write('// GENERATED by fighter_status.py: where each status function\'s original lives in lua2cpp_%s.\n' % short)
        for off, ident in sorted(binds):
            f.write('L2C_BIND(%s, %s, 0x%x)\n' % (short, ident, off))
    for e in res:
        e.pop('lines', None)
    agents = [{'name': a['name'], 'install': a['install'], 'vtable': a['vtable'], 'why': a['why'],
               'entries': [[kt, lt, fn, info['functions'].get(fn)] for kt, lt, fn in a['entries']]} for a in info['agents']]
    with open(os.path.join(OUT, short + '_status.json'), 'w') as f:
        json.dump({'functions': res, 'agents': agents, 'imports_used': sorted(used), 'float_returns': sorted(mod.float_syms),
                   'void_syms': sorted(mod.void_syms), 'value_syms': sorted(mod.value_syms)}, f, indent=1)
    return len(files), len(binds)


def summary(res):
    c = collections.Counter()
    reasons = collections.Counter()
    rbytes = collections.Counter()
    for e in res:
        c['fn'] += 1
        c['bytes'] += e['size']
        if e['ok']:
            c['ok'] += 1
            c['bytes_ok'] += e['size']
            if e.get('static'):
                c['static'] += 1
                c['bytes_static'] += e['size']
            if e.get('unreached'):
                c['unreached'] += 1
                c['bytes_unreached'] += e['size']
            c['goto'] += 1 if e.get('gotos') else 0
        else:
            k = reason_key(e['why'])
            reasons[k] += 1
            rbytes[k] += e['size']
    return c, reasons, rbytes


def _decompile(arg):
    name, write = arg
    mod, info, res = run_module(name)
    c, reasons, rbytes = summary(res)
    if write:
        write_module(mod, info, res)
    return name, dict(c), dict(reasons), dict(rbytes)


def _collect(name):
    return name, run_module(name, collect_only=True)


def cmd_collect(mods):
    os.makedirs(OUT, exist_ok=True)
    g = {}
    with concurrent.futures.ProcessPoolExecutor(JOBS) as pool:
        for name, stats in pool.map(_collect, mods):
            for s, st in stats.items():
                e = g.setdefault(s, {'protos': set(), 'dropped': 0, 'other': 0})
                e['protos'] |= set(tuple(p) for p in st['protos'])
                e['dropped'] += st['dropped']
                e['other'] += st['other']
    p = os.path.join(OUT, 'lua_calls.json')
    old = json.load(open(p)) if os.path.exists(p) else {}
    for s, e in g.items():
        old[s] = {'protos': len(e['protos']), 'proto': list(sorted(e['protos'])[0]), 'autopop': bool(e['dropped'] and not e['other'])}
    with open(p, 'w') as f:
        json.dump(old, f, indent=1, sort_keys=True)
    print('collect: %d script calls only status scripts make (%d made in more than one way)' % (len(old), sum(1 for e in old.values() if e['protos'] != 1)))


def cmd_decompile(mods, write=True, quiet=False):
    t0 = time.time()
    tot = collections.Counter()
    reasons = collections.Counter()
    rbytes = collections.Counter()
    rows = []
    with concurrent.futures.ProcessPoolExecutor(JOBS) as pool:
        for name, c, rs, rb in pool.map(_decompile, [(m, write) for m in mods]):
            c = collections.Counter(c)
            rows.append((name, c))
            tot.update(c)
            reasons.update(rs)
            rbytes.update(rb)
    lines = ['%-12s %9s %9s %8s %9s %9s %6s' % ('fighter', 'functions', 'decomp.', '%', 'bytes', 'decomp.', 'goto')]
    for name, c in rows + [('TOTAL', tot)]:
        lines.append('%-12s %9d %9d %7.1f%% %9d %9d %6d' % (name, c['fn'], c['ok'], 100.0 * c['ok'] / max(c['fn'], 1), c['bytes'], c['bytes_ok'], c['goto']))
    lines.append('')
    lines.append('decompiled %d of %d functions (%.1f%%), %d of %d bytes (%.1f%%)' %
                 (tot['ok'], tot['fn'], 100.0 * tot['ok'] / max(tot['fn'], 1), tot['bytes_ok'], tot['bytes'], 100.0 * tot['bytes_ok'] / max(tot['bytes'], 1)))
    lines.append('refused, by reason (functions, bytes):')
    for k, v in sorted(reasons.items(), key=lambda kv: -rbytes[kv[0]]):
        lines.append('  %6d %9d  %s' % (v, rbytes[k], k))
    text = '\n'.join(lines)
    if len(mods) > 3:
        with open(os.path.join(OUT, 'decompile_table.txt'), 'w') as f:
            f.write(text + '\n')
    print(text if not quiet else '\n'.join(lines[len(rows):]))
    print('(%.0f s)' % (time.time() - t0))


def cmd_show(name, pattern):
    mod, info, res = run_module(name)
    rx = re.compile(pattern)
    for e in res:
        if rx.search(e['ident']):
            print('// %s (+0x%x, %d bytes, %s)%s' % (e['ident'], e['off'], e['size'], e['role'], '' if e['ok'] else '   NOT: ' + e['why']))
            if e['ok']:
                print('\n'.join(e['lines']))


# ---------------------------------------------------------------- SDK, build, test
def cmd_sdk():
    """one SDK for this work: what the shared SDK is made from, plus the status scripts of all fighters"""
    used, floats, voids, values = set(), set(), set(), set()
    for nm in os.listdir(OUT):
        if nm.endswith('_status.json') and not nm.startswith('_'):
            j = json.load(open(os.path.join(OUT, nm)))
            used |= set(j['imports_used'])
            floats |= set(j['float_returns'])
            voids |= set(j['void_syms'])
            values |= set(j['value_syms'])
    lp = os.path.join(OUT, 'lua_calls.json')
    merged = {'imports_used': sorted(used), 'float_returns': sorted(floats), 'void_calls': sorted(voids - values), 'value_calls': sorted(values),
              'lua_calls': json.load(open(lp)) if os.path.exists(lp) else {}}
    mp = os.path.join(OUT, '_merged_status.json')
    with open(mp, 'w') as f:
        json.dump(merged, f, indent=1)
    env = dict(os.environ, SSBU_SDK_DIR=SDK_DIR, SSBU_SDK_EXTRA=mp)
    r = subprocess.run([sys.executable, os.path.join(HERE, 'gen_sdk.py')], capture_output=True, text=True, env=env)
    print((r.stdout + r.stderr).strip()[-800:])
    if r.returncode:
        raise SystemExit('gen_sdk.py failed')


def add_exclude(name, items):
    p = os.path.join(OUT, name + '_exclude.json')
    cur = json.load(open(p)) if os.path.exists(p) else {}
    cur.update(items)
    with open(p, 'w') as f:
        json.dump(cur, f, indent=1, sort_keys=True)


def _build(name):
    if ROOT:                                             # S12: the work tree needs the ACMD sources and bindings of the fighter
        import shutil
        src, dst = os.path.join(STUDY, 'fighters', name), os.path.join(FROOT, name)
        shutil.rmtree(os.path.join(dst, 'acmd'), ignore_errors=True)
        if os.path.isdir(os.path.join(src, 'acmd')):
            shutil.copytree(os.path.join(src, 'acmd'), os.path.join(dst, 'acmd'))
        os.makedirs(os.path.join(dst, 'generated'), exist_ok=True)
        if os.path.exists(os.path.join(src, 'generated', 'bindings.inc')):
            shutil.copy(os.path.join(src, 'generated', 'bindings.inc'), os.path.join(dst, 'generated', 'bindings.inc'))
    env = dict(os.environ, SSBU_FIGHTER_STATUS='1', SSBU_FIGHTER_SDK=SDK_DIR, SSBU_FIGHTER_BIN=BIN)
    r = subprocess.run([sys.executable, os.path.join(HERE, 'build_fighter.py'), name], capture_output=True, text=True, env=env)
    return name, r.returncode, r.stdout + r.stderr


def blame(name, text):
    bad = {}
    for m in re.finditer(r'status[\\/](\w+)\.cpp:(\d+):\d+: error: ([^\n]*)', text):
        src = os.path.join(FROOT, name, 'status', m.group(1) + '.cpp')
        ident = None
        with open(src) as f:
            for k, line in enumerate(f, 1):
                if line.startswith('L2C_'):
                    ident = line[line.index(',') + 1:].split(',')[0].split(')')[0].strip()
                elif line.startswith('L2CValue local::'):
                    ident = line[len('L2CValue local::'):].split('(')[0]
                if k >= int(m.group(2)):
                    break
        if ident and ident not in bad:
            bad[ident] = 'does not compile: ' + m.group(3)[:90]
    return bad


def cmd_build(mods):
    """-> modules in which functions were refused by the compiler (decompile + build again), modules that failed otherwise"""
    t0 = time.time()
    again, failed = [], []
    with concurrent.futures.ThreadPoolExecutor(max(2, JOBS // 2)) as pool:
        for name, code, text in pool.map(_build, mods):
            if not code:
                continue
            bad = blame(name, text)
            if not bad:
                print('build %s FAILED and no function could be blamed:\n%s' % (name, text[-1500:]))
                failed.append(name)
                continue
            add_exclude(name, bad)
            again.append(name)
    print('build: %d modules into %s, %d with functions refused by the compiler, %d failed (%.0f s)' % (len(mods), BIN, len(again), len(failed), time.time() - t0), flush=True)
    return again, failed


def run_offline(mods, trace=False, skipfile=None):
    while os.path.exists(os.path.join(STUDY, 'tests', 'QUIET_PLEASE')):      # S12: never start the exe while this file exists
        time.sleep(25)
    env = dict(os.environ)
    env['SSBU_FIGHTER_OFFLINE'] = ','.join(mods)
    env['SSBU_FIGHTER_BIN'] = BIN
    env['SSBU_USERDATA'] = os.path.join(STUDY, 'tests', 's8_userdata' if os.path.isdir(os.path.join(STUDY, 'tests', 's8_userdata')) else 's2_userdata')
    env['SSBU_FIGHTER_REPORT'] = os.path.join(OUT, 'offline_report.txt')
    env['SSBU_PAIR_WARM'] = '0'
    env['SSBU_SHADER_CACHE'] = '0'
    env.pop('SSBU_FIGHTER_OFFLINE_SKIP', None)
    if skipfile:
        env['SSBU_FIGHTER_OFFLINE_SKIP'] = skipfile
    if trace:
        env['SSBU_FIGHTER_OFFLINE_TRACE'] = '1'
    p = subprocess.Popen([sdk_paths.program(EXE, 'S2_EXE'), sdk_paths.main_elf(), '900'], cwd=STUDY, env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = p.communicate(timeout=1500)
    except subprocess.TimeoutExpired:
        p.kill()                                         # this process only (by handle)
        out, _ = p.communicate()
    # the host loads a private copy fighters_bin\<name>.run<pid>_<n>.dll: remove the copies of THIS process only
    fb = os.path.join(STUDY, 'fighters_bin')
    for nm in os.listdir(fb):
        if '.run%d_' % p.pid in nm and nm.endswith('.dll'):
            try:
                os.remove(os.path.join(fb, nm))
            except OSError:
                pass
    return out.decode(errors='replace')


def cmd_test(mods):
    """-> {module: result}, modules with new refusals. Status functions and ACMD scripts of the DLL are both run;
    a differing ACMD script is reported, not refused (this driver does not own them)."""
    import batch
    t0 = time.time()
    results = {}
    again = set()
    for name in mods:
        skip = []
        skipfile = os.path.join(OUT, 'offline_skip_%s.txt' % name)
        while True:
            with open(skipfile, 'w', newline='\n') as f:
                f.write('\n'.join(skip) + '\n')
            text = run_offline([name], trace=True, skipfile=skipfile)
            res, finished, last = batch.parse_offline(text)
            if name in res:
                break
            if not last or last in skip or len(skip) > 300:
                print('test: the offline test of %s gives no result:\n%s' % (name, text[-600:]))
                break
            skip.append(last)
        with open(os.path.join(OUT, 'offline_%s.log' % name), 'w', errors='replace') as f:
            f.write(text)
        if name not in res:
            continue
        r = res[name]
        sj = json.load(open(os.path.join(OUT, name + '_status.json')))
        mine = set(e['ident'] for e in sj['functions'] if e['ok'] and not e.get('static') and not e.get('unbound'))      # bound = tested by themselves
        bad = {k: v for k, v in r['bad'].items() if k in mine}
        for s in skip:
            if s in mine:
                bad[s] = 'the offline test stops in it (crash or endless run)'
        r['status_of'] = len(mine)
        r['status_bad'] = bad
        r['status_equal'] = len(mine) - len(bad)
        r['acmd_bad'] = {k: v for k, v in r['bad'].items() if k not in mine}
        r['stopped'] = [s for s in skip if s not in mine]
        results[name] = r
        if bad:
            add_exclude(name, bad)
            again.add(name)
        print('test: %s: status functions %d equal on all 3 passes, %d differing or stopping, of %d; whole DLL %d equal, %d differing, of %d' %
              (name, r['status_equal'], len(bad), len(mine), r['equal'], r['differ'], r['of']), flush=True)
    p = os.path.join(OUT, 'offline_all.json')
    tested = json.load(open(p)) if os.path.exists(p) else {}
    tested.update(results)
    with open(p, 'w') as f:
        json.dump(tested, f, indent=1)
    print('test: %d modules (%.0f s)' % (len(results), time.time() - t0))
    return results, sorted(again)


def cmd_cycle(mods):
    cmd_decompile(mods, quiet=True)
    cmd_sdk()
    todo = list(mods)
    for rounds in range(8):
        for k in range(8):
            again, failed = cmd_build(todo if k == 0 else again)
            if not again:
                break
            cmd_decompile(again, quiet=True)
        results, again = cmd_test([m for m in todo if m not in failed])
        if not again:
            break
        cmd_decompile(again, quiet=True)
        todo = again
    cmd_table()


def cmd_table():
    p = os.path.join(OUT, 'offline_all.json')
    tested = json.load(open(p)) if os.path.exists(p) else {}
    tot = collections.Counter()
    reasons = collections.Counter()
    rbytes = collections.Counter()
    lines = ['%-12s %9s %9s %9s %9s %9s %9s' % ('fighter', 'functions', 'source', 'equal', 'bytes', 'source', 'equal')]
    for name in all_modules():
        sp = os.path.join(OUT, name + '_status.json')
        if not os.path.exists(sp):
            continue
        j = json.load(open(sp))
        c, rs, rb = summary(j['functions'])
        t = tested.get(name)
        if t and t.get('status_of') == c['ok'] - c['static'] and not t.get('status_bad'):
            c['equal'] = c['ok'] - c['static']
            c['bytes_equal'] = c['bytes_ok'] - c['bytes_static']
        tot.update(c)
        reasons.update(rs)
        rbytes.update(rb)
        lines.append('%-12s %9d %9d %9d %9d %9d %9d' % (name, c['fn'], c['ok'], c['equal'], c['bytes'], c['bytes_ok'], c['bytes_equal']))
    lines.append('%-12s %9d %9d %9d %9d %9d %9d' % ('TOTAL', tot['fn'], tot['ok'], tot['equal'], tot['bytes'], tot['bytes_ok'], tot['bytes_equal']))
    lines.append('')
    lines.append('functions: %d; source: %d (%.1f%%); equal in the offline synthetic differential (in a tested DLL): %d (%.1f%%)' %
                 (tot['fn'], tot['ok'], 100.0 * tot['ok'] / max(tot['fn'], 1), tot['equal'], 100.0 * tot['equal'] / max(tot['fn'], 1)))
    lines.append('bytes: %d; source: %d (%.1f%%); equal: %d (%.1f%%)' %
                 (tot['bytes'], tot['bytes_ok'], 100.0 * tot['bytes_ok'] / max(tot['bytes'], 1), tot['bytes_equal'], 100.0 * tot['bytes_equal'] / max(tot['bytes'], 1)))
    lines.append('of the source functions, %d (%d bytes) are helpers called as plain functions: not bound, tested only inside their callers' % (tot['static'], tot['bytes_static']))
    lines.append('  of those, %d (%d bytes) have no source caller: readable text only, never run by any test' % (tot['unreached'], tot['bytes_unreached']))
    lines.append('refused, by reason (functions, bytes):')
    for k, v in sorted(reasons.items(), key=lambda kv: -rbytes[kv[0]]):
        lines.append('  %6d %9d  %s' % (v, rbytes[k], k))
    with open(os.path.join(OUT, 'status_table.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('\n'.join(lines[-(len(reasons) + 6):]))


def main():
    cmd = sys.argv[1]
    args = [a for a in sys.argv[2:] if not a.startswith('--')]
    os.makedirs(OUT, exist_ok=True)
    if cmd == 'list':
        return cmd_list(args[0])
    if cmd == 'show':
        return cmd_show(args[0], args[1] if len(args) > 1 else '.')
    if cmd == 'sdk':
        return cmd_sdk()
    if cmd == 'table':
        return cmd_table()
    mods = args if args and args != ['all'] else all_modules()
    if cmd == 'census':
        cmd_census(mods)
    elif cmd == 'collect':
        cmd_collect(mods)
    elif cmd == 'decompile':
        cmd_decompile(mods)
    elif cmd == 'stats':
        cmd_decompile(mods, write=False)
    elif cmd == 'build':
        cmd_build(mods)
    elif cmd == 'test':
        cmd_test(mods)
    elif cmd == 'cycle':
        cmd_cycle(mods)


if __name__ == '__main__':
    main()
