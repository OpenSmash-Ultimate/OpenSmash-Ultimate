"""Facts about the lua2cpp script modules: functions by kind, sizes, names, imports.

    python census.py mario            one module, detailed
    python census.py all              all 98 modules: totals (writes fighters/tools_out/census_all.json)

A function = one entry of the module's .eh_frame_hdr table (every function with unwind information); PLT stubs
are counted apart. Kinds are decided from the code alone:
  acmd thunk   registered with lib::L2CAgent::sv_set_function_hash(agent, fn, hash): the function main calls
  acmd body    the one local function such a thunk calls (the script itself)
  status thunk / status body   the same for lua2cpp::L2CAgentBase::sv_set_status_func(kind, line, fn)
  macro        local wrapper that only pushes its arguments on the Lua stack and calls one app::sv_* function
  agent        exported create_agent_* and the functions that register scripts
  ptr-taken    other functions whose address is taken (status main loops given to fastshift, vtable entries)
  tiny         <= 16 bytes (compiler generated: empty destructors, returns)
  other        everything else (helpers called directly)
"""
import collections
import json
import os
import sys

import numpy as np

import sdk_paths
from nrolib import Nro, nro_map, demangle_simple

SET_FN_HASH = '_ZN3lib8L2CAgent20sv_set_function_hashEPvN3phx6Hash40E'
SET_STATUS = '_ZN7lua2cpp12L2CAgentBase18sv_set_status_funcERKN3lib8L2CValueES4_Pv'
ACMD_KINDS = ('game', 'effect', 'sound', 'expression')

_labels = None


def script_labels():
    """hash40 -> script name, from the public label lists"""
    global _labels
    if _labels is None:
        import zlib
        _labels = {}
        for rel in (('motion_list', 'Labels.txt'), ('sqb', 'Labels.txt')):
            for line in open(sdk_paths.reference('param-labels', *rel), encoding='utf-8', errors='replace'):
                s = line.strip()
                if s:
                    _labels[zlib.crc32(s.encode()) | (len(s) << 32)] = s
        for line in open(sdk_paths.reference('param-labels', 'ParamLabels.csv'), encoding='utf-8', errors='replace'):
            h, _, s = line.strip().partition(',')
            if s:
                _labels.setdefault(int(h, 16), s)
    return _labels


def analyse(name):
    n = Nro(name)
    d = n.d
    words = np.frombuffer(d, dtype='<u4', count=n.text_size // 4, offset=n.text_off)
    pcs = np.arange(len(words), dtype=np.int64) * 4 + n.text_off
    plt = n.plt
    plt_lo = min(plt) if plt else n.text_size
    fdes = [(a, s) for a, s in n.fdes if a < plt_lo]
    starts = np.array([a for a, s in fdes], dtype=np.int64)

    def owner(pc):
        i = int(np.searchsorted(starts, pc, side='right')) - 1
        return i if i >= 0 and pc < fdes[i][0] + fdes[i][1] else -1

    # direct calls
    is_bl = (words & 0xfc000000) == 0x94000000
    imm = (words & 0x3ffffff).astype(np.int64)
    imm = np.where(imm & (1 << 25), imm - (1 << 26), imm)
    targets = pcs + imm * 4
    calls = collections.defaultdict(list)        # function index -> [(pc, target)]
    callers = collections.defaultdict(set)
    for i in np.nonzero(is_bl)[0]:
        f = owner(int(pcs[i]))
        if f >= 0:
            t = int(targets[i])
            calls[f].append((int(pcs[i]), t))
            callers[t].add(f)
    # tail calls `b <function start>` count as calls too
    is_b = (words & 0xfc000000) == 0x14000000
    start_set = set(a for a, s in fdes)
    for i in np.nonzero(is_b)[0]:
        t = int(targets[i])
        f = owner(int(pcs[i]))
        if f >= 0 and (t in plt or (t in start_set and t != fdes[f][0] and not (fdes[f][0] <= t < fdes[f][0] + fdes[f][1]))):
            calls[f].append((int(pcs[i]), t))
            callers[t].add(f)

    # addresses of functions formed in code (adrp + add) and stored in data (relocations)
    is_adrp = (words & 0x9f000000) == 0x90000000
    aimm = (((words >> 5) & 0x7ffff).astype(np.int64) << 2) | ((words >> 29) & 3)
    aimm = np.where(aimm & (1 << 20), aimm - (1 << 21), aimm)
    page = (pcs & ~0xfff) + (aimm << 12)
    code_ptr = {}                                 # pc of the add -> function address
    for i in np.nonzero(is_adrp)[0]:
        if i + 1 >= len(words):
            continue
        w = int(words[i + 1])
        if (w & 0xffc00000) == 0x91000000 and ((w >> 5) & 31) == (int(words[i]) & 31):
            a = int(page[i]) + ((w >> 10) & 0xfff)
            if a in start_set:
                code_ptr[int(pcs[i + 1])] = (a, w & 31)
    data_ptr = set(v for k, v in n.rel_ptr.items() if v in start_set)

    kind = {}
    names = {}
    labels = script_labels()
    reg_fns = set()
    # registrations: the pointer formed last before the call, in the right register
    for f, cl in calls.items():
        for pc, t in cl:
            imp = plt.get(t)
            if imp not in (SET_FN_HASH, SET_STATUS):
                continue
            reg_fns.add(f)
            want = 1 if imp == SET_FN_HASH else 3
            best = None
            for p in range(pc - 4, max(pc - 64, fdes[f][0]) - 4, -4):
                if p in code_ptr and code_ptr[p][1] == want:
                    best = code_ptr[p][0]
                    break
            if best is None:
                continue
            if imp == SET_FN_HASH:
                # hash: movz/movk x2 in the 12 instructions before the call
                h = 0
                for p in range(max(pc - 48, fdes[f][0]), pc, 4):
                    w = int(words[(p - n.text_off) // 4])
                    if (w & 0xff80001f) == 0xd2800002:
                        h = ((w >> 5) & 0xffff) << (((w >> 21) & 3) * 16)
                    elif (w & 0xff80001f) == 0xf2800002:
                        sh = ((w >> 21) & 3) * 16
                        h = (h & ~(0xffff << sh)) | (((w >> 5) & 0xffff) << sh)
                label = labels.get(h)
                names.setdefault(best, label or 'hash_%010x' % h)
                k = next((a for a in ACMD_KINDS if label and label.startswith(a + '_')), None)
                if k is None:
                    exp = n.exports.get(fdes[f][0], '')
                    k = next((a for a in ACMD_KINDS if 'animcmd_' + a in exp), 'unknown')
                kind.setdefault(best, 'acmd thunk ' + k)
            else:
                kind.setdefault(best, 'status thunk')
    idx = {a: i for i, (a, s) in enumerate(fdes)}
    for a, k in list(kind.items()):
        local = [t for pc, t in calls.get(idx[a], []) if t in start_set]
        if len(set(local)) == 1 and local[0] not in kind:
            kind[local[0]] = k.replace('thunk', 'body')
            if a in names:
                names[local[0]] = names[a]
    for i in reg_fns:
        kind.setdefault(fdes[i][0], 'agent')
    for a, nm in n.exports.items():
        if a in start_set:
            kind.setdefault(a, 'agent')
            names.setdefault(a, demangle_simple(nm))
    # macros: no local callees, calls exactly one app::sv_* import besides the Lua stack / L2CValue plumbing
    macro_of = {}
    for i, (a, s) in enumerate(fdes):
        if a in kind:
            continue
        cl = calls.get(i, [])
        if not cl or any(t in start_set for pc, t in cl):
            continue
        imps = [plt.get(t, '?') for pc, t in cl]
        core = [x for x in imps if not (x.startswith('_ZN3lib8L2C') or x.startswith('_ZNK3lib8L2C') or x == '_Unwind_Resume')]
        if len(set(core)) == 1 and core[0].startswith('_ZN3app') and any('push_lua_stack' in x for x in imps):
            kind[a] = 'macro'
            names[a] = demangle_simple(core[0])
            macro_of[a] = (core[0], sum(1 for x in imps if 'push_lua_stack' in x))
    taken = set(v for v, r in code_ptr.values()) | data_ptr
    for a, s in fdes:
        if a in kind:
            continue
        if s <= 16:
            kind[a] = 'tiny'
        elif a in taken:
            kind[a] = 'ptr-taken'
        else:
            kind[a] = 'other'
    return {'nro': n, 'fdes': fdes, 'kind': kind, 'names': names, 'calls': calls, 'plt': plt, 'macro_of': macro_of,
            'idx': idx, 'start_set': start_set, 'code_ptr': code_ptr}


def summarise(r):
    by = collections.defaultdict(lambda: [0, 0])
    for a, s in r['fdes']:
        k = r['kind'][a]
        by[k][0] += 1
        by[k][1] += s
    return {k: tuple(v) for k, v in by.items()}


def size_hist(r, pred):
    edges = [64, 256, 1024, 4096, 16384, 1 << 30]
    out = [0] * len(edges)
    for a, s in r['fdes']:
        if pred(r['kind'][a]):
            out[next(i for i, e in enumerate(edges) if s <= e)] += 1
    return out


def main():
    what = sys.argv[1]
    mods = [e['name'] for e in nro_map()] if what == 'all' else [what]
    total = collections.defaultdict(lambda: [0, 0])
    per = {}
    imports = collections.Counter()
    hist_acmd = [0] * 6
    hist_status = [0] * 6
    hist_other = [0] * 6
    named = unnamed = 0
    exported = 0
    for m in mods:
        r = analyse(m)
        s = summarise(r)
        per[m] = {k: list(v) for k, v in s.items()}
        per[m]['_plt'] = len(r['plt'])
        per[m]['_exports'] = sum(1 for a in r['nro'].exports if a in r['start_set'])
        exported += per[m]['_exports']
        for k, v in s.items():
            total[k][0] += v[0]
            total[k][1] += v[1]
        for nm in set(r['plt'].values()):
            imports[nm] += 1
        for h, p in ((hist_acmd, lambda k: k.startswith('acmd body')), (hist_status, lambda k: k in ('status body', 'ptr-taken')),
                     (hist_other, lambda k: k == 'other')):
            for i, v in enumerate(size_hist(r, p)):
                h[i] += v
        for a, k in r['kind'].items():
            if k.startswith('acmd thunk'):
                if r['names'].get(a, 'hash_').startswith('hash_'):
                    unnamed += 1
                else:
                    named += 1
        if what != 'all':
            print('%s: %d functions with unwind info, %d bytes; %d PLT stubs; %d exported functions' %
                  (m, len(r['fdes']), sum(x[1] for x in r['fdes']), len(r['plt']), per[m]['_exports']))
    print('%-22s %8s %11s %7s' % ('kind', 'count', 'bytes', 'avg'))
    for k in sorted(total, key=lambda k: -total[k][1]):
        c, b = total[k]
        print('%-22s %8d %11d %7d' % (k, c, b, b // max(c, 1)))
    print('%-22s %8d %11d' % ('TOTAL', sum(v[0] for v in total.values()), sum(v[1] for v in total.values())))
    print('exported functions: %d;  ACMD scripts whose name is in the public label lists: %d of %d' % (exported, named, named + unnamed))
    print('size classes (<=64, <=256, <=1K, <=4K, <=16K, more):')
    print('  acmd bodies          ', hist_acmd)
    print('  status body+ptr-taken', hist_status)
    print('  other                ', hist_other)
    groups = collections.Counter()
    for nm, c in imports.items():
        d = demangle_simple(nm)
        g = '::'.join(d.split('::')[:2]) if d.startswith(('app::', 'lib::', 'lua2cpp::')) else 'other (C++ runtime, nu::, operator new...)'
        if d.startswith('app::lua_bind::'):
            g = 'app::lua_bind (module calls)'
        groups[g] += 1
    print('distinct imported functions by namespace (%d distinct in all):' % len(imports))
    for g, c in groups.most_common(14):
        print('  %-44s %d' % (g, c))
    if what == 'all':
        os.makedirs(os.path.join(sdk_paths.WORK, 'fighters', 'tools_out'), exist_ok=True)
        with open(os.path.join(sdk_paths.WORK, 'fighters', 'tools_out', 'census_all.json'), 'w') as f:      # S3: game-derived output, not next to the tools
            json.dump({'per_module': per, 'total': {k: list(v) for k, v in total.items()}}, f, indent=1)


if __name__ == '__main__':
    main()
