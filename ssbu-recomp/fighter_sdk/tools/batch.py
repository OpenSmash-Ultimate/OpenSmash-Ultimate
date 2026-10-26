"""All script modules: decompile, build one DLL per fighter, offline synthetic differential test, table.

    python batch.py all [name ...]        everything below, repeated until no script fails (default: all 97 modules)
    python batch.py collect               how each script call (app::sv_animcmd::* ...) is used over all modules
                                          -> out/lua_calls_global.json (one SDK serves every module)
    python batch.py decompile [name ...]  l2c_decomp.py per module (parallel) + gen_sdk.py
    python batch.py build [name ...]      build_fighter.py per module; a script that does not compile is refused
    python batch.py test [name ...]       <exe> with SSBU_FIGHTER_OFFLINE: every source script against its lifted original,
                                          no game; a script that differs (or crashes the test) is refused
    python batch.py table                 out/batch_table.txt: per module scripts / readable / equal / refused by reason

Refusals found by the build or the test are kept in out/<name>_exclude.json ("script@offset": reason); delete the
file to try again after the decompiler has changed. Environment: S2_EXE (default ssbu_s2.exe), S2_JOBS (default 6);
the game's files are found as fighter_sdk/README.md says (RECOMP_BIN for main.elf).
A script counts as converted only when it is in the DLL and the test found it equal on all 3 passes.
"""
import collections
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sdk_paths  # noqa: E402

STUDY = sdk_paths.WORK
OUT = os.path.join(STUDY, 'fighters', 'tools_out')      # S3: everything derived from the game lives under fighters\ (not part of the published tree)
EXE = os.environ.get('S2_EXE', 'ssbu_s2.exe')
JOBS = int(os.environ.get('S2_JOBS', '6'))


def all_modules():
    import census
    return [e['name'][8:] for e in census.nro_map() if e['name'] != 'lua2cpp_common']


def _collect(name):
    import l2c_decomp
    return name, l2c_decomp.run_module(name, collect_only=True)


def _decompile(name):
    import l2c_decomp
    mod, res, reasons, out, syms, bytes_ok, bytes_all = l2c_decomp.run_module(name)
    l2c_decomp.write_module(mod.nro.short, mod, res, out, syms)
    return name, sum(1 for e in res if e['ok']), len(res)


def cmd_collect(mods):
    t0 = time.time()
    g = {}
    with concurrent.futures.ProcessPoolExecutor(JOBS) as pool:
        for name, stats in pool.map(_collect, mods):
            for s, st in stats.items():
                e = g.setdefault(s, {'protos': set(), 'dropped': 0, 'other': 0})
                e['protos'] |= set(tuple(p) for p in st['protos'])
                e['dropped'] += st['dropped']
                e['other'] += st['other']
    out = {s: {'protos': len(e['protos']), 'proto': list(sorted(e['protos'])[0]), 'autopop': bool(e['dropped'] and not e['other'])} for s, e in g.items()}
    with open(os.path.join(OUT, 'lua_calls_global.json'), 'w') as f:
        json.dump(out, f, indent=1, sort_keys=True)
    print('collect: %d script calls, %d used in more than one way, %d always dropped (%.0f s)' %
          (len(out), sum(1 for e in out.values() if e['protos'] != 1), sum(1 for e in out.values() if e['autopop']), time.time() - t0))


def cmd_decompile(mods, sdk=True):
    t0 = time.time()
    ok = tot = 0
    with concurrent.futures.ProcessPoolExecutor(JOBS) as pool:
        for name, a, b in pool.map(_decompile, mods):
            ok += a
            tot += b
    if sdk:
        r = subprocess.run([sys.executable, os.path.join(HERE, 'gen_sdk.py')], capture_output=True, text=True)
        print((r.stdout + r.stderr).strip()[-600:])
        if r.returncode:
            raise SystemExit('gen_sdk.py failed')
    print('decompile: %d modules, %d of %d registered scripts written (%.0f s)' % (len(mods), ok, tot, time.time() - t0))


def load(name):
    with open(os.path.join(OUT, name + '_acmd.json')) as f:
        return json.load(f)


def exclude(name, items):
    """items: ident -> reason"""
    p = os.path.join(OUT, name + '_exclude.json')
    cur = json.load(open(p)) if os.path.exists(p) else {}
    by_ident = {e.get('ident'): e for e in load(name)['scripts'] if e.get('ident')}
    n = 0
    for ident, why in items.items():
        e = by_ident.get(ident)
        if e:
            cur['%s@%x' % (e['name'], e['thunk'])] = why
            n += 1
    with open(p, 'w') as f:
        json.dump(cur, f, indent=1, sort_keys=True)
    return n


def _build(name):
    r = subprocess.run([sys.executable, os.path.join(HERE, 'build_fighter.py'), name], capture_output=True, text=True)
    return name, r.returncode, r.stdout + r.stderr


def script_at(path, line):
    last = None
    with open(path) as f:
        for k, text in enumerate(f, 1):
            if text.startswith('ACMD('):
                last = text[text.index(',') + 1:text.index(')')].strip()
            if k >= line:
                return last
    return last


def cmd_build(mods):
    """returns the modules in which scripts had to be refused (they need decompile + build again)"""
    t0 = time.time()
    again = []
    with concurrent.futures.ThreadPoolExecutor(max(2, JOBS // 2)) as pool:
        for name, code, text in pool.map(_build, mods):
            if not code:
                continue
            bad = {}
            for m in re.finditer(r'(fighters[\\/]%s[\\/]acmd[\\/]\w+\.cpp):(\d+):\d+: error: ([^\n]*)' % re.escape(name), text):
                ident = script_at(os.path.join(STUDY, m.group(1)), int(m.group(2)))
                if ident and ident not in bad:
                    bad[ident] = 'does not compile: ' + m.group(3)[:90]
            if not bad:
                print('build %s FAILED and no script could be blamed:\n%s' % (name, text[-1500:]))
                continue
            exclude(name, bad)
            again.append(name)
    print('build: %d modules, %d with scripts refused by the compiler (%.0f s)' % (len(mods), len(again), time.time() - t0))
    return again


def run_offline(mods, trace=False):
    env = dict(os.environ)
    env['SSBU_FIGHTER_OFFLINE'] = ','.join(mods)
    env['SSBU_USERDATA'] = os.path.join(STUDY, 'tests', 's2_userdata')
    env['SSBU_FIGHTER_REPORT'] = os.path.join(OUT, 'offline_report.txt')
    env['SSBU_PAIR_WARM'] = '0'
    env['SSBU_SHADER_CACHE'] = '0'                      # nothing is drawn: do not load the shader binaries
    if trace:
        env['SSBU_FIGHTER_OFFLINE_TRACE'] = '1'
    try:
        r = subprocess.run([sdk_paths.program(EXE, 'S2_EXE'), sdk_paths.main_elf(), '900'], cwd=STUDY, env=env,
                           capture_output=True, text=True, errors='replace', timeout=1500)
        return r.stdout + r.stderr
    except subprocess.TimeoutExpired as e:
        return (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')


def parse_offline(text):
    """-> {module: {'equal', 'differ', 'of', 'bad': {ident: why}, 'cut'}}, finished, last script traced"""
    res = {}
    bad = {}
    last = None
    for line in text.splitlines():
        m = re.match(r'fighter synthetic diff: (\S+) differs: (.*)', line)
        if m:
            bad[m.group(1)] = 'differential test: ' + m.group(2)[:110]
            continue
        m = re.match(r'fighter synthetic differential: lua2cpp_(\w+): (\d+) scripts equal on all 3 passes, (\d+) differing, of (\d+)', line)
        if m:
            res[m.group(1)] = {'equal': int(m.group(2)), 'differ': int(m.group(3)), 'of': int(m.group(4)), 'bad': bad, 'cut': 0}
            bad = {}
            continue
        m = re.match(r'fighter offline: lua2cpp_(\w+) done \((\d+) scripts were cut', line)
        if m and m.group(1) in res:
            res[m.group(1)]['cut'] = int(m.group(2))
        m = re.match(r'synth (\S+)', line)
        if m:
            last = m.group(1)
    return res, 'fighter offline: finished' in text, last


def cmd_test(mods):
    """returns (results per module, modules with new refusals)"""
    t0 = time.time()
    results = {}
    again = set()
    todo = list(mods)
    log = open(os.path.join(OUT, 'offline_last.log'), 'w', errors='replace')
    while todo:
        text = run_offline(todo)
        log.write(text)
        res, finished, _ = parse_offline(text)
        results.update(res)
        for name, r in res.items():
            if r['bad']:
                exclude(name, r['bad'])
                again.add(name)
        rest = [m for m in todo if m not in res]
        if finished or not rest:
            for m in rest:
                print('test: %s: no result (no DLL?)' % m)
            break
        culprit = rest[0]                                # the run died in this module: find the script
        text = run_offline([culprit], trace=True)
        log.write(text)
        res, finished, last = parse_offline(text)
        if finished or not last:
            print('test: %s stopped the run; alone it %s:\n%s' % (culprit, 'passes' if finished else 'gives no script', text[-800:]))
            results.update(res)
            todo = rest[1:]
            continue
        exclude(culprit, {last: 'the offline test stops in it (crash or endless run)'})
        print('test: %s: %s stops the test: refused' % (culprit, last))
        again.add(culprit)
        results[culprit] = {'equal': 0, 'differ': 0, 'of': 0, 'bad': {}, 'cut': 0, 'retest': True}
        todo = rest[1:]
    log.close()
    with open(os.path.join(OUT, 'offline_results.json'), 'w') as f:
        prev = {}
        json.dump(results, f, indent=1)
    print('test: %d modules, %d scripts equal, %d differing, %d modules with new refusals (%.0f s)' %
          (len(results), sum(r['equal'] for r in results.values()), sum(r['differ'] for r in results.values()), len(again), time.time() - t0))
    return results, sorted(again)


def reason_class(why):
    why = why.split('+0x')[0]
    for k in ('differential test', 'does not compile', 'the offline test stops', 'no body of its own', 'call to local function', 'load through',
              'Lua stack of another agent', 'is called in more than one way'):
        if why.startswith(k) or k in why:
            return k
    return why[:48]


def cmd_table():
    p = os.path.join(OUT, 'offline_all.json')
    tested = json.load(open(p)) if os.path.exists(p) else {}
    rows = []
    tot = collections.Counter()
    reasons = collections.Counter()
    rbytes = collections.Counter()
    for name in all_modules():
        if not os.path.exists(os.path.join(OUT, name + '_acmd.json')):
            continue
        j = load(name)
        seen = set()
        c = collections.Counter()
        t = tested.get(name, {})
        for e in j['scripts']:
            c['scripts'] += 1
            own = bool(e['body'])
            first = own and e['body'] not in seen
            if own:
                seen.add(e['body'])
                c['own'] += 1
            size = e['size'] if first else 0
            c['bytes'] += size
            if e['ok']:
                c['readable'] += 1
                c['rbytes'] += size
                c['goto'] += 1 if e.get('gotos') else 0
                c['hashname'] += 1 if e.get('unnamed') else 0
            else:
                k = reason_class(e['why'])
                reasons[k] += 1
                rbytes[k] += size
        equal = t.get('equal', 0) if t.get('of') == c['readable'] and not t.get('differ') else 0
        c['equal'] = equal
        c['ebytes'] = c['rbytes'] if equal == c['readable'] else 0
        rows.append((name, c))
        tot.update(c)
    lines = ['%-14s %7s %7s %8s %7s %9s %9s %6s' % ('module', 'scripts', 'own', 'readable', 'equal', 'bytes', 'readable', 'goto')]
    for name, c in rows + [('TOTAL', tot)]:
        lines.append('%-14s %7d %7d %8d %7d %9d %9d %6d' % (name, c['scripts'], c['own'], c['readable'], c['equal'], c['bytes'], c['rbytes'], c['goto']))
    lines.append('')
    lines.append('scripts with a body of their own: %d; readable C++ in a DLL: %d (%.1f%%); equal in the offline differential: %d (%.1f%%)' %
                 (tot['own'], tot['readable'], 100.0 * tot['readable'] / max(tot['own'], 1), tot['equal'], 100.0 * tot['equal'] / max(tot['own'], 1)))
    lines.append('body bytes: %d; readable: %d (%.1f%%); equal: %d (%.1f%%)' %
                 (tot['bytes'], tot['rbytes'], 100.0 * tot['rbytes'] / max(tot['bytes'], 1), tot['ebytes'], 100.0 * tot['ebytes'] / max(tot['bytes'], 1)))
    lines.append('readable scripts that contain a goto: %d; named only by their hash: %d' % (tot['goto'], tot['hashname']))
    lines.append('refused, by reason (scripts, body bytes):')
    for k, n in reasons.most_common():
        lines.append('  %6d %9d  %s' % (n, rbytes[k], k))
    text = '\n'.join(lines)
    with open(os.path.join(OUT, 'batch_table.txt'), 'w') as f:
        f.write(text + '\n')
    print('\n'.join(lines[-(len(reasons) + 6):]))


def cmd_all(mods):
    if not os.path.exists(os.path.join(OUT, 'lua_calls_global.json')):
        cmd_collect(all_modules())
    cmd_decompile(mods)
    p = os.path.join(OUT, 'offline_all.json')
    tested = json.load(open(p)) if os.path.exists(p) else {}
    todo = list(mods)
    for rounds in range(8):
        for k in range(6):
            again = cmd_build(todo if k == 0 else again)
            if not again:
                break
            cmd_decompile(again, sdk=False)
        results, again = cmd_test(todo)
        tested.update(results)
        with open(p, 'w') as f:
            json.dump(tested, f, indent=1)
        if not again:
            break
        cmd_decompile(again, sdk=False)
        todo = again
    cmd_table()


def cmd_status(name):
    """python batch.py status common: the state-machine functions (status_decomp.py): decompile, build, offline test,
    refuse what differs or does not compile (out/<name>_status_exclude.json), until stable"""
    xp = os.path.join(OUT, name + '_status_exclude.json')
    pattern = sys.argv[3:4]                              # S3: optional regex of function names (default: status_decomp's slice)
    for rounds in range(60):
        r = subprocess.run([sys.executable, os.path.join(HERE, 'status_decomp.py'), name] + pattern, capture_output=True, text=True)
        print(r.stdout.strip().splitlines()[0] if r.stdout.strip() else r.stderr[-500:], flush=True)
        cur = json.load(open(xp)) if os.path.exists(xp) else {}
        g = subprocess.run([sys.executable, os.path.join(HERE, 'gen_sdk.py')], capture_output=True, text=True)      # S3: the state machine calls game functions no ACMD script calls
        if g.returncode:
            print(g.stderr[-800:])
            return
        n, code, text = _build(name)
        if code:
            bad = {}
            for m in re.finditer(r'status[\\/](\w+)\.cpp:(\d+):\d+: error: ([^\n]*)', text):
                src = os.path.join(STUDY, 'fighters', name, 'status', m.group(1) + '.cpp')
                m = re.match(r'()(\d+)\x00(.*)', '%s\x00%s' % (m.group(2), m.group(3)))
                ident = None
                with open(src) as f:
                    for k, line in enumerate(f, 1):
                        if line.startswith('L2C_'):
                            ident = line[line.index(',') + 1:].split(',')[0].split(')')[0].strip()
                        if k >= int(m.group(2)):
                            break
                if ident and ident not in bad:
                    bad[ident] = 'does not compile: ' + m.group(3)[:90]
            if not bad:
                print(text[-1500:])
                return
            cur.update(bad)
            json.dump(cur, open(xp, 'w'), indent=1, sort_keys=True)
            continue
        # S3: one traced run per function that stops the test; the function is left out of the next run through
        # SSBU_FIGHTER_OFFLINE_SKIP (no new decompile + build per crash)
        skip = []
        skipfile = os.path.join(OUT, 'offline_skip_%s.txt' % name)
        while True:
            with open(skipfile, 'w', newline='\n') as f:
                f.write('\n'.join(skip) + '\n')
            os.environ['SSBU_FIGHTER_OFFLINE_SKIP'] = skipfile
            text = run_offline([name], trace=True)
            res, finished, last = parse_offline(text)
            if name in res:
                break
            if not last or last in skip or len(skip) > 600:
                print('status: the offline test of %s gives no result:\n%s' % (name, text[-600:]))
                return
            skip.append(last)
            cur[last] = 'the offline test stops in it (crash or endless run)'
        os.environ.pop('SSBU_FIGHTER_OFFLINE_SKIP', None)
        with open(os.path.join(OUT, 'offline_%s.log' % name), 'w', errors='replace') as f:
            f.write(text)
        r = res[name]
        print('status: %s: %d functions equal on all 3 passes, %d differing, of %d; %d stop the test' % (name, r['equal'], r['differ'], r['of'], len(skip)), flush=True)
        if skip:
            r['bad'] = dict(r['bad'])
            json.dump(cur, open(xp, 'w'), indent=1, sort_keys=True)
            if not r['bad']:
                continue
        if not r['bad']:
            with open(os.path.join(OUT, name + '_status_result.json'), 'w') as f:
                json.dump(r, f)
            return
        cur.update(r['bad'])
        json.dump(cur, open(xp, 'w'), indent=1, sort_keys=True)


def main():
    cmd = sys.argv[1]
    if cmd == 'status':
        cmd_status(sys.argv[2])
        return
    mods = sys.argv[2:] or all_modules()
    if cmd == 'collect':
        cmd_collect(all_modules())
    elif cmd == 'decompile':
        cmd_decompile(mods)
    elif cmd == 'build':
        cmd_build(mods)
    elif cmd == 'test':
        results, again = cmd_test(mods)
        p = os.path.join(OUT, 'offline_all.json')
        tested = json.load(open(p)) if os.path.exists(p) else {}
        tested.update(results)
        with open(p, 'w') as f:
            json.dump(tested, f, indent=1)
    elif cmd == 'table':
        cmd_table()
    elif cmd == 'all':
        cmd_all(mods)


if __name__ == '__main__':
    main()
