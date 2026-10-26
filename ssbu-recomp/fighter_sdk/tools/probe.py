"""One short scripted match as a yes/no probe, and a bisection over function names built on it.

    python probe.py run   <tag> [KEY=VALUE ...]            one probe; prints one verdict line
    python probe.py bisect <tag> <names file> [KEY=VALUE ...]
                                                           smallest set of names that must run LIFTED for the probe to
                                                           pass (SSBU_FIGHTER_LIFTED=<names>), by halving
    python probe.py match <tag> [KEY=VALUE ...]            a whole match: ends when the fighter modules are unloaded
                                                           (results screen) or at the time limit
    python probe.py health <tag> [KEY=VALUE ...]           stops PROBE_FIGHT (default 40) seconds into the fight and
                                                           prints the fight health numbers (KEY=(unset) removes KEY)

Every mode runs with SSBU_FIGHT_HEALTH=5 (hook_host.c): per fighter, status changes, motion changes, script calls and
damage from the game's own getters; the verdict line says HEALTHY or FROZEN, and one line per fighter follows. A match
that reaches the results with a FROZEN fighter is not a pass.

A probe = exe (PROBE_EXE, default ssbu_s4.exe) + tests/seq_battle_play.txt on a FRESH copy of your save folder
(SSBU_FIGHTER_USERDATA, else ..\\userdata; into tests/s4_userdata, removed and copied again every run), SSBU_FIXED_SEED=1 SSBU_PAIR_WARM=0, log tests/logs/s4_<tag>.log,
report tests/logs/s4_<tag>.report.txt. It is stopped (own process only) as soon as the verdict is known:
    TRAP      the log shows "GUEST TRAP" / "HOST EXCEPTION", or the exe ended by itself with a non-zero code
    RUNNING   the fighter report counts PROBE_CALLS (default 30000) calls of lua2cpp_common functions, no trap
    TIMEOUT   neither within PROBE_SECS (default 240)
No game content in this file.
"""
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sdk_paths  # noqa: E402

STUDY = sdk_paths.WORK
LOGS = os.path.join(STUDY, 'tests', 'logs')


def read(path):
    try:
        with open(path, 'r', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def common_calls(report):
    m = re.search(r'module lua2cpp_common: \d+ scripts as source; called in this run: (\d+) scripts, (\d+) calls', report)
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


HEALTH = re.compile(r'\[health\] t=([\d.]+)s fighter (\d+) kind (-?\d+): script calls (\d+) \(\+(\d+)\), status changes (\d+) \(([\d.]+)/s, \+(\d+)\), '
                    r'motion changes (\d+), change_motion calls (\d+), status requests (\d+), status (\w+), motion (\w+), damage ([\d.]+)%')


def health(text, show=True):
    """last [health] line of every fighter of the LAST fight in the log -> verdict HEALTHY / FROZEN / '' (no lines).
    FROZEN = a fighter with fewer than 0.3 status changes per second over the fight, or none at all in the last
    interval together with no motion change."""
    at = text.rfind('[health] new set of fighters')
    last = {}
    for m in HEALTH.finditer(text, max(at, 0)):
        last[int(m.group(2))] = m
    if not last:
        return ''
    verdict = 'HEALTHY'
    for k in sorted(last):
        m = last[k]
        t, rate, new_changes = float(m.group(1)), float(m.group(7)), int(m.group(8))
        # measured 2026-10-12, level-3 CPU, 40 s: healthy 2.2-2.5 changes/s; the "T-pose" regression 0.40/s and +0 at the end
        bad = t >= 10 and (rate < float(os.environ.get('PROBE_MIN_RATE', '1.0')) or new_changes == 0)
        if bad:
            verdict = 'FROZEN'
        if show:
            print('   fighter %d (kind %s) after %5.1fs: status changes %s (%s/s, last interval +%s), motion changes %s, change_motion %s, '
                  'script calls %s (+%s), damage %s%%%s' % (k, m.group(3), t, m.group(6), m.group(7), m.group(8), m.group(9), m.group(10),
                                                         m.group(4), m.group(5), m.group(14), '   <-- FROZEN' if bad else ''))
    return verdict


def run(tag, env_extra, mode='run'):
    exe = sdk_paths.program(os.environ.get('PROBE_EXE', 'ssbu_s4.exe'), 'PROBE_EXE')
    main_elf = sdk_paths.main_elf()
    saves = sdk_paths.userdata()
    secs = int(os.environ.get('PROBE_SECS', '240' if mode == 'run' else '600'))
    need = int(os.environ.get('PROBE_CALLS', '30000'))
    ud = os.path.join(STUDY, 'tests', 's4_userdata')
    shutil.rmtree(ud, ignore_errors=True)
    shutil.copytree(saves, ud)
    os.makedirs(LOGS, exist_ok=True)
    log = os.path.join(LOGS, 's4_%s.log' % tag)
    rep = os.path.join(LOGS, 's4_%s.report.txt' % tag)
    for p in (log, rep):
        if os.path.exists(p):
            os.remove(p)
    given = lambda k: k in os.environ or any(a.startswith(k + '=') for a in env_extra)     # tests\DEBUG_MODE.md: the debug variables switch the fast mode on (uncapped unless PROBE_FAST says otherwise)
    fast = os.environ.get('PROBE_FAST') or ('0' if any(given(k) for k in ('SSBU_DEBUG_MATCH', 'SSBU_DEBUG_MATCH_LIST', 'SSBU_DEBUG_END')) else '')
    with open(os.path.join(sdk_paths.TREE, 'tests', os.environ.get('PROBE_SEQ', 'seq_debug_list.txt' if given('SSBU_DEBUG_MATCH_LIST') else 'seq_battle_play_fast.txt' if fast else 'seq_battle_play.txt'))) as f:
        seq = ' '.join(l.strip() for l in f if not l.startswith('#'))
    env = dict(os.environ)
    env.update(SSBU_SEQ=seq, SSBU_USERDATA=ud, SSBU_FIXED_SEED='1', SSBU_PAIR_WARM='0', SSBU_FIGHTER_REPORT=rep,
               SSBU_FIGHTER_REPORT_EVERY='500', SSBU_REC=os.path.join(LOGS, 's4_input.rec'),
               SSBU_TRACE=os.path.join(LOGS, 's4_%s.trace.txt' % tag))
    env['SSBU_FIGHT_HEALTH'] = '5'
    if fast:                                   # tests\DEBUG_MODE.md: PROBE_FAST=<speed> (0 = uncapped) also skips the movies; needs an exe linked with debug_mode.c
        env.setdefault('SSBU_SPEED', fast)
        env.setdefault('SSBU_SKIP_MOVIES', '1')
    for kv in env_extra:
        k, v = kv.split('=', 1)
        if v == '(unset)':
            env.pop(k, None)
        else:
            env[k] = v
    fight = float(os.environ.get('PROBE_FIGHT', '40'))
    t0 = time.time()
    verdict, loaded_seen, unloaded = 'TIMEOUT', False, False
    with open(log, 'w') as out:
        p = subprocess.Popen([exe, main_elf, str(secs + 60)],
                             cwd=STUDY, env=env, stdout=out, stderr=subprocess.STDOUT)
        while time.time() - t0 < secs:
            time.sleep(2)
            text = read(log)
            if 'GUEST TRAP' in text or 'HOST EXCEPTION' in text:
                verdict = 'TRAP'
                break
            rc = p.poll()
            if rc is not None:
                verdict = 'TRAP' if rc else 'ENDED'
                break
            if mode == 'run' and common_calls(read(rep))[1] >= need:
                verdict = 'RUNNING'
                break
            if mode == 'health':
                ts = [float(m.group(1)) for m in HEALTH.finditer(text)]
                if ts and max(ts) >= fight:
                    verdict = 'FOUGHT'
                    break
            if mode == 'match':
                # the fight is over when the fighter modules loaded for it are unloaded again (results screen)
                at = text.rfind('fighter source: mario')
                if at >= 0 and 'nn::ro::UnloadModule: lua2cpp_mario' in text[at:]:
                    verdict, unloaded = 'RESULTS', True
                    time.sleep(8)
                    break
        if p.poll() is None:
            if verdict == 'TRAP':
                try:
                    p.wait(20)
                except subprocess.TimeoutExpired:
                    pass
            if p.poll() is None:
                p.kill()
                p.wait(30)
    text = read(log)
    if 'GUEST TRAP' in text or 'HOST EXCEPTION' in text:
        verdict = 'TRAP'
    why = ''
    m = re.search(r'(GUEST TRAP[^\n]*|HOST EXCEPTION[^\n]*)', text)
    if m:
        why = m.group(1)
        st = re.search(r'guest call stack[^\n]*\n((?:  f_\w+\n){1,4})', text)
        if st:
            why += ' | stack ' + ' '.join(st.group(1).split())
    scripts, calls = common_calls(read(rep))
    hv = health(text, show=False)
    print('%s %s %s  %.0fs  common: %d functions called, %d calls  %s' % (tag, verdict, hv, time.time() - t0, scripts, calls, why))
    health(text)
    sys.stdout.flush()
    return (verdict + ' ' + hv).strip()


def bisect(tag, names_file, env_extra):
    with open(names_file) as f:
        names = [l.strip() for l in f if l.strip()]
    n = [0]

    def passes(lifted):
        n[0] += 1
        mode = os.environ.get('PROBE_MODE', 'run')         # PROBE_MODE=health: pass = the fight was reached and is HEALTHY
        v = run('%s_%02d' % (tag, n[0]), env_extra + ['SSBU_FIGHTER_LIFTED=' + (','.join(lifted) if lifted else 'none')], mode)
        if mode == 'health':
            return v == 'FOUGHT HEALTHY'
        return v.split()[0] in ('RUNNING', 'RESULTS', 'ENDED')

    culprits = []
    cand = names
    while True:
        # invariant: lifting culprits + cand passes; find one more name in cand that must be lifted
        if passes(culprits):
            break
        lo = cand
        while len(lo) > 1:
            half = lo[:len(lo) // 2]
            rest = [x for x in cand if x not in half]      # everything of cand except this half stays lifted
            if passes(culprits + rest):
                lo = lo[len(lo) // 2:]                     # the half runs as source and the probe passes: culprit is in the other half
                # (the other half was lifted in that probe; narrow to it)
            else:
                lo = half
            cand_note = len(lo)
            print('  narrowed to %d' % cand_note)
        culprits.append(lo[0])
        print('culprit %d: %s' % (len(culprits), lo[0]))
        sys.stdout.flush()
        cand = [x for x in cand if x != lo[0]]
        if len(culprits) >= int(os.environ.get('PROBE_MAX_CULPRITS', '6')):
            break
    print('must run lifted: %s  (%d probes)' % (','.join(culprits) or '(nothing)', n[0]))


def main():
    mode, tag = sys.argv[1], sys.argv[2]
    if mode == 'bisect':
        bisect(tag, sys.argv[3], sys.argv[4:])
    else:
        run(tag, sys.argv[3:], mode)


main()
