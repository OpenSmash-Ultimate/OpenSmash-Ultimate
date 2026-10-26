#!/usr/bin/env python3
"""Static recompiler driver.

Usage:
  recomp.py coverage <nso> [--jobs N]
  recomp.py emit     <nso> --roots <off>[,<off>...] --out <file.c> [--depth N]
  recomp.py emit     <nso> --sample N --out <file.c>
  recomp.py image    <nso> --out <file.bin>
  recomp.py build    <nso> --out <dir> [--jobs N] [--per-file INSNS] [--exefs <dir>]
  recomp.py compile  <dir> [--jobs N]
  recomp.py link     --dirs <dir>[,<dir>...] --out <exe> [--game-dll]
  recomp.py all      <exefs dir> --out <build dir> [--jobs N] [--generate-only] [--game-dll]
  recomp.py check    <exefs dir>
  recomp.py wrap-sig <nso> <address | exported name>

The game is version 13.0.5 (tools/game_version.py); its modules are this folder's dump/exefs
and dump/nro (the fighter modules).

coverage  lift every function and report what the lifter cannot translate yet
emit      write C for the given functions and everything they call directly
image     write the module image with base relocations applied for its load address
build     write C for a whole module, split across many files, plus its loader tables
compile   compile a build directory to objects (incremental: objcache.py; reports per-file failures)
link      compile the host runtime and link module build directories into an executable; each
          directory's objects compiled already, for this build (compile): else it stops
all       image + build + compile every module of an ExeFS, then link; a module whose C is
          current (same build id, address and tools: modcache.py) is not generated again;
          --generate-only stops before compiling (the Linux build compiles the C itself:
          tools/linux/build_linux.py; so does the macOS one, tools/mac/build_mac.py, and on
          macOS all always stops there)
          --game-dll (link, all): the modules linked into a library of their own, ssbu_game.dll
          beside the exe, which the exe loads at start; the exe is then the runtime alone
          (tools/game_dll.py)
check     list each module's imports that no module exports and the runtime does not implement
wrap-sig  print the code signature (a hash) of a guest function, for an HLE_WRAP (see hle_wraps),
          an HLE_WRAP_EXPORT (by its exported name: hle_export_wraps) or a tools/hooks.txt entry

Each module is recompiled for a fixed load address (see MODULES); a function is named after
its guest address, so names are unique across modules. A module function whose exported
name has a native implementation - `HLE(name)` in runtime/*.c - is emitted as a forwarder to
it, so the replacement is reached by every caller, including direct calls inside the module.
tools/hooks.txt lists functions whose callers' whole register state a hook needs (hook_list),
and the hookable ones, which every caller reaches through a slot a hook can take over
(runtime/hookable.h). When it has an exact-callee or a hookable function, build reads main,
subsdk0 and sdk from --exefs (dump/exefs if not given) for the names other modules reach it by
(hook_imports).
"""
import bisect
import hashlib
import mmap
import os
import re
import struct
import sys
import time
from collections import Counter
from multiprocessing import Pool

# this folder, for the modules below: a Python with a ._pth file (the embeddable one the installer
# sets up when the PC has none) does not put a script's own folder on the path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_dll  # noqa: E402
import isa  # noqa: E402
import ladder  # noqa: E402
import modcache  # noqa: E402
import objcache  # noqa: E402
import progress  # noqa: E402
from funcs import discover  # noqa: E402
from lift import Lifter  # noqa: E402
from nso import Module  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Load addresses, in the order the console's loader places (and searches) the modules.
MODULES = {"main": 0x7100000000, "subsdk0": 0x7108000000, "sdk": 0x7109000000}
# Modules the game loads at run time (NROs). The runtime saves each one it is handed that is
# not compiled in yet as dump/nro/<build id>.nro; each is then given a load address of its
# own for good, recorded in dump/nro/bases.txt, so that rebuilding never moves a module
# (modcache.place).
NRO_DIR = os.path.join(ROOT, "dump", "nro")
# --game-dll (link, all; the command line sets it): link() links the modules into game_dll.DLL beside the
# exe, which the exe's runtime, compiled to (game_dll.DEFINE), loads (tools/game_dll.py).
GAME_DLL = False
_state = {}


def module_key(path):
    return os.path.splitext(os.path.basename(path))[0]


def nro_modules():
    """name -> base for every harvested NRO, naming and placing new arrivals."""
    manifest = os.path.join(NRO_DIR, "bases.txt")
    rows = modcache.read_bases(manifest)
    known = {r[3]: r for r in rows}
    added = False
    for f in sorted(os.listdir(NRO_DIR)) if os.path.isdir(NRO_DIR) else []:
        path = os.path.join(NRO_DIR, f)
        if not f.endswith(".nro") or module_key(f) in {r[0] for r in rows}:
            continue
        mod = Module(path)
        if mod.build_id in known:   # saved again under its id before it was compiled in
            os.remove(path)
            continue
        name = re.sub(r"\W", "_", module_key(mod.module_name().replace("\\", "/")).lower()) or "nro"
        if not name[0].isalpha():
            name = "nro_" + name
        base = modcache.place(name, mod.mem_end, rows)
        while name in MODULES or name in {r[0] for r in rows}:
            name += "_"
        row = [name, f"{base:x}", f"{base + mod.mem_end:x}", mod.build_id]
        rows.append(row)
        known[mod.build_id] = row
        os.replace(path, os.path.join(NRO_DIR, name + ".nro"))
        print(f"new NRO {name}: {mod.mem_end:,} bytes at {base:#x}")
        added = True
    if added:
        with open(manifest, "w", newline="\n") as f:
            f.writelines(" ".join(r) + "\n" for r in rows)
    return {r[0]: int(r[1], 16) for r in rows}


def base_of(path):
    if arg("--base"):
        return int(arg("--base"), 16)
    key = module_key(path)
    if key not in MODULES and path.endswith(".nro"):
        MODULES.update(nro_modules())
    return MODULES[key]


def hle_names():
    """Exported names that have a native implementation in the runtime sources."""
    names = set()
    runtime = os.path.join(ROOT, "runtime")
    for name in os.listdir(runtime):
        if name.endswith(".c") and not name.startswith("._"):   # (not macOS's ._* files on exFAT)
            with open(os.path.join(runtime, name), encoding="utf-8") as f:
                names |= set(re.findall(r"^HLE\((\w+)\)", f.read(), re.M))
    return names


# `/* signature: 16 words, sha256 0123456789abcdef */`, then `HLE_WRAP(<address>)` or
# `HLE_WRAP_EXPORT(<exported name>)` on the next line
WRAP = re.compile(r"^(?:/\* signature: (\d+) words?, sha256 ([0-9a-f]{16}) \*/\r?\n)?"
                  r"HLE_WRAP(?:\(([0-9a-fA-F]+)\)|_EXPORT\(([A-Za-z_][A-Za-z0-9_]*)\))", re.M)
# any signature comment on the line before an HLE_WRAP, well-formed or not
ANY_SIGNATURE = re.compile(r"^/\* signature:.*\r?\nHLE_WRAP(?:_EXPORT)?\(", re.M)
# the old form, the masked words themselves (`/* signature: 0123abcd 4567ef89 9....... */`): the game's code
WORDS_SIGNATURE = re.compile(r"/\* signature: [0-9a-f.]{8}(?: |\*/)")
# C's comments, strings and characters (each one's text, which may seem to open another)
C_COMMENT = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|/\*.*?\*/|//[^\n]*', re.S)
# any name that starts as the macros' do, in code: `HLE_WRAP_EXPORT (x)`, indented, after
# `static`, `#define W HLE_WRAP_EXPORT`, `HLE_WRAP_##k`, a name a line splice cuts in two...
ANY_WRAP = re.compile(r"\bHLE_WRAP\w*")


def wrap_uses_unread(text):
    """Whether text names an HLE_WRAP macro in code (not in a comment or a string) other than
    at the start of a line in one of WRAP's forms, or has one of those forms inside a comment.
    Each compiles, or could, with the function left unwrapped without a word (an export's:
    nothing fails to link either). Not followed: the preprocessor's conditions (`#if 0`) and a
    line splice that makes a comment of the next line, under which a form WRAP reads is no wrap
    to the compiler (the build then fails to link, where a module has the function)."""
    code = C_COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group()), text)
    return ({text.rindex("HLE_WRAP", m.start(), m.end()) for m in WRAP.finditer(text)}
            != {m.start() for m in ANY_WRAP.finditer(code)})


def runtime_wraps():
    """The runtime's wraps, ({address: signature}, {exported name: signature}): `HLE_WRAP(<address>)`
    and `HLE_WRAP_EXPORT(<name>)` in its sources (hle_wraps, hle_export_wraps), in the order of the
    sources (file names', then lines'). A signature is (word count, hash), or None. A use of
    either macro not in its form at the start of a line, or another name in code that starts as
    theirs do (wrap_uses_unread), one wrapped twice and a name both replaced (HLE) and wrapped
    stop the build."""
    by_address, by_name, replaced = {}, {}, set()
    runtime = os.path.join(ROOT, "runtime")
    for name in sorted(os.listdir(runtime)):
        if name.endswith(".c") and not name.startswith("._"):
            with open(os.path.join(runtime, name), encoding="utf-8") as f:
                text = f.read()
            if WORDS_SIGNATURE.search(text):
                raise SystemExit(f"runtime/{name}: an HLE_WRAP signature spells out the game's instruction words; "
                                 "give its hash instead (`tools/recomp.py wrap-sig <module> <address>` prints it)")
            wraps = WRAP.findall(text)
            if len(ANY_SIGNATURE.findall(text)) != sum(1 for n, _d, _a, _s in wraps if n):
                raise SystemExit(f"runtime/{name}: an HLE_WRAP signature is not in the form "
                                 "`/* signature: <n> words, sha256 <16 hex digits> */`")
            if "HLE_WRAP" in text and wrap_uses_unread(text):
                raise SystemExit(f"runtime/{name}: an HLE_WRAP is not `HLE_WRAP(<hex address>)` or "
                                 "`HLE_WRAP_EXPORT(<exported name>)` at the start of a line, outside comments "
                                 "(and code names no other that starts as they do: a string or comment may)")
            replaced |= set(re.findall(r"^HLE\((\w+)\)", text, re.M))
            for n, digest, a, symbol in wraps:
                key, found = (int(a, 16), by_address) if a else (symbol, by_name)
                if key in found:
                    raise SystemExit(f"runtime/{name}: {f'HLE_WRAP({a})' if a else f'HLE_WRAP_EXPORT({symbol})'} "
                                     "is wrapped twice")
                found[key] = (int(n), digest) if n else None
    both = sorted(replaced & set(by_name))
    if both:
        raise SystemExit(f"runtime: {both[0]} is both replaced (HLE) and wrapped (HLE_WRAP_EXPORT): one or the other")
    return by_address, by_name


def hle_wraps():
    """Guest functions (not exported) wrapped natively: `HLE_WRAP(<address>)` in the runtime
    sources, address -> (word count, hash) of its code signature, or None. The function stays,
    as f_<address>_orig; its callers reach the native wrapper. The address is 13.0.5's; a
    comment `/* signature: <n> words, sha256 <16 hex digits> */` on the line before
    (`recomp.py wrap-sig` prints one) says what the function there begins with, which
    resolve_wraps checks. Only the hash goes in the source: the words themselves are the
    game's code (CONTRIBUTING.md, rule 1), so a comment that spells them out stops the build.
    A build of modules that lack it (a made-up game's: tools/mac/boottest) links its
    f_<address>_orig to rt_wrap_missing (link)."""
    return runtime_wraps()[0]


def hle_export_wraps():
    """Exported guest functions wrapped natively by name: `HLE_WRAP_EXPORT(<name>)` in the runtime
    sources, name -> signature as hle_wraps has it. For a function whose address is not the same
    in every install (a fighter module's: each install places those modules itself). Every
    module that exports a function under that name has it wrapped as an HLE_WRAP is: the
    function stays, as f_<address>_orig, and every caller reaches the wrapper. The wrapper is
    handed the original it was reached through (HLE_WRAP_EXPORT in host.h), so it names no
    module's function: two modules that define the name each keep their own, and a build in
    which no module defines it (until that module is compiled in, or a made-up game's) links
    as it is, with nothing for rt_wrap_missing to stand in for. A function exported under
    several of the names has each one's wrapper, one inside the other (resolve_wraps)."""
    return runtime_wraps()[1]


def signature(mod, off, size, n=16):
    """The code signature of the function at module offset `off`: its first n instructions as
    hex words, with the nibbles that depend on where code and data lie ('.') - the immediates
    of branches, adr/adrp and literal loads, and the page offsets added to an adrp's result."""
    out, pages = [], set()
    for i in range(min(n, size // 4)):
        w = mod.u32(off + 4 * i)
        mask = 0xFFFFFFFF
        if w & 0x7C000000 == 0x14000000:                                  # b, bl
            mask = 0xFC000000
        elif w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000 or w & 0x3B000000 == 0x18000000:
            mask = 0xFF00001F                                             # b.cond, cbz/cbnz, ldr literal
        elif w & 0x7E000000 == 0x36000000:                                # tbz/tbnz
            mask = 0xFFF8001F
        elif w & 0x1F000000 == 0x10000000:                                # adr, adrp
            mask = 0x9F00001F
            if w >> 31:
                pages.add(w & 31)
        elif (w >> 5) & 31 in pages and (w & 0x7F800000 == 0x11000000 or w & 0x3B000000 == 0x39000000):
            mask = 0xFFC003FF                                             # add / load / store at an adrp page
        out.append("".join(f"{w >> s & 15:x}" if mask >> s & 15 == 15 else "." for s in range(28, -4, -4)))
    return out


def signature_hash(words):
    """What an HLE_WRAP's signature comment holds: the first 16 hex digits of the SHA-256 of
    signature()'s masked words, space-separated. A check of the code, not a copy of it."""
    return hashlib.sha256(" ".join(words).encode("ascii")).hexdigest()[:16]


def signature_comment(words):
    """The comment `recomp.py wrap-sig` prints for the line before an HLE_WRAP."""
    return f"/* signature: {len(words)} word{'s' if len(words) != 1 else ''}, sha256 {signature_hash(words)} */"


def signature_matches(mod, off, size, sig):
    """Whether the function at module offset `off` begins as sig, (word count, hash), says. None: no check."""
    return sig is None or (len(words := signature(mod, off, size, sig[0])) == sig[0] and signature_hash(words) == sig[1])


def resolve_wraps(mod, base, sizes, name):
    """This module's wraps: {offset: ("wrap", wrapper)} for each HLE_WRAP at an address in it, and
    {offset: ("wrap", wrapper, "export", *inner)} for each function it exports under HLE_WRAP_EXPORT
    names. A function exported under several of them (aliases, or identical code folded into
    one) has each name's wrapper, in the order of the runtime's sources: callers reach the
    first, and each is handed the next (inner) as its original, the last the function itself.
    Each wrapped function must start at its address and begin as its signature says: the
    addresses are 13.0.5's, and one that isn't there (the runtime wrapping a function that moved)
    stops the build rather than leave a hook out. So do a wrapped name this module exports as
    something other than a function, and a function wrapped both by address and by name."""
    forward, by_export = {}, {}
    by_address, by_name = runtime_wraps()
    for a, sig in sorted(by_address.items()):
        if max((b for b in MODULES.values() if b <= a), default=None) != base:
            continue   # another module's
        off = a - base
        if off not in sizes:
            raise SystemExit(f"{name}: HLE_WRAP({a:x}): no function starts there in this module "
                             "(the runtime's hooks are for game version 13.0.5)")
        if not signature_matches(mod, off, sizes[off], sig):
            raise SystemExit(f"{name}: HLE_WRAP({a:x}): the function there does not begin as its signature says "
                             "(the runtime's hooks are for game version 13.0.5)")
        forward[off] = ("wrap", f"hle_wrap_{a:x}")
    for n, off, _size, info, _shndx in mod.exports if by_name else ():
        if n not in by_name:
            continue
        if info & 0xF != 2:
            raise SystemExit(f"{name}: HLE_WRAP_EXPORT({n}): this module exports that name, but not as a function")
        if off not in sizes:
            raise SystemExit(f"{name}: HLE_WRAP_EXPORT({n}): no function starts at its address {base + off:#x}")
        if not signature_matches(mod, off, sizes[off], by_name[n]):
            raise SystemExit(f"{name}: HLE_WRAP_EXPORT({n}): the function does not begin as its signature says "
                             "(the runtime's hooks are for game version 13.0.5)")
        if off in forward:
            raise SystemExit(f"{name}: HLE_WRAP_EXPORT({n}): the function at {base + off:#x} is wrapped already "
                             f"({forward[off][1]})")
        names = by_export.setdefault(off, [])
        if n not in names:   # (a name the symbol table has twice for it: one wrapper, once)
            names.append(n)
    order = {n: i for i, n in enumerate(by_name)}
    for off, names in by_export.items():
        first, *inner = (f"hle_wrap_{n}" for n in sorted(names, key=order.get))
        forward[off] = ("wrap", first, "export", *inner)
    return forward


HOOKS = os.path.join(ROOT, "tools", "hooks.txt")
HOOK_KINDS = ("exact-callee", "exact-caller", "hookable")
# (a hookable function is an exact-callee too: a hook in its slot sees its callers' registers)
EXACT_CALLEES = ("exact-callee", "hookable")
# `<kind> <address> [<n> words, sha256 <16 hex digits>] <name>`
HOOK_LINE = re.compile(r"(\S+)\s+(?:0x)?([0-9a-fA-F]+)(?:\s+(\d+) words?, sha256 ([0-9a-f]{16}))?\s+(\S+)")
HOOK_NAME = re.compile(r"[A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)*")
# What could be the game's instruction words (word_like), anywhere in a line: a run of hex
# digits, to count, maybe with digit separators (C++'s ', or _ after 0x: unprefixed, _ is a
# name's); 8 hex digits and dots, not inside a longer word, the form HLE_WRAP signatures once
# spelled the game's code in when the dots are a run of 4 or more (signature masks at least
# that much); and 4 or more bytes in a row, as hex dumps, C arrays and strings write them
# (`ff 83 00 d1`, `0xff, 0x83, ...`, `\xff\x83...`).
HEX_RUN = re.compile(r"(?<![0-9A-Fa-f])(?:(?<=0[xX])[0-9A-Fa-f]+(?:[_'][0-9A-Fa-f]+)*|[0-9A-Fa-f]+(?:'[0-9A-Fa-f]+)*)")
MASKED_WORD = re.compile(r"(?<![0-9A-Za-z])(?:0[xX])?([0-9A-Fa-f.]{8})(?![0-9A-Za-z])")
BYTE_ROW = re.compile(r"(?<![0-9A-Za-z])(?:(?:0[xX]|\\x)?[0-9A-Fa-f]{2}(?:, ?| |(?=\\x))){3,}(?:0[xX]|\\x)?[0-9A-Fa-f]{2}"
                      r"(?![0-9A-Za-z])")


def word_like(text):
    """What in text could be one of the game's instruction words, or None: a run of 8 or more
    hex digits (a word, or words run together as a hex editor copies them), a masked word, or a
    row of bytes. The only runs that long the list needs are an address (10 digits, 7xxxxxxxxx)
    and a signature's hash (16 digits, after `sha256 `)."""
    for m in HEX_RUN.finditer(text):
        run = m.group()
        if len(re.sub(r"[_']", "", run)) < 8 or (len(run) == 10 and run[0] == "7") or (
                len(run) == 16 and text[:m.start()].endswith("sha256 ")):
            continue
        return run
    for m in MASKED_WORD.finditer(text):
        if "...." in m.group(1) and re.search(r"[0-9A-Fa-f]", m.group(1)):
            return m.group()
    m = BYTE_ROW.search(text)
    return m.group() if m else None


def hook_list(path=None):
    """tools/hooks.txt: {(kind, address): (word count, hash) of its signature, or None}. The
    functions whose callers' whole register state a hook needs (resolve_hooks); see the file.
    Read strictly: a line that isn't an entry, a comment or blank, or that holds anything like
    an instruction word (CONTRIBUTING.md, rule 1: word_like), stops the build."""
    path = path or HOOKS
    if not os.path.exists(path):
        return {}
    found = {}
    try:
        with open(path, encoding="utf-8-sig") as f:   # (as some Windows editors save it: with a BOM)
            lines = f.read().splitlines()
    except UnicodeDecodeError as e:
        raise SystemExit(f"tools/hooks.txt: not UTF-8 ({e.reason} at byte {e.start}): save it as UTF-8") from None
    for n, line in enumerate(lines, 1):
        where = f"tools/hooks.txt line {n}"
        if found_word := word_like(line):
            raise SystemExit(f"{where}: `{found_word}` could be the game's instruction words, which never go in the "
                             "repository (CONTRIBUTING.md, rule 1). Hex digits go only in an address (13.0.5's, "
                             "71xxxxxxxx) and a signature's hash (`<n> words, sha256 <16 hex digits>`, as "
                             "`tools/recomp.py wrap-sig <module> <address>` prints it)")
        text = line.split("#", 1)[0].strip()
        if not text:
            continue
        m = HOOK_LINE.fullmatch(text)
        if not m or m.group(1) not in HOOK_KINDS:
            raise SystemExit(f"{where}: not `exact-callee|exact-caller|hookable <address> [<n> words, sha256 <16 hex "
                             f"digits>] <name>`: {text}")
        kind, a, count, digest, name = m.groups()
        a = int(a, 16)
        if not MODULES["main"] <= a < modcache.NRO_BASE or a & 3:
            raise SystemExit(f"{where}: {a:#x} is not a function address in main, subsdk0 or sdk (a fighter "
                             "module's address is each install's own: dump/nro/bases.txt)")
        if count is not None and int(count) < 1:
            raise SystemExit(f"{where}: a signature covers at least one word")
        if not HOOK_NAME.fullmatch(name) or re.fullmatch(r"[0-9a-fA-F]{8,}", name):
            raise SystemExit(f"{where}: `{name}` is not a function name")
        if (kind, a) in found:
            raise SystemExit(f"{where}: {kind} {a:x} is listed twice")
        if kind in EXACT_CALLEES and any((k, a) in found for k in EXACT_CALLEES):
            raise SystemExit(f"{where}: {a:x} is both hookable and an exact-callee: a hookable function is an "
                             "exact-callee already, so list it once, as hookable")
        found[(kind, a)] = (int(count), digest) if count else None
    return found


# The modules loaded at start, in the order runtime/host.c publishes their exports
BOOT_MODULES = ("main", "subsdk0", "sdk")


def hook_imports(mods, graphs=None):
    """{symbol: address} for each name the loader binds to code that returns into the caller of
    an exact-callee of tools/hooks.txt (a hookable function is one too): the exact-callee
    itself, or a function that reaches one by tail calls (an exported wrapper ending in
    `b impl`), maybe through import stubs bound to
    such a name in turn. Other modules call these by name, through an import stub, so those
    stubs are exact-callees too (resolve_hooks); and a module's stub joining its tail calls can
    bring in more of its exports, so this goes round until nothing more is added.

    The first of main, subsdk0 and sdk to export a name defines it (runtime/host.c
    publish_exports). The NROs come later and define only names none of those does, each the
    first of them loaded, so their exports are not followed here. mods: name -> nso.Module;
    graphs: name -> (stub_symbols, tail_graph) of main, subsdk0 and sdk (by default
    module_graph)."""
    listed = {a for kind, a in hook_list() if kind in EXACT_CALLEES}
    boot = [name for name in BOOT_MODULES if name in mods] if listed else []
    bound = {}
    for name in boot:
        for n, v, _s, _info, _x in mods[name].exports:
            bound.setdefault(n, MODULES[name] + v)
    if graphs is None:
        graphs = {name: module_graph(name, mods[name]) for name in boot}
    names = set()
    while True:
        reached = set(listed)
        for name in boot:
            base, (stubs, tails) = MODULES[name], graphs[name]
            seeds = {a - base for a in listed if module_base(a) == base} | {s for s, sym in stubs.items() if sym in names}
            reached |= {base + f for f in close_over_tail_calls(seeds, tails)}
        more = {n for n, a in bound.items() if a in reached}
        if more == names:
            return {n: bound[n] for n in sorted(names)}
        names = more


def module_base(a):
    """The load address of the module (main, subsdk0, sdk or an NRO) address a is in."""
    return max((b for b in MODULES.values() if b <= a), default=None)


def import_stub_symbols(mod):
    """{import stub offset: the symbol its GOT slot is bound to}."""
    from funcs import import_stubs
    slots = {r[0]: mod.symbols[r[2]][0] for r in mod.relocs if r[2]}
    return {stub: slots[slot] for stub, slot in import_stubs(mod).items() if slot in slots}


def native_only(forward):
    """The functions of a module's forward (forwards) whose code never runs: replaced by a
    native function (a wrap's original still runs, called by its wrapper)."""
    return {a for a, v in forward.items() if isinstance(v, str)}


def stub_symbols(mod, sizes):
    """{import stub offset: the symbol it is bound to}, for the stubs that are functions here."""
    return {stub: sym for stub, sym in import_stub_symbols(mod).items() if stub in sizes}


def tail_graph(mod, sizes, entries=None, native=()):
    """{function: the functions it jumps to without a call} (lift.tail_calls, with the Lifter's
    entries: by default the function starts). A function replaced natively (native:
    native_only) runs none of its own code, so its jumps are left out: they would only make
    more calls exact than need be."""
    from lift import tail_calls
    return tail_calls(mod.image, sorted((a, s) for a, s in sizes.items() if a not in native), entries or set(sizes))


def module_graph(name, mod):
    """How a module (main, subsdk0, sdk) reaches an exact-callee without a call of its own, for
    hook_imports: (stub_symbols, tail_graph), from its functions as build lifts them."""
    found, funcs = functions(mod)
    sizes = dict(funcs)
    native = native_only(forwards(mod, MODULES[name], found, funcs, name))
    return stub_symbols(mod, sizes), tail_graph(mod, sizes, set(found), native)


def close_over_tail_calls(callees, tails):
    """callees and every function that reaches one of them by tail calls ({function: the
    functions it jumps to}, lift.tail_calls)."""
    jumpers = {}
    for f, targets in tails.items():
        for t in targets:
            jumpers.setdefault(t, set()).add(f)
    out, work = set(callees), list(callees)
    while work:
        for f in jumpers.get(work.pop(), ()):
            if f not in out:
                out.add(f)
                work.append(f)
    return out


def resolve_hooks(mod, base, sizes, name, entries=None, imports=None, native=()):
    """This module's entries of tools/hooks.txt: (exact-callee offsets, exact-caller offsets),
    for the Lifter. As with a wrap, each must be a function start that begins as its signature
    (if any) says: one that isn't stops the build rather than leave a hook's registers stale.

    A hook's registers are those of the call it returns to (its LR), so exact-callees are also:
    this module's import stubs bound to a name in imports (hook_imports: the exact-callees'
    names, and those of functions that reach one), which calls from another module go through;
    and every function that reaches one by tail calls (tail_graph, with the Lifter's entries,
    by default the function starts, and the functions replaced natively: native_only), since
    what it reaches returns to the caller of the function that jumped. A hookable function is
    an exact-callee (hookables gives the Lifter its slots)."""
    exact = {kind: set() for kind in HOOK_KINDS}
    for (kind, a), sig in sorted(hook_list().items()):
        if module_base(a) != base:
            continue   # another module's
        off = a - base
        if off not in sizes:
            raise SystemExit(f"{name}: tools/hooks.txt: {kind} {a:x}: no function starts there in this module "
                             "(the list is for game version 13.0.5)")
        if not signature_matches(mod, off, sizes[off], sig):
            raise SystemExit(f"{name}: tools/hooks.txt: {kind} {a:x}: the function there does not begin as its "
                             "signature says (the list is for game version 13.0.5)")
        exact[kind].add(off)
    callees = exact["exact-callee"] | exact["hookable"]
    if imports:
        callees |= {stub for stub, sym in stub_symbols(mod, sizes).items() if sym in imports}
    if callees:
        callees = close_over_tail_calls(callees, tail_graph(mod, sizes, entries, native))
    return callees, exact["exact-caller"]


def hookables(base, setjmp=()):
    """This module's hookable functions of tools/hooks.txt (offsets), each reached only through
    its slot (lift.Lifter.entry; runtime/hookable.h). resolve_hooks checks they are functions.
    A call to setjmp becomes the host's setjmp (setjmp_targets), which no slot could stand in
    for, so listing one stops the build."""
    found = {a - base for kind, a in hook_list() if kind == "hookable" and module_base(a) == base}
    if found & set(setjmp):
        raise SystemExit(f"tools/hooks.txt: hookable {base + min(found & set(setjmp)):x}: calls to it become the "
                         "host's setjmp, which cannot go through a slot")
    return found


def tools_fingerprint():
    """What the generated C depends on besides the module and its address (modcache)."""
    import lift
    by_address, by_name = runtime_wraps()
    wraps = sorted(by_address.items())
    hooks = sorted(hook_list().items())
    return modcache.fingerprint("\n".join(sorted(hle_names())) + "\n" + repr(wraps) + "\n" + repr(hooks)
                                + f"\nregcache {int(lift.REGCACHE)}"
                                # (in the sources' order: the order of the wrappers of a function exported under several)
                                + (f"\nexport wraps {list(by_name.items())!r}" if by_name else ""))


def write_if_changed(path, text):
    """Leave an identical file untouched so its object stays up to date. A changed one is
    written as a new file (modcache.replace_file)."""
    if os.path.exists(path):
        with open(path, encoding="utf-8", newline="") as f:
            if f.read() == text:
                return False
    modcache.replace_file(path, text)
    return True


def _init(image_path, entries, base, forward, setjmp=None, pads=None, exact_callees=None, exact_callers=None,
          hookable=None):
    f = open(image_path, "rb")
    _state["img"] = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    _state["args"] = (entries, base, forward, setjmp, pads, exact_callees, exact_callers, hookable)


def unwind_jumpto(mod, found):
    """libunwind's Registers_arm64::jumpto: loads every register from the context it is given
    and returns into the frame being resumed. That cannot work as a host return, so it is
    replaced natively; found by its code (ldp x2, x3, [x0, #0x10] ... ldr x30, [x0, #0x100])."""
    text = bytes(mod.image[mod.text.mem:mod.text.end])
    for m in re.finditer(re.escape(struct.pack("<I", 0xA9410C02)), text):
        if m.start() % 4:
            continue
        a = mod.text.mem + m.start()
        if a in found and struct.pack("<I", 0xF940801E) in text[m.start():m.start() + 0x40]:
            return {a: "rt_unwind_jumpto"}
    return {}


SETJMP_NAMES = {"setjmp", "_setjmp", "__setjmp"}


def setjmp_targets(mod):
    """Where calls to setjmp land in this module: its own definitions and its import stubs."""
    targets = {v for n, v, _s, info, _x in mod.exports if n in SETJMP_NAMES}
    targets |= {stub for stub, sym in import_stub_symbols(mod).items() if sym in SETJMP_NAMES}
    return targets


def _lifter():
    return Lifter(_state["img"], *_state["args"])


def _cover(funcs):
    lifter = _lifter()
    ok = ok_insns = insns = bad_insns = 0
    reasons = Counter()
    blocked = Counter()
    for start, size in funcs:
        _code, why, _callees = lifter.lift_function(start, size)
        insns += size // 4
        if why:
            bad_insns += sum(why.values())
            reasons.update(why)
            kinds = {r for r, _w in why}
            if len(kinds) == 1:
                blocked[next(iter(kinds))] += 1
        else:
            ok += 1
            ok_insns += size // 4
    return (ok, ok_insns, insns, bad_insns, reasons, blocked, lifter.tables, lifter.indirect_tail,
            lifter.table_like, lifter.unresolved, lifter.assumed)


def _emit(args):
    index, funcs, outdir = args
    lifter = _lifter()
    own = {a for a, _ in funcs}
    bodies, ext, bad = [], set(), 0
    for start, size in funcs:
        code, why, callees = lifter.lift_function(start, size)
        bad += sum(why.values())
        bodies.append(code)
        ext |= callees
    name = f"g{index:04d}.c"
    slots = sorted(lifter.hookable & (ext | own))   # (the hookable functions it calls or defines)
    text = ('#include "recomp.h"\n' + ('#include "hookable.h"\n' if slots else "") + "\n"
            + "".join(f"GuestFn f_{lifter.base + a:x};\n" for a in sorted(ext | own))
            + "".join(f"extern RtHook rt_hook_{lifter.base + a:x};\n" for a in slots)
            + "\n" + "\n\n".join(bodies) + "\n")
    # Each listed function reached only through its slot. The list is read again here, not taken
    # from the Lifter, so a build that lost the Lifter's hookable set (build's Pool arguments out
    # of step with _init's) stops rather than generate plain calls.
    problems, listed = [], lifter.hookable | hookables(lifter.base)
    if listed:
        import hook_inline_check
        problems = hook_inline_check.check_c({name: text}, {lifter.base + a for a in listed}, whole=False)
    write_if_changed(os.path.join(outdir, name), text)
    insns = sum(s for _, s in funcs) // 4
    return name, ext - own, insns, max(s for _, s in funcs) // 4, bad, lifter.unresolved, problems


def load(path):
    """(module, {entry: sources}, [(entry, size)]) with every function that has code."""
    mod = Module(path)
    return (mod, *functions(mod))


def functions(mod):
    """({entry: sources}, [(entry, size)]) with every function of a module that has code."""
    found, sizes = discover(mod)
    funcs = sorted((a, sizes[a]) for a, src in found.items() if "eh" in src or "plt" in src)
    # Hand-written assembly (system-call wrappers, memcpy, setjmp, ...) has no unwind entry.
    # Such a routine runs from its entry to the next known entry or described function,
    # less any trailing padding.
    starts = [a for a, _ in funcs]
    ends = [a + s for a, s in funcs]

    def described(a):
        k = bisect.bisect_right(starts, a) - 1
        return k >= 0 and a < ends[k]

    # Compiled code also reaches such routines by tail-jump alone; a `b` out of a described
    # function to somewhere no description covers is an entry too. (A `b` inside an assembly
    # routine is left alone: those are its own internal labels.)
    text = bytes(mod.image[mod.text.mem:mod.text.end])
    for m in re.finditer(rb"[\x14-\x17]", text[3::4]):
        src = mod.text.mem + m.start() * 4
        imm = struct.unpack_from("<I", text, m.start() * 4)[0] & 0x3FFFFFF
        target = src + (imm - 0x4000000 if imm & 0x2000000 else imm) * 4
        if mod.text.mem <= target < mod.text.end and target not in found and described(src) and not described(target):
            found[target] = {"b"}
    marks = sorted(set(found) | set(starts) | {mod.text.end})
    extra = []
    for a in sorted(found):
        if "eh" in found[a] or "plt" in found[a] or not mod.text.mem <= a < mod.text.end or a & 3:
            continue
        if described(a):
            continue   # an address inside a described function: a data pointer, not an entry
        end = marks[bisect.bisect_right(marks, a)]
        while end > a and mod.u32(end - 4) in (0, 0xD503201F, 0xE7FFDEFE):
            end -= 4
        if end > a:
            extra.append((a, end - a))
    return found, sorted(funcs + extra)


def image_file(mod):
    build_dir = os.path.join(ROOT, "build")
    os.makedirs(build_dir, exist_ok=True)
    path = os.path.join(build_dir, f"image-{mod.build_id[:16]}.img")
    if not os.path.exists(path) or os.path.getsize(path) != len(mod.image):
        with open(path, "wb") as f:
            f.write(mod.image)
    return path


def coverage(path, jobs):
    import capstone
    mod, found, funcs = load(path)
    step = 400
    chunks = [funcs[i:i + step] for i in range(0, len(funcs), step)]
    ok = ok_insns = insns = bad = tables = tails = table_like = assumed = 0
    reasons, blocked, unresolved = Counter(), Counter(), []
    with Pool(jobs, _init, (image_file(mod), set(found), base_of(path), {})) as pool:
        for r in pool.imap_unordered(_cover, chunks):
            ok += r[0]; ok_insns += r[1]; insns += r[2]; bad += r[3]
            reasons.update(r[4]); blocked.update(r[5])
            tables += r[6]; tails += r[7]; table_like += r[8]; unresolved += r[9]; assumed += r[10]

    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    by_kind, by_mnem = Counter(), Counter()
    for (why, w), n in reasons.items():
        by_kind[why] += n
        d = next(cs.disasm_lite(struct.pack("<I", w), 0), None)
        by_mnem[(why, d[2] if d else "?")] += n

    print(f"functions lifted completely : {ok:,} / {len(funcs):,} ({ok * 100 / len(funcs):.1f}%)")
    print(f"instructions lifted         : {insns - bad:,} / {insns:,} ({(insns - bad) * 100 / insns:.2f}%)")
    print(f"instructions in clean funcs : {ok_insns:,} ({ok_insns * 100 / insns:.1f}%)")
    print(f"jump tables resolved        : {tables:,} of {table_like:,} table-style dispatches"
          f" ({assumed:,} by validation rather than proof)    br treated as indirect tail call: {tails:,}")
    if unresolved:
        print("unresolved table dispatches   : " + " ".join(f"{a:#x}" for a in sorted(unresolved)[:40]))
    if by_kind:
        print("\nunsupported instructions by reason:")
        for why, n in by_kind.most_common():
            print(f"  {n:>9,}  {why:<42} sole blocker of {blocked[why]:,} functions")
        print("\ntop unsupported mnemonics:")
        for i, ((why, m), n) in enumerate(by_mnem.most_common(48)):
            print(f"  {m:<9}{n:>8,}", end="\n" if i % 6 == 5 else "")
        print()


def emit(path, roots, out, depth, sample=0):
    mod, found, funcs = load(path)
    sizes = dict(funcs)
    lifter = Lifter(mod.image, set(found), base_of(path))
    base = lifter.base
    if sample:   # every k-th function that lifts cleanly, callees left as stubs
        clean = [a for a, size in funcs[::max(1, len(funcs) // (sample * 2))]
                 if not lifter.lift_function(a, size)[1]]
        roots, depth = clean[:sample], 0
    bodies, stubs, seen = {}, set(), set()
    frontier = list(roots)
    for _ in range(depth + 1):
        nxt = []
        for a in frontier:
            if a in seen:
                continue
            seen.add(a)
            if a not in sizes:
                stubs.add(a)
                continue
            code, why, callees = lifter.lift_function(a, sizes[a])
            if why:
                print(f"  f_{base + a:x}: {sum(why.values())} unsupported instruction(s) -> traps at runtime")
            bodies[a] = code
            nxt.extend(callees)
        frontier = nxt
    stubs |= {a for a in frontier if a not in seen}
    with open(out, "w") as f:
        f.write('#include "recomp.h"\n\n')
        for a in sorted(set(bodies) | stubs):
            f.write(f"void f_{base + a:x}(Cpu *c);\n")
        f.write("\n")
        for a in sorted(stubs):
            f.write(f'void f_{base + a:x}(Cpu *c) {{ rt_trap(c, 0x{base + a:X}ull, "function not recompiled"); }}\n')
        for a in sorted(bodies):
            f.write("\n" + bodies[a] + "\n")
        f.write("\nconst struct RtFunc { u64 addr; GuestFn *fn; } rt_funcs[] = {\n")
        for a in sorted(bodies):
            f.write(f"    {{0x{base + a:X}ull, f_{base + a:x}}},\n")
        f.write(f"}};\nconst u32 rt_func_count = {len(bodies)};\n")
    print(f"wrote {out}: {len(bodies)} functions, {len(stubs)} stubs")


def image(path, out):
    mod, base = Module(path), base_of(path)
    img = bytearray(mod.image) + bytes(mod.mem_end - len(mod.image))
    n = 0
    for off, typ, _sym, addend in mod.relocs:
        if typ == 1027:
            struct.pack_into("<Q", img, off, base + addend)
            n += 1
    with open(out, "wb") as f:
        f.write(img)
    print(f"wrote {out}: {len(img):,} bytes, {n:,} base relocations applied for {base:#x}")


def module_table(mod, name, base, funcs, missing):
    """C source for a module's loader tables: functions, exports, and symbol relocations."""
    imports, relocs = {}, []
    for off, typ, sym, addend in mod.relocs:
        if typ == 1027:
            continue
        sname, _value, _size, info, _shndx = mod.symbols[sym]
        index = imports.setdefault(sname, (len(imports), info >> 4 == 2))[0]
        relocs.append((off, addend, index))
    defined = [a for a, _ in funcs]
    out = ['#include "module.h"\n\n']
    out += [f"GuestFn f_{base + a:x};\n" for a in defined]
    out += [f'void f_{base + a:x}(Cpu *c) {{ rt_trap(c, 0x{base + a:X}ull, "no code at call target"); }}\n'
            for a in sorted(missing)]
    out.append("\nstatic const RtFunc funcs[] = {\n")
    out += [f"    {{0x{base + a:X}ull, f_{base + a:x}}},\n" for a in defined]
    out.append("};\nstatic const RtSym exports[] = {\n")
    out += [f'    {{"{n}", 0x{base + v:X}ull}},\n' for n, v, _s, _i, _x in mod.exports]
    out.append("};\nstatic const char *const imports[] = {\n")
    out += [f'    "{n}",\n' for n in imports]
    out.append("    0\n};\nstatic const u8 import_weak[] = {" + ",".join(str(int(w)) for _i, w in imports.values()) + ",0};\n")
    out.append("static const RtReloc relocs[] = {\n")
    out += [f"    {{0x{o:X}ull, {a}, {i}}},\n" for o, a, i in relocs]
    out.append("    {0, 0, 0}\n};\n\n")
    out.append(f"const RtModule rt_module_{name} = {{\n"
               f'    "{name}", 0x{base:X}ull, 0x{mod.mem_end:X}ull, 0x{mod.text.end:X}ull, 0x{base + mod.dyn.get(12, 0):X}ull,\n'
               f"    funcs, {len(defined)}, exports, {len(mod.exports)}, imports, import_weak, {len(imports)}, "
               f'relocs, {len(relocs)}, "{mod.build_id}",\n}};\n')
    return "".join(out), len(imports), len(relocs)


def forwards(mod, base, found, funcs, name):
    """{offset: what replaces the function there}: the native function of an exported name
    the runtime implements (hle_names), libunwind's jumpto, and the wraps (resolve_wraps). A
    function both replaced and wrapped (two names for it) stops the build."""
    hle = hle_names()
    forward = {v: f"hle_{n}" for n, v, _s, info, _x in mod.exports if n in hle and info & 0xF == 2}
    forward.update(unwind_jumpto(mod, found))
    for off, wrap in sorted(resolve_wraps(mod, base, dict(funcs), name).items()):
        if off in forward:
            raise SystemExit(f"{name}: the function at {base + off:#x} is both replaced ({forward[off]}) "
                             f"and wrapped ({wrap[1]})")
        forward[off] = wrap
    return forward


def build(path, outdir, jobs, per_file, imports=None):
    """imports: hook_imports, for this module's import stubs (none: the stubs are not exact)."""
    mod, found, funcs = load(path)
    name, base = module_key(path), base_of(path)
    os.makedirs(outdir, exist_ok=True)
    modcache.clear_stamp(outdir)   # (until generated and compiled again)
    forward = forwards(mod, base, found, funcs, name)
    exact_callees, exact_callers = resolve_hooks(mod, base, dict(funcs), name, set(found), imports, native_only(forward))
    setjmp = setjmp_targets(mod)
    hookable = hookables(base, setjmp)
    from eh import landing_pads
    pads = landing_pads(mod)
    chunks, cur, count = [], [], 0
    for a, size in funcs:   # a function bigger than the budget gets a file to itself
        if cur and count + size // 4 > per_file:
            chunks.append(cur)
            cur, count = [], 0
        cur.append((a, size))
        count += size // 4
    if cur:
        chunks.append(cur)
    defined = {a for a, _ in funcs}
    missing, manifest, bad, unresolved, problems = set(), [], 0, [], []
    with Pool(jobs, _init, (image_file(mod), set(found), base, forward, setjmp, pads, exact_callees, exact_callers,
                            hookable)) as pool:
        tasks = [(i, c, outdir) for i, c in enumerate(chunks)]
        for fname, ext, insns, biggest, b, unres, probs in pool.imap_unordered(_emit, tasks):
            missing |= ext - defined
            manifest.append((fname, insns, biggest))
            bad += b
            unresolved += unres
            problems += probs
    if problems:   # (the stamp stays cleared: the next build generates the C again)
        raise SystemExit(f"{name}: a hookable function is reached other than through its slot (tools/hook_inline_check.py): "
                         + "; ".join(sorted(problems)[:5]))
    table, imports, relocs = module_table(mod, name, base, funcs, missing)
    write_if_changed(os.path.join(outdir, "g_module.c"), table)
    keep = {n for n, _i, _b in manifest} | {"g_module.c"}
    for old in os.listdir(outdir):   # drop files left over from a different partitioning
        if old.startswith("g") and old.endswith((".c", ".obj")) and os.path.splitext(old)[0] + ".c" not in keep:
            os.remove(os.path.join(outdir, old))
    write_if_changed(os.path.join(outdir, "manifest.tsv"), "".join(f"{n}\t{i}\t{b}\n" for n, i, b in sorted(manifest)))
    size = sum(os.path.getsize(os.path.join(outdir, n)) for n, _i, _b in manifest)
    print(f"{name}: {len(manifest)} files, {size / 1048576:,.0f} MB of C, {len(defined):,} functions, "
          f"{sum(i for _n, i, _b in manifest):,} instructions; {bad} untranslated, {len(missing)} calls to nowhere, "
          f"{len(unresolved)} unresolved jump tables, {len(forward)} replaced natively; "
          f"{imports:,} symbols referenced by {relocs:,} relocations" + (f"; {len(hookable)} hookable" if hookable else ""))


def build_folder(outdir):
    """The build folder a module folder is in (build_all's <out>/<module> or <out>/nro/<module>), whose
    record (tools/isa.py: isa.txt, which link writes) says what its objects are compiled for."""
    out = os.path.dirname(os.path.abspath(outdir))
    return os.path.dirname(out) if os.path.basename(out) == "nro" else out


def plan(outdir, times=None):
    """(stale_files, the folder's objcache.Record, {object: (its key, key_for(step): its key compiled a
    ladder step's way)}, Counter of why each stale one is, {object: its ladder.Info}). A generated file
    includes recomp.h, one with hook slots hookable.h too, g_module.c module.h (_emit, module_table;
    objcache.game_headers). The key's command names the file and its object by their names in the
    folder: the folder spelled another way (build/main, .\\build\\main, a full path), or moved,
    compiles nothing again. times: the build folder's ladder.Times (read when not given), which the
    estimates go by. In the manifest's order (with SSBU_LONGEST_FIRST=1, once one of them is
    measured, the longest first: ladder.longest_first)."""
    import cc
    with open(os.path.join(outdir, "manifest.tsv")) as f:
        rows = [line.split("\t") for line in f.read().splitlines()]
    # (a folder an older version built, without a record: its objects that the date said were current are kept -
    # only if they were compiled with today's flags, for today's instruction set: isa.adopts, which no target is today)
    adopt = (objcache.header_files(cc.RUNTIME, "recomp.h", "module.h")
             if isa.adopts(build_folder(outdir), cc.ISA, isa.CLANG_CL[cc.ISA], isa.LEGACY_CLANG_CL) else None)
    record = objcache.Record(outdir, scan=(objcache.HOOK_SLOTS,), adopt_by_date=adopt,
                             adopt_found={objcache.HOOK_SLOTS: objcache.header_files(cc.RUNTIME, "hookable.h")})
    compiler = objcache.compiler_id(cc.CLANG)
    headers = {h: objcache.digest_files(objcache.header_files(cc.RUNTIME, *h)) for h in objcache.GAME_HEADERS}
    times = times or ladder.Times(build_folder(outdir))
    above = ladder.optnone_setting()
    todo, keys, infos, why = [], {}, {}, Counter()
    for name, insns, biggest in rows + [["g_module.c", "0", "0"]]:
        src = os.path.join(outdir, name)
        obj = src[:-2] + ".obj"
        insns, biggest = int(insns), int(biggest)
        own = ladder.eager(src, biggest, above) if name != "g_module.c" else None   # (SSBU_OPTNONE_ABOVE's)
        left = own.left if own else biggest
        # very large functions make the optimiser superlinear; build those at a lower level
        opt = "/O1" if left > 20000 or name == "g_module.c" else "/O2"
        key = game_key(record, src, compiler, headers[objcache.game_headers(name, record.source(src)[1])])
        start = own.above if own else 0
        keys[obj] = (key(opt, own.header if own else None),
                     lambda step, key=key, start=start: ladder.ladder_entry(key(step.level, step.header), step, start))
        reason = ladder.check(record, obj, *keys[obj], src, start)
        if reason:
            why[reason] += 1
            model = progress.TABLE if name == "g_module.c" else progress.file_cost(insns, left)
            rel = os.path.relpath(src, build_folder(outdir)).replace(os.sep, "/")
            digest = record.source(src)[0]
            measured = times.measured(rel, digest, opt)
            estimate = model if measured is None else measured
            infos[obj] = ladder.Info(src, rel, insns, biggest, False, model, estimate, digest,
                                     *((own.above, own.header, own.names, own.left) if own else ()),
                                     measured=measured is not None)
            todo.append((src, obj, opt, estimate))
    record.save()   # (the sources' digests: the next look reads only the files that changed)
    # (SSBU_LONGEST_FIRST=1, for benchmarking: the longest first, once they have been measured)
    todo = ladder.longest_first(todo, infos) or todo
    return todo, record, keys, why, infos


def game_key(record, src, compiler, headers):
    """key(level, header): a generated file's objcache key compiled at that level, with that optnone
    header's text (or None: tools/ladder.py), its command naming it and its object by their names in
    their folder."""
    import cc
    name = os.path.basename(src)
    obj = name[:-2] + ".obj"
    return lambda level, header: record.entry(
        [src], ladder.key_command(cc.compile_command(name, obj, level, cc.GAME_CFLAGS, ladder.header_name(obj) if header else None),
                                  header), compiler, headers)


def stale_files(outdir):
    """What compile_dir compiles, in order (plan's): (source, object, optimisation level,
    estimated seconds: what it took last time, else progress.file_cost) of each file whose object is
    missing or not current: compiled from other C, headers or flags, or by another compiler (objcache.py)."""
    return plan(outdir)[0]


def compile_dir(outdir, jobs, shown=None, steps=None):
    """Compile a module's stale files; `shown`, a progress.Build, follows them. steps: the build's
    ladder.Ladder (tools/ladder.py), else one of its own, whose report is said at the end."""
    import cc
    ladder.settings()
    times = ladder.Times(build_folder(outdir))
    todo, record, keys, why, infos = plan(outdir, times)
    if objcache.summary(why):
        print(f"{os.path.basename(outdir)}: {objcache.summary(why)}")
    record.forget([obj for _src, obj, _opt, _cost in todo])
    for _src, obj, _opt, _cost in todo:   # a new file, not one written through (it may be a hard link: modcache.replace_file)
        if os.path.exists(obj):
            os.remove(obj)
    t0 = time.time()
    if shown:
        shown.begin("compile", os.path.basename(outdir), [(src, cost) for src, _obj, _opt, cost in todo])

    def compiled(job, _level, step=None):
        # under the key of its own level even when a crash had it compiled at /O1 instead (cc.RETRY): kept until what
        # it is built from changes, rather than compiled again at every build in case the compiler no longer crashes;
        # one the ladder compiled another way, under the key of that way (ladder.check)
        key, key_for = keys[job[1]]
        record.compiled(job[1], key if step is None else key_for(step))
    own = steps is None
    steps = steps or ladder.Ladder(progress.level_cost, "clang-cl")
    failures = cc.compile_each([t[:3] for t in todo], jobs, *((shown.started, shown.finished) if shown else ()),
                               compiled=compiled, flags=cc.GAME_CFLAGS, shown=shown, infos=infos, steps=steps, times=times)
    if shown:
        shown.end("compile", os.path.basename(outdir))
    objs = [n for n in os.listdir(outdir) if n.endswith(".obj")]
    total = sum(os.path.getsize(os.path.join(outdir, n)) for n in objs)
    print(f"{os.path.basename(outdir)}: compiled {len(todo) - len(failures)} of {len(todo)} stale files in "
          f"{time.time() - t0:,.0f}s; {len(objs)} objects, {total / 1048576:,.0f} MB")
    for src, err in failures[:5]:
        print(f"FAILED {os.path.basename(src)}:\n{err[:1500]}")
    if own:
        steps.end(build_folder(outdir))
    return not failures


def link(dirs, exe, rt_dir=None, steps=None):
    """Compile the host runtime and link it with the given module build directories. The
    runtime's objects go to rt_dir (default: rt beside the exe). steps: the build's ladder.Ladder
    (a runtime file the compiler crashed on is in its report), else one of its own, whose report
    is said once the runtime is compiled (and written beside the exe). With GAME_DLL the modules
    are linked into game_dll.DLL beside the exe instead, with the library's side of the interface
    (game_tables.c) and the NROs' list, and the exe is the runtime with the program's side
    (game_funcs.c): tools/game_dll.py. The game's objects are the same either way."""
    import cc
    t0 = time.time()
    rt_dir = os.path.abspath(rt_dir or os.path.join(os.path.dirname(exe), "rt"))   # (one spelling: its objects' keys name it)
    os.makedirs(rt_dir, exist_ok=True)
    # An object is compiled again when its source, a runtime header, the compiler or its compile command
    # changes (objcache.py): cc.CFLAGS change as SDL3, libopus or libvpx come and go.
    record, compiler = objcache.Record(rt_dir), objcache.compiler_id(cc.CLANG)
    headers = objcache.digest_files([os.path.join(cc.RUNTIME, n) for n in os.listdir(cc.RUNTIME) if n.endswith(".h")])
    jobs, keys, why = {}, {}, Counter()   # (jobs: by their flags, None for cc.CFLAGS)
    print(f"instruction set: {cc.ISA} ({cc.ISA_WHY})")

    def want(src, obj, opt, flags=None):
        keys[obj] = record.entry([src], cc.compile_command(src, obj, opt, flags), compiler, headers)
        reason = record.check(obj, keys[obj])
        if reason:
            why[reason] += 1
            jobs.setdefault(None if flags is None else tuple(flags), []).append((src, obj, opt))
    rt_flags = [*cc.CFLAGS, "/D" + game_dll.DEFINE] if GAME_DLL else None   # (its runtime loads the library: runtime/host.c)
    sources = [n for n in os.listdir(cc.RUNTIME) if re.fullmatch(r"(host|kernel|gpu\w*|hle\w*|ipc\w*|debug\w*|net\w*)\.c", n)]
    for n in sources:
        want(os.path.join(cc.RUNTIME, n), os.path.join(rt_dir, n[:-2] + ".obj"), "/O2", rt_flags)
    # the processor check, for any x64 processor (it runs before the rest: runtime/cpu_check.c)
    check = os.path.join(rt_dir, "cpu_check.obj")
    want(os.path.join(cc.RUNTIME, "cpu_check.c"), check, "/O2", cc.CHECK_CFLAGS)
    # Native implementations by name, for symbols no module defines.
    names = sorted(hle_names())
    table = os.path.join(rt_dir, "hle_table.c")
    write_if_changed(table, '#include "host.h"\n\n' + "".join(f"GuestFn hle_{n};\n" for n in names)
                     + "const HleEntry hle_table[] = {\n" + "".join(f'    {{"{n}", hle_{n}}},\n' for n in names)
                     + f"    {{0, 0}}\n}};\nconst u32 hle_count = {len(names)};\n")
    want(table, table[:-2] + ".obj", "/O1", rt_flags)
    # The run-time loadable modules (NROs) linked in, for nn::ro to find by build id.
    nros = [os.path.basename(os.path.normpath(d)) for d in dirs
            if os.path.basename(os.path.dirname(os.path.normpath(d))) == "nro"]
    nro_table = os.path.join(rt_dir, "nro_table.c")
    write_if_changed(nro_table, '#include "module.h"\n\n'
                     + "".join(f"extern const RtModule rt_module_{n};\n" for n in nros)
                     + "const RtModule *const rt_nro_modules[] = {"
                     + "".join(f"&rt_module_{n}, " for n in nros) + "0};\n"
                     + f"const u32 rt_nro_count = {len(nros)};\n")
    want(nro_table, nro_table[:-2] + ".obj", "/O1")
    funcs, tables = os.path.join(rt_dir, game_dll.FUNCS), os.path.join(rt_dir, game_dll.TABLES)
    if GAME_DLL:   # (the program's side of the interface, and the library's, compiled as the game is)
        write_if_changed(funcs, game_dll.funcs_text(cc.RUNTIME))
        write_if_changed(tables, game_dll.tables_text(cc.RUNTIME))
        want(funcs, funcs[:-2] + ".obj", "/O1", rt_flags)
        want(tables, tables[:-2] + ".obj", "/O1", cc.GAME_CFLAGS)
    if objcache.summary(why):
        print(f"runtime: {objcache.summary(why)}")
    every = [job for group in jobs.values() for job in group]
    record.forget([obj for _src, obj, _opt in every])
    for _src, obj, _opt in every:   # (as compile_dir: a file that fails to compile leaves no old object)
        if os.path.exists(obj):
            os.remove(obj)
    failures, own = [], steps is None
    steps = steps or ladder.Ladder(progress.level_cost, "clang-cl")
    try:
        for flags, group in jobs.items():
            failures += cc.compile_each(group, 4, compiled=lambda job, _level: record.compiled(job[1], keys[job[1]]),
                                        flags=None if flags is None else list(flags), steps=steps)
    finally:
        if own:
            steps.end(os.path.dirname(os.path.abspath(exe)))
    for src, err in failures:
        print(f"FAILED {os.path.basename(src)}:\n{err[:4000]}")
    if failures:
        return False
    objs = [os.path.join(rt_dir, n[:-2] + ".obj") for n in sources] + [check, table[:-2] + ".obj", nro_table[:-2] + ".obj"]
    game = []
    for d in dirs:
        game += sorted(os.path.join(d, n) for n in os.listdir(d) if n.endswith(".obj"))
    game_lib = os.path.join(os.path.dirname(os.path.abspath(exe)), game_dll.DLL)
    if os.path.exists(game_lib):   # (a --game-dll build's: made again below, or not wanted)
        os.remove(game_lib)
    if GAME_DLL:   # (the NROs' list is the library's)
        objs, library = objs[:-1] + [funcs[:-2] + ".obj"], [tables[:-2] + ".obj", objs[-1]] + game
        cc.link(objs, exe)
        cc.link_library(library, game_lib)
    else:
        objs += game
        # a wrap no module has (a made-up game's modules, tools/mac/boottest): its original is a stub nothing calls
        cc.link(objs, exe, [f"/alternatename:f_{a:x}_orig=rt_wrap_missing" for a in sorted(hle_wraps())])
    isa.record(os.path.dirname(os.path.abspath(exe)), cc.ISA, cc.ISA_TOLD)   # (what build_game.py's build_is_current compares)
    # the controllers' SDL3.dll, which the runtime loads from the exe's own folder
    if cc.SDL3_DLL:
        import shutil
        dll = os.path.join(os.path.dirname(exe), "SDL3.dll")
        if not os.path.exists(dll) or os.path.getsize(dll) != os.path.getsize(cc.SDL3_DLL) or \
                os.path.getmtime(dll) < os.path.getmtime(cc.SDL3_DLL):
            shutil.copy2(cc.SDL3_DLL, dll)
    print(f"linked {len(objs)} objects into {exe} ({os.path.getsize(exe) / 1048576:,.0f} MB) in {time.time() - t0:,.0f}s")
    if GAME_DLL:
        print(f"linked {len(library)} objects into {game_lib} ({os.path.getsize(game_lib) / 1048576:,.0f} MB)")
    return True


def link_compiled(dirs, exe, rt_dir=None):
    """recomp.py link: link(), when every module folder's objects are current (plan: compiled with this
    build's command, so for its instruction set, cc.ISA). link compiles the runtime for it and writes
    it into isa.txt beside the exe (tools/isa.py), which must name what every object it links is
    compiled for: avx2 objects linked as x86-64-v2 would pass the processor check on a processor
    without AVX2 (it checks for what the runtime is compiled for), and build_game.py would take the
    program as current. (build_all links the folders it has just compiled.)"""
    import cc
    for d in dirs:
        if not os.path.isfile(os.path.join(d, "manifest.tsv")):
            print(f"{d}: not a folder recomp.py generated (no manifest.tsv): recomp.py link links only those")
            return False
        todo, _record, _keys, why, _infos = plan(d)
        if todo:
            built = isa.built_for(build_folder(d))
            print(f"{os.path.basename(d)}: {len(todo)} object{'s' if len(todo) != 1 else ''} not compiled for this build"
                  + (f" (the folder is built for {built}, this build is for {cc.ISA})" if built != cc.ISA else "")
                  + f": compile them first (recomp.py compile {d}, or recomp.py all)")
            if objcache.summary(why):
                print(f"{os.path.basename(d)}: {objcache.summary(why)}")
            return False
    return link(dirs, exe, rt_dir)


def module_paths(exefs):
    """name -> module file, for the ExeFS's modules and dump/nro's NROs (placing new NROs)."""
    nros = nro_modules()
    MODULES.update(nros)
    return {name: os.path.join(NRO_DIR, name + ".nro") if name in nros else os.path.join(exefs, name) for name in MODULES}


def check(exefs, mods=None):
    """The imports no module exports and the runtime does not implement, listed before
    anything is compiled. mods: the modules, if loaded already (name -> Module)."""
    mods = mods or {name: Module(path) for name, path in module_paths(exefs).items()}
    return modcache.report_imports(mods, hle_names(), os.path.join(ROOT, "build", "imports.unresolved.txt"))


def build_all(exefs, out, jobs, compile_c=True):
    """Every module's C first, then each module's compile, then the link: once the C is there,
    what is left to compile is known, and the progress bar's time left (progress.Build) can say
    how long it takes. compile_c False (--generate-only): the C only."""
    ladder.settings()   # (SSBU_COMPILE_LIMIT, SSBU_OPTNONE_ABOVE, SSBU_LONGEST_FIRST: one that is not a number stops the build now)
    paths = module_paths(exefs)
    mods = {name: Module(path) for name, path in paths.items()}
    check(exefs, mods)
    fp = tools_fingerprint()
    targets = {name: os.path.join(out, "nro", name) if os.path.dirname(nso) == NRO_DIR else os.path.join(out, name)
               for name, nso in paths.items()}
    shown = progress.Build(min(jobs, 16), compile=compile_c, link=progress.LINK if compile_c else None)

    def stale(target):   # (for the bar; none when the C is compiled elsewhere: --generate-only)
        return [(t[0], t[3]) for t in stale_files(target)] if compile_c else []
    generate = False
    for name, mod in mods.items():
        current = modcache.build_info(targets[name]) == (mod.build_id, MODULES[name], name, fp)
        generate |= not current
        shown.module(name, (mod.text.end - mod.text.mem) // 4, stale(targets[name]) if current else None)
    build_ids = {name: mod.build_id for name, mod in mods.items()}
    # (for the modules whose C is generated again; the names come from the boot modules, 13.0.5's,
    # so a module whose C is current has them already)
    imports = hook_imports(mods) if generate else {}
    del mods   # (the compilers want the memory)
    exe = os.path.join(out, "ssbu.exe")
    with progress.Line(shown.status, title=f"building {os.path.basename(exe)}" if compile_c else "writing the game's C"):
        reused = 0
        for name, nso in paths.items():
            shown.begin("gen", name)
            target, build_id, base = targets[name], build_ids[name], MODULES[name]
            img = os.path.join(ROOT, "dump", "modules", name + ".reloc.bin")
            if not os.path.exists(img) or os.path.getmtime(img) < os.path.getmtime(nso):
                image(nso, img)
            if modcache.build_info(target) == (build_id, base, name, fp):
                print(f"{name}: C up to date (build id {build_id[:16]}, same tools)")
                reused += 1
            else:
                build(nso, target, jobs, 40000, imports)
            if not compile_c:
                modcache.write_stamp(target, build_id, base, name, fp)
            shown.written(name, stale(target))
            shown.end("gen", name)
        print(f"{reused} of {len(paths)} modules reused without generating their C again")
        if not compile_c:
            print(f"the C of {len(paths)} modules is in {out} (--generate-only: not compiled here)")
            return True
        steps = ladder.Ladder(progress.level_cost, "clang-cl")   # (one pace and one report for the whole build)
        try:
            for name in paths:
                if not compile_dir(targets[name], min(jobs, 16), shown, steps):
                    return False
                modcache.write_stamp(targets[name], build_ids[name], MODULES[name], name, fp)
            shown.begin("link")
            ok = link(list(targets.values()), exe, steps=steps)
            shown.end("link")
            return ok
        finally:
            steps.end(out)


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, nso = sys.argv[1], sys.argv[2]
    GAME_DLL = "--game-dll" in sys.argv
    if cmd == "coverage":
        coverage(nso, int(arg("--jobs", "20")))
    elif cmd == "emit":
        roots = [int(r, 16) for r in arg("--roots", "").split(",") if r]
        emit(nso, roots, arg("--out"), int(arg("--depth", "8")), int(arg("--sample", "0")))
    elif cmd == "image":
        image(nso, arg("--out"))
    elif cmd == "build":
        imports = None
        if any(kind in EXACT_CALLEES for kind, _a in hook_list()):   # (this module's import stubs: hook_imports)
            exefs = arg("--exefs", os.path.join(ROOT, "dump", "exefs"))
            if not all(os.path.isfile(os.path.join(exefs, n)) for n in ("main", "subsdk0", "sdk")):
                sys.exit(f"tools/hooks.txt has exact-callees (or hookable functions, which are exact-callees too), and "
                         f"finding the import stubs bound to them needs "
                         f"main, subsdk0 and sdk: not all in {exefs} (--exefs <dir>)")
            imports = hook_imports({n: Module(os.path.join(exefs, n)) for n in ("main", "subsdk0", "sdk")})
        build(nso, arg("--out"), int(arg("--jobs", "20")), int(arg("--per-file", "40000")), imports)
    elif cmd in ("compile", "link") and sys.platform == "darwin":
        sys.exit(f"recomp.py {cmd} builds with clang-cl, for Windows: on macOS, tools/mac/build_mac.py compiles and links")
    elif cmd == "compile":
        sys.exit(0 if compile_dir(nso, int(arg("--jobs", "16"))) else 1)
    elif cmd == "link":
        sys.exit(0 if link_compiled(arg("--dirs").split(","), arg("--out"), arg("--rt", None)) else 1)
    elif cmd == "all":
        # (on macOS the C is compiled by tools/mac/build_mac.py, the compiling and linking here being clang-cl's)
        compile_c = "--generate-only" not in sys.argv and sys.platform != "darwin"
        sys.exit(0 if build_all(nso, arg("--out"), int(arg("--jobs", "20")), compile_c) else 1)
    elif cmd == "check":
        check(nso)
    elif cmd == "wrap-sig":   # (an address, or for HLE_WRAP_EXPORT the exported name)
        m = Module(nso)
        if re.fullmatch(r"(?:0x)?[0-9a-fA-F]+", sys.argv[3]):
            a = int(sys.argv[3], 16)
            off = a - base_of(nso)
        else:
            off = next((v for n, v, _s, info, _x in m.exports if n == sys.argv[3] and info & 0xF == 2), None)
            if off is None:
                sys.exit(f"{nso} exports no function named {sys.argv[3]}")
            a = base_of(nso) + off
        sizes = dict(load(nso)[2])
        if off not in sizes:
            sys.exit(f"{a:x} is not a function start in {nso}")
        print(signature_comment(signature(m, off, sizes[off])))
    else:
        sys.exit(__doc__)
