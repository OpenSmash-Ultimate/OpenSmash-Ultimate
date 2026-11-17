"""The delay from a press to the first picture that shows it, end to end in the runtime (SSBU_LAT_TRACE).

    python tools/latency_trace.py <trace> <pictures> <reference pictures>

<trace>: SSBU_LAT_TRACE of a run with presses (runtime/ipc_vi.c): R (a refresh signalled to the game), Q (a frame
queued), S (a frame shown: ms before and after its present, frames shown, fight frame) and P (player 1's buttons as
the game reads them: ms, frames shown, fight frame, buttons).
<pictures>: SSBU_PRESENT_HASH of the same run (Direct3D 11); <reference pictures>: of the same run without the
presses (SSBU_DETERMINISTIC=1 both, so that the pictures are alike until a press shows).

For every press (player 1's buttons from none to some) it finds the first fight frame after the read whose picture
differs from the reference's (on the 16 x 9 brightness grid, by more than the two runs' own difference before
it), and the time from the read to that picture's present: in ms and in frames of 1/60 s. It also gives how the
frame is spread: refresh -> pad read, refresh -> frame queued, queued -> shown (the hold in the runtime's queue).
A picture is shown at the monitor's next refresh after its present: up to one refresh more (half on average with
clock pacing), which a headless run cannot see."""
import statistics
import sys

FRAME_MS = 1000.0 / 60


def read_trace(path):
    R, Q, S, P = [], [], [], []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.split()
            try:
                if p[0] == 'R':
                    R.append(float(p[1]))
                elif p[0] == 'Q':
                    Q.append(float(p[1]))
                elif p[0] == 'S' and len(p) >= 5:
                    S.append((float(p[1]), float(p[2]), int(p[3]), int(p[4])))
                elif p[0] == 'P' and len(p) >= 5:
                    P.append((float(p[1]), int(p[2]), int(p[3]), int(p[4], 16)))
            except (ValueError, IndexError):
                pass
    return R, Q, S, P


def read_pictures(path):
    """{fight frame: grid}; the fight frame a picture was presented in (its S line has it + 1)"""
    out = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.split()
            if len(p) >= 5 and p[1] != '0':
                g = bytes.fromhex(p[4])
                out[int(p[1])] = g
    return out


def gdiff(a, b):
    return max(abs(x - y) for x, y in zip(a, b))


def after(times, t):
    lo, hi = 0, len(times)
    while lo < hi:
        m = (lo + hi) // 2
        if times[m] <= t:
            lo = m + 1
        else:
            hi = m
    return times[lo] if lo < len(times) else None


def before(times, t):
    lo, hi = 0, len(times)
    while lo < hi:
        m = (lo + hi) // 2
        if times[m] <= t:
            lo = m + 1
        else:
            hi = m
    return times[lo - 1] if lo else None


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 2
    R, Q, S, P = read_trace(argv[1])
    pics, ref = read_pictures(argv[2]), read_pictures(argv[3])
    shown_by_fight = {}
    for s in S:
        shown_by_fight.setdefault(s[3], s)   # (the S line's fight frame is the picture's + 1)
    res, prev = [], 0
    for t, _shown, fight, buttons in P:
        pressed = buttons and not prev
        prev = buttons
        if not pressed or fight <= 0:
            continue
        common = [f for f in range(max(1, fight - 60), fight) if f in pics and f in ref]
        floor = max((gdiff(pics[f], ref[f]) for f in common), default=0)
        first = next((f for f in range(fight, fight + 30) if f in pics and f in ref and gdiff(pics[f], ref[f]) > floor + 2), None)
        if first is None or first + 1 not in shown_by_fight:
            continue
        s = shown_by_fight[first + 1]
        r = before(R, t)
        res.append((fight, first - fight, s[0] - t, s[1] - t, t - r if r else None, floor))
    print(f'{argv[1]}: {len(R)} refreshes, {len(Q)} frames queued, {len(S)} shown, {len(P)} pad changes; {len(res)} presses found')
    for fight, frames, ms0, ms1, poll, floor in res:
        print(f'  press read at fight frame {fight}: first picture {frames} fight frames later, present called {ms0:.1f} ms '
              f'after the read ({ms0 / FRAME_MS:.2f} frames; present returned at {ms1:.1f}), the read {poll:.1f} ms after its refresh (noise {floor})')
    if res:
        m = [x[2] for x in res]
        print(f'press -> present: median {statistics.median(m):.1f} ms = {statistics.median(m) / FRAME_MS:.2f} frames '
              f'(min {min(m):.1f}, max {max(m):.1f}); + the wait for the monitor\'s refresh after the present')
    # how a frame is spread, over the run
    rq = [q - r for q in Q if (r := before(R, q)) is not None and q - r < 3 * FRAME_MS]
    qs = [s[0] - q for s in S if (q := before(Q, s[0])) is not None and s[0] - q < 3 * FRAME_MS]
    rp = [t - r for t, *_ in P if (r := before(R, t)) is not None and t - r < 3 * FRAME_MS]
    gaps = [b[0] - a[0] for a, b in zip(S, S[1:])]
    if rq:
        print(f'refresh -> queued: median {statistics.median(rq):.1f} ms; queued -> present: median {statistics.median(qs):.1f} ms'
              + (f'; refresh -> pad read: median {statistics.median(rp):.1f} ms' if rp else ''))
    if gaps:
        late = sum(1 for g in gaps if g > FRAME_MS * 1.5)
        print(f'frames shown: {len(gaps) + 1}, {late} gaps over 1.5 frames ({100.0 * late / len(gaps):.2f} %), '
              f'gap sd {statistics.pstdev(gaps):.2f} ms')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
