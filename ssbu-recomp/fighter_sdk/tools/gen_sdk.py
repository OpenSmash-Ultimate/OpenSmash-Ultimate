"""Generate the SDK headers from what the decompiled scripts use.

    python gen_sdk.py            reads fighters/tools_out/*_acmd.json (acmd_decomp.py) and writes, in fighters/sdk_generated/:
        lua_const.h   the named Lua constants (index in const_value_table__)
        lua_bind.h    symbol slots + one inline wrapper per game function the scripts call
        fs_syms.inc   name + argument description of every slot (for binding and the differential test)

Parameter types come from the mangled names; return types from reference/skyline-smash/src/cpp.rs (public
bindings) where it lists the function, otherwise uint64_t (x0).
Argument description, one letter per parameter in order: T this, p pointer, q 64-bit, i 32-bit, b bool, H hash40,
V pointer to an L2CValue (compared by content), f float; a leading S = result is an L2CValue written through x8.
"""
import glob
import json
import os
import re

import luaconst
import mangle
from acmd_decomp import wrapper_name, singleton_name
import sdk_paths
from nrolib import Nro, STUDY

INC = SRC = os.path.join(STUDY, 'fighters', 'sdk_generated')      # S3: generated from the game -> with the generated sources, not with the SDK
# S8 (options, the default is unchanged): SSBU_SDK_DIR=<folder under fighters\> writes the headers there instead (work
# in progress keeps its own SDK; build_fighter.py reads it with SSBU_FIGHTER_SDK); SSBU_SDK_EXTRA=<json> adds what
# that file lists (imports_used, float_returns, void_calls, lua_calls) to what the tools_out files give.
if os.environ.get('SSBU_SDK_DIR'):
    INC = SRC = os.path.join(STUDY, 'fighters', os.environ['SSBU_SDK_DIR'])

FIXED = [  # slot name, mangled name (or unique substring to look up in the common import list), spec override
    ('S_clear_lua_stack', '_ZN3lib8L2CAgent15clear_lua_stackEv', None),
    ('S_push_lua_stack', '_ZN3lib8L2CAgent14push_lua_stackERKNS_8L2CValueE', None),
    ('S_pop_lua_stack', '_ZN3lib8L2CAgent13pop_lua_stackEi', 'STi'),
    ('S_get_format', 'Variadic10get_format', 'p'),
    ('S_push_variadic', 'L2CValue13push_variadic', 'Vqpp'),
    ('S_const_value_table', '_ZN7lua2cpp21L2CAgentGeneratedBase19const_value_table__E', ''),
    ('S_lv_bool', '_ZNK3lib8L2CValuecvbEv', ''),
    ('S_lv_as_integer', '_ZNK3lib8L2CValue10as_integerEv', ''),
    ('S_lv_as_number', '_ZNK3lib8L2CValue9as_numberEv', ''),
    ('S_lv_as_bool', '_ZNK3lib8L2CValue7as_boolEv', ''),
    ('S_lv_as_hash', '_ZNK3lib8L2CValue7as_hashEv', ''),
    ('S_lv_eq', '_ZNK3lib8L2CValueeqERKS0_', ''),
    ('S_lv_lt', '_ZNK3lib8L2CValueltERKS0_', ''),
    ('S_lv_le', '_ZNK3lib8L2CValueleERKS0_', ''),
    ('S_lv_add', '_ZNK3lib8L2CValueplERKS0_', ''),
    ('S_lv_sub', '_ZNK3lib8L2CValuemiERKS0_', ''),
    ('S_lv_mul', '_ZNK3lib8L2CValuemlERKS0_', ''),
    ('S_lv_div', '_ZNK3lib8L2CValuedvERKS0_', ''),
    ('S_lv_mod', '_ZNK3lib8L2CValuermERKS0_', ''),
    ('S_lv_neg', '_ZNK3lib8L2CValuengEv', ''),
    ('S_lv_not', '_ZNK3lib8L2CValuentEv', ''),
    ('S_lv_and', '_ZNK3lib8L2CValueanERKS0_', ''),
    ('S_lv_or', '_ZNK3lib8L2CValueorERKS0_', ''),
    ('S_lv_shl', '_ZNK3lib8L2CValuelsERKS0_', ''),
    ('S_lv_shr', '_ZNK3lib8L2CValuersERKS0_', ''),
    ('S_lv_ix_int', '_ZNK3lib8L2CValueixEi', ''),
    ('S_lv_ix_hash', '_ZNK3lib8L2CValueixEN3phx6Hash40E', ''),
    ('S_lv_ix_value', '_ZNK3lib8L2CValueixERKS0_', ''),
    ('S_lv_assign', '_ZN3lib8L2CValueaSERKS0_', ''),
]
# S21: lib::L2CValue's own functions a source may use (string values, length): a slot only when used, and a define
# FS_HAVE_<slot> that fighter_sdk.h tests (L2CString, fs_lv_length)
STR_SLOTS = {'_ZN3lib8L2CValueC1EPKc': 'S_lv_ctor_string', '_ZN3lib8L2CValueD1Ev': 'S_lv_dtor', '_ZNK3lib8L2CValue6lengthEv': 'S_lv_length'}
OPTIONAL = set(e[0] for e in FIXED[6:])       # lib::L2CValue's own functions: a module that never uses one does not import it

RUST_RET = {'bool': 'bool', 'f32': 'float', 'u64': 'uint64_t', 'usize': 'uint64_t', 'i32': 'int32_t', 'libc::c_int': 'int32_t',
            'u32': 'uint32_t', 'libc::c_uint': 'uint32_t', 'i64': 'int64_t', 'libc::c_long': 'int64_t', 'libc::c_ulong': 'uint64_t',
            'u8': 'uint32_t', 'libc::c_uchar': 'uint32_t'}


def rust_returns():
    with open(sdk_paths.reference('skyline-smash', 'src', 'cpp.rs'), encoding='utf-8') as f:
        text = f.read()
    out = {}
    for m in re.finditer(r'link_name = "\\u\{1\}(\w+)"\]\s*pub fn \w+\s*\(([^;]*?)\)\s*(?:->\s*([^;]+?))?\s*;', text, re.S):
        out[m.group(1)] = (m.group(3) or '').strip()
    return out


def cpp_ret(rust):
    if rust is None:
        return 'uint64_t'
    if rust == '':
        return 'void'
    if rust in RUST_RET:
        return RUST_RET[rust]
    return 'uint64_t'


def param(t, i):
    """(C++ parameter type, expression passed on, spec letter)"""
    st = mangle.strip(t)
    a = 'a%d' % i
    if st == 'f':
        return 'float', a, 'f'
    if st in ('i', 's', 'c', 'a'):
        return 'int32_t', a, 'i'
    if st in ('j', 't', 'h'):
        return 'uint32_t', a, 'i'
    if st == 'b':
        return 'bool', a, 'b'
    if st == 'l' or st == 'x':
        return 'int64_t', a, 'q'
    if st == 'm' or st == 'y':
        return 'uint64_t', a, 'q'
    if st == ('class', 'phx::Hash40'):
        return 'Hash40', a, 'H'
    if st == ('class', 'lib::L2CValue'):
        return 'const L2CValue &', a, 'V'
    if isinstance(st, tuple) and st[0] == 'class':
        return 'int32_t', a, 'i'                                   # enum class
    if isinstance(st, tuple) and st[0] in ('ptr', 'ref'):
        inner = mangle.strip(st[1])
        if inner == ('class', 'lib::L2CValue'):
            return 'const L2CValue &', a, 'V'
        if inner == ('class', 'lua_State'):
            return 'L2CAgent *', a + '->lua_state', 'p'
        if inner == ('class', 'app::BattleObjectModuleAccessor'):
            return 'BattleObjectModuleAccessor *', a, 'p'
        if isinstance(inner, tuple) and inner[0] == 'class' and inner[1] in ('phx::Vector2f', 'phx::Vector3f', 'phx::Vector4f'):
            # S10: a vector by address: the test compares its CONTENT (letters 2 3 4); besides the wrapper that takes
            # a pointer (one the game returned) a second one takes the object built in the source (4th element)
            return 'void *', a, inner[1][11], 'const %s &' % inner[1][5:]
        return 'void *', a, 'p'
    raise ValueError(str(t))


def in_namespaces(name, body):
    parts = name.split('::')
    return ''.join('namespace %s { ' % p for p in parts[:-1]) + body + ' }' * (len(parts) - 1)


def main():
    used = set()
    lua_calls = {}
    floats, voids = set(), set()       # found by the decompiler: result read from the float register / called without a result buffer
    values = set()                     # S10: called WITH a result somewhere: never declared void (see below)
    for path in glob.glob(os.path.join(STUDY, 'fighters', 'tools_out', '*_acmd.json')) + glob.glob(os.path.join(STUDY, 'fighters', 'tools_out', '*_status.json')):
        j = json.load(open(path))
        used |= set(j['imports_used'])
        floats |= set(j.get('float_returns', []))
        voids |= set(j.get('void_calls', []))
        values |= set(j.get('value_calls', []))
    with open(os.path.join(STUDY, 'fighters', 'tools_out', 'lua_calls_global.json')) as f:       # batch.py: how each script call is used over all modules
        g = json.load(f)
    sp = os.path.join(STUDY, 'fighters', 'tools_out', 'status_lua_calls.json')                  # S3: script calls only the state machine makes (status_decomp.py)
    if os.environ.get('SSBU_SDK_STATUS_CALLS'):                                                 # S10: the copy of a work run (status_decomp.py SSBU_FIGHTERS_ROOT)
        sp = os.environ['SSBU_SDK_STATUS_CALLS']
    if os.path.exists(sp):
        for s, e in json.load(open(sp)).items():
            g.setdefault(s, e)
    for extra in [p for p in (os.environ.get('SSBU_SDK_EXTRA') or '').split(';') if p]:         # S8; S10: several files, ';' between them
        j = json.load(open(extra))
        used |= set(j.get('imports_used', []))
        floats |= set(j.get('float_returns', []))
        voids |= set(j.get('void_calls', []))
        values |= set(j.get('value_calls', []))          # called without a result in one module, with one in another: the wrapper returns it
        for s, e in j.get('lua_calls', {}).items():
            g.setdefault(s, e)
    # S10: over ALL inputs (it was only applied inside one extra file: a function lua2cpp_common calls with a result
    # and a fighter's status script without one became void, and common no longer compiled)
    voids -= values
    lua_calls ={s: {'proto': g[s]['proto'], 'autopop': g[s]['autopop']} for s in used if s in g}
    common_imports = set(Nro('mario').plt.values()) | set(Nro('mario').got.values())
    rets = rust_returns()
    slots = []          # (enum name, mangled, spec)
    for enum, key, spec in FIXED:
        full = key if key.startswith('_Z') else next(n for n in sorted(common_imports) if key in n)
        if spec is None:
            path, params, const = mangle.parse(full)
            spec = ('T' if mangle.is_member(path) else '') + ''.join(param(t, i)[2] for i, t in enumerate(params))
        slots.append((enum, full, spec))
    wrappers = []
    seen_names = {}
    for sym in sorted(used):
        if singleton_name(sym):                                     # S8: a data symbol: the slot only (read with singleton(slot))
            slots.append(('S_singleton_' + singleton_name(sym), sym, ''))
            continue
        if mangle.gvalue_name(sym):                                 # S20: a constant L2CValue of main (math_pi): the slot only
            slots.append(('S_gvalue_' + mangle.gvalue_name(sym), sym, ''))
            continue
        if sym in STR_SLOTS:                                        # S21: string values (fighter_sdk.h: L2CString): the slot only
            slots.append((STR_SLOTS[sym], sym, ''))
            continue
        path, params, const = mangle.parse(sym)
        name = wrapper_name(sym)
        enum = 'S_' + re.sub(r'\W', '_', name)
        if enum in seen_names:                                      # overloads: one slot per mangled name
            seen_names[enum] += 1
            enum = '%s_%d' % (enum, seen_names[enum])
        else:
            seen_names[enum] = 0
        member = mangle.is_member(path)
        ps = [param(t, i) for i, t in enumerate(params)]
        spec = ('T' if member else '') + ''.join(p[2] for p in ps)
        slots.append((enum, sym, spec))
        short = name.split('::')[-1]
        if sym in lua_calls and any('@' in p or ':' in p for p in lua_calls[sym]['proto'] or ()):
            continue                                                # S21: a vector of pushed values: called through lua_ret (status_local.h), the slot only
        if sym in lua_calls:
            lc = lua_calls[sym]
            if not lc['proto']:
                if sym in floats:                      # S8: returns its float directly (sv_kinetic_energy::get_speed_x): nothing is popped
                    body = ('template <class... A> inline float %s(L2CAgent *agent, A... v) { L2CValue args[sizeof...(v) + 1] = {L2CValue(v)...}; '
                            'fs_lua_clear(agent); for (int i = 0; i < (int)sizeof...(v); i++) fs_lua_push(agent, args[i]); '
                            'return gcall<float>(%s, agent->lua_state); }' % (short, enum))
                elif lc['autopop']:
                    body = ('template <class... A> inline void %s(L2CAgent *agent, A... v) { L2CValue args[sizeof...(v) + 1] = {L2CValue(v)...}; '
                            'fs_lua_call(agent, %s, args, (int)sizeof...(v)).pop(); }' % (short, enum))
                else:
                    body = ('template <class... A> inline LuaCall %s(L2CAgent *agent, A... v) { L2CValue args[sizeof...(v) + 1] = {L2CValue(v)...}; '
                            'return fs_lua_call(agent, %s, args, (int)sizeof...(v)); }' % (short, enum))
            else:
                n = len(lc['proto'])
                decl = ', '.join('L2CValue v%d' % i for i in range(n))
                pushes = ' '.join('fs_lua_push(agent, v%d);' % i for i in range(n))
                conv = ', '.join('v%d.%s()' % (i, c) for i, c in enumerate(lc['proto']))
                tail = 'fs_lua_pop(agent);' if lc['autopop'] else 'return LuaCall{agent};'
                if sym in floats:                      # returns its float directly, nothing is popped (get_value_float)
                    body = ('inline float %s(L2CAgent *agent, %s) { fs_lua_clear(agent); %s return gcall<float>(%s, agent->lua_state, %s); }'
                            % (short, decl, pushes, enum, conv))
                else:
                    body = ('inline %s %s(L2CAgent *agent, %s) { fs_lua_clear(agent); %s gcall<void>(%s, agent->lua_state, %s); %s }'
                            % ('void' if lc['autopop'] else 'LuaCall', short, decl, pushes, enum, conv, tail))
        else:
            # S3: a return type declared in cpp.rs wins; "read from the float register" is only an inference of the
            # decompiler (a stale read of v0 after WorkModule::is_flag once made every is_flag return float)
            ret = cpp_ret(rets.get(sym)) if sym in rets else 'float' if sym in floats else cpp_ret(None)
            decl = (['L2CAgent *self'] if member else []) + ['%s a%d' % (p[0], i) for i, p in enumerate(ps)]
            passed = (['self'] if member else []) + [p[1] for p in ps]
            if member and path[0] == 'lua2cpp' and sym in voids:
                body = 'inline void %s(%s) { gcall<void>(%s); }' % (short, ', '.join(decl), ', '.join([enum] + passed))
            elif (member and path[0] == 'lua2cpp') or mangle.is_static_value(path):      # the script classes' functions return an L2CValue (buffer in x8)
                slots[-1] = (enum, sym, 'S' + spec)
                body = 'inline L2CValue %s(%s) { return gcall_value(%s); }' % (short, ', '.join(decl), ', '.join([enum] + passed))
            else:
                body = 'inline %s %s(%s) { return gcall<%s>(%s); }' % (ret, short, ', '.join(decl), ret, ', '.join([enum] + passed))
                if any(len(p) > 3 for p in ps):          # S10: the same function taking its vectors as objects
                    decl2 = (['L2CAgent *self'] if member else []) + ['%s a%d' % (p[3] if len(p) > 3 else p[0], i) for i, p in enumerate(ps)]
                    wrappers.append(in_namespaces(name, 'inline %s %s(%s) { return gcall<%s>(%s); }' % (ret, short, ', '.join(decl2), ret, ', '.join([enum] + passed))))
        wrappers.append(in_namespaces(name, body))
    os.makedirs(SRC, exist_ok=True)
    with open(os.path.join(INC, 'lua_bind.h'), 'w', newline='\n') as f:
        f.write('// GENERATED by fighter_sdk/tools/gen_sdk.py - do not edit. One wrapper per game function the scripts call.\n#pragma once\n\n')
        f.write('enum FsSlot : int {\n' + ''.join('    %s,\n' % s[0] for s in slots) + '    S_COUNT\n};\n\n')
        have = [e for e in STR_SLOTS.values() if any(s[0] == e for s in slots)]
        if have:                                                    # S21: see STR_SLOTS
            f.write(''.join('#define FS_HAVE_%s 1\n' % e for e in have) + '\n')
        f.write('// S8: the instance of one of the game\'s manager objects (lib::Singleton<app::X>::instance_), slot S_singleton_X\n'
                'inline void *singleton(int slot) { return *(void **)(uintptr_t)fs_addr[slot]; }\n\n')
        f.write('\n'.join(wrappers) + '\n')
    with open(os.path.join(SRC, 'fs_syms.inc'), 'w', newline='\n') as f:
        f.write('// GENERATED by fighter_sdk/tools/gen_sdk.py - do not edit. Slot -> game symbol, argument description.\n')
        for enum, sym, spec in slots:
            f.write('    { "%s", "%s" },\n' % (sym, spec))
    names = luaconst.table_names()
    with open(os.path.join(INC, 'lua_const.h'), 'w', newline='\n') as f:
        f.write('// GENERATED by fighter_sdk/tools/gen_sdk.py - do not edit. The game\'s Lua constants by name\n'
                '// (value read at run time from lua2cpp::L2CAgentGeneratedBase::const_value_table__).\n#pragma once\n\n')
        f.write('inline LuaConst lua_const_at(int32_t index) { return LuaConst{index}; }\n')
        done = set()
        for i, (nm, v) in enumerate(names):
            if nm and nm not in done:
                done.add(nm)
                f.write('static const LuaConst %s{%d};\n' % (nm, i))
    print('lua_bind.h: %d slots, %d wrappers (%d with a return type from cpp.rs); lua_const.h: %d constants'
          % (len(slots), len(wrappers), sum(1 for s in used if s in rets), len(done)))


if __name__ == '__main__':
    main()
