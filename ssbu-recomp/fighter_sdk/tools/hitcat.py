"""Catalogue of hit properties, mined from the fighters' move sources (fighters/<name>/acmd/game.cpp).

Every ATTACK(...) call of every fighter and weapon is read into one table with named columns, then classified
into the properties a designer thinks in (spike, set knockback, multi-hit, windbox, element, ...). The output is
the first part of the "mechanics encyclopedia": what exists in the game, how it is written, how often, and
where to look at a real example.

    python fighter_sdk/tools/hitcat.py            writes fighters/catalog/hitboxes.csv, hit_properties.json,
                                                   HIT_PROPERTIES.md
    python fighter_sdk/tools/hitcat.py --find spike [--fighter mario]     list the hitboxes having a property

The parameter names are the modding community's (skyline-smash / smash-script documentation). The outputs are
made from the game's scripts: they stay under fighters/ and are not published.
"""
import collections
import csv
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sdk_paths  # noqa: E402

STUDY = sdk_paths.WORK
OUT = os.path.join(STUDY, 'fighters', 'catalog')

COLS = ['id', 'part', 'bone', 'damage', 'angle', 'kbg', 'fkb', 'bkb', 'size', 'x', 'y', 'z', 'x2', 'y2', 'z2',
        'hitlag', 'sdi', 'setoff', 'facing', 'set_weight', 'shield_damage', 'trip', 'rehit', 'reflectable',
        'absorbable', 'flinchless', 'no_hitlag', 'direct', 'situation', 'category', 'part_mask', 'friendly_fire',
        'effect', 'sound_level', 'sound', 'region']
HEAD = re.compile(r'^ACMD\w*\(\s*(\w+)\s*,\s*(\w+)\s*\)')
CALL = re.compile(r'\bATTACK\(agent,')


def split_args(text):
    """arguments of a call, text starting right after the opening parenthesis; returns (args, chars used)"""
    out, cur, depth, quote = [], [], 0, False
    for i, ch in enumerate(text):
        if quote:
            cur.append(ch)
            quote = ch != '"'
        elif ch == '"':
            cur.append(ch)
            quote = True
        elif ch == '(':
            depth += 1
            cur.append(ch)
        elif ch == ')':
            if depth == 0:
                out.append(''.join(cur).strip())
                return out, i + 1
            depth -= 1
            cur.append(ch)
        elif ch == ',' and depth == 0:
            out.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    return out, len(text)


def num(s):
    s = s.strip()
    if s in ('NIL', ''):
        return None
    if s in ('true', 'false'):
        return s == 'true'
    try:
        return float(s[:-1]) if s.endswith('f') else float(s)
    except ValueError:
        return s


def text(s):
    m = re.match(r'hash40\("([^"]*)"\)', s)
    return m.group(1) if m else s


def read_all():
    rows = []
    for path in sorted(glob.glob(os.path.join(STUDY, 'fighters', '*', 'acmd', 'game.cpp'))):
        module = os.path.basename(os.path.dirname(os.path.dirname(path)))
        agent = script = None
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = HEAD.match(line)
                if m:
                    agent, script = m.group(1), m.group(2)
                    continue
                for c in CALL.finditer(line):
                    args, _ = split_args(line[c.end():])
                    if len(args) != len(COLS):
                        continue
                    r = {'module': module, 'agent': agent, 'script': script}
                    for k, v in zip(COLS, args):
                        r[k] = text(v) if k in ('bone', 'effect') else num(v)
                    rows.append(r)
    return rows


def isnum(v):
    return isinstance(v, float)


# name, how to recognise it, what it means (community terms)
PROPERTIES = [
    ('spike', lambda r: isnum(r['angle']) and 240 <= r['angle'] <= 300,
     'angle between 240 and 300 (270 = straight down): sends downwards; a meteor when it hits an airborne opponent'),
    ('sakurai_angle', lambda r: r['angle'] == 361,
     'angle 361: the "Sakurai angle", low on the ground at low knockback, about 38-45 degrees otherwise'),
    ('special_angle', lambda r: isnum(r['angle']) and r['angle'] > 361,
     'angle above 361: engine-defined behaviours (autolink angles 362-368: the launch follows the attacker\'s '
     'movement or pulls towards the hitbox; used for drag-down and multi-hit links)'),
    ('semi_spike', lambda r: isnum(r['angle']) and (0 <= r['angle'] <= 25 or 155 <= r['angle'] <= 180),
     'angle within 25 degrees of horizontal: sends low and far'),
    ('vertical_launcher', lambda r: isnum(r['angle']) and 80 <= r['angle'] <= 100,
     'angle 80-100: sends straight up'),
    ('set_knockback', lambda r: isnum(r['fkb']) and r['fkb'] > 0,
     'fixed knockback value above 0: knockback does not depend on the target\'s damage'),
    ('set_weight', lambda r: r['set_weight'] is True,
     'set weight: every target is treated as the same weight (consistent multi-hits and throws)'),
    ('multi_hit', lambda r: isnum(r['rehit']) and r['rehit'] > 0,
     'rehit rate above 0: the same hitbox can hit again every N frames'),
    ('windbox', lambda r: r['flinchless'] is True,
     'flinchless: pushes without making the target flinch'),
    ('no_hitlag', lambda r: r['no_hitlag'] is True,
     'hitlag disabled: neither side freezes on hit'),
    ('high_hitlag', lambda r: isnum(r['hitlag']) and r['hitlag'] >= 1.5,
     'hitlag multiplier 1.5 or more: long freeze on hit (heavy feel)'),
    ('low_hitlag', lambda r: isnum(r['hitlag']) and 0 < r['hitlag'] <= 0.6,
     'hitlag multiplier 0.6 or less: short freeze (fast multi-hits)'),
    ('sdi_reduced', lambda r: isnum(r['sdi']) and r['sdi'] < 1.0,
     'SDI multiplier below 1: harder to shift out of'),
    ('sdi_increased', lambda r: isnum(r['sdi']) and r['sdi'] > 1.0,
     'SDI multiplier above 1: easier to shift out of'),
    ('trip', lambda r: isnum(r['trip']) and r['trip'] > 0,
     'trip chance above 0: can make a grounded target trip'),
    ('shield_damage_bonus', lambda r: isnum(r['shield_damage']) and r['shield_damage'] > 0,
     'extra shield damage'),
    ('shield_damage_malus', lambda r: isnum(r['shield_damage']) and r['shield_damage'] < 0,
     'reduced shield damage'),
    ('ground_only', lambda r: str(r['situation']).rstrip('_').endswith('MASK_G'),
     'hits grounded targets only'),
    ('air_only', lambda r: str(r['situation']).rstrip('_').endswith('MASK_A'),
     'hits airborne targets only'),
    ('indirect', lambda r: r['direct'] is False,
     'not a direct attack (projectile-like: counters and some effects treat it differently)'),
    ('reflectable', lambda r: r['reflectable'] is True, 'can be reflected'),
    ('absorbable', lambda r: r['absorbable'] is True, 'can be absorbed'),
    ('transcendent', lambda r: str(r['setoff']).endswith('_THRU'),
     'priority "through": does not clash with other hitboxes'),
    ('no_clang', lambda r: str(r['setoff']).endswith('_OFF'),
     'does not rebound on clash'),
    ('extended', lambda r: r['x2'] is not None,
     'second point given: the hitbox is a capsule stretched between two points'),
    ('faces_forward_only', lambda r: str(r['facing']).endswith('CHECK_F'),
     'target is sent the way the attacker faces'),
    ('faces_backward', lambda r: str(r['facing']).endswith('CHECK_B'),
     'target is sent behind the attacker'),
    ('by_position', lambda r: str(r['facing']).endswith('CHECK_POS'),
     'target is sent away from the attacker\'s position'),
    ('by_speed', lambda r: str(r['facing']).endswith('CHECK_SPEED'),
     'direction follows the hitbox owner\'s speed (projectiles)'),
    ('friendly_fire', lambda r: r['friendly_fire'] is True, 'can hit team mates regardless of the rule'),
    ('zero_damage', lambda r: r['damage'] == 0.0, 'no damage (pure push, grab-like or trigger hitbox)'),
]


def classify(rows):
    by = collections.OrderedDict((name, []) for name, _, _ in PROPERTIES)
    for r in rows:
        tags = []
        for name, test, _ in PROPERTIES:
            try:
                if test(r):
                    tags.append(name)
                    by[name].append(r)
            except TypeError:
                pass
        r['tags'] = tags
    return by


def where(r):
    return '%s %s' % (r['agent'], r['script'])


def examples(rs, n=4):
    seen, out = set(), []
    for r in rs:
        if r['module'] not in seen:
            seen.add(r['module'])
            out.append(where(r))
            if len(out) == n:
                break
    return out


def main():
    rows = read_all()
    by = classify(rows)
    if '--find' in sys.argv:
        name = sys.argv[sys.argv.index('--find') + 1]
        fighter = sys.argv[sys.argv.index('--fighter') + 1] if '--fighter' in sys.argv else None
        hits = [r for r in rows if (name in r['tags'] or r['effect'] == name or r['effect'] == 'collision_attr_' + name)
                and (not fighter or r['module'] == fighter)]
        for r in hits:
            print('%-40s id %-2s dmg %-5s angle %-4s kbg %-4s fkb %-4s bkb %-4s %s  %s'
                  % (where(r), int(r['id']) if isnum(r['id']) else r['id'], r['damage'], r['angle'], r['kbg'], r['fkb'],
                     r['bkb'], r['effect'].replace('collision_attr_', ''), ' '.join(r['tags'])))
        print('%d hitboxes' % len(hits))
        return
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'hitboxes.csv'), 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f)
        w.writerow(['module', 'agent', 'script'] + COLS + ['tags'])
        for r in rows:
            w.writerow([r['module'], r['agent'], r['script']] + [r[c] for c in COLS] + [' '.join(r['tags'])])
    effects = collections.Counter(r['effect'] for r in rows)
    eff_ex = {e: examples([r for r in rows if r['effect'] == e], 3) for e in effects}
    regions = collections.Counter(str(r['region']) for r in rows)
    sounds = collections.Counter(str(r['sound']) for r in rows)
    angles = collections.Counter(int(r['angle']) for r in rows if isnum(r['angle']))
    data = {
        'hitboxes': len(rows),
        'modules': len({r['module'] for r in rows}),
        'scripts': len({(r['agent'], r['script']) for r in rows}),
        'properties': [{'name': n, 'meaning': d, 'count': len(by[n]), 'examples': examples(by[n])}
                       for n, _, d in PROPERTIES],
        'effects': [{'name': e, 'count': c, 'examples': eff_ex[e]} for e, c in effects.most_common()],
        'regions': regions.most_common(),
        'sounds': sounds.most_common(),
        'special_angles': sorted((a, c) for a, c in angles.items() if a > 361),
    }
    with open(os.path.join(OUT, 'hit_properties.json'), 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=1)
    n = len(rows)
    md = ['# Hit properties found in the game\n',
          'Made by `fighter_sdk/tools/hitcat.py` from %d hitboxes (`ATTACK` calls) in %d move scripts of %d modules '
          '(fighters and their weapons). Game-derived: not published. Full table: `hitboxes.csv`; '
          '`python fighter_sdk/tools/hitcat.py --find <property|effect> [--fighter <name>]` lists the hitboxes.\n'
          % (n, data['scripts'], data['modules']),
          '## How a hitbox is written\n',
          '`ATTACK(agent, ' + ', '.join(COLS) + ')`\n',
          '## Properties\n',
          '| Property | How it is written | Hitboxes | Share | Examples (agent script) |', '|---|---|---|---|---|']
    for p in data['properties']:
        md.append('| %s | %s | %d | %.1f%% | %s |' % (p['name'], p['meaning'], p['count'], 100.0 * p['count'] / max(n, 1),
                                                    '; '.join(p['examples'])))
    md += ['\n## Angles above 361 (engine-defined)\n', '| Angle | Hitboxes |', '|---|---|']
    md += ['| %d | %d |' % ac for ac in data['special_angles']]
    md += ['\n## Effects (the `effect` argument: element and special reaction)\n',
           '| Effect | Hitboxes | Examples |', '|---|---|---|']
    for e in data['effects']:
        md.append('| %s | %d | %s |' % (e['name'].replace('collision_attr_', ''), e['count'], '; '.join(e['examples'])))
    md += ['\n## Regions (what the hit counts as: fist, kick, sword, ...)\n', '| Region | Hitboxes |', '|---|---|']
    md += ['| %s | %d |' % (r.replace('ATTACK_REGION_', ''), c) for r, c in data['regions']]
    with open(os.path.join(OUT, 'HIT_PROPERTIES.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(md) + '\n')
    print('%d hitboxes in %d scripts of %d modules -> %s' % (n, data['scripts'], data['modules'], OUT))
    for p in data['properties']:
        print('  %-22s %6d  %5.1f%%' % (p['name'], p['count'], 100.0 * p['count'] / max(n, 1)))
    print('  effects: %d kinds; angles above 361: %s' % (len(effects), data['special_angles']))


if __name__ == '__main__':
    main()
