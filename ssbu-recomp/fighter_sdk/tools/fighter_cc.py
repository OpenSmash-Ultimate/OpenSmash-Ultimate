"""The compiler fighters and mods are built with: the one the game is built with, for the same processor.

A fighter's C++ replaces lifted functions of the game and has to compute exactly what they compute (the fight
hash of a match with source fighters is compared with the lifted game's). So it is compiled by the clang the
root build picks, with the root build's target and floating-point flags: tools/cc.py's on Windows (clang-cl,
for the instruction set tools/isa.py chooses), tools/linux/toolchain.py's on Linux, tools/mac/toolchain.py's
on macOS. Those modules are only imported, never changed.

The floating-point rules are pinned last on every compile line: -fno-fast-math, then -ffp-contract=off (in
that order: -fno-fast-math can put the contraction back to clang's default). Without -ffp-contract=off clang
fuses a * b + c into one multiply-add on arm64 and with -mfma, which rounds once where the game's separate
multiply and add round twice.

Of the root's flags these builds leave out -w (they show warnings), -fno-pic (a shared library is position
independent: -fPIC on Linux) and the runtime's include folder. They add the SDK's C++ (C++14, no exceptions,
no RTTI) and -O1, as before.

A fighter or mod is <name>.dll on Windows, <name>.so on Linux and <name>.dylib on macOS: what the runtime's
LoadLibraryA opens for "<name>.dll" there (ssbu-recomp/runtime/plat_posix.c)."""
import importlib.util
import os
import sys

import sdk_paths

TARGETS = ('windows', 'linux', 'macos')
EXT = {'windows': '.dll', 'linux': '.so', 'macos': '.dylib'}
OBJ = {'windows': '.obj', 'linux': '.o', 'macos': '.o'}
ROOT_MODULES = {'windows': ('tools', 'cc.py'), 'linux': ('tools', 'linux', 'toolchain.py'), 'macos': ('tools', 'mac', 'toolchain.py')}
PINNED = ('-fno-fast-math', '-ffp-contract=off')
DROPPED = ('-w', '-fno-pic')
OPT = '-O1'
CXX = ('-x', 'c++', '-std=c++14', '-fno-exceptions', '-fno-rtti')
CXX_CL = ('/TP', '/std:c++14', '/GR-')         # (clang-cl: C++ exceptions are off unless /EH is given)
CL_DIAG = '/clang:-fdiagnostics-format=clang'  # file:line:col: error: as on the other systems (batch.py and fighter_status.py read it)
WARNINGS = ('-Wall', '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-unused-const-variable', '-Wno-unused-function')


def host():
    return 'windows' if sys.platform == 'win32' else 'macos' if sys.platform == 'darwin' else 'linux'


def library(name, target=None):
    """The file name of a fighter or mod library."""
    return name + EXT[target or host()]


def load_root(target, repo=None):
    """The root build's toolchain module for a target, imported under a name of its own (tools/linux and
    tools/mac both have a toolchain.py)."""
    repo = repo or sdk_paths.REPO
    if not repo:
        raise sdk_paths.missing("The project's build tools (tools/cc.py and tools/isa.py)", ['every folder above ' + sdk_paths.HERE],
                                'Run the fighter SDK from the repository, or name the repository in SSBU_REPO.')
    path = sdk_paths.first("The root build's toolchain for %s" % target, [os.path.join(repo, *ROOT_MODULES[target])])
    name = 'fighter_sdk_root_' + target
    if name not in sys.modules:
        tools = os.path.join(repo, 'tools')
        if tools not in sys.path:
            sys.path.append(tools)              # (cc.py imports isa, ladder and the like from there)
        spec = importlib.util.spec_from_file_location(name, path)
        mod = importlib.util.module_from_spec(spec)
        sys.modules[name] = mod
        try:
            spec.loader.exec_module(mod)
        except BaseException:
            del sys.modules[name]
            raise
    return sys.modules[name]


def keep(flags, runtime=None):
    """The root's flags without DROPPED and without the runtime's include folder."""
    out, i = [], 0
    while i < len(flags):
        f = flags[i]
        if f in ('-I', '/I') and runtime and i + 1 < len(flags) and os.path.normcase(os.path.abspath(flags[i + 1])) == os.path.normcase(os.path.abspath(runtime)):
            i += 2
            continue
        if f not in DROPPED:
            out.append(f)
        i += 1
    return out


class Toolchain:
    """compile_command() and link_command() for one target; `root` is the root build's toolchain module."""

    def __init__(self, target, root):
        self.target, self.root = target, root
        self.ext, self.obj = EXT[target], OBJ[target]
        self.cc = root.CLANG
        if target == 'windows':
            self.isa, why = root.ISA, root.ISA_WHY
            self.flags = keep(root.base_cflags(root.ISA))
            self.about = '%s (%s), instruction set %s: %s' % (root.CLANG, root.TOOLCHAIN, root.ISA, why)
        elif target == 'linux':
            self.isa, why = root.ISA, root.ISA_WHY
            self.flags = keep(root.cflags(root.ISA), root.RUNTIME) + ['-fPIC']
            self.about = '%s, instruction set %s: %s' % (root.CLANG, root.ISA, why)
        else:
            self.isa = 'arm64'
            self.flags = keep(root.CFLAGS, getattr(root, 'RUNTIME', None))
            self.about = '%s, arm64 for macOS %s' % (root.CLANG, root.MIN_MACOS)

    def library(self, name):
        return name + self.ext

    def compile_command(self, src, obj, includes=(), force=(), defines=()):
        """The command that compiles one C++ source: includes are searched in order, force are included before
        the source, defines are NAME or NAME=VALUE."""
        if self.target == 'windows':
            return [self.cc, '/c', '/clang:' + OPT, *CXX_CL, CL_DIAG, *['/clang:' + w for w in WARNINGS], *self.flags,
                    *[a for d in includes for a in ('/I', d)], *['/FI' + f for f in force], *['/D' + d for d in defines],
                    *['/clang:' + p for p in PINNED], src, '/Fo' + obj]
        return [self.cc, '-c', OPT, *CXX, *WARNINGS, *self.flags, *[a for d in includes for a in ('-I', d)],
                *[a for f in force for a in ('-include', f)], *['-D' + d for d in defines], *PINNED, src, '-o', obj]

    def link_command(self, objs, out, rsp=None):
        """The command that links objects into a shared library (rsp: a response file that lists them)."""
        inputs = ['@' + rsp] if rsp else list(objs)
        if self.target == 'windows':
            return [self.cc, '/nologo', *self.root.LINK_FLAGS, '/LD', *inputs, '/Fe' + out, '/link', *self.root.LINK_OPTIONS]
        if self.target == 'linux':
            return [self.cc, *self.root.TARGET, '-shared', '-fuse-ld=lld', *inputs, '-o', out]
        return [self.cc, *self.root.TARGET, '-dynamiclib', *inputs, '-o', out]

    def response_file(self, objs, path):
        """Writes a response file listing the objects (forward slashes: clang reads backslashes in quotes
        as escapes on Linux and macOS)."""
        with open(path, 'w') as f:
            f.write('\n'.join('"%s"' % o.replace('\\', '/') for o in objs))
        return path


def toolchain(target=None, root=None):
    """The toolchain fighters and mods are built with on this machine (or for `target`, with `root` the
    root build's toolchain module, which tests stand in for)."""
    target = target or host()
    if target not in TARGETS:
        raise SystemExit('target %s: one of %s' % (target, ', '.join(TARGETS)))
    return Toolchain(target, root if root is not None else load_root(target))
