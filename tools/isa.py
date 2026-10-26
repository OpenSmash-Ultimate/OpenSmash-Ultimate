"""The instruction set the game is compiled for, on x86-64 (the Windows and Linux builds; the macOS
build is arm64 and has none of this):

  avx2       the default: AVX2, FMA, BMI1, BMI2 and LZCNT (Intel since Haswell, 2013; AMD since
             Excavator, 2015). What every build was before there was a choice.
  x86-64-v2  for older processors: SSE4.2 and POPCNT (Intel since Nehalem, 2008; AMD since
             Bulldozer, 2011). Expected to be slower (how much is not measured): fma() is a library
             call there, and nothing is vectorised wider than 128 bits.

The lifter's C asks for no extension by name: fma() (ARM's fused multiply-add, one rounding on
every target), __builtin_roundeven and the bit counts are plain C, and half floats and TBL are
done in software (runtime/recomp.h). So the choice is only the compile flags: CLANG_CL (tools/cc.py),
CLANG (tools/linux/toolchain.py) and opus's defines (tools/codec_libs.py). x86-64-v2 is the floor:
below it the rounding functions become library calls too, and Windows' C runtime has no roundeven.

Which one (choose): SSBU_ISA=avx2 or SSBU_ISA=x86-64-v2 when set (SSBU_ISA=auto: as if unset);
else x86-64-v2 if this PC's processor is known to lack something avx2 needs; else the build
folder's last choice if SSBU_ISA made it (told once, kept); else avx2 if the processor is known to
have it; else, when it cannot be asked, the folder's last choice (last_choice: avx2 for a folder an
older version linked a program in), else x86-64-v2. A build writes what it compiled for into its
folder's isa.txt (record) as it links: build/isa.txt on Windows, build-linux/isa.txt on Linux, which
build_game.py compares with today's choice (another one builds again; tools/objcache.py then
compiles every object again, their command having changed). A folder without one is avx2's
(built_for). A folder an older version built has no objcache.json either; its objects are taken as
current by date (objcache's adopt_by_date) only when they were compiled with the flags today's
build compiles with (adopts: the folder built for today's target, and that target's flags the older
versions' own, LEGACY_CLANG_CL and LEGACY_CLANG). No target's flags are theirs today, so such a
folder is compiled again whole, once (tools/recomp.py's plan, tools/linux/build_linux.py's game_jobs).

The program checks the processor itself before anything else of it runs (runtime/cpu_check.c): one
built for AVX2 on a processor without it says what is missing and what to do, instead of crashing."""
import os
import sys

TARGETS = ("avx2", "x86-64-v2")
DEFAULT = "avx2"
ANY = "x86-64"   # (not a target: what runtime/cpu_check.c is compiled for, so that it runs on any x86-64 processor)
# what each needs of the processor, in the names cpu_features gives
V2 = ("sse3", "ssse3", "sse4.1", "sse4.2", "popcnt", "cx16")
NEEDS = {"avx2": V2 + ("avx", "avx2", "fma", "bmi1", "bmi2", "lzcnt"), "x86-64-v2": V2}
# The compile flags of each. clang-cl's /arch:AVX2 is -march=haswell (AVX2, FMA, BMI1, BMI2, LZCNT,
# MOVBE, F16C); without MOVBE and F16C, which the compiler would use unasked (a load and byte swap is
# MOVBE), it needs what NEEDS says, as on Linux. Its x64 default is x86-64 (SSE2). /clang: hands -march
# to clang itself: a clang-cl that does not know -march as its own would leave it out with a warning
# that -w hides.
CLANG_CL = {"avx2": ("/arch:AVX2", "-mfma", "-mno-movbe", "-mno-f16c"), "x86-64-v2": ("/clang:-march=x86-64-v2",), ANY: ()}
CLANG = {"avx2": ("-mavx2", "-mfma", "-mbmi", "-mbmi2", "-mlzcnt", "-mpopcnt"), "x86-64-v2": ("-march=x86-64-v2",),
         ANY: ("-march=x86-64",)}
# The flags every older version compiled with (before there was a choice or an isa.txt: tools/cc.py's
# BASE_CFLAGS, tools/linux/toolchain.py's ARCH), so what the objects of a folder it built are compiled
# for: avx2 with MOVBE and F16C on Windows, with F16C on Linux, neither of which today's avx2 needs. Such
# a folder's objects are kept by date only by a build whose flags are these (adopts): none, today.
LEGACY_CLANG_CL = ("/arch:AVX2", "-mfma", "-mf16c")
LEGACY_CLANG = ("-mavx2", "-mfma", "-mf16c", "-mbmi", "-mbmi2", "-mlzcnt", "-mpopcnt")
RECORD = "isa.txt"
PROGRAMS = ("ssbu.exe", "ssbu")   # what a build folder's link makes (Windows, Linux): last_choice

# /proc/cpuinfo's names (Linux lists avx only when the kernel saves the YMM registers; abm is LZCNT)
CPUINFO = {"pni": "sse3", "ssse3": "ssse3", "sse4_1": "sse4.1", "sse4_2": "sse4.2", "popcnt": "popcnt", "cx16": "cx16",
           "avx": "avx", "avx2": "avx2", "fma": "fma", "bmi1": "bmi1", "bmi2": "bmi2", "abm": "lzcnt"}
# Windows' IsProcessorFeaturePresent: PF_SSE3_INSTRUCTIONS_AVAILABLE, PF_COMPARE_EXCHANGE128 and, from
# Windows 10 version 2004 (build 19041), PF_SSSE3/SSE4_1/SSE4_2/AVX/AVX2_INSTRUCTIONS_AVAILABLE
PF = {13: "sse3", 14: "cx16", 36: "ssse3", 37: "sse4.1", 38: "sse4.2", 39: "avx", 40: "avx2"}
XSTATE_AVX = 1 << 2   # GetEnabledXStateFeatures: the system saves the YMM registers


def cpuinfo_features(text):
    """The extensions in /proc/cpuinfo's first flags line (names as in NEEDS), or None without one."""
    for line in text.splitlines():
        key, _, value = line.partition(":")
        if key.strip() == "flags":
            return frozenset(CPUINFO[f] for f in value.split() if f in CPUINFO)
    return None


def windows_features(build, present, xstate):
    """The extensions Windows says the processor has (present(PF number), xstate(): the enabled XSTATE
    mask), or None before build 19041, which cannot be asked about SSSE3 to AVX2. Windows has no flag
    for POPCNT, FMA, BMI1, BMI2 or LZCNT: every processor with SSE4.2 has POPCNT and every one with
    AVX2 the rest (runtime/cpu_check.c asks the processor about each as the game starts)."""
    if build < 19041:
        return None
    has = {name for pf, name in PF.items() if present(pf)}
    if not xstate() & XSTATE_AVX:
        has -= {"avx", "avx2"}
    if "sse4.2" in has:
        has.add("popcnt")
    if "avx2" in has:
        has |= {"fma", "bmi1", "bmi2", "lzcnt"}
    return frozenset(has)


def cpu_features():
    """The extensions this PC's processor has and its system saves the registers of, or None when that
    cannot be told (another system, an older Windows, no /proc/cpuinfo)."""
    try:
        if sys.platform == "win32":
            import ctypes
            k32 = ctypes.windll.kernel32
            k32.GetEnabledXStateFeatures.restype = ctypes.c_uint64
            return windows_features(sys.getwindowsversion().build, lambda pf: bool(k32.IsProcessorFeaturePresent(pf)),
                                    k32.GetEnabledXStateFeatures)
        if sys.platform.startswith("linux"):
            with open("/proc/cpuinfo", encoding="utf-8", errors="replace") as f:
                return cpuinfo_features(f.read())
    except (OSError, AttributeError, ValueError):
        pass
    return None


def best(features):
    """The fastest target these extensions run, or None below x86-64-v2."""
    return next((t for t in TARGETS if set(NEEDS[t]) <= set(features)), None)


def missing(target, features):
    """What of `target`'s needs these extensions lack, in NEEDS' order."""
    return [f for f in NEEDS[target] if f not in features]


def choose(env, features, record):
    """(target, told: SSBU_ISA chose it, why: a few words for the build's log) for the environment
    `env`, this processor's extensions (None: unknown) and the folder's record ((target, told) or None)."""
    want = (env.get("SSBU_ISA") or "").strip().lower()
    if want in TARGETS:
        return want, True, "SSBU_ISA"
    if want not in ("", "auto"):
        raise SystemExit(f"SSBU_ISA={env['SSBU_ISA']}: the instruction set to build for is one of {', '.join(TARGETS)} "
                         "(or auto: this processor's)")
    last, told = record or (None, False)
    if features is not None and best(features) != "avx2":
        lacks = ", ".join(missing("avx2", features))
        if best(features) is None:
            return "x86-64-v2", False, f"this processor lacks {', '.join(missing('x86-64-v2', features))}: WindowSmash cannot run on it"
        return "x86-64-v2", False, f"this processor lacks {lacks}: the build for older processors, expected to be slower"
    if told and not want:
        return last, True, f"as SSBU_ISA chose for the last build ({RECORD}; SSBU_ISA=auto chooses by the processor)"
    if features is not None:
        return "avx2", False, "this processor has AVX2"
    if last:
        other = "SSBU_ISA=x86-64-v2 builds for older processors" if last == "avx2" else "SSBU_ISA=avx2 builds for AVX2"
        return last, False, f"as the last build: the processor could not be asked ({other})"
    return "x86-64-v2", False, ("the processor could not be asked whether it has AVX2: the build for older processors, "
                                "expected to be slower (SSBU_ISA=avx2 builds for AVX2)")


def recorded(folder):
    """(target, told) from a build folder's record, or None. (utf-8-sig: a record saved again with a BOM reads
    as the installer's Get-Content reads it.)"""
    try:
        with open(os.path.join(folder, RECORD), encoding="utf-8-sig") as f:
            lines = f.read().splitlines()
    except OSError:
        return None
    if not lines or lines[0].strip() not in TARGETS:
        return None
    return lines[0].strip(), "told" in lines[1:2]


def built_for(folder):
    """What a build folder's objects are compiled for: its record's, else avx2 (every build was, before
    the record was written)."""
    return (recorded(folder) or (DEFAULT, False))[0]


def last_choice(folder):
    """choose()'s `record` for a build folder: its record's (target, told); without one, (avx2, chosen)
    when an older version linked a program there; else None. Every older version built for avx2, and
    its installer stopped for a processor whose name said it had no AVX2, so the processor of a folder
    it linked most likely runs avx2: kept when this one cannot be asked (Windows before build 19041,
    a name the installer does not know) rather than built again for x86-64-v2, slower. Where it does
    not (that program never started), the program now says so as it starts, and how to build for
    x86-64-v2."""
    found = recorded(folder)
    if found is None and any(os.path.isfile(os.path.join(folder, p)) for p in PROGRAMS):
        return DEFAULT, False
    return found


def adopts(folder, target, flags, legacy):
    """Whether a build for `target`, compiling with the instruction-set flags `flags`, may take the
    objects of a folder an older version built (no objcache.json) as current by date (objcache's
    adopt_by_date): only when they were compiled with these very flags, that is when the folder is
    built for `target` (built_for: avx2 without a record) and `flags` are the older versions' own
    (`legacy`: LEGACY_CLANG_CL or LEGACY_CLANG). Otherwise its objects could need of the processor what
    today's build does not check for (on Windows, an older version's avx2 objects can use MOVBE)."""
    return built_for(folder) == target and tuple(flags) == tuple(legacy)


def record(folder, target, told):
    """Writes a build folder's record: what it was built for, and whether SSBU_ISA said so (then kept).
    Written as the build links, not as it starts: the record stands for the program beside it too
    (build_game.py takes a program whose record names today's choice as current), so one written by
    a build that then stopped would vouch for the program an earlier build made, for the other
    target. So an SSBU_ISA choice is kept once a build for it has linked."""
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, RECORD), "w", encoding="utf-8", newline="\n") as f:
        f.write(f"{target}\n{'told' if told else 'chosen'}\n# the instruction set this folder's build is compiled for "
                f"(tools/isa.py): {' or '.join(TARGETS)}; told: SSBU_ISA chose it, and later builds keep it\n")


def target(folder):
    """choose() for this process: its environment, this PC's processor and `folder`'s last choice."""
    return choose(os.environ, cpu_features(), last_choice(folder))
