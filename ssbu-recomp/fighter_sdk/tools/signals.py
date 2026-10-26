"""Static alarm signals for the state-machine source (no game run, no model).

For every function of fighters/<fighter>/status/*.cpp it compares the SOURCE text with the function's machine
code in the game's script module and prints what does not fit:

  RED     the binary calls a named game function the source never names (a dropped call)
  RED     the source names a module function the binary never calls (an invented call)
  RED     source function not found in the module (wrong name)
  YELLOW  the number of call sites of a named function differs (source vs binary)
  YELLOW  large machine code, tiny source body
  YELLOW  floating-point arithmetic in the binary, no float in the source

It proves nothing by itself (a clean function can still be wrong): it is a cheap filter that runs before the
differential and the health probe. Names compared: the `lua_bind` module functions (`XxxModule::name`) and the
sibling functions of the same class (`L2CFighterCommon::name`).

    python fighter_sdk/tools/signals.py [fighter=common] [--all] [--csv out.csv]
"""
import collections
import csv
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nrolib  # noqa: E402
from capstone.arm64_const import ARM64_OP_IMM  # noqa: E402

STUDY = nrolib.STUDY
HEAD = re.compile(r'^(?:L2C_(?:VOID_)?FUNCTION\d?\(\s*(\w+)\s*,\s*|L2CValue (local)::)(\w+)')    # S8: also a fighter's plain helper functions
QUAL = re.compile(r'\b([A-Z]\w+)::(\w+)\s*\(')
FLOATLIT = re.compile(r'\b\d+\.\d*f?\b|\bfloat\b|_FLOAT_|get_float|set_float')
FP_ARITH = {'fadd', 'fsub', 'fmul', 'fdiv', 'fmadd', 'fmsub', 'fnmul', 'fsqrt', 'fabs', 'fneg', 'fcmp', 'fcmpe',
            'fmax', 'fmin', 'fmaxnm', 'fminnm', 'scvtf', 'ucvtf', 'fcvtzs', 'fcvtzu'}


def components(mangled):
    """nested-name components of an Itanium `_ZN...E` / `_ZNK...E` name (no templates needed here)"""
    m = re.match(r'_ZNK?', mangled)
    if not m:
        return []
    i, out = m.end(), []
    while i < len(mangled) and mangled[i].isdigit():
        j = i
        while mangled[j].isdigit():
            j += 1
        n = int(mangled[i:j])
        out.append(mangled[j:j + n])
        i = j + n
    return out


def callee_key(sym):
    """('M', 'WorkModule::set_float') for lua_bind functions, ('S', name) for class siblings, else None"""
    c = components(sym)
    if len(c) >= 3 and c[0] == 'app' and c[1] == 'lua_bind' and '__' in c[2]:
        mod, fn = c[2].split('__', 1)
        if fn.endswith('_impl'):
            fn = fn[:-5]
        return ('M', mod + '::' + fn)
    if len(c) >= 3 and c[0] == 'lua2cpp' and c[1].startswith('L2C'):     # S8: any script class (L2CFighterCommon, L2CWeaponCommon, L2CAgentBase ...)
        return ('S', c[2])
    return None


def local_functions(fighter):
    """S8: a fighter's own status functions have no exported names; fighter_status.py names them and writes
    name -> offset to fighters/<fighter>/generated/status_local.h (FS_OFF_<name> = 0x...)"""
    path = os.path.join(STUDY, 'fighters', fighter, 'generated', 'status_local.h')
    if not os.path.exists(path):
        return {}
    with open(path, encoding='utf-8', errors='replace') as f:
        return {m.group(1): int(m.group(2), 16) for m in re.finditer(r'FS_OFF_(\w+) = 0x([0-9a-f]+)', f.read())}


def source_functions(fighter):
    out = {}
    for path in sorted(glob.glob(os.path.join(STUDY, 'fighters', fighter, 'status', '*.cpp'))):
        name, body = None, []
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = HEAD.match(line)
                if m:
                    name, body = m.group(3), []
                elif name is not None:
                    if line.startswith('}'):
                        out[name] = (os.path.basename(path), ''.join(body))
                        name = None
                    else:
                        body.append(line)
    return out


def binary_functions(nro):
    """name -> (offset, size) for the class's exported functions"""
    out = {}
    for off, sym in nro.exports.items():
        c = components(sym)
        if len(c) >= 3 and c[0] == 'lua2cpp' and c[1].startswith('L2C'):
            size = nro.func_size(off)
            if size:
                out.setdefault(c[2], []).append((off, size))
    return out


class LocalName(str):
    """marks a local function's name in the offset -> symbol map (callee_key: a sibling)"""


def binary_calls(nro, off, size, by_off):
    calls, fp = collections.Counter(), 0
    for ins in nro.disasm(off, size):
        if ins.mnemonic in ('bl', 'b') and ins.operands and ins.operands[0].type == ARM64_OP_IMM:
            t = ins.operands[0].imm
            if off <= t < off + size:
                continue
            sym = nro.plt.get(t) or by_off.get(t)
            key = ('S', str(sym)) if isinstance(sym, LocalName) else callee_key(sym) if sym else None
            if key:
                calls[key] += 1
        elif ins.mnemonic in FP_ARITH:
            fp += 1
    return calls, fp


def check(fighter):
    nro = nrolib.Nro('lua2cpp_' + fighter)
    by_off = dict(nro.exports)
    binf = binary_functions(nro)
    for nm, off in local_functions(fighter).items():     # S8: per-fighter modules
        if nro.func_size(off):
            binf.setdefault(nm, []).append((off, nro.func_size(off)))
            by_off.setdefault(off, LocalName(nm))
    # every lua_bind class the module imports (WorkModule, FighterMotionModuleImpl, Article, ...): the names compared
    classes = {k[1].split('::')[0] for k in (callee_key(s) for s in nro.plt.values()) if k and k[0] == 'M'}
    rows = []
    for name, (path, body) in sorted(source_functions(fighter).items()):
        if name not in binf:
            rows.append((name, path, 'RED', 'not found in the module', 0, len(body.splitlines())))
            continue
        cands = binf[name]
        off, size = cands[0]
        lines = len([ln for ln in body.splitlines() if ln.strip()])
        if len(cands) > 1:
            rows.append((name, path, 'INFO', '%d overloads in the module: compared with the first' % len(cands), size, lines))
        calls, fp = binary_calls(nro, off, size, by_off)
        src_mod = collections.Counter(m + '::' + f for m, f in QUAL.findall(body) if m in classes)
        bin_mod = collections.Counter({k[1]: v for k, v in calls.items() if k[0] == 'M'})
        for fn in sorted(set(bin_mod) - set(src_mod)):
            rows.append((name, path, 'RED', 'binary calls %s (%d), source never does' % (fn, bin_mod[fn]), size, lines))
        for fn in sorted(set(src_mod) - set(bin_mod)):
            rows.append((name, path, 'RED', 'source calls %s (%d), binary never does' % (fn, src_mod[fn]), size, lines))
        for fn in sorted(set(src_mod) & set(bin_mod)):
            if src_mod[fn] != bin_mod[fn]:
                rows.append((name, path, 'YELLOW', '%s: %d call sites in source, %d in binary' % (fn, src_mod[fn], bin_mod[fn]), size, lines))
        for (kind, sib), n in sorted(calls.items()):
            if kind == 'S' and sib != name and not re.search(r'\b%s\b' % re.escape(sib), body):
                rows.append((name, path, 'RED', 'binary calls sibling %s (%d), source never names it' % (sib, n), size, lines))
        if size >= 600 and size > 400 * max(lines, 1):
            rows.append((name, path, 'YELLOW', 'large machine code (%d B), %d source lines' % (size, lines), size, lines))
        if fp and not FLOATLIT.search(body):
            rows.append((name, path, 'YELLOW', '%d floating-point instructions, no float in the source' % fp, size, lines))
    return rows, len(source_functions(fighter))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    fighter = args[0] if args else 'common'
    rows, total = check(fighter)
    if '--csv' in sys.argv:
        with open(sys.argv[sys.argv.index('--csv') + 1], 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f)
            w.writerow(['function', 'file', 'level', 'signal', 'binary_bytes', 'source_lines'])
            w.writerows(rows)
    level = collections.Counter(r[2] for r in rows)
    flagged = {lv: len({r[0] for r in rows if r[2] == lv}) for lv in ('RED', 'YELLOW')}
    show = rows if '--all' in sys.argv else [r for r in rows if r[2] == 'RED']
    for name, path, lv, msg, size, lines in show:
        print('%-6s %-58s %s  [%s]' % (lv, name, msg, path))
    clean = total - len({r[0] for r in rows if r[2] in ('RED', 'YELLOW')})
    print('\n%s: %d source functions; RED %d signals in %d functions, YELLOW %d signals in %d functions, clean %d'
          % (fighter, total, level['RED'], flagged['RED'], level['YELLOW'], flagged['YELLOW'], clean))


if __name__ == '__main__':
    main()
