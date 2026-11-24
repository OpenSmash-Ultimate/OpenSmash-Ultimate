"""The delay from a press to the first picture that shows it, end to end in the runtime (SSBU_LAT_TRACE).

    python tools/latency_trace.py <trace> [--game G] [--json]
    python tools/latency_trace.py --calibrate <trace> <pictures> <reference trace> <reference pictures>

<trace>: SSBU_LAT_TRACE of a run (runtime/ipc_vi.c). Every line carries the game's own logic frame (its frame counter,
read on the thread that reads the pads or submits the work), never a count of pictures shown, so two runs, two back
ends or two display modes are compared frame for frame whatever their pacing:
  R <ms> <ms of the refresh>                 a refresh signalled to the game (low_latency holds it after the refresh)
  I <ms> <logic frame>                       player 1's first pad read of a logic frame
  P <ms> <shown> <fight frame> <buttons> <logic frame>   player 1's buttons changed (as the game reads them)
  Q <ms> <logic frame> <fight on>            a frame queued
  S <ms before present> <ms after> <shown> <fight frame> <logic frame now> <picture's logic frame> <known>
  C <ms> <logic frame> <status> <motion>     the first fighter's status or motion changed (with SSBU_FIGHT_HASH)
A picture's logic frame is the frame the game was on when it submitted the last draw into the target shown
(runtime/gpu.c), noted when the back end composes it: how old what is on screen is, measured, not inferred.

What it gives (fight frames only):
- picture age: present time minus the pad read of the picture's logic frame, in ms and in frames of 1/60 s, and how
  many logic frames behind the game the picture is. Anything the runtime adds or removes between the read and the
  present (frame index, queue depth, pacing, waits) moves this number; the game's own part does not.
- press -> picture: for each press (player 1's buttons from none to some, read at logic frame a), the first change of
  the first fighter's status or motion after it (C, at logic frame s: the game's part, s - a, which online delay and
  run-ahead move), then the first picture presented after it whose logic frame is s + RENDER_FRAMES or later (the frame
  whose draws show that state: found once with --calibrate), and the time from the read to that present. Frames run again
  (run-ahead, rollback) keep the count of the real-time frame they stand in for, so a press that run-ahead shows a
  frame early is found in the picture of an earlier frame. Without C lines (no SSBU_FIGHT_HASH): a + --game G.
  A picture is on the monitor at its next refresh after the present: up to one refresh more, which a hidden window
  cannot see.

--calibrate: two runs of the same deterministic fight (SSBU_DETERMINISTIC=1 SSBU_FIXED_SEED=1, Direct3D 11 with
SSBU_PRESENT_HASH), one with presses and one without. Pictures are matched by their logic frame counted from the
fight's start (not by when they were shown), and for the FIRST press only (the runs differ for good after it) the
first picture differing from the reference's beyond their own noise gives G."""
import bisect
import json
import statistics
import sys

FRAME_MS = 1000.0 / 60
GAME_FRAMES = 2     # (no C lines: the game's part, Mario's jab from standing)
RENDER_FRAMES = 0   # (the picture drawn in the frame whose logic changed the status: the first that can show it. Measured
                    # 2026-11-24 with --calibrate, Mario's jab from standing: the status changes 1 frame after the read and
                    # the 16 x 9 brightness grid first sees the jab 2 frames later - its first poses, not the pipeline)


def read_trace(path):
    t = {'R': [], 'T': [], 'Q': [], 'I': {}, 'P': [], 'S': [], 'C': []}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.split()
            try:
                k = p[0]
                if k == 'R':
                    t['R'].append(float(p[1]))
                    if len(p) >= 3 and float(p[2]) > 0:
                        t['T'].append(float(p[2]))
                elif k == 'Q' and len(p) >= 4:
                    t['Q'].append((float(p[1]), int(p[2]), int(p[3])))
                elif k == 'I' and len(p) >= 3:
                    t['I'].setdefault(int(p[2]), float(p[1]))
                elif k == 'P' and len(p) >= 6:
                    t['P'].append((float(p[1]), int(p[4], 16), int(p[5])))
                elif k == 'C' and len(p) >= 5:
                    t['C'].append((float(p[1]), int(p[2]), int(p[3]), p[4]))
                elif k == 'S' and len(p) >= 8:
                    t['S'].append((float(p[1]), float(p[2]), int(p[5]), int(p[6]), int(p[7])))
            except (ValueError, IndexError):
                pass   # (a line cut short at the end of a run)
    return t


def fight_span(t):
    """(first, last) logic frame queued while a fight was on, or None"""
    on = [k for _, k, f in t['Q'] if f]
    return (min(on), max(on)) if on else None


def stats(xs):
    xs = sorted(xs)
    if not xs:
        return None
    q = lambda f: xs[min(len(xs) - 1, int(f * len(xs)))]
    return {'n': len(xs), 'median': statistics.median(xs), 'mean': statistics.fmean(xs), 'p10': q(0.1), 'p90': q(0.9),
            'min': xs[0], 'max': xs[-1], 'sd': statistics.pstdev(xs)}


def analyse(t, game=GAME_FRAMES, render=RENDER_FRAMES):
    span = fight_span(t)
    res = {'refreshes': len(t['R']), 'queued': len(t['Q']), 'shown': len(t['S']), 'pad_changes': len(t['P'])}
    if not span:
        res['note'] = 'no fight in the trace: the whole run (menus) is measured'
        ticks = [k for _, k, _ in t['Q']]
        if not ticks:
            res['error'] = 'no frame queued'
            return res
        span = (300 if max(ticks) > 600 else 0, max(ticks))   # (past the first seconds of the start)
    lo, hi = span
    shown = [s for s in t['S'] if s[4] and lo <= s[3] <= hi]
    res['fight_pictures'] = len(shown)
    res['known'] = sum(1 for s in t['S'] if lo <= s[2] <= hi and s[4]) / max(1, sum(1 for s in t['S'] if lo <= s[2] <= hi))
    age = [s[0] - t['I'][s[3]] for s in shown if s[3] in t['I']]
    behind = [s[2] - s[3] for s in shown]
    res['age_ms'] = stats(age)
    res['behind_frames'] = stats(behind)
    presses, prev = [], 0
    for ms, buttons, tick in t['P']:
        pressed = buttons and not prev
        prev = buttons
        if not pressed or not lo <= tick <= hi:
            continue
        if t['C']:
            ch = next((c for c in t['C'] if c[0] > ms and c[1] >= tick), None)
            if ch is None:
                continue
            part, target, after = ch[1] - tick, ch[1] + render, ch[0]
        else:
            part, target, after = game, tick + game, ms
        s = next((s for s in shown if s[0] > after and s[3] >= target), None)
        if s:
            presses.append({'tick': tick, 'ms': s[0] - ms, 'picture_tick': s[3], 'game': part})
    res['presses'] = presses
    res['press_ms'] = stats([p['ms'] for p in presses])
    res['game_frames'] = stats([p['game'] for p in presses])
    res['status_lines'] = len(t['C'])
    gaps = [b[0] - a[0] for a, b in zip(shown, shown[1:])]
    if gaps:
        res['present_gap_ms'] = stats(gaps)
        res['gaps_over_1_5_frames'] = sum(1 for g in gaps if g > 1.5 * FRAME_MS)
    pd = [s[1] - s[0] for s in shown]
    res['present_call_ms'] = stats(pd)
    # how the age is spread: refresh -> pad read, pad read -> the next frame queued, queued -> present
    qt = [q[0] for q in t['Q']]
    rr, rq, qs = [], [], []
    for s in shown:
        r = t['I'].get(s[3])
        if r is None:
            continue
        i = bisect.bisect_right(t['R'], r)
        if i:
            rr.append(r - t['R'][i - 1])
        j = bisect.bisect_left(qt, r)
        if j < len(qt):
            rq.append(qt[j] - r)
            qs.append(s[0] - qt[j])
    res['refresh_to_read_ms'], res['read_to_queued_ms'], res['queued_to_present_ms'] = stats(rr), stats(rq), stats(qs)
    # (newer traces: the refresh's own tick, before low_latency's hold of the signal)
    tr = []
    for s in shown:
        r = t['I'].get(s[3])
        i = bisect.bisect_right(t['T'], r) if r is not None else 0
        if i:
            tr.append(r - t['T'][i - 1])
    res['tick_to_read_ms'] = stats(tr)
    return res


def fmt(st, unit='ms', frames=True):
    if not st:
        return 'none'
    f = (lambda v: f'{v:.1f}') if unit == 'ms' else (lambda v: f'{v:g}')
    s = f"median {f(st['median'])} {unit}, mean {f(st['mean'])}, p10-p90 {f(st['p10'])}-{f(st['p90'])}, sd {st['sd']:.2f} (n {st['n']})"
    if frames and unit == 'ms':
        s += f" = {st['median'] / FRAME_MS:.2f} frames"
    return s


def report(path, r, game):
    print(f"{path}: {r['refreshes']} refreshes, {r['queued']} queued, {r['shown']} shown, {r['pad_changes']} pad changes")
    if 'error' in r:
        print('  ' + r['error'])
        return
    if 'note' in r:
        print('  ' + r['note'])
    print(f"  fight pictures with a known logic frame: {r['fight_pictures']} ({100 * r['known']:.1f} % of the fight's)")
    print(f"  picture age (pad read of its logic frame -> present): {fmt(r['age_ms'])}")
    print(f"  picture behind the game: {fmt(r['behind_frames'], 'logic frames', False)}")
    if r['status_lines']:
        print(f"  press -> first status change (the game's part): {fmt(r['game_frames'], 'logic frames', False)}")
    else:
        print(f"  (no C lines: the game's part taken as {game} logic frames)")
    print(f"  press -> present: {fmt(r['press_ms'])}")
    if r.get('present_gap_ms'):
        print(f"  present gaps: {fmt(r['present_gap_ms'], frames=False)}; over 1.5 frames: {r['gaps_over_1_5_frames']}")
    print(f"  present call: {fmt(r['present_call_ms'], frames=False)}")
    if r.get('tick_to_read_ms'):
        print(f"  spread: refresh tick -> pad read (low_latency's hold in it) {fmt(r['tick_to_read_ms'], frames=False)}")
    print(f"  spread: signal -> pad read {fmt(r['refresh_to_read_ms'], frames=False)}")
    print(f"          pad read -> frame queued {fmt(r['read_to_queued_ms'], frames=False)}")
    print(f"          queued -> present {fmt(r['queued_to_present_ms'], frames=False)}")


def read_pictures(path):
    """[(logic frame, grid)] of the pictures whose logic frame is known, in order shown"""
    out = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.split()
            if len(p) >= 7 and p[6] == '1':
                out.append((int(p[5]), bytes.fromhex(p[4])))
    return out


def gdiff(a, b):
    return max(abs(x - y) for x, y in zip(a, b))


def calibrate(trace, pics, rtrace, rpics):
    t, rt = read_trace(trace), read_trace(rtrace)
    s, rs = fight_span(t), fight_span(rt)
    if not s or not rs:
        print('no fight in one of the traces')
        return 1
    first = {}
    for k, g in read_pictures(pics):
        first.setdefault(k - s[0], g)
    ref = {}
    for k, g in read_pictures(rpics):
        ref.setdefault(k - rs[0], g)
    prev, press = 0, None
    for ms, buttons, tick in t['P']:
        if buttons and not prev and s[0] <= tick <= s[1]:
            press = tick - s[0]
            break
        prev = buttons
    if press is None:
        print('no press in the fight')
        return 1
    # (the fight's start is seen at a frame end, which two runs can place a frame apart: the reference is shifted to
    # the alignment whose pictures before the press agree best)
    best = None
    for shift in range(-6, 7):
        common = [k for k in range(press - 120, press) if k in first and k + shift in ref]
        if len(common) < 30:
            continue
        noise = max(gdiff(first[k], ref[k + shift]) for k in common)
        if best is None or noise < best[0]:
            best = (noise, shift, len(common))
    if best is None:
        print('too few pictures before the press in both runs')
        return 1
    noise, shift, n = best
    c = next((k for k in range(press, press + 30) if k in first and k + shift in ref
              and gdiff(first[k], ref[k + shift]) > noise + 2), None)
    print(f'first press read at fight logic frame {press}; {n} pictures before it compared (reference shifted {shift}), noise {noise}')
    if c is None:
        print('no picture differs within 30 logic frames')
        return 1
    print(f'first picture that shows it: logic frame {c}: {c - press} logic frames after the read')
    ch = next((x for x in t['C'] if x[1] - s[0] >= press), None)
    if ch:
        print(f'first status change after the read: logic frame {ch[1] - s[0]}: RENDER_FRAMES = {c - (ch[1] - s[0])}')
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    if argv[1] == '--calibrate':
        return calibrate(*argv[2:6])
    game, as_json, paths = GAME_FRAMES, False, []
    i = 1
    while i < len(argv):
        if argv[i] == '--game':
            game = int(argv[i + 1])
            i += 1
        elif argv[i] == '--json':
            as_json = True
        else:
            paths.append(argv[i])
        i += 1
    for path in paths:
        r = analyse(read_trace(path), game)
        if as_json:
            print(json.dumps({'trace': path, 'game': game, **r}))
        else:
            report(path, r, game)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
