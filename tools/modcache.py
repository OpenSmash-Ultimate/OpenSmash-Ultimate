"""What tools/recomp.py keeps between builds: NRO load addresses, the stamp that says a module's
build dir is current, and the check that every module's imports resolve.

NRO load addresses: dump/nro/bases.txt, "<name> <base> <end> <build id>", one row per fighter
module (NRO) the game has, each placed for good (place) so that rebuilding never moves one.
Modules must not share a 16 MB dispatch slot (runtime/host.c SLOT_SHIFT), and NROs live
between subsdk/sdk's slots and the heap: [0x7200000000, 0x7400000000), 512 slots.

Module build dirs: each holds a stamp, module.txt: "<build id> <base> <name> <tools
fingerprint>", written once its C is generated and compiled (by a build that compiles it
elsewhere - Linux, macOS: recomp.py all --generate-only - once it is generated). With the same
build id, base and tools fingerprint (the generator's sources and the runtime's native-function
set) the C is not generated again. recomp.py replaces a file rather than writing into it
(write_if_changed, compile_dir): a file can be a hard link that an earlier OpenSmash, which
shared build dirs between game versions, made to another folder's.
"""
import ast
import hashlib
import os
import re

TOOLS = os.path.dirname(os.path.abspath(__file__))
NRO_BASE, NRO_LIMIT, SLOT = 0x7200000000, 0x7400000000, 0x1000000
# the sources whose output is the generated C (a change in any of them generates it again)
GENERATOR = ("recomp.py", "lift.py", "lift_simd.py", "a64.py", "funcs.py", "eh.py", "nso.py", "regcache.py", "plugin_eh.py")


def replace_file(path, text):
    """Write a file as a new one (never through a hard link to another folder's)."""
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(tmp, path)


# ---- NRO load addresses -------------------------------------------------------------
def read_bases(path):
    """[name, base, end, build id] rows of dump/nro/bases.txt."""
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [l.split() for l in f if len(l.split()) == 4]


def slots(base, end):
    return set(range(base // SLOT, (end - 1) // SLOT + 1))


def place(name, size, rows):
    """The base for a new NRO among this folder's NROs (`rows`: bases.txt rows): past all of
    them, else the first free slots."""
    used = set()
    for r in rows:
        used |= slots(int(r[1], 16), int(r[2], 16))
    top = max([NRO_BASE] + [int(r[2], 16) for r in rows])
    base = (top + SLOT - 1) // SLOT * SLOT
    if base + size > NRO_LIMIT:
        base = next((b for b in range(NRO_BASE, NRO_LIMIT - size + 1, SLOT) if not slots(b, b + size) & used), None)
        if base is None:
            raise SystemExit(f"no room left for {name} ({size:,} bytes) among the game's modules")
    return base


# ---- module build dirs ---------------------------------------------------------------
# (SSBU_GEN_FINGERPRINT, on unless 0) The generator's sources are fingerprinted by what they do, not by their bytes:
# their syntax tree without comments, docstrings, layout and print()s that call nothing, and recomp.py's without the
# functions below, which compile and link what is generated and never write any of it (tests/test_gen_fingerprint.py
# checks that nothing the generator runs reaches them). An edit to recomp.py's link step, or to a comment or a
# message in lift.py, then no longer writes all 106 modules' C again (10 to 25 minutes of an update on a fast PC:
# v0.5.0 to v0.5.1 changed only link()). A stamp written under the fingerprint by the bytes stays current while
# those bytes are today's (recomp.module_fingerprints), so turning this on writes nothing again. 0: by the bytes only,
# as before.
DRIVER_ONLY = {"recomp.py": ("link", "link_compiled", "compile_dir", "compile_dirs", "plan", "stale_files", "game_key",
                             "c_needed", "main_kind_room", "one_queue", "scripts_level", "build_folder", "coverage",
                             "emit", "check", "library_objects", "_library_inputs", "_library_record", "library_stamp",
                             "library_kept", "library_touch", "compile_and_link", "_without_date")}


def by_meaning():
    return os.environ.get("SSBU_GEN_FINGERPRINT", "1").strip() != "0"


def _strip_docstrings(node):
    for n in ast.walk(node):
        body = getattr(n, "body", None)
        if isinstance(n, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) and body \
                and isinstance(body[0], ast.Expr) and isinstance(body[0].value, ast.Constant) \
                and isinstance(body[0].value.value, str):
            n.body = body[1:] or [ast.Pass()]


def _says_only(stmt):
    """A print() to the console whose arguments call nothing: what it says is never in the C (the generator writes
    its files itself), and saying it changes nothing else."""
    if not (isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Call) and isinstance(stmt.value.func, ast.Name)
            and stmt.value.func.id == "print" and not any(k.arg == "file" for k in stmt.value.keywords)):
        return False
    return not any(isinstance(n, (ast.Call, ast.NamedExpr, ast.Await, ast.Yield, ast.YieldFrom))
                   for a in stmt.value.args + [k.value for k in stmt.value.keywords] for n in ast.walk(a))


def _strip_prints(node):
    for n in ast.walk(node):
        for field in ("body", "orelse", "finalbody"):
            body = getattr(n, field, None)
            if isinstance(body, list) and body and isinstance(body[0], ast.stmt):
                kept = [s for s in body if not _says_only(s)]
                if len(kept) != len(body):
                    setattr(n, field, kept or [ast.Pass()])


def meaning(name, data):
    """What a generator source does, as bytes: its syntax tree, without docstrings (comments and layout are not
    in it), without what it only says (a print() that calls nothing) and without the DRIVER_ONLY functions (top
    level). Its bytes when it does not parse."""
    try:
        tree = ast.parse(data)
    except (SyntaxError, ValueError):
        return data.replace(b"\r\n", b"\n")
    drop = set(DRIVER_ONLY.get(name, ()))
    tree.body = [n for n in tree.body if not (isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name in drop)]
    _strip_docstrings(tree)
    _strip_prints(tree)
    return ast.dump(tree, annotate_fields=True, include_attributes=False).encode()


_MEANINGS = {}


def _meaning_of(name, data):
    """meaning()'s digest, once per source text in a run (module_fingerprint asks again for each module with hooks)."""
    key = (name, hashlib.sha256(data).digest())
    if key not in _MEANINGS:
        _MEANINGS[key] = hashlib.sha256(meaning(name, data)).digest()
    return _MEANINGS[key]


def fingerprint(extra, legacy=None):
    """The tools' fingerprint: the generator's sources and `extra` (the runtime's set of
    native functions and wraps, and tools/hooks.txt's entries, which change what is generated).
    legacy: by the sources' bytes, as before SSBU_GEN_FINGERPRINT (default: when it is 0)."""
    legacy = not by_meaning() if legacy is None else legacy
    h = hashlib.sha256()
    if not legacy:
        h.update(b"meaning 1\0")
    for n in GENERATOR:
        with open(os.path.join(TOOLS, n), "rb") as f:
            data = f.read()
        h.update(n.encode() + b"\0" + (data.replace(b"\r\n", b"\n") if legacy else _meaning_of(n, data)) + b"\0")
    h.update(extra.encode())
    return h.hexdigest()[:16]


TAIL = re.compile(r'const RtModule rt_module_(\w+) = \{\s*"\w+", 0x([0-9A-F]+)ull,.*?"([0-9A-F]+)",\s*\};', re.S)


def build_info(d):
    """(build id, base, name, tools fingerprint or None) of a module build dir, or None:
    from its stamp, or for a dir made before stamps, from the end of its g_module.c."""
    try:
        with open(os.path.join(d, "module.txt")) as f:
            p = f.read().split()
        return p[0], int(p[1], 16), p[2], p[3]
    except (OSError, IndexError, ValueError):
        pass
    try:
        with open(os.path.join(d, "g_module.c"), "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - 4096))
            m = TAIL.search(f.read().decode("utf-8", "replace"))
    except OSError:
        return None
    return (m.group(3), int(m.group(2), 16), m.group(1), None) if m else None


def write_stamp(d, build_id, base, name, fp):
    replace_file(os.path.join(d, "module.txt"), f"{build_id} {base:x} {name} {fp}\n")


def clear_stamp(d):
    try:
        os.remove(os.path.join(d, "module.txt"))
    except OSError:
        pass


# ---- imports ---------------------------------------------------------------------------
def unresolved_imports(mods, native):
    """name -> (strong, weak) symbols the module refers to (by relocation) that no module
    exports and the runtime does not implement (`native`): what would stay unbound when the
    game runs (runtime/host.c resolve). mods: name -> nso.Module."""
    exported = set()
    for m in mods.values():
        exported |= {s[0] for s in m.exports}
    out = {}
    for name, m in mods.items():
        refs = {r[2] for r in m.relocs if r[1] != 1027 and r[2]}
        strong, weak = set(), set()
        for i in refs:
            sname, _v, _s, info, shndx = m.symbols[i]
            if not sname or shndx or sname in exported or sname in native:
                continue
            (weak if info >> 4 == 2 else strong).add(sname)
        out[name] = (sorted(strong), sorted(weak))
    return out


def report_imports(mods, native, path):
    """Print and write (to `path`) every module's unresolved imports; returns how many strong ones."""
    res = unresolved_imports(mods, native)
    lines = ["# symbols a module refers to that no module exports and the runtime does not implement (HLE):",
             "# <module> strong|weak <symbol>   (strong ones trap when called; weak ones read as 0)"]
    total_s = total_w = 0
    for name in mods:
        strong, weak = res[name]
        total_s += len(strong)
        total_w += len(weak)
        lines += [f"{name} strong {s}" for s in strong] + [f"{name} weak {s}" for s in weak]
        if strong:
            more = f" (+{len(strong) - 3} more)" if len(strong) > 3 else ""
            print(f"  {name}: {len(strong)} unresolved import(s): {', '.join(strong[:3])}{more}")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    replace_file(path, "\n".join(lines) + "\n")
    print(f"imports: {total_s} strong and {total_w} weak symbols unresolved across {len(mods)} modules (listed in {path})")
    return total_s
