"""Toolchain for the macOS build: Apple's clang from the Command Line Tools, for arm64 (Apple
silicon), macOS 14 or newer.

Everything comes with the Command Line Tools (xcode-select --install; Install
WindowSmash.command does it): clang, the macOS SDK, ar, nm and codesign. xcrun finds them, so a
selected Xcode works too (xcode-select, DEVELOPER_DIR). SSBU_CLANG names another clang, SDKROOT
another SDK. The SDK must be macOS 15's or newer (MIN_SDK): an older one lacks what the runtime
uses (runtime/macos), which @available cannot leave out when it is compiled.
SDL3 is the official SDL3.framework in deps/SDL3 (SSBU_SDL3_DIR names another folder holding
one); the codecs are built from third_party/ (tools/codec_libs.py)."""
import json
import os
import re
import subprocess
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
RUNTIME = os.path.join(REPO, "runtime")
MIN_MACOS = "14.0"   # (Info.plist's LSMinimumSystemVersion says the same: tools/mac/build_mac.py)
# The oldest macOS SDK the runtime compiles with (Metal's math mode is macOS 15's; tools/mac/install_mac.py
# checks the same). The Command Line Tools 16 and newer have it; on macOS 14 they need 14.5 or newer.
MIN_SDK = (15, 0)
SDL3_DIR = os.environ.get("SSBU_SDL3_DIR") or os.path.join(REPO, "deps", "SDL3")
SDL3_FRAMEWORK = os.path.join(SDL3_DIR, "SDL3.framework")
MISSING_CLT = ("the Command Line Tools for Xcode are not installed: run  xcode-select --install  (Install "
               "WindowSmash.command does), or name a clang in SSBU_CLANG")
LICENCE = ("Xcode's licence has not been accepted: run  sudo xcodebuild -license accept  in Terminal, or use the "
           "Command Line Tools instead:  sudo xcode-select -s /Library/Developer/CommandLineTools")


def _developer_dir():
    """The selected developer folder (Command Line Tools or Xcode), or None. Asked before xcrun,
    which without one opens the Command Line Tools' install dialog instead of answering."""
    d = os.environ.get("DEVELOPER_DIR")
    if not d:
        try:
            r = subprocess.run(["xcode-select", "-p"], capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired):
            return None
        d = r.stdout.strip() if r.returncode == 0 else ""
    return d if d and os.path.isdir(d) else None


def _xcrun(*args):
    try:
        r = subprocess.run(["xcrun", *args], capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if r.returncode and "license" in r.stderr.lower():   # (a selected Xcode whose licence nobody agreed to yet)
        raise SystemExit(LICENCE)
    out = r.stdout.strip()
    return out if r.returncode == 0 and out else None


def sdk_version(sdk):
    """(major, minor) of a macOS SDK: its SDKSettings.json's, else xcrun's answer; None if neither says."""
    try:
        with open(os.path.join(sdk, "SDKSettings.json"), encoding="utf-8") as f:
            text = json.load(f).get("Version")
    except (OSError, ValueError, AttributeError):
        text = _xcrun("--show-sdk-version")
    m = re.match(r"(\d+)(?:\.(\d+))?", str(text or ""))
    return (int(m.group(1)), int(m.group(2) or 0)) if m else None


def old_sdk(version, sdk):
    """What to do about an SDK older than MIN_SDK."""
    return (f"the macOS SDK is {version[0]}.{version[1]} ({sdk}), and WindowSmash needs the macOS {MIN_SDK[0]} SDK or newer: "
            "update the Command Line Tools in System Settings > General > Software Update (on macOS 14, update macOS to "
            "14.5 or newer first), or install Command Line Tools for Xcode 16 or newer from "
            "https://developer.apple.com/download/all/"
            + ("; SDKROOT names this SDK: unset it, or name a newer one" if os.environ.get("SDKROOT") else ""))


def _tool(name):
    path = _xcrun("--find", name)
    if not path or not os.access(path, os.X_OK):
        raise SystemExit(f"{name} was not found: {MISSING_CLT}")
    return path


if not _developer_dir() and not os.environ.get("SSBU_CLANG"):
    raise SystemExit(MISSING_CLT)
CLANG = os.environ.get("SSBU_CLANG") or _tool("clang")
SDK = os.environ.get("SDKROOT") or _xcrun("--show-sdk-path")
if not SDK or not os.path.isdir(SDK):
    raise SystemExit(f"the macOS SDK was not found: {MISSING_CLT}")
SDK_VERSION = sdk_version(SDK)
if SDK_VERSION and SDK_VERSION < MIN_SDK:
    raise SystemExit(old_sdk(SDK_VERSION, SDK))
AR = _tool("ar")
NM = _tool("nm")
CODESIGN = "/usr/bin/codesign"

# Every object (game, runtime, codecs): arm64 for macOS 14 on, tuned for the M1 and later.
# Guest FP semantics need IEEE-exact arithmetic: no contraction (clang fuses multiply-adds on
# arm64 unless told not to), no fast-math, as on Windows and Linux. No stack protector (a
# check in every generated function that uses a stack array) and hidden symbols (a Mach-O
# program otherwise exports every global: millions of f_* in its export trie). The unwind
# tables stay: C++ exceptions pass through the generated C frames (runtime/recomp.h).
TARGET = ["-arch", "arm64", f"-mmacosx-version-min={MIN_MACOS}", "-isysroot", SDK]
CFLAGS = TARGET + ["-mcpu=apple-m1", "-fno-strict-aliasing", "-ffp-contract=off", "-fno-stack-protector",
                   "-fvisibility=hidden", "-w"]
CXXFLAGS = ["-x", "c++", "-std=gnu++20", "-fexceptions"]
OBJCFLAGS = ["-fobjc-arc"]


def language(src, flags=()):
    """The flags a source's language adds: Objective-C with ARC (.m), C++ (.cpp, .cc, a generated
    _cxx.cpp wrapper, or "c++" among the job's flags), else none (C, and .S assembled as it is)."""
    if src.endswith(".m"):
        return OBJCFLAGS
    if src.endswith(".mm"):
        return ["-x", "objective-c++", "-std=gnu++20", "-fexceptions", *OBJCFLAGS]
    if src.endswith((".cpp", ".cc")) or "c++" in flags:
        return CXXFLAGS
    return []


def wrapped(cmd):
    """(what runs a command below normal priority, what subprocess is given for it; tools/ladder.py
    starts the compilers so): on every core, as taskpolicy -b would keep the build on the efficiency cores."""
    return ["nice", "-n", "10", *cmd], {}


def run(cmd):
    argv, priority = wrapped(cmd)
    r = subprocess.run(argv, capture_output=True, text=True, **priority)
    return r.returncode, r.stdout + r.stderr


def compile_command(src, obj, opt, flags=(), extra=(), header=None):
    """The command compile_each compiles src into obj with; flags: the job's own ("c++" compiles it as
    C++); header: the compile ladder's optnone header, given before the file (-include), when it has one."""
    return [CLANG, "-c", opt, *CFLAGS, *language(src, flags), *extra, *[f for f in flags if f != "c++"],
            *(["-include", header] if header else []), src, "-o", obj]


def compile_each(jobs_list, jobs=16, extra=()):
    """Compile (src, obj, opt[, flags]) jobs in parallel; returns [(src, error text)] for failures."""
    def one(job):
        src, obj, opt = job[:3]
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        code, out = run(compile_command(src, obj, opt, job[3] if len(job) > 3 else [], extra))
        return (src, out) if code else None
    with ThreadPoolExecutor(jobs) as pool:
        return [f for f in pool.map(one, jobs_list) if f]
