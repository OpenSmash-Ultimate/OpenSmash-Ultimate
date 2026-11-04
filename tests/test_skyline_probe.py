"""Skyline discovery must not use player data or silently accept a failed boot."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import cc
import skyline


class ProbeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="skyline probe test ")
        self.addCleanup(self.tmp.cleanup)
        self.tree = Path(self.tmp.name)
        self.sky = self.tree / "dump/skyline"
        self.sky.mkdir(parents=True)
        (self.sky / "plugins.txt").write_text("example\n", encoding="utf-8")
        self.enterContext(mock.patch.object(skyline, "program", return_value=str(self.tree / "game")))

    def probe(self):
        return skyline.probe(str(self.tree), lambda _line: None)

    def test_empty_list_does_not_start_a_probe(self):
        (self.sky / "plugins.txt").write_text("", encoding="utf-8")
        with mock.patch.object(subprocess, "run") as run:
            self.assertFalse(self.probe())
        run.assert_not_called()

    def test_data_is_fresh_plugin_free_and_removed_after_each_boot(self):
        roots = []
        def boot(command, **kwargs):
            env = kwargs["env"]
            data = Path(env["SSBU_DATA_ROOT"])
            roots.append(data)
            self.assertTrue(data.is_dir())
            self.assertEqual(list(data.iterdir()), [])
            self.assertNotEqual(data, self.tree / "save")
            self.assertEqual(command[1], str(self.tree / "dump/modules"))
            for name in ("SSBU_MODS", "SSBU_CHARACTERS", "SSBU_INPUT", "SSBU_SEQ", "SSBU_NET_HOST",
                         "SSBU_CRASH_TEST", "SSBU_DEBUG_MATCH", "SSBU_SKYLINE_LOG"):
                self.assertNotIn(name, env)
            self.assertEqual(env["SSBU_HEADLESS"], "1")
            self.assertEqual(env["SSBU_RB"], "0")
            self.assertEqual(env["SSBU_RUNAHEAD"], "0")
            self.assertEqual(env["SSBU_SKYLINE_PROBE"], "1")
            self.assertEqual(env["PATH"], os.environ["PATH"])
            (data / "plugins").mkdir()
            (data / "plugins/unexpected.dll").write_bytes(b"synthetic prior probe artifact")
            return subprocess.CompletedProcess(command, 0)
        inherited = {name: "player value" for name in (
            "SSBU_MODS", "SSBU_CHARACTERS", "SSBU_INPUT", "SSBU_SEQ", "SSBU_NET_HOST",
            "SSBU_DATA_ROOT", "SSBU_CRASH_TEST", "SSBU_DEBUG_MATCH", "SSBU_SKYLINE_LOG")}
        with mock.patch.dict(os.environ, inherited), mock.patch.object(subprocess, "run", side_effect=boot):
            self.assertFalse(self.probe())
            self.assertFalse(self.probe())
        self.assertNotEqual(roots[0], roots[1])
        self.assertTrue(all(not path.exists() for path in roots))

    def test_failed_runtime_boot_is_an_error_with_persistent_diagnostics(self):
        data = []
        def boot(command, **kwargs):
            data.append(Path(kwargs["env"]["SSBU_DATA_ROOT"]))
            kwargs["stdout"].write("[skyline] probe failed: 1 plugins could not load\n")
            return subprocess.CompletedProcess(command, 1)
        with mock.patch.object(subprocess, "run", side_effect=boot), \
             self.assertRaisesRegex(RuntimeError, "code 1; dump/skyline/probe.log"):
            self.probe()
        self.assertFalse(data[0].exists())
        self.assertIn("could not load", (self.sky / "probe.log").read_text(encoding="utf-8"))

    def test_timeout_is_an_error_and_cleans_temporary_data(self):
        data = []
        def boot(command, **kwargs):
            data.append(Path(kwargs["env"]["SSBU_DATA_ROOT"]))
            raise subprocess.TimeoutExpired(command, kwargs["timeout"])
        with mock.patch.object(subprocess, "run", side_effect=boot), \
             self.assertRaisesRegex(RuntimeError, "probe timed out"):
            self.probe()
        self.assertFalse(data[0].exists())

    def test_launch_error_also_cleans_temporary_data(self):
        data = []
        def boot(command, **kwargs):
            data.append(Path(kwargs["env"]["SSBU_DATA_ROOT"]))
            raise OSError("synthetic launch failure")
        with mock.patch.object(subprocess, "run", side_effect=boot), \
             self.assertRaisesRegex(OSError, "synthetic launch failure"):
            self.probe()
        self.assertFalse(data[0].exists())

    def test_character_entries_and_callbacks_still_detect_changes(self):
        (self.sky / "csk.json").write_text("[]", encoding="utf-8")
        def boot(command, **kwargs):
            (self.sky / "csk.json").write_text(json.dumps([{"name": "synthetic"}]), encoding="utf-8")
            (self.sky / "callbacks.txt").write_text("synthetic callback", encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.object(subprocess, "run", side_effect=boot):
            self.assertTrue(self.probe())

    def test_unchanged_character_entries_do_not_reapply_resources(self):
        (self.sky / "csk.json").write_text("[]", encoding="utf-8")
        with mock.patch.object(subprocess, "run", return_value=mock.Mock(returncode=0)):
            self.assertFalse(self.probe())
        self.assertEqual((self.sky / "csk.json").read_text(encoding="utf-8"), "[]")


@unittest.skipUnless(os.name == "nt" and (os.path.isfile(cc.CLANG) or shutil.which(cc.CLANG)),
                     "needs Windows clang-cl")
class DeviceIdTests(unittest.TestCase):
    def test_actual_shim_writes_one_stable_uuid_and_returns_success(self):
        # Compile the actual function, not a Python reimplementation, with a synthetic
        # guest mapping so using the raw guest pointer would fail this test.
        text = (ROOT / "runtime/hle_skyline.c").read_text(encoding="utf-8")
        start = text.index("static void pseudo_device_id(Cpu *c) {")
        end = text.index("\n}", start) + 2
        function = text[start:end]
        self.assertIn('{"_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", pseudo_device_id}', text)
        source = r"""
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint64_t u64;
typedef struct { u64 x[31]; } Cpu;
static unsigned char memory[64];
#define G2H(address) (memory + (address))
""" + function + r"""
int main(void) {
    Cpu c = {0};
    unsigned char first[16];
    memset(memory, 0xA5, sizeof memory);
    c.x[0] = 8;
    c.x[1] = 0x12345678;
    pseudo_device_id(&c);
    if (c.x[0] != 0 || c.x[1] != 0x12345678) return 1;
    memcpy(first, memory + 8, sizeof first);
    int changed = 0;
    for (int i = 0; i < 16; i++) changed |= first[i] != 0xA5;
    if (!changed) return 2;
    for (int i = 0; i < 64; i++)
        if ((i < 8 || i >= 24) && memory[i] != 0xA5) return 3;
    c.x[0] = 32;
    pseudo_device_id(&c);
    if (c.x[0] || memcmp(first, memory + 32, 16)) return 4;
    c.x[0] = 0;
    pseudo_device_id(&c);
    return c.x[0] != 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="skyline UUID ") as folder:
            path = Path(folder) / "test.c"
            exe = Path(folder) / "test.exe"
            path.write_text(source, encoding="utf-8")
            command = [cc.CLANG, "/nologo", "/O1", *cc.BASE_CFLAGS, str(path), "/Fe" + str(exe),
                       *cc.LINK_FLAGS, "/link", *cc.LINK_OPTIONS]
            ran = subprocess.run(command, cwd=folder, capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            ran = subprocess.run([str(exe)], capture_output=True)
            self.assertEqual(ran.returncode, 0)


if __name__ == "__main__":
    unittest.main()
