"""The runtime/game split (--game-dll): the game's modules linked into a library of their own,
which the runtime program loads at start (runtime/game_dll.h, runtime/host.c), rather than into
the program. The program is then the runtime alone, which can be built once, and a player's PC
compiles and links only the game. build_game.py --game-dll hands the switch to tools/recomp.py
(Windows: ssbu_game.dll beside build/ssbu.exe), tools/linux/build_linux.py (ssbu_game.so beside
build-linux/ssbu) and tools/mac/build_mac.py (the app's Contents/Frameworks/ssbu_game.dylib).
The game's C is the same either way: only how it is compiled into objects and linked differs.

This writes the two files that join the two, from the runtime's sources:
  game_tables.c  the library's (tables_text): for each runtime function the game's C can call
                 (CORE, each HLE(<name>), each HLE_WRAP), a function of that name that calls
                 through a slot the program fills by the name; the list of the hook slots the
                 modules' constructors add; each HLE_WRAP's original; the modules; and the one
                 function the library exports, rt_game_tables, which returns all of it
  game_funcs.c   the program's (funcs_text): each of those runtime functions by name, and each
                 HLE_WRAP's original (f_<address>_orig, which the runtime calls), through the slot
                 the program fills from the library's
Without the switch neither file is made, and the build is what it was."""
import os
import re

DEFINE = "SSBU_GAME_DLL"   # (the program's runtime is compiled with it: runtime/host.c loads the library)
ENTRY = "rt_game_tables"
DLL, SO, DYLIB = "ssbu_game.dll", "ssbu_game.so", "ssbu_game.dylib"   # (runtime/game_dll.h's RT_GAME_LIBRARY)
TABLES, FUNCS = "game_tables.c", "game_funcs.c"

# The runtime functions the generated C calls besides the HLE(...) and HLE_WRAP(...) ones, as
# runtime/recomp.h declares them (tools/lift.py writes the calls; rt_unwind_jumpto is the native
# form of libunwind's jumpto, tools/recomp.py): (name, head of the library's function, its
# arguments). rt_eh_filter's first parameter is the host exception's, which differs on Windows.
# Not rt_hook_add (hookable.h): the library's own lists its hook slots, for the program to add.
CORE = (
    ("rt_lookup", "GuestFn *rt_lookup(u64 guest_addr)", "guest_addr"),
    ("rt_lookup_cached", "GuestFn *rt_lookup_cached(u64 guest_addr, RtCache *cache)", "guest_addr, cache"),
    ("rt_setjmp", "jmp_buf *rt_setjmp(Cpu *c)", "c"),
    ("rt_eh_filter", ("int rt_eh_filter(struct _EXCEPTION_POINTERS *e, u64 sp, u64 lp)",
                      "int rt_eh_filter(const RtEhSignal *e, u64 sp, u64 lp)"), "e, sp, lp"),
    ("rt_trap", "_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why)", "c, pc, why"),
    ("rt_svc", "void rt_svc(Cpu *c, u32 number)", "c, number"),
    ("rt_ticks", "u64 rt_ticks(void)", ""),
    ("rt_unwind_jumpto", "void rt_unwind_jumpto(Cpu *c)", "c"),
)
MISSING = "wrapped guest function missing from this build's modules"   # (runtime/host.c rt_wrap_missing's words)


def runtime_functions(runtime):
    """(HLE names, HLE_WRAP addresses, HLE_WRAP_EXPORT names), each sorted: what runtime/*.c
    defines, found as the builds find them (tools/recomp.py checks their form)."""
    names, addresses, exported = set(), set(), set()
    for n in sorted(os.listdir(runtime)):
        if n.endswith(".c") and not n.startswith("._"):   # (not macOS's ._* files on exFAT)
            with open(os.path.join(runtime, n), encoding="utf-8") as f:
                text = f.read()
            names |= set(re.findall(r"^HLE\((\w+)\)", text, re.M))
            addresses |= {int(a, 16) for a in re.findall(r"^HLE_WRAP\(([0-9a-fA-F]+)\)", text, re.M)}
            exported |= set(re.findall(r"^HLE_WRAP_EXPORT\(([A-Za-z_]\w*)\)", text, re.M))
    return sorted(names), sorted(addresses), sorted(exported)


def imports(runtime):
    """[(name, kind)] of every runtime function the library calls through a slot, in slot order:
    kind "core" (CORE), "hle" (HLE), "wrap" (HLE_WRAP, by address) or "export" (HLE_WRAP_EXPORT).
    Every one the runtime has, used or not: the program checks that the library's set is its own."""
    names, addresses, exported = runtime_functions(runtime)
    return ([(name, "core") for name, _head, _args in CORE] + [(f"hle_{n}", "hle") for n in names]
            + [(f"hle_wrap_{a:x}", "wrap") for a in addresses] + [(f"hle_wrap_{n}", "export") for n in exported])


def _call(head, slot, args):
    """The body of a library function that calls through its slot, as typed as its declaration."""
    name = re.search(r"(\w+)\(", head).group(1)
    call = f"((__typeof__(&{name}))rt_game_slots[{slot}])({args})"
    if head.startswith("_Noreturn"):
        return f"{{ {call}; __builtin_unreachable(); }}"
    return f"{{ {call}; }}" if head.startswith("void ") else f"{{ return {call}; }}"


def tables_text(runtime):
    """game_tables.c, the library's side (see the top of this file and runtime/game_dll.h)."""
    _names, addresses, _exported = runtime_functions(runtime)
    every = imports(runtime)
    core = {name: (head, args) for name, head, args in CORE}
    out = ["/* The game library's side of its interface with the runtime program (runtime/game_dll.h), written by\n"
           " * tools/game_dll.py from the runtime's sources: each runtime function the game's C calls is one here that\n"
           " * calls the program's through a slot runtime/host.c fills by its name. */\n",
           '#include "game_dll.h"\n\n',
           f"static RtGameFn *rt_game_slots[{len(every)}];\n"]
    for slot, (name, kind) in enumerate(every):
        if kind == "core":
            head, args = core[name]
            if isinstance(head, tuple):
                out.append(f"#ifdef _WIN32\n{head[0]} {_call(head[0], slot, args)}\n#else\n{head[1]} {_call(head[1], slot, args)}\n"
                           "#endif\n")
            else:
                out.append(f"{head} {_call(head, slot, args)}\n")
        elif kind == "export":
            out.append(f"void {name}(Cpu *c, GuestFn *orig) {{ ((void (*)(Cpu *, GuestFn *))rt_game_slots[{slot}])(c, orig); }}\n")
        else:
            out.append(f"void {name}(Cpu *c) {{ ((GuestFn *)rt_game_slots[{slot}])(c); }}\n")
    out.append("static const RtGameImport imports[] = {\n" + "".join(f'    {{"{name}", &rt_game_slots[{slot}]}},\n'
                                                               for slot, (name, _k) in enumerate(every)) + "    {0, 0}\n};\n\n")
    out.append("/* The hook slots (hookable.h), which the modules' constructors add as the library loads: listed, for the\n"
               " * program to add to its own. */\n"
               "static RtHook *hooks;\n"
               "void rt_hook_add(RtHook *h) { h->next = hooks; hooks = h; }\n\n")
    out.append("/* Each HLE_WRAP's original, which the runtime's wrapper calls. One no module of this build has (a made-up\n"
               " * game's, tools/mac/boottest) is this stub, which nothing reaches: on Windows by the linker's /alternatename,\n"
               " * elsewhere a weak definition the module's own replaces. */\n"
               f'void rt_game_missing(Cpu *c) {{ rt_trap(c, 0, "{MISSING}"); }}\n')
    for a in addresses:
        out.append(f'#ifdef _WIN32\nGuestFn f_{a:x}_orig;\n#pragma comment(linker, "/alternatename:f_{a:x}_orig=rt_game_missing")\n'
                   f"#else\n__attribute__((weak)) void f_{a:x}_orig(Cpu *c) {{ rt_game_missing(c); }}\n#endif\n")
    out.append("static const RtGameOrig origs[] = {\n" + "".join(f"    {{0x{a:X}ull, f_{a:x}_orig}},\n" for a in addresses)
               + "    {0, 0}\n};\n\n")
    out.append("extern const RtModule rt_module_main, rt_module_subsdk0, rt_module_sdk;\n"
               "static const RtModule *const boot[] = {&rt_module_main, &rt_module_subsdk0, &rt_module_sdk};\n\n"
               f"RT_GAME_EXPORT const RtGameTables *{ENTRY}(void) {{\n"
               "    static RtGameTables t;\n"
               "    t = (RtGameTables){RT_GAME_ABI, RT_GAME_SIZES, RT_GAME_ISA, boot, rt_nro_modules, rt_nro_count,\n"
               f"                       {len(every)}, imports, origs, {len(addresses)}, &hooks}};\n"
               "    return &t;\n"
               "}\n")
    return "".join(out)


def funcs_text(runtime):
    """game_funcs.c, the program's side (see the top of this file and runtime/game_dll.h)."""
    _names, addresses, exported = runtime_functions(runtime)
    out = ["/* The runtime program's side of its interface with the game library (runtime/game_dll.h), written by\n"
           " * tools/game_dll.py from the runtime's sources: the runtime functions the library calls, by name (the HLE ones\n"
           " * are hle_table's), and each HLE_WRAP's original, which the library has: runtime/host.c fills its slot. */\n",
           '#include "host.h"\n#include "game_dll.h"\n\n',
           "GuestFn rt_unwind_jumpto, rt_wrap_missing;\n"]
    out += [f"GuestFn hle_wrap_{a:x};\n" for a in addresses]
    out += [f"void hle_wrap_{n}(Cpu *c, GuestFn *orig);\n" for n in exported]
    out += [f"static GuestFn *orig_{a:x} = rt_wrap_missing;\nvoid f_{a:x}_orig(Cpu *c) {{ orig_{a:x}(c); }}\n" for a in addresses]
    rows = [f'    {{"{name}", (RtGameFn *){name}, 0, 0}},\n' for name, _h, _a in CORE]
    rows += [f'    {{"hle_wrap_{a:x}", (RtGameFn *)hle_wrap_{a:x}, 0x{a:X}ull, &orig_{a:x}}},\n' for a in addresses]
    rows += [f'    {{"hle_wrap_{n}", (RtGameFn *)hle_wrap_{n}, 0, 0}},\n' for n in exported]
    out.append("\nconst RtGameFunc rt_game_funcs[] = {\n" + "".join(rows) + "    {0, 0, 0, 0}\n};\n"
               f"const u32 rt_game_func_count = {len(rows)};\n")
    return "".join(out)


def abi(runtime):
    """RT_GAME_ABI, as runtime/game_dll.h has it."""
    with open(os.path.join(runtime, "game_dll.h"), encoding="utf-8") as f:
        return int(re.search(r"^#define RT_GAME_ABI (\d+)$", f.read(), re.M).group(1))
