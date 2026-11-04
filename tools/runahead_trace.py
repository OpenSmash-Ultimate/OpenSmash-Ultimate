"""Run-ahead's trace (SSBU_RA_TRACE, runtime/hle_rb_core.c): the delay from a press to its picture, and
whether run-ahead changed the game.

    python tools/runahead_trace.py <trace> [<reference trace>]

A trace has, per fight safe point:
    R <logic frame> <fight hash> <player 1's status> <motion>          a real state
    S <frame shown> <logic frame> <fight hash> <status> <motion>       the state the frame shown draws
    P <frame shown> <logic frame> <buttons>                            player 1's buttons changed (real reads)
For every press (player 1's buttons from none to some) it finds the first frame shown whose player 1
differs (status or motion) from the frame shown before the press, and the first real logic frame that
differs from the one before the press: the delay to the picture in frames shown, and the game's own part
in logic frames. With a reference trace (the same run without run-ahead, SSBU_RB=1 SSBU_RUNAHEAD=0) it also
compares the real states logic frame by logic frame (they must all be the same) and each frame shown with
the reference's real state of its logic frame (with run-ahead 1 all the same; with 2, different only in the
frames that predicted a press or a release wrongly)."""
import statistics
import sys


def read(path):
    real, shown, pads, conflicts = {}, [], [], 0
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            try:
                if p[0] == 'R' and len(p) >= 5:
                    k = int(p[1])
                    v = (p[2], int(p[3]), p[4])
                    if k in real and real[k] != v:
                        conflicts += 1
                    real.setdefault(k, v)
                elif p[0] == 'S' and len(p) >= 6:
                    shown.append((int(p[1]), int(p[2]), p[3], int(p[4]), p[5]))
                elif p[0] == 'P' and len(p) >= 4:
                    pads.append((int(p[1]), int(p[2]), int(p[3], 16)))
            except ValueError:
                pass   # (a line cut short at the end of a run)
    return real, shown, pads, conflicts


def delays(real, shown, pads):
    """[(frame shown of the press, logic frame, frames shown to the picture, logic frames to the change)]"""
    by_host = {h: (k, st, mot) for h, k, _, st, mot in shown}
    hosts = sorted(by_host)
    out, prev = [], 0
    for host, key, buttons in pads:
        pressed = buttons and not prev
        prev = buttons
        if not pressed or host - 1 not in by_host or key - 1 not in real:
            continue
        before = by_host[host - 1][1:]
        pic = next((h - host for h in hosts if h >= host and by_host[h][1:] != before), None)
        rb = real[key - 1][1:]
        logic = next((k - key for k in range(key, key + 120) if k in real and real[k][1:] != rb), None)
        if pic is not None and logic is not None:
            out.append((host, key, pic, logic))
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    real, shown, pads, conflicts = read(argv[1])
    print(f'{argv[1]}: {len(real)} real logic frames, {len(shown)} frames shown, {len(pads)} pad changes; '
          f'{conflicts} logic frames with two different real states')
    d = delays(real, shown, pads)
    if d:
        pics = [x[2] for x in d]
        logic = [x[3] for x in d]
        print(f'presses: {len(d)}; press to picture: median {statistics.median(pics)} frames shown (min {min(pics)}, max {max(pics)}); '
              f"the game's own part: median {statistics.median(logic)} logic frames (min {min(logic)}, max {max(logic)})")
        print('  per press (frame shown, logic frame, to picture, logic):', ' '.join(f'{h}/{k}:{p}/{lg}' for h, k, p, lg in d))
    bad = conflicts
    if len(argv) > 2:
        ref, _, _, rc = read(argv[2])
        common = sorted(set(real) & set(ref))
        diff = [k for k in common if real[k][0] != ref[k][0]]
        print(f'real states against {argv[2]}: {len(common)} logic frames in both, {len(diff)} DIFFERENT'
              + (f' (first {diff[0]})' if diff else ''))
        same = sum(1 for _, k, h, _, _ in shown if k in ref and ref[k][0] == h)
        known = sum(1 for _, k, _, _, _ in shown if k in ref)
        print(f'frames shown against the reference\'s real states: {same} of {known} the same, {known - same} predicted differently')
        bad += len(diff) + rc
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
