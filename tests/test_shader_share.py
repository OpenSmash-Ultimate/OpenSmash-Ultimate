"""One shader preparation for the launcher's background run and a game's own (tools/shader_aot.py PrepareLock,
vulkan_ready.txt, --by-game making the Vulkan part alone; runtime/gpu_vk.c prep_follow reads the same files)."""
from pathlib import Path
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import shader_aot as S  # noqa: E402


def read_shared(path, size):
    """The file's start, opened as runtime/gpu_vk.c prep_other opens the lock: sharing reading, writing and deleting
    (its holder has it open for writing, to be deleted on close)."""
    if os.name != "nt":
        with open(path, "rb") as f:
            return f.read(size)
    import ctypes
    import msvcrt
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateFileW.restype = ctypes.c_void_p
    k.CreateFileW.argtypes = (ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p)
    h = k.CreateFileW(path, 0x80000000, 0x7, None, 3, 0, None)   # (GENERIC_READ, every sharing, OPEN_EXISTING)
    if not h or h == ctypes.c_void_p(-1).value:
        raise OSError(ctypes.get_last_error(), "the lock cannot be read")
    fd = msvcrt.open_osfhandle(h, os.O_RDONLY)
    try:
        return os.read(fd, size)
    finally:
        os.close(fd)


class LockTests(unittest.TestCase):
    def test_one_holder_at_a_time_and_the_file_goes_with_it(self):
        with tempfile.TemporaryDirectory() as d:
            a, b = S.PrepareLock(d), S.PrepareLock(d)
            self.assertTrue(a.acquire())
            self.assertFalse(b.acquire())
            self.assertTrue(os.path.exists(a.path))
            a.release()
            self.assertFalse(os.path.exists(a.path))
            self.assertTrue(b.acquire())
            b.release()

    def test_the_state_is_one_line_another_process_can_read(self):
        with tempfile.TemporaryDirectory() as d:
            a = S.PrepareLock(d)
            self.assertTrue(a.acquire())
            try:
                for phase, pct, want in (("list", 40, "list 40"), ("vulkan", 7, "vulkan 7"), ("vulkan", 250, "vulkan 100"), ("rest", 0, "rest 0")):
                    a.say(phase, pct)
                    self.assertEqual(read_shared(a.path, S.PrepareLock.RECORD).decode().strip(), want)
            finally:
                a.release()

    @unittest.skipUnless(os.name == "nt", "the lock files are held the Windows way")
    def test_a_lock_nobody_holds_is_not_held_and_is_removed(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, S.PREPARE_LOCK)
            with open(path, "w") as f:
                f.write("vulkan 12\n")   # (left by a power cut)
            self.assertFalse(S.held(path))
            self.assertFalse(os.path.exists(path))
            a = S.PrepareLock(d)
            with open(path, "w") as f:
                f.write("vulkan 12\n")
            self.assertTrue(a.acquire())
            self.assertTrue(S.held(path))
            a.release()

    def test_a_second_run_waits_for_the_first_and_says_so(self):
        with tempfile.TemporaryDirectory() as d:
            a, b = S.PrepareLock(d), S.PrepareLock(d)
            self.assertTrue(a.acquire())
            said = []
            self.assertTrue(b.wait(sleep=lambda _s: a.release(), log=lambda text, **_k: said.append(text)))
            self.assertTrue(said[0].startswith("another shader preparation is running"))   # (the launcher's bar reads this)
            self.assertEqual(len(said), 2)
            b.release()

    @unittest.skipUnless(os.name == "nt", "the lock files are held the Windows way")
    def test_the_run_does_not_pause_for_the_game_that_waits_for_it(self):
        with tempfile.TemporaryDirectory() as d:
            run, game = S.PrepareLock(d), S.PrepareLock(d)
            game.path = os.path.join(d, S.GAME_WAITS)
            self.assertTrue(run.acquire())
            try:
                with mock.patch.object(S, "PREP", run), mock.patch.dict(os.environ, {"SSBU_SHADER_SHARE": "1"}):
                    self.assertFalse(S.game_waits(d))
                    self.assertTrue(game.acquire())
                    self.assertTrue(S.game_waits(d))
                    self.assertFalse(S.game_running())
                    with mock.patch.dict(os.environ, {"SSBU_SHADER_SHARE": "0"}):
                        self.assertFalse(S.game_waits(d))
            finally:
                game.release()
                run.release()


class MarkerTests(unittest.TestCase):
    def test_vulkan_ready_is_for_one_translator(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.dict(os.environ, {"SSBU_SHADER_SHARE": "1"}):
            self.assertFalse(S.vulkan_ready(d, "g1002.0000000b"))
            S.write_vulkan_ready(d, "g1002.0000000b")
            self.assertEqual(Path(d, S.VULKAN_READY).read_text(), "spirv g1002.0000000b\n")   # (gpu_vk.c reads "spirv <names>")
            self.assertTrue(S.vulkan_ready(d, "g1002.0000000b"))
            self.assertFalse(S.vulkan_ready(d, "g1003.0000000b"))
            self.assertFalse(S.vulkan_ready(d, ""))
            self.assertFalse(S.forget_vulkan_ready(d, "g1002.0000000b"))   # (the same names: it stays through a new run)
            self.assertTrue(S.forget_vulkan_ready(d, "g1003.0000000b"))
            self.assertFalse(os.path.exists(os.path.join(d, S.VULKAN_READY)))

    def test_vulkan_ready_keeps_the_other_translation(self):
        """The plain SPIR-V (the launcher's run) and the exact-f32tof16 one (an AMD game's run, #353) share the folder."""
        header = Path(S.ROOT, "runtime", "gpu_shader_cache.h").read_text(encoding="utf-8")
        version = S.cache_version() * 100 + int(S.SPV_VERSION.search(header).group(1))
        with tempfile.TemporaryDirectory() as d, mock.patch.dict(os.environ, {"SSBU_SHADER_SHARE": "1"}):
            plain, exact = sorted(S.spirv_siblings("g%d.0000000b" % version))
            self.assertNotEqual(plain, exact)
            S.write_vulkan_ready(d, exact)
            S.write_vulkan_ready(d, plain)
            self.assertTrue(S.vulkan_ready(d, plain) and S.vulkan_ready(d, exact))
            self.assertFalse(S.forget_vulkan_ready(d, exact))
            self.assertTrue(S.vulkan_ready(d, plain) and S.vulkan_ready(d, exact))
            self.assertTrue(S.forget_vulkan_ready(d, "g1.0000000b"))

    def test_not_written_when_not_shared_or_without_spirv(self):
        with tempfile.TemporaryDirectory() as d:
            with mock.patch.dict(os.environ, {"SSBU_SHADER_SHARE": "0"}):
                S.write_vulkan_ready(d, "g1002.0000000b")
            S.write_vulkan_ready(d, "")
            self.assertEqual(os.listdir(d), [])


class ByGameTests(unittest.TestCase):
    ENV = {"SSBU_AOT_BY_GAME": "1", "SSBU_SHADER_SHARE": "1", "SSBU_SHADER_SPIRV_FIRST": "1"}

    def test_the_games_run_never_starts_the_windowless_game(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.dict(os.environ, self.ENV), \
                mock.patch.object(S, "LINUX", False), mock.patch.object(S.subprocess, "Popen") as popen:
            self.assertFalse(S.pipeline_warm(d, game=__file__, limit=1.0))
            popen.assert_not_called()

    def run_queue(self, d, env, made=True):
        with mock.patch.dict(os.environ, env), mock.patch.object(S, "LINUX", False), mock.patch.object(S, "SPIRV", "g1002.0000000b"), \
                mock.patch.object(S, "discover", return_value=([], 0)), mock.patch.object(S, "drawn_keys", return_value=[]), \
                mock.patch.object(S, "run_spirv_pass", return_value=made), mock.patch.object(S, "pipeline_warm") as warm, \
                mock.patch.object(S, "cached_keys", return_value=set()), mock.patch.object(S, "load_times", return_value={}), \
                mock.patch("builtins.print"):
            return S.run_queue("exe", os.path.join(d, "programs.pack"), d, 2, False), warm

    def test_the_games_run_makes_the_vulkan_part_and_leaves_the_rest(self):
        with tempfile.TemporaryDirectory() as d:
            ok, warm = self.run_queue(d, self.ENV)
            self.assertIsNone(ok)   # (main: no complete.txt, exit 0)
            warm.assert_not_called()
            self.assertTrue(S.vulkan_ready(d, "g1002.0000000b"))

    def test_a_spirv_pass_that_failed_is_not_marked_ready(self):
        with tempfile.TemporaryDirectory() as d:
            ok, _warm = self.run_queue(d, self.ENV, made=False)
            self.assertIs(ok, False)
            self.assertFalse(os.path.exists(os.path.join(d, S.VULKAN_READY)))

    def test_the_launchers_run_marks_the_vulkan_part_then_goes_on(self):
        with tempfile.TemporaryDirectory() as d:
            env = dict(self.ENV, SSBU_AOT_BY_GAME="")
            ok, warm = self.run_queue(d, env)
            self.assertTrue(ok)   # (nothing left to compile: complete.txt follows)
            warm.assert_called_once()
            self.assertTrue(S.vulkan_ready(d, "g1002.0000000b"))

    def test_not_shared_the_games_run_is_the_whole_one(self):
        with mock.patch.dict(os.environ, dict(self.ENV, SSBU_SHADER_SHARE="0")), mock.patch.object(S, "LINUX", False), \
                mock.patch.object(S, "SPIRV", "g1002.0000000b"):
            self.assertFalse(S.vulkan_part_only())
        with mock.patch.dict(os.environ, dict(self.ENV, SSBU_SHADER_SPIRV_FIRST="0")), mock.patch.object(S, "LINUX", False), \
                mock.patch.object(S, "SPIRV", "g1002.0000000b"):
            self.assertFalse(S.vulkan_part_only())
        with mock.patch.dict(os.environ, self.ENV), mock.patch.object(S, "LINUX", False), mock.patch.object(S, "SPIRV", "g1002.0000000b"):
            self.assertTrue(S.vulkan_part_only())

    def test_the_vulkan_parts_progress_goes_to_the_lock(self):
        with tempfile.TemporaryDirectory() as d:
            run = S.PrepareLock(d)
            self.assertTrue(run.acquire())
            try:
                shown = []
                with mock.patch.object(S, "PREP", run):
                    show = S.said_to_lock(lambda state, **more: shown.append(state))
                    show([(1, None)])
                    self.assertIsNone(run.said)
                    show([(30, 120)], work=(50.0, 100.0))
                self.assertEqual(run.said, "vulkan 50")
                self.assertEqual(len(shown), 2)
            finally:
                run.release()


if __name__ == "__main__":
    unittest.main()
