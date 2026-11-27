"""Battle Arenas' "Enter Arena ID" (method 56 of matchmaking, runtime/net_nex_match.c's ar_56, #602): the game sends
the ID inside a Structure (version 0, its length, the String, 8 more bytes), and a bare String is still read. A small
C program includes net_nex_match.c, so it calls ar_56 itself against a store holding one arena. Skipped where there
is no clang-cl (tools/cc.py)."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

HAVE_CLANG = os.name == 'nt' and (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None)

HARNESS = r'''
#define _CRT_SECURE_NO_WARNINGS
#include "net_nex_match.c"
#include <stdio.h>
static NexSession arena;
static int count(void *ctx) { (void)ctx; return 1; }
static NexSession *at(void *ctx, int i) { (void)ctx; (void)i; return &arena; }
static int hex(int ch) { return ch <= '9' ? ch - '0' : (ch | 32) - 'a' + 10; }
int main(int argc, char **argv) {
    NexStore store; memset(&store, 0, sizeof store); store.count = count; store.at = at;
    NexIo io; memset(&io, 0, sizeof io);
    NexServer *s = nex_server_new(&io, store, 0);
    strcpy(arena.arena_id, "AB2CD"); arena.gid = 42;
    for (int a = 1; a < argc; a++) {   /* each argument a request body in hex: prints "<result> <gid>" */
        uint8_t body[256]; size_t n = 0;
        for (const char *h = argv[a]; h[0] && h[1] && n < sizeof body; h += 2) body[n++] = (uint8_t)(hex(h[0]) << 4 | hex(h[1]));
        NexConn c; memset(&c, 0, sizeof c);
        NexArenaImpl mem = {0};
        NexIn in = ni_make(body, n, (NexArena *)&mem);
        NexOut out = {0};
        uint32_t r = ar_56(s, &c, &in, &out);
        long gid = out.n >= 4 ? (long)((uint32_t)out.p[0] | out.p[1] << 8 | out.p[2] << 16 | (uint32_t)out.p[3] << 24) : -1;
        printf("%#x %ld\n", r, gid);
    }
    return 0;
}
'''


def string(text):
    data = text.encode() + b'\0'
    return len(data).to_bytes(2, 'little') + data


def structure(payload):
    return b'\0' + len(payload).to_bytes(4, 'little') + payload


@unittest.skipUnless(HAVE_CLANG, 'needs clang-cl (tools/cc.py)')
class ArenaIdSearch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        src = os.path.join(cls.dir, 'arena56.c')
        with open(src, 'w') as f:
            f.write(HARNESS)
        cls.exe = os.path.join(cls.dir, 'arena56.exe')
        runtime = str(ROOT / 'runtime')
        r = subprocess.run([cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, '/I', runtime, src,
                            os.path.join(runtime, 'net_nex_core.c'), os.path.join(runtime, 'net_nex_ranked.c'),
                            '/Fe' + cls.exe, '/Fo' + cls.dir + os.sep, '/link', *cc.LINK_OPTIONS],
                           capture_output=True, text=True, cwd=cls.dir)
        if r.returncode:
            raise AssertionError('the ar_56 harness did not build:\n' + r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def ask(self, *bodies, env=None):
        r = subprocess.run([self.exe, *(b.hex() for b in bodies)], capture_output=True, text=True, check=True,
                           env={**os.environ, **(env or {})})
        return [tuple(line.split()) for line in r.stdout.splitlines()]

    def test_the_games_request(self):
        game = structure(string('AB2CD') + bytes(8))   # (the bytes the game sent in #602)
        self.assertEqual(game.hex(), '00' '10000000' '0600' '414232434400' '0000000000000000')
        self.assertEqual(self.ask(game, structure(string(' ab2cd ') + bytes(8))), [('0', '42'), ('0', '42')])

    def test_a_bare_string_as_before(self):
        self.assertEqual(self.ask(string('AB2CD'), string('ab2cd')), [('0', '42'), ('0', '42')])

    def test_no_such_arena(self):
        self.assertEqual(self.ask(structure(string('ZZ9ZZ') + bytes(8)), string('ZZ9ZZ')),
                         [('0x8006000a', '-1'), ('0x8006000a', '-1')])

    def test_a_structure_not_the_whole_argument_is_not_read(self):
        bad = b'\0' + (32).to_bytes(4, 'little') + string('AB2CD') + bytes(8)
        self.assertEqual(self.ask(bad), [('0', '0')])

    def test_switch_off_is_the_old_parse(self):
        game = structure(string('AB2CD') + bytes(8))
        self.assertEqual(self.ask(game, string('AB2CD'), env={'SSBU_NEX_ARENA_STRUCT': '0'}), [('0', '0'), ('0', '42')])


if __name__ == '__main__':
    unittest.main()
