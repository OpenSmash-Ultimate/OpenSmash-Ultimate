#!/usr/bin/env python3
"""Compare two SSBU_FIGHT_HASH logs (docs/TESTING-RUNS.md): the first line that differs, its frame,
fighter and the fields that differ, or how many lines are the same.

Usage: fight_hash_diff.py <log A> <log B> [--context N]

Only whole hash lines are read ("F ..." to "...%)"): a run that was stopped leaves its last line
cut, and a log written to the standard output (SSBU_FIGHT_HASH=1) has the game's other lines in
it. The logs are compared over the lines both have, as two runs end a few frames apart: a line
is paired with the other log's line of the same frame (F) and fighter (e), so a line one log
lost moves nothing after it. As ssbu-recomp's tests/s11_run.py diff, with the fields named.

Exit status: 0 the same, 1 a line differs (or no frame and fighter is in both), 2 a log missing or
without hash lines.
"""
import sys


def hash_lines(path):
    with open(path, errors='replace') as f:
        return [line.rstrip('\r\n') for line in f if line.startswith('F ') and line.rstrip('\r\n').endswith('%)')]


def fields(line):
    """The line's (name, value) pairs; the part in brackets is the same numbers for reading."""
    words = line.split('  (')[0].split()
    return list(zip(words[0::2], words[1::2]))


def keys(lines):
    """Each line's key: its frame (F), the fighter's entry id (e), and how many lines of both came
    before it (where the entry id was not found, every fighter's e is -1)."""
    seen, out = {}, []
    for line in lines:
        f = dict(fields(line))
        k = (f.get('F'), f.get('e'))
        seen[k] = n = seen.get(k, -1) + 1
        out.append((*k, n))
    return out


def pair(a, b):
    """The lines both logs have, in A's order: [(index in A, index in B)] of the lines of one frame
    and fighter; and how many lines either log has among them that the other lacks (lost from a
    log: a line the game's other output cut in two), apart from those after the last pair (a run
    that went on)."""
    ka, kb = keys(a), keys(b)
    in_a, in_b = {k: i for i, k in enumerate(ka)}, {k: j for j, k in enumerate(kb)}
    pairs = [(i, in_b[k]) for i, k in enumerate(ka) if k in in_b]
    if not pairs:
        return pairs, 0
    last_a, last_b = pairs[-1][0], max(j for _i, j in pairs)
    lost = sum(1 for i, k in enumerate(ka) if i < last_a and k not in in_b) + sum(1 for j, k in enumerate(kb) if j < last_b and k not in in_a)
    return pairs, lost


def first_difference(a, b, pairs):
    """(place in pairs, differing field names) of the first pair of lines that differ, or None."""
    for p, (i, j) in enumerate(pairs):
        if a[i] != b[j]:
            fa, fb = fields(a[i]), fields(b[j])
            names = [na for (na, va), (nb, vb) in zip(fa, fb) if na != 'H' and (na != nb or va != vb)]
            return p, names
    return None


def main(argv):
    args = list(argv)
    context = 2
    if '--context' in args:
        k = args.index('--context')
        try:
            context = int(args[k + 1])
        except (IndexError, ValueError):
            return usage()
        del args[k:k + 2]
    if len(args) != 2:
        return usage()
    logs = []
    for path in args:
        try:
            lines = hash_lines(path)
        except OSError as e:
            print(f'{path}: {e.strerror}', file=sys.stderr)
            return 2
        if not lines:
            print(f'{path}: no hash lines', file=sys.stderr)
            return 2
        logs.append(lines)
    (pa, pb), (a, b) = args, logs
    pairs, lost = pair(a, b)
    if not pairs:
        print(f'{pa} vs {pb}: no frame and fighter in both ({len(a)} / {len(b)} lines)')
        return 1
    found = first_difference(a, b, pairs)
    if found is None:
        also = f'; {lost} line{"s" if lost != 1 else ""} in one log only, left out' if lost else ''
        print(f'{pa} vs {pb}: the same over {len(pairs)} lines ({len(a)} / {len(b)} lines; last frame {a[pairs[-1][0]].split()[1]}{also})')
        return 0
    p, names = found
    i, j = pairs[p]
    f = dict(fields(a[i]))
    what = ('fields ' + ' '.join(names) if names else
            'only H (the running hash: the generators\' whole hash, which a line shows folded to 32 bits, here or before, or the '
            'lines before it in another order)')
    where = f'{i + 1}' if i == j else f'{i + 1} / {j + 1}'
    print(f'{pa} vs {pb}: first difference at line {where} of {len(a)} / {len(b)}: frame {f.get("F", "?")} fighter e {f.get("e", "?")}: {what}')
    for k in range(max(0, p - context), min(len(pairs), p + context + 1)):
        mark = '>' if k == p else ' '
        print(f'{mark}  A {a[pairs[k][0]]}')
        print(f'{mark}  B {b[pairs[k][1]]}')
    return 1


def usage():
    print(__doc__.split('\n\n')[1], file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
