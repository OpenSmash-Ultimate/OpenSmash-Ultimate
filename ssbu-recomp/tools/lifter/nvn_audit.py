"""NVN entry point audit: one row per function the game obtains through nvnDeviceGetProcAddress / nvnBootstrapLoader.

    python nvn_audit.py <counts file> [--md <out.md>] [--missing]

<counts file> is written by a run with SSBU_AUDIT_COUNTS=<file> (hostrt.c): every run-time host function with its call count.
The handler of each name is read from the sources (the table k_nvn in nvn_front.c, `special` in nvn_stub.c); the meaning of each
function and why a handler may legitimately do nothing comes from nvn_audit_notes.tsv (name <tab> class <tab> text), where class
is `real` or `noop` (no effect on this platform by the API's own semantics; the text says why). A name with a handler but no
note is listed as `real (no note)`; a name without a handler is `MISSING`."""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TRIVIAL = {'n_nop', 'n_zero', 'n_true', 'nvn_true', 'nvn_generic'}


def handlers():
    h = {}
    front = open(os.path.join(HERE, 'nvn_front.c'), encoding='latin1').read()      # the NVN front end: k_nvn and its handlers
    table = front[front.index('} k_nvn[] = {'):]
    for name, fn in re.findall(r'\{"(nvn\w+)",\s*(\w+)\}', table):
        h[name] = ('nvn_front.c', fn)
    st = open(os.path.join(HERE, 'nvn_stub.c'), encoding='latin1').read()
    for name, fn in re.findall(r'\{"(nvn\w+)",\s*(\w+)\}', st):
        h.setdefault(name, ('nvn_stub.c', fn))
    for name in ('nvnDeviceGetProcAddress', 'nvnDeviceGetInteger'):
        h[name] = ('nvn_stub.c', 'nvn_get_proc' if name.endswith('Address') else 'nvn_get_integer')
    return h


def notes():
    n = {}
    p = os.path.join(HERE, 'nvn_audit_notes.tsv')
    if os.path.exists(p):
        for line in open(p, encoding='utf-8'):
            f = line.rstrip('\n').split('\t')
            if len(f) >= 3 and not line.startswith('#'):
                n[f[0]] = (f[1], f[2])
    return n


def describe(name):
    """What a function does, from its name, for the families that only store or return state or record a command."""
    m = re.match(r'nvn(\w+?)(Builder|State|View)?(Set|Get)(\w+)$', name)
    if name.startswith('nvnCommandBuffer'):
        rest = name[len('nvnCommandBuffer'):]
        if rest.startswith('Bind'):
            return 'Recorded; when the handle is submitted the binding takes effect for the following draws / dispatches.'
        if rest.startswith('Set') and not rest.startswith(('SetMemoryCallback', 'SetDebugLabel', 'SetCommandMemory')):
            return 'Recorded; applied to the OpenGL state when the handle is submitted.'
        if rest.startswith(('Draw', 'Clear', 'Copy', 'Downsample', 'TiledDownsample', 'UpdateUniformBuffer', 'MultiDraw')):
            return 'Recorded; executed with OpenGL when the handle is submitted.'
        if rest.startswith('Get'):
            return 'Returns the value kept for the command buffer.'
        if 'Debug' in rest:
            return 'Recorded; becomes an OpenGL debug group / marker when the handle is submitted.'
    if m and m.group(3) == 'Set' and m.group(2):
        return 'Stores the value in the %s%s object (guest memory); used when the object is bound or built.' % (m.group(1), m.group(2))
    if m and m.group(3) == 'Get':
        return 'Returns what the object holds (%s).' % m.group(4)
    if name.endswith('SetDebugLabel'):
        return 'Stores the label (nvn_label_get); textures and programs also pass it to glObjectLabel.'
    if name.endswith('GetDebugID'):
        return 'A number unique to the object (its address).'
    if name.endswith('Initialize'):
        return 'Creates the host record of the object from the builder.'
    if name.endswith('Finalize'):
        return 'Releases the host record (OpenGL objects are deleted by the render thread).'
    return ''


def main():
    counts = {}
    for line in open(sys.argv[1], encoding='latin1'):
        f = line.rstrip('\n').split('\t')
        if len(f) >= 3 and f[0] == 'hostfn' and f[1].startswith('nvn'):
            counts[f[1]] = int(f[2])
    h, n = handlers(), notes()
    rows, tot = [], {'real': 0, 'noop': 0, 'do-nothing': 0, 'MISSING': 0}
    for name in sorted(counts):
        src, fn = h.get(name, ('', ''))
        cls, text = n.get(name, ('', ''))
        if not text:
            text = describe(name)
        if not fn:
            status = 'MISSING'
        elif fn in TRIVIAL:
            status = 'noop' if cls == 'noop' else 'do-nothing'
        else:
            status = 'real'
        tot[status] += 1
        rows.append((name, status, counts[name], '%s:%s' % (src, fn) if fn else '-', text))
    called = sum(1 for r in rows if r[2])
    summary = '%d NVN names obtained, %d called; real %d, documented no-op %d, do-nothing without a reason %d, missing %d' % (
        len(rows), called, tot['real'], tot['noop'], tot['do-nothing'], tot['MISSING'])
    if '--missing' in sys.argv:
        for r in sorted(rows, key=lambda r: -r[2]):
            if r[1] in ('MISSING', 'do-nothing'):
                print('%-10s %9d  %-52s %s' % (r[1], r[2], r[0], r[3]))
    if '--md' in sys.argv:
        out = sys.argv[sys.argv.index('--md') + 1]
        with open(out, 'w', encoding='utf-8') as f:
            f.write(open(os.path.join(HERE, 'nvn_audit_head.md'), encoding='utf-8').read() if os.path.exists(os.path.join(HERE, 'nvn_audit_head.md')) else '# NVN audit\n\n')
            f.write('\n**Totals (generated):** %s.\n\n' % summary)
            f.write('| function | status | calls | handler | semantics / reason |\n|---|---|---:|---|---|\n')
            for r in rows:
                f.write('| `%s` | %s | %d | `%s` | %s |\n' % r)
    print(summary)


main()
