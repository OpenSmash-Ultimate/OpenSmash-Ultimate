"""Build a mod (a DLL that uses fighter_sdk/include/hook.h) into a mods folder.

    python fighter_sdk/tools/build_mod.py <mod.cpp> [<out folder, default mods>]

Run the game with SSBU_MODS=<folder>: every DLL of the folder is loaded before the game starts and its
mod_init(const HookApi *) is called. Nothing of the game is rebuilt. The compiler and its flags are the
game's own (fighter_cc.py); on Linux and macOS the mod is <name>.so / <name>.dylib, which the port does not
load yet (hook_host.c loads mods on Windows only). The default folder is mods/ in the work folder.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fighter_cc  # noqa: E402
import sdk_paths   # noqa: E402


def main():
    if len(sys.argv) not in (2, 3) or sys.argv[1].startswith('-'):
        print(__doc__)
        return 2
    src = os.path.abspath(sdk_paths.first('The mod source', [sys.argv[1]]))
    out = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.join(sdk_paths.WORK, 'mods')
    os.makedirs(out, exist_ok=True)
    tc = fighter_cc.toolchain()
    dll = os.path.join(out, tc.library(os.path.splitext(os.path.basename(src))[0]))
    with tempfile.TemporaryDirectory(prefix='build_mod-') as tmp:
        obj = os.path.join(tmp, 'mod' + tc.obj)
        for cmd in (tc.compile_command(src, obj, [sdk_paths.INCLUDE]), tc.link_command([obj], dll)):
            r = subprocess.run(cmd, capture_output=True, text=True)
            text = (r.stdout + r.stderr).strip()
            if text:
                print(text[-3000:])
            if r.returncode:
                return 1
    for ext in ('.lib', '.exp'):                           # (the linker's import library, on Windows)
        if os.path.exists(os.path.splitext(dll)[0] + ext):
            os.remove(os.path.splitext(dll)[0] + ext)
    print('built %s (%d KB)' % (dll, os.path.getsize(dll) // 1024))
    return 0


if __name__ == '__main__':
    sys.exit(main())
