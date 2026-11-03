"""Test of tools/lifter/build_opt.py's Linux / macOS compile scheduling, with a fake compiler (no clang, no game files):

    python tests/build_opt_sched_test.py [linux | macos | windows | linux-inline | macos-inline]
                                                             (default: all five)

Checks: the compilers' reservations never exceed the shared RAM (MEM_BUDGET) and are all given back; on Linux a file over
its reservation is compiled again with twice as much (up to the budget); on Linux / macOS a ladder retry that would leave
the same functions unoptimised as the attempt that just failed is skipped; on Windows (no RECOMP_TARGET) nothing of that
happens: no reservations, every ladder step as before.

`linux-inline` / `macos-inline` run the same files through compile_file_inline (RECOMP_INLINE=1, the game's default on
those targets): the inlined compile reserves twice the estimate, and for a file as large as c_00a3.c (215,493 statements)
that is more than the whole budget - which used to wait in mem_acquire for room no compile could free and stopped the
step with no compiler running (2026-10-22, issue 75). The reservations must stay within the budget there too, and a
watchdog fails the run instead of hanging if a wait ever stops the test.
"""
import concurrent.futures
import os
import subprocess
import sys
import tempfile
import threading
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
LIFTER = os.path.join(HERE, '..', 'tools', 'lifter')
GB = 1 << 30
WATCHDOG = 20                                    # seconds without a compile starting or finishing before the test gives up
# file -> (statements of each function, GB it really needs, seconds at -O1; None: always over the time limit unless at least
# `ok_from` functions are unoptimised)
SPEC = dict({'small%02d' % i: ([500, 300], 0.4, 0.02, 0) for i in range(24)},
            huge=([215493, 2846, 198], 12.0, None, 99),     # like c_00a3.c: over every limit
            same=([77061, 1186], 3.0, None, 99),            # like c_03d9.c: steps 1 and 2 = step 0
            mid=([30000, 6000], 2.5, 0.02, 0),              # needs more than its first reservation
            lad=([16581, 2101, 1717], 1.0, None, 2))        # like c_03fc.c: fits once two functions are unoptimised


def child(target):
    inline = target.endswith('-inline')                  # the inlined compile (RECOMP_INLINE): build_opt reads it at import
    if inline:
        target = target[:-len('-inline')]
        os.environ['RECOMP_INLINE'] = '1'
    if target != 'windows':
        os.environ['RECOMP_TARGET'] = target
    sys.modules['build'] = types.ModuleType('build')         # build.py / build_nro.py are not needed by compile_file
    sys.modules['build_nro'] = types.ModuleType('build_nro')
    sys.path.insert(0, LIFTER)
    import build_opt as B
    if target != 'windows':
        B.ram_bytes = lambda: 14 * GB                        # a 16 GB machine as Linux reports it (a Steam Deck)
    B.limit_memory(8)
    lock = threading.Lock()
    calls, peak = [], [0]

    def fake_run_limited(cmd, limit, mem=0):
        name = os.path.basename(cmd[-3])[:-2]
        funcs, need, secs, ok_from = SPEC[name]
        hdr = cmd[cmd.index('-include') + 1] if '-include' in cmd else None
        n_off = sum(1 for l in open(hdr) if l.startswith('void ')) if hdr else 0
        with lock:
            calls.append((name, n_off, mem))
            peak[0] = max(peak[0], B._mem_used)
            assert B._mem_used <= max(B.MEM_BUDGET, 0), 'reservations over the budget'
        if B.MEM_LIMIT and need * GB > (mem or B.MEM_LIMIT):
            return None, B.OOM, 0.005
        if secs is None and n_off < ok_from:
            return None, '', 0.005
        time.sleep(secs or 0.005)
        open(cmd[-1], 'w').close()
        return 0, '', secs or 0.005

    class Done:
        returncode, stdout, stderr = 0, '', ''

    def fake_run(cmd, **kw):
        with lock:
            calls.append((os.path.basename(cmd[-3])[:-2], 'O0', None))
        return Done()

    B.run_limited = fake_run_limited
    B.subprocess = types.SimpleNamespace(run=fake_run)
    d = tempfile.mkdtemp()
    for n, (funcs, _, _, _) in SPEC.items():
        with open(os.path.join(d, n + '.c'), 'w') as f:
            for i, k in enumerate(funcs):
                f.write('void f%d(Ctx *c) {\n' % i + 'x;\n' * k + '}\n')
    order = sorted(SPEC, key=lambda n: -os.path.getsize(os.path.join(d, n + '.c')))
    progress = [time.time()]

    def watchdog():                      # a wait that never ends would hang CI: say which wait and fail
        while True:
            time.sleep(1)
            if time.time() - progress[0] > WATCHDOG:
                print('%-13s FAILED: nothing started or finished for %d s (a reservation mem_acquire can never grant?)'
                      % (target + ('-inline' if inline else ''), WATCHDOG), flush=True)
                os._exit(1)

    if target != 'windows':
        threading.Thread(target=watchdog, daemon=True).start()
    compile_one = B.compile_file_inline if inline else B.compile_file

    def one(n):
        progress[0] = time.time()                # the watchdog times starts as well as ends: a compile may be long
        r = compile_one(os.path.join(d, n + '.c'), os.path.join(d, n + '.obj'), [])
        progress[0] = time.time()
        return n, r

    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        res = dict(pool.map(one, order))
    import shutil
    shutil.rmtree(d, ignore_errors=True)
    steps = {n: [c[1] for c in calls if c[0] == n] for n in ('huge', 'same', 'mid', 'lad')}
    mems = {n: [c[2] for c in calls if c[0] == n] for n in ('huge', 'same', 'mid')}
    bad = []
    if any(r[0] for r in res.values()):
        bad.append('a compile failed: %r' % {n: r for n, r in res.items() if r[0]})
    if B._mem_used:
        bad.append('reservations not given back: %d' % B._mem_used)
    if inline:
        # the inlined compile reserves twice the estimate; over the budget it is cut to it (the file runs alone), which is
        # what keeps the step moving. c_00a3.c-like: 8.2 GB of estimate -> 16.4 GB asked of a 10 GB budget
        budget = 10 * GB
        if not B.INLINE:
            bad.append('RECOMP_INLINE is off: the inlined path was not tested')
        if B.MEM_BUDGET != budget:
            bad.append('budget %r, expected 10 GB' % B.MEM_BUDGET)
        over = [c for c in calls if c[2] and c[2] > budget]
        if over:
            bad.append('a reservation over the budget: %r' % over[:3])
        if max([c[2] or 0 for c in calls if c[0] == 'huge'] or [0]) != budget:
            bad.append('huge did not reserve the whole budget: %r' % mems['huge'])
        if peak[0] > budget:
            bad.append('peak %d over the budget' % peak[0])
    elif target == 'windows':
        if B.MEM_BUDGET or any(c[2] for c in calls):
            bad.append('reservations on Windows')
        if steps['same'] != [1, 1, 1, 'O0']:
            bad.append('Windows ladder changed: %r' % steps['same'])
    else:
        budget = 10 * GB
        if B.MEM_BUDGET != budget:
            bad.append('budget %r, expected 10 GB' % B.MEM_BUDGET)
        if target == 'linux':
            # same: 2.9 GB reservation over its 3 GB need -> 5.9 GB, then the time limit; the identical steps 1 and 2 skipped
            if steps['same'] != [1, 1, 'O0'] or mems['same'][1] <= mems['same'][0]:
                bad.append('same: %r %r' % (steps['same'], mems['same']))
            if mems['mid'] != [1.5 * GB, 3 * GB]:
                bad.append('mid reservations %r' % mems['mid'])
            if B.MEM_FLOOR[0] != (1536 << 20) * 5 // 4:            # mid ran out at the floor: raised by a quarter, once
                bad.append('floor %r' % B.MEM_FLOOR[0])
            if mems['huge'][-2] != budget:
                bad.append('huge did not reach the budget: %r' % mems['huge'])
        else:
            if steps['same'] != [1, 'O0']:
                bad.append('same: %r' % steps['same'])
        if steps['lad'] != [0, 1, 3]:
            bad.append('lad: %r' % steps['lad'])
        if peak[0] > budget:
            bad.append('peak %d over the budget' % peak[0])
    label = target + ('-inline' if inline else '')
    print('%-13s %s   ladder %s   peak reserved %.1f GB' % (label, 'ok' if not bad else 'FAILED: ' + '; '.join(bad), steps, peak[0] / GB))
    return 1 if bad else 0


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == '--child':
        sys.exit(child(sys.argv[2]))
    targets = sys.argv[1:] or ['linux', 'macos', 'windows', 'linux-inline', 'macos-inline']
    rc = 0
    for t in targets:                       # one process each: build_opt reads RECOMP_TARGET / RECOMP_INLINE at import
        rc |= subprocess.call([sys.executable, os.path.abspath(__file__), '--child', t])
    sys.exit(rc)
