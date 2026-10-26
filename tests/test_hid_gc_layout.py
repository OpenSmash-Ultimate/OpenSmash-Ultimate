"""Exercise the runtime writer with synthetic pads; no game files or SDL required."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GcStateLayoutTests(unittest.TestCase):
    def test_runtime_writes_gc_layout_without_overrunning_it(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            self.skipTest('C compiler not installed')
        source = (ROOT / 'runtime/hle_hid.c').read_text()
        begin = source.index('static void npad_state(')
        end = source.index('\nHLE(', begin)
        harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
typedef uint64_t u64; typedef uint32_t u32; typedef int32_t i32;
enum { STYLE_FULLKEY=1, STYLE_GC=32, NPAD_NO1=0, ATTR_CONNECTED=1, ATTR_WIRED=2 };
enum { B_L=6, B_R=7, B_SL_LEFT=16, B_SL_UP, B_SL_RIGHT, B_SL_DOWN,
       B_SR_LEFT, B_SR_UP, B_SR_RIGHT, B_SR_DOWN };
typedef struct { u32 connected,gc; u64 buttons; i32 lx,ly,rx,ry,lt,rt; } PadState;
/* Public libnx HidNpadGcState layout: attributes directly precede the triggers.
 * https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/hid.h */
typedef struct { u64 sampling,buttons; i32 left[2],right[2];
                 u32 attributes,trigger_l,trigger_r; } GcState;
_Static_assert(offsetof(GcState,trigger_l)==36,"GC L offset");
_Static_assert(offsetof(GcState,trigger_r)==40,"GC R offset");
_Static_assert(sizeof(GcState)==48,"GC size");
static PadState pad;
static u32 active_style=STYLE_GC;
static int64_t sampling;
static u32 pad_style(u32 id) { (void)id; return active_style; }
static int seq_player(u32 id) { (void)id; return 0; }
static void seq_player_poll(u64*b,i32*x,i32*y) { (void)b;(void)x;(void)y; }
static int pad_get(u32 id,PadState*p) { (void)id; *p=pad;return 1; }
static int pad_background(void) { return 1; }
static int gfx_window_focused(void) { return 1; }
static u64 scripted_buttons(i32*x,i32*y) { (void)x;(void)y;return 0; }
static int keyboard_slot(void) { return -1; }
static u64 keyboard_buttons(i32*x,i32*y,i32*z,i32*w) { (void)x;(void)y;(void)z;(void)w;return 0; }
static int menu_blocks_input(void) { return 0; }
static int64_t InterlockedIncrement64(int64_t*p) { return ++*p; }
static void st64(u64 p,u64 v) { memcpy((void*)(uintptr_t)p,&v,8); }
static void st32(u64 p,u32 v) { memcpy((void*)(uintptr_t)p,&v,4); }
'''
        harness += source[begin:end]
        harness += r'''
int main(void) {
    struct { GcState state; unsigned char guard[16]; } out;
    const i32 pressure[][2]={{0,0},{12345,0},{0,23456},{12345,23456},{32767,32767}};
    pad.buttons=1;pad.lx=123;pad.ly=-456;pad.rx=789;pad.ry=-1011;
    for (unsigned i=0;i<sizeof pressure/sizeof *pressure;i++) {
        memset(&out,0xa5,sizeof out);
        pad.lt=pressure[i][0];pad.rt=pressure[i][1];
        npad_state((u64)(uintptr_t)&out.state,0,STYLE_GC);
        assert(out.state.trigger_l==(u32)pad.lt);
        assert(out.state.trigger_r==(u32)pad.rt);
        assert(out.state.buttons==1 && out.state.left[0]==123 && out.state.left[1]==-456);
        assert(out.state.right[0]==789 && out.state.right[1]==-1011);
        assert(out.state.attributes==(ATTR_CONNECTED|ATTR_WIRED));
        for (unsigned j=0;j<sizeof out.guard;j++) assert(out.guard[j]==0xa5);
    }
    /* FullKey keeps its reserved word and must not acquire GC trigger fields. */
    unsigned char fullkey[56]; memset(fullkey,0xa5,sizeof fullkey);
    active_style=STYLE_FULLKEY;
    npad_state((u64)(uintptr_t)fullkey,0,STYLE_FULLKEY);
    u32 reserved;memcpy(&reserved,fullkey+36,4);assert(reserved==0);
    for (unsigned i=40;i<sizeof fullkey;i++) assert(fullkey[i]==0xa5);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'gc_layout.c'
            exe = Path(tmp) / ('gc_layout.exe' if os.name == 'nt' else 'gc_layout')
            src.write_text(harness)
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, capture_output=True, text=True)


if __name__ == '__main__':
    unittest.main()
