"""Standalone plugin imports and preparation, using synthetic files and subprocesses only."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import mods
import prepare_plugins as prepare
import skyline
import recomp


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.tree = mods.Tree(str(self.root))
        (self.root / "build").mkdir()
        self.exe = self.root / "build" / "ssbu.exe"
        self.exe.write_bytes(b"synthetic executable")
        self.sky = self.root / "dump" / "skyline"
        self.sky.mkdir(parents=True)

    def imported(self):
        archive = self.root / "release.zip"
        prefix = "atmosphere/contents/01006A800016E000/romfs/skyline/plugins/"
        # A made-up NRO header is enough for identifying and installing these files.
        with zipfile.ZipFile(archive, "w") as output:
            for i, name in enumerate(("libnro_hook.nro", "libparam_hook.nro", "libtraining_modpack.nro"), 1):
                data = bytearray(0x60)
                data[0x10:0x14] = b"NRO0"
                data[0x40] = i
                output.writestr(prefix + name, data)
        return mods.install(self.tree, str(archive), "Training")


class TreeTests(Fixture):
    def test_original_sd_archive_keeps_and_selects_dependency_names(self):
        self.imported()
        names = [Path(source).name for _mod, source in skyline.turned_on(str(self.root))]
        self.assertEqual(names, ["libnro_hook.nro", "libparam_hook.nro", "libtraining_modpack.nro"])
        self.assertFalse(any(self.root.rglob("dependency_nro_hook.nro")))

    def test_disabled_archive_is_not_selected(self):
        name = self.imported()[0]
        state = self.tree.load()
        state["mods"][name]["enabled"] = False
        self.tree.save(state)
        self.assertEqual(skyline.turned_on(str(self.root)), [])

    def test_dynamic_plugin_does_not_require_being_linked_into_game(self):
        (self.sky / "plugins.txt").write_text("example\n")
        (self.sky / "plugin_dlls.txt").write_text("build/plugins/example.dll\n")
        with mock.patch.object(skyline, "unbuilt_standard", return_value=None), \
             mock.patch.object(skyline, "stale_hooks", return_value=[]), \
             mock.patch.object(skyline, "built_in") as built:
            self.assertIsNone(skyline.game_link_reason(str(self.root)))
        built.assert_not_called()

    def test_generated_inline_site_still_requires_linking_into_executable(self):
        folder = self.root / "build/main"
        folder.mkdir()
        (folder / "g_module.c").write_text("/* synthetic */")
        generated = folder / "g0000.c"
        generated.write_text("/* hook already generated */")
        (self.sky / "hooks.txt").write_text("main 40 inline\n")
        os.utime(self.exe, (1, 1))
        with mock.patch.object(skyline, "unbuilt_standard", return_value=None), \
             mock.patch.object(skyline, "stale_hooks", return_value=[]):
            self.assertIn("have not been linked", skyline.game_link_reason(str(self.root)))

    def test_removing_last_code_patch_requires_restoring_generated_module(self):
        folder = self.root / "build" / "main"
        folder.mkdir()
        (folder / "g_module.c").write_text("/* synthetic */")
        (folder / "plugin_patches.txt").write_text("40 1 2\n")
        with mock.patch.object(recomp, "TREE", str(self.root)):
            self.assertEqual(skyline._stale_hooks(str(self.root)), ["main"])

    def test_missing_inline_site_is_detected(self):
        folder = self.root / "build" / "main"
        folder.mkdir()
        (folder / "g_module.c").write_text("/* synthetic */")
        (folder / "g0000.c").write_text("/* no inline call here */")
        (self.sky / "hooks.txt").write_text("main 40 inline\n")
        with mock.patch.object(recomp, "TREE", str(self.root)):
            self.assertEqual(skyline._stale_hooks(str(self.root)), ["main"])
            (folder / "g0000.c").write_text(f"rt_inline_hook(c, 0x{recomp.MODULES['main'] + 0x40:x}ull);")
            self.assertEqual(skyline._stale_hooks(str(self.root)), [])

    def test_unchanged_character_entries_do_not_reapply_resources(self):
        (self.sky / "plugins.txt").write_text("example\n")
        (self.sky / "csk.json").write_text("[]\n")
        with mock.patch.object(subprocess, "run", return_value=mock.Mock(returncode=0)):
            self.assertFalse(skyline.probe(str(self.root), lambda _s: None))
        self.assertEqual((self.sky / "csk.json").read_text(), "[]\n")

    def test_probe_failure_is_not_reported_as_no_changes(self):
        (self.sky / "plugins.txt").write_text("example\n")
        with mock.patch.object(subprocess, "run", return_value=mock.Mock(returncode=5)), \
             self.assertRaisesRegex(RuntimeError, "probe failed"):
            skyline.probe(str(self.root), lambda _s: None)

    def test_probe_clears_automation_and_network_and_isolates_save(self):
        (self.sky / "plugins.txt").write_text("example\n")
        with mock.patch.dict(os.environ, {"SSBU_INPUT": "8:A", "SSBU_NET_HOST": "example.invalid", "SSBU_DATA_ROOT": "player-save"}), \
             mock.patch.object(subprocess, "run", return_value=mock.Mock(returncode=0)) as run:
            self.assertFalse(skyline.probe(str(self.root), lambda _s: None))
        env = run.call_args.kwargs["env"]
        self.assertNotIn("SSBU_INPUT", env)
        self.assertNotIn("SSBU_NET_HOST", env)
        self.assertEqual(env["SSBU_RB"], "0")
        self.assertEqual(Path(env["SSBU_DATA_ROOT"]), self.root / "build/work/skyline-probe")

    def test_compile_failure_does_not_publish_a_partial_dll_list(self):
        (self.sky / "plugin_dlls.txt").write_text("previous.dll\n")
        folder = self.root / "build/nro/example"
        folder.mkdir(parents=True)
        (folder / "manifest.tsv").write_text("synthetic")
        with mock.patch.object(skyline, "turned_on", return_value=[("Example", "example.nro")]), \
             mock.patch.object(skyline, "module_of", return_value=("ID", "example")), \
             mock.patch.object(skyline, "dll_current", return_value=False), \
             mock.patch.object(recomp, "compile_dir", return_value=False), \
             self.assertRaisesRegex(RuntimeError, "did not compile"):
            skyline.cache_mod_plugins(str(self.root), 1, lambda _s: None, strict=True)
        self.assertEqual((self.sky / "plugin_dlls.txt").read_text(), "previous.dll\n")


class PreparationTests(Fixture):
    def setUp(self):
        super().setUp()
        self.imported()
        self.enterContext(mock.patch.object(prepare, "game_open", return_value=False))
        self.build = self.enterContext(mock.patch.object(skyline, "build", return_value=0))
        self.enterContext(mock.patch.object(skyline, "plugin_refresh_reason", return_value=None))

    def run_prepare(self, **kwargs):
        prepare.prepare(self.root, log=lambda _s: None, **kwargs)

    def test_running_game_prevents_rebuild(self):
        with mock.patch.object(prepare, "game_open", return_value=True), \
             self.assertRaisesRegex(RuntimeError, "Close the game"):
            self.run_prepare()
        self.build.assert_not_called()

    def test_success_is_cached(self):
        self.run_prepare()
        self.run_prepare()
        self.assertEqual(self.build.call_count, 1)

    def test_changed_plugin_bytes_invalidate_same_build_id(self):
        self.run_prepare()
        plugin = next((self.root / "mods").rglob("libtraining_modpack.nro"))
        plugin.write_bytes(plugin.read_bytes() + b"new contents")
        self.run_prepare()
        self.assertEqual(self.build.call_count, 2)

    def test_cached_success_needs_no_compiler_environment(self):
        with mock.patch.dict(os.environ, {"SSBU_CLANG": "local-compiler"}):
            self.run_prepare()
        with mock.patch.dict(os.environ, {"SSBU_CLANG": ""}):
            self.run_prepare()
        self.assertEqual(self.build.call_count, 1)

    def test_changed_game_invalidates_cache(self):
        self.run_prepare()
        self.exe.write_bytes(b"different build")
        self.run_prepare()
        self.assertEqual(self.build.call_count, 2)

    def test_new_runtime_hook_invalidates_cache(self):
        self.run_prepare()
        (self.sky / "hooks.txt").write_text("main 40 inline\n")
        self.run_prepare()
        self.assertEqual(self.build.call_count, 2)

    def test_failed_build_is_remembered_until_retry(self):
        self.build.return_value = 1
        with self.assertRaisesRegex(RuntimeError, "build failed"):
            self.run_prepare()
        with self.assertRaisesRegex(RuntimeError, "previous plugin preparation failed"):
            self.run_prepare()
        self.assertEqual(self.build.call_count, 1)
        self.build.return_value = 0
        self.run_prepare(force=True)
        self.assertEqual(self.build.call_count, 2)

    def test_disabling_last_plugin_clears_stale_code_patches(self):
        self.run_prepare()
        state = self.tree.load()
        for entry in state["mods"].values():
            entry["enabled"] = False
        self.tree.save(state)
        patch = self.sky / "patches.txt"
        patch.write_text("main 40 1 2\n")
        self.run_prepare()
        self.assertEqual(patch.read_text(), "")
        self.assertEqual(self.build.call_count, 2)

    def test_lock_rejects_concurrent_preparation_and_releases_after_error(self):
        work = self.root / "build/work"
        work.mkdir()
        with self.assertRaisesRegex(RuntimeError, "Another plugin preparation"):
            with prepare.preparing(work), prepare.preparing(work):
                pass
        with prepare.preparing(work):
            pass


if __name__ == "__main__":
    unittest.main()
