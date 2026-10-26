"""Check that each hookable function of tools/hooks.txt is reached only through its slot
(runtime/hookable.h): in the generated C, and in an object compiled from it.

Usage: hook_inline_check.py <module C dir>... [--obj <object dir>]...

Each C dir is a module's build folder (build/main, ...); its hookable functions are the entries
of tools/hooks.txt in that module. --obj gives the object folder of the C dir in the same place
(by default the C dir itself, as the Windows build keeps them); the objects of the files that
define or call a hookable function are read too. Exit status: 0 if all is well, 1 if a problem
was found, 2 if not, but a file that defines or calls one had no object that could be read.
recomp.py build runs the C check on every file it writes when the list has a hookable
function; tests/test_hookable.py runs both checks on a made-up module.
"""
import os
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "runtime", "hookable.h")


def names(addr):
    """(the function itself, its forwarder, its slot) for a hookable function at addr."""
    a = f"{addr:x}"
    return f"f_{a}_body", f"f_{a}", f"rt_hook_{a}"


# a hookable function's names in a line (f_<a>, f_<a>_body, rt_hook_<a>), the address captured
NAMED = re.compile(r"\b(?:f|rt_hook)_([0-9a-f]+)")


def check_c(files, hookables, whole=True):
    """Problems, as strings, in generated C (files: {name: text}) for these hookable functions
    (guest addresses). The function itself, f_<a>_body, is named only where it is declared,
    put in its slot and defined, out of line (RT_HOOKABLE); f_<a> only calls through the slot;
    and no call or tail call names either: each loads the slot. The file that defines it also
    adds its slot to the list and includes hookable.h. whole: these are all of a module's files,
    so each function is defined, with its slot and forwarder, exactly once (and g_module.c, if
    given, holds the forwarder for rt_lookup). One pass over each file however many functions
    are listed: only a line that names one is looked at closely."""
    want, found, count, spell = {}, {}, {}, {}
    for addr in hookables:
        body, fwd, slot = names(addr)
        want[f"{addr:x}"] = addr
        found[addr], count[addr] = [], {"body": 0, "slot": 0, "add": 0, "forwarder": 0, "table": 0}
        spell[addr] = {"body": body, "fwd": fwd, "slot": slot, "decl": f"GuestFn {body};",
                       "slot_def": f"RtHook {slot} = {{0x{addr:X}ull, {body}, {body}}};",
                       "forwarder": f"void {fwd}(Cpu *c) {{ TAIL(RT_HOOK({slot})) }}",
                       "head": f"RT_HOOKABLE void {body}(Cpu *c) {{", "add": f"RT_HOOK_ADD({slot})",
                       "table": f"{{0x{addr:X}ull, {fwd}}}", "uses_body": re.compile(rf"\b{body}\b"),
                       "calls": re.compile(rf"\b{fwd}\(c\)|TAIL\({fwd}\)")}
    for name, text in sorted(files.items()):
        lines = text.splitlines()
        here, defines = {}, set()   # function: the lines here that name it (stripped); the functions defined here
        for n, line in enumerate(lines, 1):
            hits = {want[h] for h in NAMED.findall(line) if h in want}
            if not hits:
                continue
            s, where = line.strip(), f"{name}:{n}"
            for addr in hits:
                k, problems, c = spell[addr], found[addr], count[addr]
                here.setdefault(addr, set()).add(s)
                if k["uses_body"].search(s):
                    if s.startswith(k["head"]):
                        c["body"] += 1
                        defines.add(addr)
                    elif s == k["slot_def"]:
                        c["slot"] += 1
                    elif s != k["decl"]:
                        problems.append(f"{where}: {k['body']} named other than in its slot: {s[:120]}")
                if k["calls"].search(s):
                    problems.append(f"{where}: calls {k['fwd']} directly, not through its slot: {s[:120]}")
                if s.startswith(f"void {k['fwd']}(Cpu *c)"):
                    c["forwarder"] += 1
                    if s != k["forwarder"]:
                        problems.append(f"{where}: {k['fwd']} does not call through its slot: {s[:120]}")
                c["add"] += s == k["add"]
                c["table"] += s.startswith(k["table"])
        for addr in sorted(defines):
            k, seen = spell[addr], here[addr]
            for what, ok in ((f"{k['slot']}'s definition", k["slot_def"] in seen), (k["add"], k["add"] in seen),
                             (k["fwd"], k["forwarder"] in seen), ('#include "hookable.h"', '#include "hookable.h"' in lines)):
                if not ok:
                    found[addr].append(f"{name}: defines {k['body']} without {what}")
    problems = []
    for addr in sorted(hookables):
        problems += found[addr]
        if whole:
            c, body, fwd = count[addr], spell[addr]["body"], spell[addr]["fwd"]
            for what, n in (("defined", c["body"]), ("given a slot", c["slot"]), ("added to the list", c["add"]),
                            ("given a forwarder", c["forwarder"])):
                if n != 1:
                    problems.append(f"{body}: {what} {n} times, not once")
            if "g_module.c" in files and c["table"] != 1:
                problems.append(f"g_module.c: {fwd} is not in the module's table once")
    return problems


def check_header(text=None):
    """Problems with runtime/hookable.h's promises: RT_HOOKABLE keeps the function out of line on
    every platform, and on Windows in a section of its own (not a COMDAT, which /OPT:ICF could
    fold); RT_HOOK loads the slot with acquire."""
    if text is None:
        with open(HEADER, encoding="utf-8") as f:
            text = f.read()
    problems = []
    hookable = re.findall(r"^#define RT_HOOKABLE (.*)$", text, re.M)
    if len(hookable) != 2 or not all("noinline" in d for d in hookable):
        problems.append("hookable.h: RT_HOOKABLE is not noinline on every platform")
    win = re.search(r"#ifdef _WIN32\s*\n#define RT_HOOKABLE (.*)\n", text)
    if not win or not re.search(r'section\("\.text\$\w+"\)', win.group(1)):
        problems.append("hookable.h: on Windows RT_HOOKABLE does not put the function in a section of its own")
    if not re.search(r"^#define RT_HOOK\(h\) __atomic_load_n\(&\(h\)\.slot, __ATOMIC_ACQUIRE\)$", text, re.M):
        problems.append("hookable.h: RT_HOOK does not load the slot with acquire")
    return problems


# ---- objects ---------------------------------------------------------------------------
IMAGE_SCN_CNT_CODE, IMAGE_SCN_LNK_COMDAT, IMAGE_SCN_MEM_EXECUTE = 0x20, 0x1000, 0x20000000
IMAGE_SCN_LNK_NRELOC_OVFL = 0x01000000


def coff_code(data):
    """(functions, {function: the symbols its code's relocations name}, the functions in a COMDAT
    section) of a COFF object, as clang-cl writes them (the big-object form too, which a file
    with very many functions needs, and a section with more than 65,535 relocations, whose count
    is in its first relocation record)."""
    if data[:4] == b"\0\0\xff\xff":   # (ANON_OBJECT_HEADER_BIGOBJ: more than 65,279 sections)
        nsec, symptr, nsym = struct.unpack_from("<III", data, 44)
        sec_at, sym_size, sym_form = 56, 20, "<8sIiHBB"
    else:
        _machine, nsec, _time, symptr, nsym, opt, _flags = struct.unpack_from("<HHIIIHH", data, 0)
        sec_at, sym_size, sym_form = 20 + opt, 18, "<8sIhHBB"
    strings = symptr + sym_size * nsym

    def name(raw):
        if raw[:4] == b"\0\0\0\0":
            at = strings + struct.unpack_from("<I", raw, 4)[0]
            return data[at:data.index(b"\0", at)].decode()
        return raw.rstrip(b"\0").decode()
    sections = [struct.unpack_from("<8sIIIIIIHHI", data, sec_at + 40 * i) for i in range(nsec)]
    symbols, i = [], 0
    while i < nsym:
        raw, value, sec, typ, _cls, aux = struct.unpack_from(sym_form, data, symptr + sym_size * i)
        symbols += [(name(raw), value, sec, typ)] + [None] * aux
        i += 1 + aux
    funcs = [(sec, value, n) for n, value, sec, typ in filter(None, symbols) if typ == 0x20 and sec > 0]
    refs = {n: set() for _s, _v, n in funcs}
    for index, (_n, _vs, _va, _size, _raw, relocs, _ln, nrel, _nln, flags) in enumerate(sections, 1):
        if not flags & (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE):
            continue
        here = sorted((v, n) for s, v, n in funcs if s == index)
        first = 0
        if flags & IMAGE_SCN_LNK_NRELOC_OVFL and nrel == 0xFFFF:   # (the real count, which counts that record too)
            nrel, first = struct.unpack_from("<I", data, relocs)[0], 1
        for r in range(first, nrel):
            at, sym, _type = struct.unpack_from("<IIH", data, relocs + 10 * r)
            owner = [n for v, n in here if v <= at]
            if owner and symbols[sym]:
                refs[owner[-1]].add(symbols[sym][0])
    comdat = {n for s, _v, n in funcs if sections[s - 1][9] & IMAGE_SCN_LNK_COMDAT}
    return {n for _s, _v, n in funcs}, refs, comdat


def _objdump(objdump, args, path):
    r = subprocess.run([objdump, *args, path], capture_output=True, text=True, timeout=300)
    if r.returncode:
        raise RuntimeError(f"{objdump} failed on {path}: {r.stderr[:500]}")
    return r.stdout


def objdump_code(path, objdump):
    """(functions, {function: the symbols its code names, by relocation or as a branch target},
    no COMDATs) of an ELF or Mach-O object, from objdump -t and -d -r (LLVM's or GNU's).

    objdump labels code with one symbol per address, and that may be an assembler-private one
    (Mach-O's ltmp0 at the start of a section, which the first function shares), so a function's
    name comes from the symbol table. An instruction that has a relocation names what the
    relocation names, not the placeholder its operand shows (an adrp of 0x0 would show whatever
    function is at 0); a relocation against a section or a private label (.text+0x84) names the
    function at that offset (or 4 past it: x86's PC-relative addend)."""
    with open(path, "rb") as f:
        macho = f.read(4) in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe")

    def private(name, section):
        return (not name.startswith("_")) if macho else (name.startswith(".L") or name == section)
    at, where = {}, {}   # (section, address): [(rank, name)]; private symbol: (section, address)
    for line in _objdump(objdump, ["-t"], path).splitlines():
        m = re.match(r"^([0-9a-fA-F]+) (.{7}) (\S+)\s+(.*)$", line)
        if not m or m.group(3).startswith("*"):   # (*UND*, *ABS*, *COM*)
            continue
        flags, section, name = m.group(2), m.group(3), m.group(4).split()[-1]
        key = (section, int(m.group(1), 16))
        if private(name, section):
            where[name] = key
        at.setdefault(key, []).append((private(name, section), "g" not in flags, name))

    def norm(s):
        s = re.split(r"[-+@]", s, maxsplit=1)[0]
        return s[1:] if macho and s.startswith("_") else s

    def named(key):
        return min(at[key])[2] if key in at and not min(at[key])[0] else None

    def target(s, addend):
        m = re.match(r"^(.*?)([+-]0x[0-9a-fA-F]+)?$", s)
        if m.group(1) in where:
            section, a = where[m.group(1)]
            a += addend + (int(m.group(2), 16) if m.group(2) else 0)
            s = named((section, a)) or named((section, a + 4)) or s
        return norm(s)
    funcs, refs, cur, section, pending, addend = set(), {}, None, None, set(), 0

    def flush():
        if cur is not None:
            refs[cur].update(pending - {cur})
        pending.clear()
    for line in _objdump(objdump, ["-d", "-r"], path).splitlines():
        if m := re.match(r"^Disassembly of section (.+):$", line):
            flush()
            section, cur = m.group(1), None
        elif m := re.match(r"^([0-9a-fA-F]+) <(.+)>:$", line):
            flush()
            cur = norm(named((section, int(m.group(1), 16))) or m.group(2))
            funcs.add(cur)
            refs.setdefault(cur, set())
        elif cur is None:
            continue
        elif m := re.match(r"^\s+[0-9a-fA-F]+:\s+((?:R_|ARM64_RELOC_|X86_64_RELOC_)\w+)\s+(\S+)", line):
            pending.clear()
            if m.group(1) == "ARM64_RELOC_ADDEND":   # (Mach-O: the addend of the relocation after it)
                addend = int(m.group(2), 16)
                continue
            refs[cur].add(target(m.group(2), addend))
            addend = 0
        elif re.match(r"^\s+[0-9a-fA-F]+:", line):   # an instruction
            flush()
            pending.update(norm(t) for t in re.findall(r"<([^>]+)>", line))
    flush()
    return funcs, refs, set()


def find_objdump():
    return shutil.which("llvm-objdump") or shutil.which("objdump")


def object_code(path, objdump=None):
    """(functions, {function: the symbols its code names}, the functions in a COMDAT section) of
    an object file: COFF read here, ELF and Mach-O through objdump (None if there is none)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] in (b"\x64\x86", b"\x64\xaa") or data[:4] == b"\0\0\xff\xff":   # (AMD64, ARM64, a big object)
        return coff_code(data)
    objdump = objdump or find_objdump()
    return objdump_code(path, objdump) if objdump else None


def check_object(path, hookables, objdump=None):
    """Problems in a compiled object for these hookable functions, or None if it cannot be read
    (no objdump for ELF or Mach-O). Where f_<a> is defined, f_<a>_body is too (not inlined into
    it or folded away), and f_<a> names the slot, not the function; no code anywhere names
    f_<a>_body (a call that bypasses the slot); in COFF, f_<a>_body is not in a COMDAT section,
    so /OPT:ICF cannot fold it with another function."""
    code = object_code(path, objdump)
    if code is None:
        return None
    funcs, refs, comdat = code
    problems, where = [], os.path.basename(path)
    for addr in sorted(hookables):
        body, fwd, slot = names(addr)
        if fwd in funcs:
            if body not in funcs:
                problems.append(f"{where}: {body} is not in the object (inlined, or folded away)")
            if slot not in refs.get(fwd, ()) or body in refs.get(fwd, ()):
                problems.append(f"{where}: {fwd} does not call through {slot}")
        for f, named in sorted(refs.items()):
            if body in named and f != body:
                problems.append(f"{where}: {f} names {body}: a call that bypasses the slot")
        if body in comdat:
            problems.append(f"{where}: {body} is in a COMDAT section, which /OPT:ICF may fold with another function")
    return problems


# ---- the command -----------------------------------------------------------------------
def module_hookables(c_dir):
    """The hookable functions of tools/hooks.txt in the module whose C is in c_dir (its base from
    g_module.c)."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import recomp
    with open(os.path.join(c_dir, "g_module.c"), encoding="utf-8") as f:
        base = int(re.search(r'^    "\w+", 0x([0-9A-F]+)ull,', f.read(), re.M).group(1), 16)
    return {a for kind, a in recomp.hook_list() if kind == "hookable" and recomp.module_base(a) == base}


def main(argv):
    dirs, objs, args = [], [], iter(argv)
    for a in args:
        if a == "--obj":
            objs.append(next(args, None))
        else:
            dirs.append(a)
    if not dirs or None in objs:
        sys.exit(__doc__)
    problems, unchecked = check_header(), 0
    for i, c_dir in enumerate(dirs):
        hookables = module_hookables(c_dir)
        files = {}
        for n in sorted(os.listdir(c_dir)):
            if re.fullmatch(r"g(\d{4}|_module)\.c", n):
                with open(os.path.join(c_dir, n), encoding="utf-8") as f:
                    files[n] = f.read()
        problems += check_c(files, hookables)
        obj_dir = objs[i] if i < len(objs) else c_dir
        for n, text in files.items():
            if "rt_hook_" not in text:
                continue
            obj = next((os.path.join(obj_dir, n[:-2] + e) for e in (".obj", ".o")
                        if os.path.exists(os.path.join(obj_dir, n[:-2] + e))), None)
            found = check_object(obj, hookables) if obj else None
            if found is None:
                print(f"{c_dir}/{n}: no object, or no objdump to read it with: not checked")
                unchecked += 1
            problems += found or []
        print(f"{c_dir}: {len(hookables)} hookable functions")
    for p in problems:
        print(p)
    if problems:
        return 1
    if unchecked:   # (not a pass: the objects were not read)
        print(f"{unchecked} files that define or call a hookable function have no object checked (--obj <object dir>)")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
