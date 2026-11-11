"""A mod plugin's own DLL (tools/skyline.py cache_mod_plugins): the loader stub it is linked with
(_BIND) provides every runtime function translated plugin code calls. A plugin whose code makes a
system call (an `svc`: Raichu's moveset, svcQueryMemory) calls rt_svc; without it in the stub its
DLL does not link ("undefined symbol: rt_svc") and the mod never runs. On Windows the stub and a
made-up translated module are compiled and linked as cache_mod_plugins does it (cc.compile_command
with GAME_CFLAGS, cc.link_library), and a host binds the DLL as runtime/host.c load_plugin_dlls
does; elsewhere with mingw-w64, the host run under wine. Skipped without those."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import cc  # noqa: E402
import skyline  # noqa: E402

GCC = shutil.which("x86_64-w64-mingw32-gcc")
WINE = shutil.which("wine") or shutil.which("wine64")
if os.name == "nt":
    HAVE = os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None
    WHY = "needs clang-cl (deps\\llvm, or Visual Studio's C++ Clang tools)"
    RUNS, RUNS_WHY = HAVE, WHY
else:
    HAVE = GCC is not None
    WHY = "needs clang-cl on Windows, or mingw-w64 elsewhere"
    RUNS, RUNS_WHY = HAVE and WINE is not None, "needs mingw-w64 and wine outside Windows"

# a translated plugin: its module record, and a function whose code is one `svc #6`
MODULE = r'''
#include "module.h"
const RtModule rt_module_madeup = {"madeup"};
__declspec(dllexport) void madeup_query_memory(Cpu *c) { rt_svc(c, 6); }
'''

# the runtime's side: binds the DLL as runtime/host.c load_plugin_dlls does, then runs the function
HOST = r'''
#include <windows.h>
#include <stdio.h>
#include "module.h"
static u32 svc_number = 0xFFFFFFFF;
GuestFn *host_lookup(u64 a) { (void)a; return 0; }
GuestFn *host_lookup_cached(u64 a, RtCache *c) { (void)a; (void)c; return 0; }
void host_trap(Cpu *c, u64 pc, const char *why) { (void)c; (void)pc; printf("trap %s\n", why); }
void host_svc(Cpu *c, u32 n) { (void)c; svc_number = n; }
int main(void) {
    HMODULE lib = LoadLibraryA("madeup.dll");
    if (!lib) { printf("no dll\n"); return 2; }
    void (*bind)(GuestFn *(*)(u64), GuestFn *(*)(u64, RtCache *), void (*)(Cpu *, u64, const char *),
                 void (*)(Cpu *, u32)) = (void *)GetProcAddress(lib, "ssbu_plugin_bind");
    GuestFn *fn = (GuestFn *)GetProcAddress(lib, "madeup_query_memory");
    if (!bind || !fn) { printf("missing export\n"); return 3; }
    bind(host_lookup, host_lookup_cached, host_trap, host_svc);
    static Cpu cpu;
    fn(&cpu);
    printf("svc %u\n", svc_number);
    return 0;
}
'''


@unittest.skipUnless(HAVE, WHY)
class PluginBindTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="plugin bind test ")
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        for name, text in (("bind.c", skyline._BIND.format(name="madeup")), ("module.c", MODULE), ("host.c", HOST)):
            (self.dir / name).write_text(text, encoding="utf-8")

    def link_plugin(self):
        """'' when the plugin's DLL linked, else the compiler's or linker's output."""
        dll = self.dir / "madeup.dll"
        if os.name != "nt":
            r = subprocess.run([GCC, "-shared", "-O1", "-I", str(ROOT / "runtime"), "-o", str(dll),
                                str(self.dir / "module.c"), str(self.dir / "bind.c")], capture_output=True, text=True)
            return "" if r.returncode == 0 else r.stdout + r.stderr
        objs = []
        for name in ("module", "bind"):
            obj = str(self.dir / (name + ".obj"))
            r = subprocess.run(cc.compile_command(str(self.dir / (name + ".c")), obj, "/O1", cc.GAME_CFLAGS),
                               capture_output=True, text=True)
            if r.returncode:
                return r.stdout + r.stderr
            objs.append(obj)
        try:
            cc.link_library(objs, str(dll))
        except RuntimeError as e:
            return str(e)
        return ""

    def run_host(self):
        exe = self.dir / "host.exe"
        if os.name == "nt":
            build = [cc.CLANG, "/nologo", "/O1", *cc.LINK_FLAGS, *cc.BASE_CFLAGS, "/I", str(ROOT / "runtime"),
                     str(self.dir / "host.c"), "/Fe" + str(exe), "/Fo" + str(self.dir) + os.sep, "/link", *cc.LINK_OPTIONS]
            run, env = [str(exe)], None
        else:
            build = [GCC, "-O1", "-I", str(ROOT / "runtime"), "-o", str(exe), str(self.dir / "host.c")]
            run, env = [WINE, str(exe)], {**os.environ, "WINEDEBUG": "-all", "WINEPREFIX": str(self.dir / "wp")}
        subprocess.run(build, check=True, capture_output=True, text=True)
        r = subprocess.run(run, cwd=self.dir, env=env, capture_output=True, text=True, timeout=300)
        return r.stdout, r.stderr   # (wine's own messages go to stderr)

    def test_plugin_with_a_system_call_links(self):
        self.assertEqual(self.link_plugin(), "")

    @unittest.skipUnless(RUNS, RUNS_WHY)
    def test_the_runtimes_svc_handles_the_plugins_system_call(self):
        self.assertEqual(self.link_plugin(), "")
        out, err = self.run_host()
        self.assertEqual(out.split()[-2:], ["svc", "6"], out + err)


if __name__ == "__main__":
    unittest.main()
