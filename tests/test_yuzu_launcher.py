"""PowerShell launcher behavior with synthetic executables, no game or network."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

# This process stands in for the game and Python. The production launch script
# runs unchanged in an isolated tree. Each child records its actual environment.
HELPER = r'''
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
    char path[4096]; const char *root=getenv("YUZU_TEST_ROOT");
    if(!root) return 90;
    const char *kind=argc>1 && !strcmp(argv[1],"--yuzu-client-check")?"feature":
        argc>1 && strstr(argv[1],"skyline.py")?"prepare":
        argc>1 && strstr(argv[1],"yuzu_client_deps.py")?"deps":"game";
    snprintf(path,sizeof path,"%s/calls.txt",root);
    FILE *f=fopen(path,"ab"); if(!f) return 91;
    fprintf(f,"%s\n",kind); fclose(f);
    if(!strcmp(kind,"feature")) {
        puts(getenv("YUZU_TEST_OLD_EXE")?"old-runtime":"yuzu-room-client protocol=1"); return 0;
    }
    if(!strcmp(kind,"prepare")) {
        const char *code=getenv("YUZU_TEST_PREPARE_EXIT");return code?atoi(code):0;
    }
    if(!strcmp(kind,"deps")) return 0;
    snprintf(path,sizeof path,"%s/observed.txt",root);
    f=fopen(path,"wb");if(!f)return 92;
    const char *keys[]={"SSBU_NET_BACKEND","SSBU_YUZU_SERVER","SSBU_NET_NAME",
        "SSBU_YUZU_PROXY_PROFILE","SSBU_YUZU_PASSWORD","SSBU_RB","SSBU_RUNAHEAD",
        "SSBU_NET_HOST","SSBU_NET_RELAY","SSBU_RB_NET","SSBU_DATA_ROOT","SSBU_INPUT"};
    for(unsigned i=0;i<sizeof keys/sizeof *keys;i++){
        const char *v=getenv(keys[i]);fprintf(f,"%s=%s\n",keys[i],v?v:"");
    }
    for(int i=1;i<argc;i++)fprintf(f,"arg=%s\n",argv[i]);
    fclose(f);return 0;
}
'''


@unittest.skipUnless(os.name == 'nt', 'Windows PowerShell launcher')
class YuzuLauncherTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import cc
        clang = cc.CLANG if Path(cc.CLANG).is_file() else shutil.which(cc.CLANG)
        if not clang:
            raise unittest.SkipTest('clang-cl required')
        cls.compiled = tempfile.TemporaryDirectory(prefix='yuzu launcher helper ')
        folder = Path(cls.compiled.name)
        source = folder / 'helper.c'; source.write_text(HELPER)
        cls.helper = folder / 'helper.exe'
        built = subprocess.run([clang, '/nologo', '/O1', *cc.LINK_FLAGS, str(source),
                                '/Fe' + str(cls.helper), '/Fo' + str(folder) + os.sep,
                                '/link', *cc.LINK_OPTIONS], capture_output=True, text=True, timeout=60)
        if built.returncode:
            cls.compiled.cleanup()
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.compiled.cleanup()

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='yuzu launcher tree ')
        self.addCleanup(temp.cleanup)
        self.tree = Path(temp.name)
        for folder in ('tools', 'build/yuzu', '.venv/Scripts', 'config'):
            (self.tree / folder).mkdir(parents=True)
        shutil.copy2(ROOT / 'tools/yuzu_client.ps1', self.tree / 'tools/yuzu_client.ps1')
        shutil.copy2(self.helper, self.tree / 'build/ssbu.exe')
        shutil.copy2(self.helper, self.tree / '.venv/Scripts/python.exe')
        for name in ('enet.dll', 'libzstd.dll'):
            (self.tree / 'build/yuzu' / name).write_bytes(b'fixture-only')
        self.config = self.tree / 'config/yuzu_client.json'
        self.config.write_text(json.dumps({'server': 'saved.example:24872', 'nickname': 'SavedName'}))

    def run_launcher(self, *args, prepare_exit=None, old_exe=False):
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('SSBU_', 'YUZU_TEST_'))}
        env.update(YUZU_TEST_ROOT=str(self.tree), SSBU_NET_HOST='1', SSBU_NET_RELAY='native.example:39420',
                   SSBU_RB_NET='online', SSBU_DATA_ROOT='old-root', SSBU_INPUT='old-input')
        if prepare_exit is not None:
            env['YUZU_TEST_PREPARE_EXIT'] = str(prepare_exit)
        if old_exe:
            env['YUZU_TEST_OLD_EXE'] = '1'
        return subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                               str(self.tree / 'tools/yuzu_client.ps1'), '-NoPrompt', *args],
                              env=env, cwd=ROOT, capture_output=True, text=True, timeout=30)

    def calls(self):
        path = self.tree / 'calls.txt'
        return path.read_text().splitlines() if path.exists() else []

    def test_check_only_prepares_plugins_and_preserves_saved_settings(self):
        before = self.config.read_bytes()
        ran = self.run_launcher('-CheckOnly')
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertEqual(self.calls(), ['feature', 'prepare'])
        self.assertIn('UDP profile: eden', ran.stdout)
        self.assertEqual(self.config.read_bytes(), before)
        self.assertFalse((self.tree / 'observed.txt').exists())

    def test_preparation_failure_stops_before_starting_or_saving(self):
        before = self.config.read_bytes()
        ran = self.run_launcher('-Server', 'override.example:5000', prepare_exit=7)
        self.assertNotEqual(ran.returncode, 0)
        self.assertIn('Plugin preparation failed', ' '.join(ran.stderr.split()))   # (PowerShell wraps its errors at the console's width)
        self.assertEqual(self.calls(), ['feature', 'prepare'])
        self.assertEqual(self.config.read_bytes(), before)

    def test_old_runtime_is_rejected_before_preparation(self):
        ran = self.run_launcher('-CheckOnly', old_exe=True)
        self.assertNotEqual(ran.returncode, 0)
        self.assertIn('has no yuzu client', ' '.join(ran.stderr.split()))
        self.assertEqual(self.calls(), ['feature'])

    def test_invalid_saved_profile_is_rejected(self):
        self.config.write_text(json.dumps({'server': 'saved.example:24872', 'nickname': 'SavedName',
                                          'proxy_profile': 'unknown'}))
        ran = self.run_launcher('-CheckOnly')
        self.assertNotEqual(ran.returncode, 0)
        self.assertIn('ProxyProfile must be eden or yuzu', ' '.join(ran.stderr.split()))
        self.assertEqual(self.calls(), ['feature'])

    def test_launch_selects_profile_and_clears_native_settings(self):
        ran = self.run_launcher('-Server', 'room.example:5000', '-Nickname', 'TestGuest', '-ProxyProfile', 'YUZU')
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        observed = self.tree / 'observed.txt'
        end = time.monotonic() + 5
        while not observed.exists() and time.monotonic() < end:
            time.sleep(.02)
        self.assertTrue(observed.exists(), ran.stdout + ran.stderr)
        values = dict(line.split('=', 1) for line in observed.read_text().splitlines())
        self.assertEqual(self.calls(), ['feature', 'prepare', 'game'])
        for key, value in {'SSBU_NET_BACKEND': 'yuzu', 'SSBU_YUZU_SERVER': 'room.example:5000',
                           'SSBU_NET_NAME': 'TestGuest', 'SSBU_YUZU_PROXY_PROFILE': 'yuzu',
                           'SSBU_RB': '0', 'SSBU_RUNAHEAD': '0'}.items():
            self.assertEqual(values[key], value)
        for key in ('SSBU_NET_HOST', 'SSBU_NET_RELAY', 'SSBU_RB_NET', 'SSBU_DATA_ROOT', 'SSBU_INPUT', 'SSBU_YUZU_PASSWORD'):
            self.assertEqual(values[key], '')
        self.assertEqual(json.loads(self.config.read_text(encoding='utf-8-sig')),
                         dict(server='room.example:5000', nickname='TestGuest', proxy_profile='yuzu'))
        self.assertIn('--online', observed.read_text())


if __name__ == '__main__':
    unittest.main()
