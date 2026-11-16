"""Named Skyline libraries must not disappear when ARCropolis owns plugin.nro loading."""
from pathlib import Path
import json
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import mods
import paramconfig
import skyline


class LoadListTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.plugins = []
        self.enterContext(mock.patch.object(skyline, "seed_lists"))
        self.enterContext(mock.patch.object(skyline, "generate"))
        self.enterContext(mock.patch.object(skyline, "built_in", return_value=True))
        self.enterContext(mock.patch.object(paramconfig, "write"))
        def module_of(_paths, source, _record, _prefer=None):
            name = Path(source).stem.removeprefix("lib")
            return name.upper(), name
        self.enterContext(mock.patch.object(skyline, "module_of", side_effect=module_of))
        self.enterContext(mock.patch.object(skyline, "turned_on", side_effect=lambda _tree: self.plugins))

    def add(self, name, file):
        source = self.root / ("skyline/plugins" if name.startswith("(standard)") else "mods/" + name) / file
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(b"synthetic")
        self.plugins.append((name, str(source)))

    def sync(self):
        return skyline.sync(str(self.root), lambda _line: None)

    def test_training_libraries_stay_direct_while_moveset_plugin_is_chainloaded(self):
        self.add("(standard) libarcropolis.nro", "libarcropolis.nro")
        self.add("(standard) libnro_hook.nro", "libnro_hook.nro")
        self.add("Moveset", "plugin.nro")
        self.add("Training", "atmosphere/contents/title/romfs/skyline/plugins/libparam_hook.nro")
        self.add("Training", "atmosphere/contents/title/romfs/skyline/plugins/libtraining_modpack.nro")
        self.assertEqual(self.sync(), ["arcropolis", "nro_hook", "param_hook", "training_modpack"])
        path = self.root / "dump/skyline/plugins.txt"
        self.assertEqual(path.read_text().splitlines(), ["arcropolis", "nro_hook", "param_hook", "training_modpack"])
        # The chainloaded plugin is still live for patch ownership/pruning.
        record = json.loads((self.root / "dump/skyline/plugins.json").read_text())
        self.assertIn("PLUGIN", record)

    def test_without_arcropolis_every_plugin_is_direct(self):
        self.add("Moveset", "plugin.nro")
        self.add("Training", "libtraining_modpack.nro")
        self.assertEqual(self.sync(), ["plugin", "training_modpack"])

    def test_standard_plugin_named_plugin_nro_stays_direct(self):
        self.add("(standard) libarcropolis.nro", "libarcropolis.nro")
        self.add("(standard) plugin.nro", "plugin.nro")
        self.assertEqual(self.sync(), ["arcropolis", "plugin"])

    def test_skip_list_cannot_be_bypassed_by_named_library(self):
        self.add("(standard) libarcropolis.nro", "libarcropolis.nro")
        self.add("Training", "libtraining_modpack.nro")
        folder = self.root / "dump/skyline"
        folder.mkdir(parents=True)
        (folder / "skip.txt").write_text("training_modpack\n")
        self.assertEqual(self.sync(), ["arcropolis"])

    @unittest.skipUnless(sys.platform == "win32", "Windows plugins are DLLs")
    def test_named_dll_does_not_require_being_linked_into_executable(self):
        folder = self.root / "dump/skyline"
        folder.mkdir(parents=True)
        (folder / "plugins.txt").write_text("training_modpack\n")
        (folder / "plugin_dlls.txt").write_text("build/plugins/training_modpack.dll\n")
        (self.root / "build").mkdir()
        exe = self.root / "build/ssbu.exe"
        exe.write_bytes(b"synthetic")
        with mock.patch.object(skyline, "program", return_value=str(exe)), \
             mock.patch.object(skyline, "unbuilt_standard", return_value=None), \
             mock.patch.object(skyline, "stale_hooks", return_value=[]), \
             mock.patch.object(skyline, "built_in", return_value=False), \
             mock.patch("std_plugins.PLUGINS", {}):
            self.assertIsNone(skyline.game_link_reason(str(self.root)))

    def test_same_module_is_not_initialized_twice(self):
        self.add("(standard) libarcropolis.nro", "libarcropolis.nro")
        self.add("One", "libtraining_modpack.nro")
        self.add("Two", "libtraining_modpack.nro")
        self.assertEqual(self.sync(), ["arcropolis", "training_modpack"])


if __name__ == "__main__":
    unittest.main()
