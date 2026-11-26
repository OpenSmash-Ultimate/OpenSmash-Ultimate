"""The keyboard, mouse and plural npad reads only Skyline plugins make (runtime/hle_hid.c, issue #618), with synthetic
pads; no game files or SDL required."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HidPluginTests(unittest.TestCase):
    def test_plugin_reads_answer_without_the_game_reads(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            self.skipTest('C compiler not installed')
        source = (ROOT / 'runtime/hle_hid.c').read_text()
        begin = source.index('static int hid_plugin_on(void)')
        harness = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef uint64_t u64; typedef uint32_t u32; typedef int32_t i32; typedef long LONG; typedef int64_t LONG64;
typedef struct Cpu { u64 x[31]; } Cpu;
typedef void GuestFn(Cpu *);
#define HLE_WRAP_EXPORT(sym) void hle_wrap_##sym(Cpu *c, GuestFn *orig)
#define P(T, a) ((T *)(uintptr_t)(a))
enum { STYLE_FULLKEY=1, STYLE_GC=32, ATTR_CONNECTED=1, ATTR_WIRED=2, PAD_SLOTS=8 };
typedef struct { u32 connected,gc; u64 buttons; i32 lx,ly,rx,ry,lt,rt; } PadState;
static PadState pad;
static u32 active_style=STYLE_FULLKEY;
static int menu=0, remote=0;
static volatile int input_gate;
static u32 pad_style(u32 id) { return id==0 ? active_style : 0; }
static int seq_player(u32 id) { (void)id; return 0; }
static int pad_get(u32 id,PadState*p) { (void)id; *p=pad; return 1; }
static int pad_background(void) { return 1; }
static int gfx_window_focused(void) { return 1; }
static int keyboard_slot(void) { return -1; }
static u64 keyboard_buttons(i32*x,i32*y,i32*z,i32*w) { (void)x;(void)y;(void)z;(void)w; return 0; }
static int menu_blocks_input(void) { return menu; }
static int bench_on(void) { return 0; }
static int bench_pads(void) { return 0; }
static int rb_slot_remote(u32 id) { (void)id; return remote; }
static u32 rb_slot_phys(u32 id) { return id; }
/* (no rb_pad, rb_pad_sampling, seq_player_poll, InterlockedIncrement64 of the game's sampling: the plugin's reads must
 * not reach them, and the harness does not compile if they do) */
static LONG InterlockedIncrement(volatile LONG *p) { return ++*p; }
static LONG64 InterlockedIncrement64(volatile LONG64 *p) { return ++*p; }
static u32 ld32(u64 p) { u32 v; memcpy(&v,(void*)(uintptr_t)p,4); return v; }
static void st64(u64 p,u64 v) { memcpy((void*)(uintptr_t)p,&v,8); }
static void st32(u64 p,u32 v) { memcpy((void*)(uintptr_t)p,&v,4); }
static int orig_calls;
static void orig(Cpu *c) { (void)c; orig_calls++; }
'''
        harness += source[begin:]
        harness += r'''
typedef struct { u64 sampling,buttons; i32 l[2],r[2]; u32 attributes,tl,tr,pad; } Gc;
int main(void) {
    Cpu c; u32 id=0; unsigned char out[0x80];
    /* the keyboard and the mouse: not connected, one state, nothing past it */
    memset(out,0xa5,sizeof out); memset(&c,0,sizeof c);
    c.x[0]=(u64)(uintptr_t)out; c.x[1]=4;
    hle_wrap__ZN2nn3hid17GetKeyboardStatesEPNS0_13KeyboardStateEi(&c,orig);
    assert(c.x[0]==1);
    for (int i=8;i<0x30;i++) assert(out[i]==0);
    for (int i=0x30;i<0x80;i++) assert(out[i]==0xa5);
    memset(out,0xa5,sizeof out); c.x[0]=(u64)(uintptr_t)out;
    hle_wrap__ZN2nn3hid13GetMouseStateEPNS0_10MouseStateE(&c,orig);
    for (int i=8;i<0x28;i++) assert(out[i]==0);
    for (int i=0x28;i<0x80;i++) assert(out[i]==0xa5);
    c.x[0]=0; hle_wrap__ZN2nn3hid18InitializeKeyboardEv(&c,orig); hle_wrap__ZN2nn3hid15InitializeMouseEv(&c,orig);
    /* a count of 0 writes nothing */
    memset(out,0xa5,sizeof out); c.x[0]=(u64)(uintptr_t)out; c.x[1]=0;
    hle_wrap__ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi(&c,orig);
    assert(c.x[0]==0 && out[0]==0xa5);
    /* the plural npad read: this PC's pad, connected in its own style, GameCube triggers at +36 / +40 */
    pad.connected=1; pad.buttons=5; pad.lx=100; pad.ly=-200; pad.rx=300; pad.ry=-400; pad.lt=1234; pad.rt=4321;
    active_style=STYLE_GC;
    memset(out,0xa5,sizeof out); c.x[0]=(u64)(uintptr_t)out; c.x[1]=16; c.x[2]=(u64)(uintptr_t)&id;
    hle_wrap__ZN2nn3hid13GetNpadStatesEPNS0_11NpadGcStateEiRKj(&c,orig);
    Gc g; memcpy(&g,out,sizeof g);
    assert(c.x[0]==1 && g.buttons==5 && g.l[0]==100 && g.l[1]==-200 && g.r[0]==300 && g.r[1]==-400);
    assert(g.attributes==(ATTR_CONNECTED|ATTR_WIRED) && g.tl==1234 && g.tr==4321);
    for (int i=0x30;i<0x80;i++) assert(out[i]==0xa5);
    /* asked as a Pro Controller while it is a GameCube one: not connected, zeros */
    memset(out,0xa5,sizeof out); c.x[0]=(u64)(uintptr_t)out; c.x[1]=1;
    hle_wrap__ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj(&c,orig);
    for (int i=8;i<0x28;i++) assert(out[i]==0);
    for (int i=0x28;i<0x80;i++) assert(out[i]==0xa5);
    /* the menu holds the input: connected, nothing pressed; the other PC's player: not this PC's pad */
    active_style=STYLE_FULLKEY; menu=1;
    c.x[0]=(u64)(uintptr_t)out; c.x[1]=1;
    hle_wrap__ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj(&c,orig);
    memcpy(&g,out,sizeof g); assert(g.buttons==0 && g.l[0]==0 && g.attributes==(ATTR_CONNECTED|ATTR_WIRED));
    menu=0; remote=1;
    c.x[0]=(u64)(uintptr_t)out; c.x[1]=1;
    hle_wrap__ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj(&c,orig);
    memcpy(&g,out,sizeof g); assert(g.buttons==0 && g.attributes==0);
    assert(orig_calls==0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'hid_plugin.c'
            exe = Path(tmp) / ('hid_plugin.exe' if os.name == 'nt' else 'hid_plugin')
            src.write_text(harness)
            build = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                                    str(src), '-o', str(exe)], capture_output=True, text=True)
            if build.returncode:
                self.fail(f"the harness did not compile with {compiler}:\n{build.stdout}{build.stderr}")
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            if run.returncode:
                self.fail(f"the harness failed ({run.returncode}):\n{run.stdout}{run.stderr}")
            self.assertIn('answered natively', run.stderr)

    def test_switch_off_runs_the_sdk_code(self):
        source = (ROOT / 'runtime/hle_hid.c').read_text()
        tail = source[source.index('static int hid_plugin_on(void)'):]
        self.assertIn('getenv("SSBU_HID_PLUGIN")', tail)
        wraps = [line for line in tail.splitlines() if line.startswith('HLE_WRAP_EXPORT(')]
        self.assertGreaterEqual(len(wraps), 10)
        for line in wraps:
            self.assertIn('if (!hid_plugin_on()) { orig(c); return; }', line)


if __name__ == '__main__':
    unittest.main()
