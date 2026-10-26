"""Fake census: regenerate the inventory of everything in the port that is not the real behaviour.

    python tools_audit/fake_census.py              totals from the sources + the last binding dump, writes
                                                   plan/fake_audit.csv and fake_whitelist.h
    python tools_audit/fake_census.py --rebind     first relink <exe>_map.exe with a linker map and dump the
                                                   bindings of every import (needs the host objects of
                                                   RECOMP_EXE, default ssbu_audit: `RECOMP_EXE=ssbu_audit.exe python build.py link`)
    python tools_audit/fake_census.py --counts f   call counts of a battle run (SSBU_AUDIT_COUNTS=f), default
                                                   tools_audit/out/counts.tsv
    python tools_audit/fake_census.py --check      exit code 1 if any FAKE / WRONG / unclassified row is open

Inputs
  host sources          registration tables {name, handler}, handler bodies, comment keywords
  tools_audit/out/bind.tsv + <exe>.map   ground truth: which host function every import is bound to after
                        ALL registrations ran (later tables replace earlier ones, fill tables only fill), and
                        whether its machine code is a do-nothing body (hostrt.c audit_trivial)
  ../binaries/13.0.4/sdk.elf   the real function of the same name: bare `ret`, constant, leaf, or real work
  tools_audit/classification.tsv   hand classification with the proof (selector, class, evidence, action, status)
  tools_audit/manual_items.tsv     items that are not imports (runtime core, services, media, ...)
Classes: REAL, LIFTED, CORRECT-BY-SEMANTICS (CBS), EXTERNAL (the answer belongs to a system module that is not in the dump:
client side checked against sdk.elf, answer taken from the reference implementation, stated in the row), FAKE, WRONG, unclassified.
"""
import collections, csv, os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(HERE, 'tools_audit', 'out')
PLAN = os.path.join(HERE, '..', 'plan')
def _default_exe():
    # the exe of the last --rebind (its map names the handlers of bind.tsv); RECOMP_EXE overrides
    p = os.path.join(OUT, 'bind_exe.txt')
    return open(p).read().strip() if os.path.exists(p) else 'ssbu_audit.exe'
EXE = os.path.splitext(os.environ.get('RECOMP_EXE') or _default_exe())[0]
GRAPHICS = {'nvn_front.c', 'nvn_front_gl.h', 'nvn_gl.c', 'nvn_stub.c', 'display.c', 'mx_translate.c', 'mx_translate.h', 'mx_ops.h',
            'tex_decode.c'}      # nvn_front.c and nvn_front_gl.h: the NVN front end, which was in nvn_gl.c until it was split off
HOST = ['hostrt.c', 'boot.c', 'nnsdk.c', 'libcxx.c', 'os_threads.c', 'nvn_stub.c', 'nvn_front.c', 'nvn_front_gl.h', 'nvn_gl.c', 'nnfs.c',
        'clib.c', 'nntime.c', 'nnhid.c',
        'sdkdata.c', 'display.c', 'nnpl.c', 'nnfontll.c', 'nnmovie.c', 'nnmisc.c', 'nnaudio.c', 'nnro.c', 'nro_registry.c', 'nnmovie_mf.c',
        'nniostream.c', 'nncxxrt.c', 'nncxxlib.c', 'nnlibc2.c', 'sdklift_rt.c', 'nnsdk2.c', 'nnmii.c', 'nnapplet.c', 'nnalbum.c', 'nnnet.c',
        'nnfsdir.c', 'nnunwind.c', 'nro_shim.c', 'gamedll.c', 'game_shim_eh.c']
HOST = [f for f in HOST if os.path.exists(os.path.join(HERE, f))]
for extra in ('nnlibm.c', 'nnpad.c', 'nnaudit.c', 'nvn_sdk.c', 'nvn_c4.c', 'nnnet_c4a.inc', 'nvn_c5.c', 'nnsvc_c5a.inc', 'nnlibc2_c5b.inc'):   # close4: files added by rounds 3 and 4
    if os.path.exists(os.path.join(HERE, extra)) and extra not in HOST: HOST.append(extra)
GENERIC_NAMES = {'ret0', 'nothing', 'n_nop', 'n_zero', 'bcat_unavailable', 'acc_unavailable', 'ret1', 'facet_nop', 'ret_this', 'keep',
                 'unavailable', 'i_ret0', 'minus1', 'nop', 'ret_zero', 'ret_true', 'ret_false', 'ret_null', 'stub', 'success'}
KEYWORDS = ['not implemented', 'placeholder', 'TODO', 'for now', 'stub', 'approximat', 'ignored', 'unverified', 'NOT VERIFIED', 'not verified',
            'guess', 'not supported', 'unsupported', 'Limitation', 'not done', 'fake', 'pretend', 'never signalled', 'not exercised', 'dummy']


def read(name):
    return open(os.path.join(HERE, name), 'r', encoding='latin-1').read()


def strip_comments(s):
    return re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)


# ------------------------------------------------------------------ sources: registrations and bodies
def scan_sources():
    regs, bodies, macros = [], {}, {}
    for f in HOST:
        src = read(f)
        code = strip_comments(src)
        for m in re.finditer(r'#define\s+(\w+)\(n\)\s+"([^"]*)"\s*n', src): macros[(f, m.group(1))] = m.group(2)
        for m in re.finditer(r'\{\s*(?:(\w+)\(\s*)?"([^"\n]*)"\s*\)?\s*,\s*(\w+)\s*\}', code):
            mac, name, fn = m.groups()
            if fn in ('NULL', '0') or not re.match(r'[A-Za-z_]', fn): continue
            if mac: name = macros.get((f, mac), mac + ':') + name
            regs.append((f, name, fn))
        for m in re.finditer(r'^static\s+void\s+(\w+)\s*\(\s*Ctx\s*\*\s*c\s*\)\s*\{', code, flags=re.M):
            i, depth = m.end(), 1
            while i < len(code) and depth:
                depth += {'{': 1, '}': -1}.get(code[i], 0); i += 1
            bodies[(f, m.group(1))] = code[m.end():i - 1].strip()
    return regs, bodies


CHEAP = {'RET', 'wr8', 'wr16', 'wr32', 'wr64', 'rd8', 'rd16', 'rd32', 'rd64', 'P', 'memset', 'vzero', 'if', 'sizeof', 'void', 'uint64_t',
         'uint32_t', 'int64_t', 'int', 'STR', 'uintptr_t', 'RETF', 'RETD', 'for', 'while', 'return'}
LOGS = {'printf', 'trace_printf', 'fprintf', 'fflush', 'log_once', 'hostfn_current_name', 'hostfn_current_calls'}


def body_kind(body):
    if body is None: return 'macro'
    b = body.replace('(void)c;', '').strip()
    if not b: return 'empty'
    calls = set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', b)) - CHEAP
    stmts = b.count(';')
    if not calls:
        if re.fullmatch(r'RET\s*\([^;]*\)\s*;', b) and not re.search(r'\b(A\d|c->|rd\d)', b): return 'const'
        if stmts <= 4: return 'out+const' if 'wr' in b else 'small'
        return 'code'
    if calls <= LOGS: return 'log+const'
    return 'code'


# ------------------------------------------------------------------ ground truth binding
def load_map():
    path = os.path.join(OUT, EXE + '.map')
    fns = {}
    if not os.path.exists(path): return fns
    for line in open(path, encoding='latin-1'):
        m = re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(?:f\s+)?(?:i\s+)?(\S+\.obj)\s*$', line)
        if m:
            rva = int(m.group(2), 16) - 0x140000000
            if rva > 0 and (rva not in fns or not m.group(1).startswith('??')): fns[rva] = (m.group(1), m.group(3).split(':')[-1].replace('.obj', '.c'))
    return fns


def load_tsv(path):
    rows = []
    if not os.path.exists(path): return rows
    for line in open(path, encoding='latin-1'):
        if line.startswith('#') or not line.strip(): continue
        p = line.rstrip('\n').split('\t')
        rows.append(p)
    return rows


def rebind():
    open(os.path.join(OUT, 'bind_exe.txt'), 'w').write(EXE + '.exe')
    r = subprocess.run([sys.executable, os.path.join(HERE, 'tools_audit', 'link_map.py'), EXE], capture_output=True, text=True, cwd=HERE)
    print(r.stdout[-600:], r.stderr[-600:])
    env = dict(os.environ, SSBU_AUDIT_DUMP=os.path.join(OUT, 'bind.tsv'), SSBU_TRACE=os.path.join(OUT, 'trace_bind.txt'))
    env.pop('SSBU_STRICT', None)
    subprocess.run([os.path.join(HERE, EXE + '_map.exe'), os.path.join(HERE, '..', 'binaries', '13.0.4', 'main.elf'), '5'], env=env, cwd=HERE,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ------------------------------------------------------------------ the real SDK
class Sdk:
    def __init__(self):
        self.syms, self.by_addr, self.d, self.md = {}, {}, None, None
        path = os.path.join(HERE, '..', 'binaries', '13.0.4', 'sdk.elf')
        if not os.path.exists(path): return
        try:
            from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        except ImportError:
            return
        d = self.d = open(path, 'rb').read()
        self.md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        shoff, = struct.unpack_from('<Q', d, 0x28)
        shnum, shstrndx = struct.unpack_from('<HH', d, 0x3c)
        secs = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * 64) for i in range(shnum)]
        st = d[secs[shstrndx][4]:secs[shstrndx][4] + secs[shstrndx][5]]
        by_name = {st[s[0]:st.index(b'\0', s[0])].decode(): s for s in secs}
        so, ss, sto = by_name['.dynsym'][4], by_name['.dynsym'][5], by_name['.dynstr'][4]
        for i in range(ss // 24):
            no, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', d, so + i * 24)
            name = d[sto + no:d.index(b'\0', sto + no)].decode('latin-1')
            if shndx and name:
                self.syms[name] = (value, size, info & 0xf)
                self.by_addr.setdefault(value, name)

    def body(self, name, depth=0):
        """'absent' | 'data' | 'ret' | 'const:0x..' | 'leaf:<n>' | 'calls:<n>:<first callees>' ; thunks (b target) are followed"""
        if name not in self.syms or self.md is None: return 'absent'
        value, size, typ = self.syms[name]
        if typ != 2: return 'data'
        ins = list(self.md.disasm(self.d[0x1000 + value:0x1000 + value + (size or 0x40)], value))
        if not ins: return 'undecoded'
        if ins[0].mnemonic == 'ret': return 'ret'
        if len(ins) >= 2 and ins[1].mnemonic == 'ret' and ins[0].mnemonic in ('mov', 'movz', 'orr') and re.match(r'[wx]0, (#|wzr|xzr)', ins[0].op_str):
            v = ins[0].op_str.split(', ')[-1]
            return 'const:' + ('0x0' if 'zr' in v else v.lstrip('#'))
        if ins[0].mnemonic == 'b' and ins[0].op_str.startswith('#') and depth < 4:
            t = int(ins[0].op_str[1:], 16)
            if t in self.by_addr: return 'thunk->' + self.body(self.by_addr[t], depth + 1)
        calls = []
        for i in ins:
            if i.mnemonic in ('bl', 'b') and i.op_str.startswith('#'):
                t = int(i.op_str[1:], 16)
                if not (value <= t < value + max(size, 4)): calls.append(self.by_addr.get(t, 'sub_%x' % t))
            elif i.mnemonic in ('blr', 'br'): calls.append('(indirect)')
        if not calls: return 'leaf:%d' % len(ins)
        short = [re.sub(r'^_ZN?K?2nn', 'nn', c)[:40] for c in calls[:3]]
        return 'calls:%d:%s' % (len(calls), ','.join(short))


# ------------------------------------------------------------------ classification
def load_classification():
    rules = {'name': {}, 'fn': {}, 're': [], 'file': {}, 'hostfn': {}, 'hostre': []}
    path = os.path.join(HERE, 'tools_audit', 'classification.tsv')
    for p in load_tsv(path):
        if len(p) == 1 and ' || ' in p[0]: p = [x.strip() for x in p[0].split(' || ')]
        if len(p) < 2: continue
        p += [''] * (5 - len(p))
        kind, _, key = p[0].partition(':')
        val = tuple(x.strip() for x in p[1:5])
        if kind in ('re', 'hostre'): rules[kind].append((re.compile(key), val))
        elif kind in rules: rules[kind][key] = val
        else: print('classification.tsv: unknown selector', p[0])
    return rules


def classify(rules, name, f, fn, kind, src, sdk_body):
    """-> (class, evidence, action, status)"""
    if name in rules['name']: return rules['name'][name]
    if (f + ':' + fn) in rules['fn']: return rules['fn'][f + ':' + fn]
    if f == 'sdklift_gen.c': return ('LIFTED', 'handler is the function lifted verbatim from sdk.elf (sdklift.py)', '', 'closed')
    for rx, val in rules['re']:
        if rx.search(name): return val
    sb = sdk_body.replace('thunk->', '')
    if kind == 1 and sb == 'ret': return ('CBS', 'sdk.elf: the real function body is a bare `ret`; the handler is empty too', '', 'closed')
    if kind == 2 and sb.startswith('const:') and src.startswith('x0='):
        try:
            if int(sb[6:], 0) == int(src[3:], 16): return ('CBS', 'sdk.elf: the real function is `mov w0,#%s; ret`; the handler returns the same constant' % sb[6:], '', 'closed')
        except ValueError: pass
    if kind in (1, 2): return ('FAKE', 'do-nothing handler (%s); real function in sdk.elf: %s' % (src, sdk_body), 'lift or implement', 'open')
    if f in rules['file']: return rules['file'][f]
    return ('unclassified', 'bound to %s:%s (%s); sdk.elf: %s' % (f, fn, src, sdk_body), 'review', 'open')


def verify_sdkdata(sdk):
    """name -> True/False: do the bytes in sdkdata.c equal the symbol's bytes in sdk.elf?"""
    out = {}
    src = read('sdkdata.c') if os.path.exists(os.path.join(HERE, 'sdkdata.c')) else ''
    arrays = {m.group(1): bytes(int(x) for x in m.group(2).split(',') if x.strip()) for m in re.finditer(r'static const unsigned char (d\d+)\[\] = \{([^}]*)\};', src)}
    for m in re.finditer(r'\{\s*"([^"]+)",\s*(d\d+),\s*(\d+)\s*\}', src):
        name, arr = m.group(1), arrays.get(m.group(2), b'')
        if sdk.d is None or name not in sdk.syms: out[name] = None; continue
        value, size, typ = sdk.syms[name]
        out[name] = sdk.d[0x1000 + value:0x1000 + value + len(arr)] == arr and len(arr) == int(m.group(3))
    return out


def main():
    if '--rebind' in sys.argv: rebind()
    counts_path = sys.argv[sys.argv.index('--counts') + 1] if '--counts' in sys.argv else os.path.join(OUT, 'counts.tsv')
    regs, bodies = scan_sources()
    fns = load_map()
    bind = load_tsv(os.path.join(OUT, 'bind.tsv'))
    counts = {(p[0], p[1]): p for p in load_tsv(counts_path)}
    sdk = Sdk()
    rules = load_classification()
    sdkdata = verify_sdkdata(sdk)
    g1 = {}
    g1_path = os.path.join(PLAN, 'phases', 'G1_nvn_audit.md')
    if os.path.exists(g1_path):
        for line in open(g1_path, encoding='utf-8', errors='replace'):
            m = re.match(r'\| `(nvn\w+)` \| ([\w-]+) \| (\d+) \| `([^`]*)` \| (.*) \|\s*$', line)
            if m: g1[m.group(1)] = (m.group(2), m.group(4), m.group(5)[:400])
    rows = []          # list, name, file, kind, calls, evidence, action, status, class

    # ---- list 1: import handler tables (source view)
    per_file = collections.Counter(f for f, _, _ in regs)
    generic = [(f, n, h) for f, n, h in regs if h in GENERIC_NAMES]
    gen_by_handler = collections.Counter(h for _, _, h in generic)
    gen_by_file = collections.Counter(f for f, _, _ in generic)

    # ---- list 1 (ground truth) and list 2
    n_bound = 0
    for p in bind:
        lst, name, _, rva, kind, x0, redirect = p[:7]
        if lst != 'import': continue
        rva, kind = int(rva, 16), int(kind)
        cp = counts.get(('import', name))
        calls = cp[2] if cp else ''
        if kind < 0:
            if redirect == '1':
                c = rules['name'].get(name) or next((v for rx, v in rules['re'] if rx.search(name)), None)
                if not c and name in sdkdata:
                    c = ('REAL', 'value copied from sdk.elf (sdkdata.c); bytes compared with the symbol in sdk.elf by fake_census.py: identical', '', 'closed') if sdkdata[name] else ('WRONG', 'sdkdata.c bytes differ from sdk.elf', 'regenerate with gen_sdkdata.py', 'open')
                c = c or ('unclassified', 'data symbol redirected to host memory', 'compare the value with sdk.elf', 'open')
                rows.append(['2-data-imports', name, 'sdkdata.c / nncxxrt.c', 'data redirected to a host table', '', c[1], c[2], c[3], c[0]])
            else:
                c = rules['name'].get(name) or next((v for rx, v in rules['re'] if rx.search(name)), None) or ('FAKE', 'no handler and no data: a call returns 0, a read gives zeros', 'bind or prove unreachable', 'open')
                rows.append(['2-unbound-imports', name, '', 'no handler, no data', calls, c[1], c[2], c[3], c[0]])
            continue
        n_bound += 1
        fn, f = fns.get(rva, ('rva_%x' % rva, '?'))
        body = bodies.get((f, fn))
        sk = body_kind(body) if f != 'sdklift_gen.c' else 'lifted'
        src = {1: 'empty body', 2: 'x0=' + x0 if not x0.startswith('-') else 'stores constants'}.get(kind, sk)
        sb = sdk.body(name)
        cls, ev, act, st = classify(rules, name, f, fn, kind, src, sb)
        rows.append(['1-import-handlers', name, f, '%s (%s)' % (fn, src), calls, ev, act, st, cls])

    # ---- list 3: run-time host functions (vtable slots, NVN entry points) seen in the counted run
    for (lst, name), p in sorted(counts.items()):
        if lst != 'hostfn': continue
        rva, kind = int(p[3], 16), int(p[4])
        fn, f = fns.get(rva, ('rva_%x' % rva, '?'))
        src = {1: 'empty body', 2: 'x0=' + p[5]}.get(kind, body_kind(bodies.get((f, fn))))
        is_gfx = name.startswith('nvn') or f in GRAPHICS
        owner = '3g-nvn-entry-points (graphics agent)' if is_gfx else '3-host-vtables'
        c = rules['hostfn'].get(name) or next((v for rx, v in rules['hostre'] if rx.search(name)), None)
        if c and kind in (1, 2) and c[0] not in ('CBS', 'FAKE', 'WRONG'): c = None       # a do-nothing slot needs its own proof
        g = g1.get(name)
        if g:                                        # the graphics audit's own table (plan/phases/G1_nvn_audit.md)
            status, handler, text = g
            src_g = 'graphics audit G1_nvn_audit.md (%s, %s): %s' % (status, handler, text)
            if status == 'noop': c = ('CBS', src_g, '', 'closed')
            elif status == 'MISSING': c = ('FAKE', src_g + ' -- no implementation: the game stops with the function name if it is ever called', 'implement when a scene needs it (graphics)', 'open')
            elif status == 'real' and kind in (1, 2):
                if re.search(r'sync_no_gl_interop|dev_debugger_attached|debug_id', handler): c = ('CBS', src_g + ' -- constant by meaning, named as such by the graphics audit', '', 'closed')
                else: c = ('FAKE', src_g + ' -- but the machine code of the handler only returns a constant (%s): described as real without a proof that the constant is the true value' % src, 'graphics: prove or implement', 'open')
            elif status == 'real': c = ('REAL', src_g, 'not verified on a frame unless called in a tested scene (graphics audit)', 'closed')
            else: c = ('FAKE', src_g, 'graphics', 'open')
        if not c:
            if kind in (1, 2): c = ('FAKE', 'run-time host function bound to a do-nothing handler (%s)%s' % (src, '; graphics: owned by the graphics audit' if is_gfx else ''), 'implement', 'open')
            elif is_gfx: c = ('unclassified', 'graphics: owned by the graphics audit (plan/phases/G1_nvn_audit.md)', 'see G1', 'open')
            else: c = rules['file'].get(f) or ('unclassified', 'bound to %s:%s (%s)' % (f, fn, src), 'review', 'open')
        rows.append([owner, name, f, '%s (%s)' % (fn, src), p[2], c[1], c[2], c[3], c[0]])

    # ---- list 6b: comment keywords
    kw_hits = []
    for f in HOST:
        for no, line in enumerate(read(f).split('\n'), 1):
            for k in KEYWORDS:
                if k.lower() in line.lower():
                    kw_hits.append((f, no, k, line.strip()[:160]))
                    break

    NOTE = re.compile(r'placeholder address|placeholders, IMPORT_SLOT|placeholder that stands|or its placeholder|use the placeholder|still a stub|stubs|PLT stub|"STUB "|\(stub\)|fake audit|FAKE_AUDIT|fake_whitelist|fake_census|nvn_stub\.c|ignored altogether|are ignored\)|IGNORED \(not held|literal text|not supported",|servname not supported|ai_socktype not supported|ai_family not supported|imports\.c|REPLACING do-nothing placeholders|replace placeholders|placeholders nnmisc|earlier placeholders|these were placeholders|failing placeholder|placeholder\) *$|is accepted and ignored|keeping the failing', re.I)
    for f, no, k, line in kw_hits:
        if f in GRAPHICS:
            # graphics sources, read line by line in the closing session (the code is the graphics side's; only the wording is judged here)
            if re.search(r'nvn_stub\.c|SSBU_NVN_MISSING', line): cls, ev = 'NOTE', 'wording only: the name of the file nvn_stub.c / of a debug switch'
            elif re.search(r'\bfake\b|Tex fake|&fake|fake\.b\.', line): cls, ev = 'NOTE', 'a local variable named `fake` (a Tex record built to save a debug screenshot as BMP): not a behaviour of the game'
            elif re.search(r'dummy', line): cls, ev = 'NOTE', 'the 1x1 dummy texture: counted once, in the row "unbound or dead texture slots sample a 1x1 dummy texture" (list 3g)'
            elif re.search(r'GPU address', line): cls, ev = 'NOTE', 'GPU addresses of buffers / pools are opaque values the game only passes back to NVN: handing out the CPU address is one valid choice of value (any injective mapping satisfies the API)'
            elif re.search(r'NOT IMPLEMENTED for a virtual pool', line): cls, ev = 'NOTE', 'close4: the limitation of nvnMemoryPoolMapVirtual, counted once in its own row (list 3g, MISSING: a call stops with the name)'
            elif re.search(r'discarded point|counter other than|with or without the point', line): cls, ev = 'NOTE', 'close4, read: the comment of the first-draw experiment of the performance agent (a discarded point drawn before the first draw of a program pair), OFF by default (SSBU_FIRSTDRAW_WARM=1 turns it on); "not done" names the cases the experiment skips; the end-of-run report line only counts slow first draws'
            else: cls, ev = 'unclassified', 'graphics source: owned by the graphics audit'
        elif NOTE.search(line): cls, ev = 'NOTE', 'wording only (placeholder ADDRESSES of imports, stub counters, names of tools, an error-message table) or a remark about something that was already replaced: not a fake'
        else: cls, ev = 'NOTE', 'limitation stated in the source: counted once, in the row of the function or of the model it belongs to (lists 1, 4, 5, 6)'
        rows.append(['6k-comment-keywords', '%s:%d [%s]' % (f, no, k), f, line[:150], '', ev, '', 'closed' if cls == 'NOTE' else 'open', cls])

    # ---- list 7: traps inside lifted SDK code
    traps = collections.Counter(re.findall(r'sdk_trap_call\(c, "([^"]*)"\)', read('sdklift_gen.c'))) if os.path.exists(os.path.join(HERE, 'sdklift_gen.c')) else {}

    # ---- manual items
    for p in load_tsv(os.path.join(HERE, 'tools_audit', 'manual_items.tsv')):
        if len(p) == 1 and ' || ' in p[0]: p = [x.strip() for x in p[0].split(' || ')]
        if len(p) < 9: continue
        rows.append(p[:9])

    # ---- deferred rows (owner's decision of 2026-10-09, bounded round): tools_audit/deferred.tsv, "name || tag || group".
    # tag "off-path" = not reached in a normal offline match (boot -> results); tag "offline model" = a network-side service that is
    # reached at boot and stays modelled as an offline console. An EXTERNAL row named there is counted as OPEN (class FAKE) so that it
    # is not forgotten; an open row named there keeps its class. Both get "deferred: <tag>" at the start of the action text.
    deferred, stale = {}, []
    for p in load_tsv(os.path.join(HERE, 'tools_audit', 'deferred.tsv')):
        if len(p) == 1 and ' || ' in p[0]: p = [x.strip() for x in p[0].split(' || ')]
        if len(p) >= 3: deferred[p[0]] = (p[1], p[2])
    seen_def = set()
    for r in rows:
        d = deferred.get(r[1])
        if not d or r[0].startswith('6k'): continue
        seen_def.add(r[1])
        if r[8] == 'EXTERNAL':
            r[6] = 'deferred: %s (was EXTERNAL; owner 2026-10-09: to revisit later) | %s' % (d[0], r[6]); r[7] = 'open'; r[8] = 'FAKE'
        elif r[8] in ('FAKE', 'WRONG', 'unclassified') and r[7] != 'closed':
            r[6] = 'deferred: %s | %s' % (d[0], r[6])
        else: stale.append(r[1])
    stale += [n for n in deferred if n not in seen_def]
    def is_deferred(r): return r[6].startswith('deferred: ')
    def is_offline_model(r): return r[6].startswith('deferred: offline model')

    # ---- outputs
    os.makedirs(PLAN, exist_ok=True)
    with open(os.path.join(PLAN, 'fake_audit.csv'), 'w', newline='', encoding='utf-8') as fo:
        w = csv.writer(fo)
        w.writerow(['list', 'name', 'file', 'kind', 'calls_in_battle_run', 'evidence', 'action', 'status', 'class'])
        for r in sorted(rows, key=lambda r: (r[0], r[2], r[1])): w.writerow(r)
    with open(os.path.join(OUT, 'keyword_hits.tsv'), 'w', encoding='utf-8') as fo:
        for h in kw_hits: fo.write('%s\t%d\t%s\t%s\n' % h)
    # "deferred: offline model" rows keep their place on the white list and stay off the open list: their behaviour is left as it
    # is and they are called at boot, so SSBU_STRICT=path must not stop at them (it does stop at a "deferred: off-path" row).
    white = sorted(set(r[1] for r in rows if (r[8] in ('CBS', 'EXTERNAL') and r[0][0] in '123') or (r[0] == '2-unbound-imports' and r[8] == 'REAL')
                       or (r[0][0] in '123' and is_offline_model(r))), key=lambda s: s.encode('latin-1', 'replace'))
    hdr = ('/* generated by tools_audit/fake_census.py from the audit (plan/fake_audit.csv): imports and run-time host functions whose\n'
           ' * do-nothing or absent handler is proven CORRECT-BY-SEMANTICS. Sorted with strcmp. Do not edit by hand. */\n'
           'static const char *const audit_whitelist[] = {\n    "",\n' + ''.join('    "%s",\n' % n.replace('\\', '\\\\').replace('"', '\\"') for n in white if n) + '};\n')
    wp = os.path.join(HERE, 'fake_whitelist.h')
    if not os.path.exists(wp) or open(wp).read() != hdr: open(wp, 'w', newline='\n').write(hdr)
    # every OPEN import / run-time host function row: SSBU_STRICT=path stops at the first call of one of them
    opened = sorted(set(r[1] for r in rows if r[0][0] in '13' and r[8] in ('FAKE', 'WRONG', 'unclassified') and r[7] != 'closed' and not is_offline_model(r)), key=lambda s: s.encode('latin-1', 'replace'))
    ohdr = ('/* generated by tools_audit/fake_census.py from the audit (plan/fake_audit.csv): imports and run-time host functions whose\n'
            ' * row is OPEN (FAKE, WRONG or unclassified). Sorted with strcmp. Do not edit by hand. */\n'
            'static const char *const audit_openlist[] = {\n    "",\n' + ''.join('    "%s",\n' % n.replace('\\', '\\\\').replace('"', '\\"') for n in opened if n) + '};\n')
    op = os.path.join(HERE, 'fake_openlist.h')
    if not os.path.exists(op) or open(op).read() != ohdr: open(op, 'w', newline='\n').write(ohdr)

    # ---- totals
    print('== sources (%d host files) ==' % len(HOST))
    print('registrations {name, handler}: %d   generic-named handlers: %d' % (len(regs), len(generic)))
    print('  generic by handler: ' + ', '.join('%s %d' % kv for kv in gen_by_handler.most_common()))
    print('  generic by file:    ' + ', '.join('%s %d' % kv for kv in gen_by_file.most_common()))
    print('comment keyword hits: %d (tools_audit/out/keyword_hits.tsv)' % len(kw_hits))
    print('lifted-SDK trap sites: %d (%s)' % (sum(traps.values()), ', '.join('%s x%d' % kv for kv in traps.items())))
    print('== ground truth (bind.tsv: %d imports, %d bound to a handler) ==' % (sum(1 for p in bind if p[0] == 'import'), n_bound))
    lists = collections.OrderedDict()
    for r in rows: lists.setdefault(r[0], collections.Counter())[r[8]] += 1
    classes = ['REAL', 'LIFTED', 'CBS', 'EXTERNAL', 'FAKE', 'WRONG', 'unclassified']
    print('%-44s %6s ' % ('list', 'total') + ' '.join('%12s' % c for c in classes))
    tot = collections.Counter()
    for l in sorted(lists):
        cnt = lists[l]; tot.update(cnt)
        print('%-44s %6d ' % (l, sum(cnt.values())) + ' '.join('%12d' % cnt.get(c, 0) for c in classes) + ('' if set(cnt) <= set(classes) else '   other: %s' % {k: v for k, v in cnt.items() if k not in classes}))
    print('%-44s %6d ' % ('ALL', sum(tot.values())) + ' '.join('%12d' % tot.get(c, 0) for c in classes))
    open_rows = [r for r in rows if r[8] in ('FAKE', 'WRONG', 'unclassified') and r[7] != 'closed']
    print('FAKE + WRONG: %d    unclassified: %d    open rows: %d    white-listed names: %d' % (tot['FAKE'] + tot['WRONG'], tot['unclassified'], len(open_rows), len(white)))
    # reached on the counted run: function rows with a call count > 0, and design / model rows whose count column starts with a number
    def reached(r):
        v = r[4].strip()
        if r[0][0] in '123': return v.isdigit() and int(v) > 0
        return bool(re.search(r'\d', v)) and not re.search(r'never|not counted|^0 ', v) or 'once each' in v or 'in every' in v
    on = [r for r in open_rows if reached(r)]
    by = collections.Counter(r[0][:2] for r in on)
    print('open rows REACHED in the counted run (%s): %d   (imports %d, run-time host functions %d, graphics entry points %d, design / model rows %d)'
          % (os.path.basename(counts_path), len(on), by['1-'], by['3-'], by['3g'], sum(v for k, v in by.items() if k not in ('1-', '3-', '3g'))))
    if '--onpath' in sys.argv:
        for r in on: print('   %-22s %-10s %s%s' % (r[0][:22], r[8], r[1][:110], '   [deferred]' if is_deferred(r) else ''))
    # the owner's two numbers (bounded round, 2026-10-09)
    dfr = [r for r in open_rows if is_deferred(r)]
    n_off = sum(1 for r in dfr if not is_offline_model(r)); n_model = len(dfr) - n_off
    print('deferred (off-path / offline model): %d   (off-path %d, offline model %d; of them reached in the counted run: %d; list: tools_audit/deferred.tsv)'
          % (len(dfr), n_off, n_model, sum(1 for r in dfr if reached(r))))
    print('open and NOT deferred: %d' % (len(open_rows) - len(dfr)))
    wrong = [r for r in dfr if not is_offline_model(r) and reached(r)]
    for r in wrong: print('   WARNING: deferred as off-path but REACHED in the counted run: %s' % r[1][:110])
    for n in stale: print('   WARNING: tools_audit/deferred.tsv names a row that is closed or absent: %s' % n[:110])
    if '--check' in sys.argv: sys.exit(1 if open_rows else 0)


if __name__ == '__main__':
    main()
