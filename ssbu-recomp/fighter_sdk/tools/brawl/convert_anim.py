"""Animation converter: bone animations (CHR0) of a motion archive -> native .nuanmb files.

    python -I convert_anim.py convert <motion.pac> <costume.pac> <native motion folder> <out folder>
            [--only name,name,...] [--scale 1.0] [--root TransN]
    python -I convert_anim.py check <native motion folder>

The tracks are written for the PACKAGE's bone names (the converted skeleton keeps them, see convert_model.py), so
no retargeting is involved: every frame's local scale / rotation / translation is sampled from the key frames and
stored as the native packed track. The translation track of the package's root-motion bone (--root) is moved to
the native root bone "Trans". A CHR0 is converted when the native motion folder (the donor's files, named by the
hash of the file name) has a file <letter><2 digits><name>.nuanmb; the output file gets that hashed name.

"check" reads the game's own animation files with this tool's description of the packed track and reports how
many fit (bit width, size, unit quaternions).
"""
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from brawl import arc, brres, chr0, mdl0, nuanmb, ssbh     # noqa: E402
from brawl.motion_list import OWN, own_file                # noqa: E402

NATIVE_ROOT = 'Trans'


def hash40(s):
    b = s.lower().encode('ascii')
    return len(b) << 32 | (zlib.crc32(b) & 0xFFFFFFFF)


# motions whose native name differs from the old game's (same motion; the old game has one file where the native
# game has a left and a right one, or dropped the "quick / slow" pair)
RENAMED = {
    'cliffattackquick': ['cliffattack'], 'cliffclimbquick': ['cliffclimb'], 'cliffescapequick': ['cliffescape'],
    'cliffjumpquick1': ['cliffjump1'], 'cliffjumpquick2': ['cliffjump2'],
    'runbrake': ['runbrakel', 'runbraker'], 'walkbrake': ['walkbrakel', 'walkbraker'],
    'appeals': ['appealsl', 'appealsr'], 'appeallw': ['appeallwl', 'appeallwr'], 'appealhil': ['appealhil'],
    'attacks4scharged': [], 'slipdash': ['slipdash'], 'slipturn': ['slipturn'],
    # nearest counterparts (to be corrected by someone who knows both games; the specials are NOT guessed here)
    'attacks4hold': ['attacks4charge'], 'attackhi4hold': ['attackhi4charge'], 'attacklw4hold': ['attacklw4charge'],
    'swing4hold': ['swing4charge'], 'heavywalk1': ['heavywalk'],
    'furasleepstart': ['sleepstart'], 'furasleeploop': ['sleeploop'], 'furasleepend': ['sleepend'],
    'landingheavy': ['landinglight'], 'fall': ['runfalll', 'runfallr', 'walkfalll', 'walkfallr'],
    'turn': ['turndash'], 'downdamageu': ['downdamageu3'], 'downdamaged': ['downdamaged3'],
    'damageflyroll': ['damageflyrollend'], 'damageflylw': ['damageflymeteor'], 'guardoff': ['justshieldoff'],
    'downspotd': ['downspotu'], 'catchwait': ['catchpull'], 'stopceil': ['ceildamage'],
    # The donor's special / final slots are NOT given package animations (the owner's decision: no stopgap; the
    # fighter gets its own moves under its own motion names): they stay on the idle fill.
}


# swing animation of a smash: the start-up animation that precedes it in ONE native motion
COMBINE = {'AttackS4S': 'AttackS4Start', 'AttackHi4': 'AttackHi4Start', 'AttackLw4': 'AttackLw4Start'}
FULL_BODY = 100            # a donor motion file with fewer transform nodes moves only part of the body
_NODES = {}
_PLAN = {}


def donor_nodes(native_dir, hashed):
    """-> set of the transform node names of the donor's motion file of that (hashed) name, None when absent."""
    key = (native_dir, hashed)
    if key not in _NODES:
        p = os.path.join(native_dir, hashed)
        if not os.path.exists(p):
            _NODES[key] = None
        else:
            with open(p, 'rb') as f:
                v = ssbh.parse(f.read())[2]
            _NODES[key] = {n['name'] for g in v['groups'] if g['type'] == 1 for n in g['nodes']}
    return _NODES[key]


def bone_plan_cached(model):
    from brawl.convert_model import bone_plan
    if id(model) not in _PLAN:
        _PLAN[id(model)] = bone_plan(model)
    return _PLAN[id(model)]


def native_names(chr_name, have):
    """-> list of (native file name, hashed name) the animation is written to."""
    out = []
    low = chr_name.lower()
    for cand in [low] + RENAMED.get(low, []):
        for a in 'abcdefghijklmnopqrstuvwxyz':
            for n in range(100):
                fn = '%s%02d%s.nuanmb' % (a, n, cand)
                hashed = '0x%010x.nuanmb' % hash40(fn)
                if hashed in have and (fn, hashed) not in out:
                    out.append((fn, hashed))
    return out


def native_name(chr_name, have):
    lst = native_names(chr_name, have)
    return lst[0] if lst else (None, None)


def load_model(pac):
    a = arc.load(pac)
    for e in a.entries:
        if e.kind() == 'BRRES':
            b = brres.Brres(e.data)
            if b.models():
                return mdl0.Mdl0(e.data, b.models()[0][1])
    raise SystemExit('no model in ' + pac)


def sample_animation(c, model, scale, root):
    """-> {bone name: [ (scale, quat, trans) per frame ]}, in the package's bone names + the native root."""
    # Tracks are written for EVERY bone of the native-named skeleton that comes from package bones (a bone the
    # animation has no entry for gets its bind value, which is what the old game shows for it; the native engine
    # is not known to reset a bone without a track). Names and merged chains: convert_model.bone_plan.
    from brawl.convert_model import bone_plan
    bones = {b.name: b for b in model.bones}
    entries = {en.name: en for en in c.entries}

    def local(name, f):
        b, en = bones[name], entries.get(name)
        if en is None:
            s, r, t = b.scale, b.rot, b.trans
        else:
            s, r, t = en.sample(float(f))
            if s is None:
                s = b.scale if en.code >> 7 & 1 else (1.0, 1.0, 1.0)
            if r is None:
                r = b.rot if en.code >> 8 & 1 else (0.0, 0.0, 0.0)
            if t is None:
                t = b.trans if en.code >> 9 & 1 else (0.0, 0.0, 0.0)
        return tuple(s), nuanmb.quat_from_euler_deg(r), tuple(x * scale for x in t)

    plan, _names = bone_plan(model)
    from brawl.convert_model import reoriented
    turned = reoriented(model)
    index_name = {b.index: b.name for b in model.bones}
    half = (1.0, 0.0, 0.0, 0.0)                 # a half turn about X as a quaternion (x, y, z, w)
    out = {}
    for name, chain, _parent in plan:
        frames = []
        prev = None
        b0 = bones[chain[0]]
        bone_turned = len(chain) == 1 and b0.name in turned
        parent_turned = len(chain) == 1 and b0.parent >= 0 and index_name[b0.parent] in turned
        for f in range(c.frames):
            s, q, t = local(chain[0], f)
            if parent_turned:                  # the same change of local frame as in the skeleton (convert_model)
                q = quat_mul(half, q)
                t = (t[0], -t[1], -t[2])
            if bone_turned:
                q = quat_mul(q, half)
            for nxt in chain[1:]:              # compose parent * child (scales of merged bones are 1 in practice)
                s2, q2, t2 = local(nxt, f)
                rt = quat_rotate(q, t2)
                t = (t[0] + rt[0], t[1] + rt[1], t[2] + rt[2])
                q = quat_mul(q, q2)
                s = (s[0] * s2[0], s[1] * s2[1], s[2] * s2[2])
            if prev is None:
                # THE GAME'S FILES START EVERY ROTATION TRACK WITH w >= 0 (36,055 of 36,055 tracks of a fighter's
                # 344 motion files) and keep the sign continuous from there. The engine blends between motions
                # (and between the three walk files); a track that starts in the other half would be blended
                # against its opposite.
                if q[3] < 0:
                    q = tuple(-x for x in q)
            elif sum(x * y for x, y in zip(prev, q)) < 0:
                q = tuple(-x for x in q)
            prev = q
            frames.append((s, q, t))
        out[name] = frames
    return out


def quat_mul(a, b):
    x1, y1, z1, w1 = a
    x2, y2, z2, w2 = b
    return (w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2, w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
            w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2, w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2)


def bool_track(values):
    """-> (flags, frame count, bytes): a boolean track, constant (0x508) or packed one bit per frame (0x408:
    16-byte header {4, 0, offset of the default 0x20, 1 bit per frame, offset of the bits 0x21, frame count},
    16 zero bytes, the default byte, the bits, least significant first)."""
    if all(v == values[0] for v in values):
        return 0x508, 1, bytes([1 if values[0] else 0])
    bits = bytearray((len(values) + 7) // 8)
    for i, v in enumerate(values):
        if v:
            bits[i >> 3] |= 1 << (i & 7)
    return 0x408, len(values), (struct.pack('<HHHHII', 4, 0, 0x20, 1, 0x21, len(values)) + b'\0' * 16 +
                                bytes([1 if values[0] else 0]) + bytes(bits))


def read_bool_track(data, flags, frame_count):
    if flags in (0x508, 0x208):
        return [bool(data[0])] * max(1, frame_count)
    _u, _f, _d, bpe, off, n = struct.unpack_from('<HHHHII', data, 0)
    return [bool(data[off + (i >> 3)] >> (i & 7) & 1) for i in range(n)]


def build_file(name, tracks, frame_count, visibility=None, transform_flags=None):
    """visibility: {mesh name: [bool per frame]} -> a Visibility group as the game's motion files have it."""
    buf = bytearray()
    vis_nodes = []
    for mesh in sorted(visibility or {}):
        flags, cnt, data = bool_track(visibility[mesh])
        buf.extend(b'\0' * (-len(buf) % 64))
        vis_nodes.append({'name': mesh, 'tracks': [{'name': 'Visibility', 'flags': flags, 'frame_count': cnt,
                                                    'transform_flags': 0, 'data_offset': len(buf),
                                                    'data_size': len(data)}]})
        buf.extend(data)
    nodes = []
    for bone in sorted(tracks):
        flags, cnt, data = nuanmb.encode(tracks[bone])
        buf.extend(b'\0' * (-len(buf) % 64))
        off = len(buf)
        buf.extend(data)
        nodes.append({'name': bone, 'tracks': [{'name': 'Transform', 'flags': flags, 'frame_count': cnt,
                                                'transform_flags': (transform_flags or {}).get(bone, 0),
                                                'data_offset': off, 'data_size': len(data)}]})
    value = {'final_frame_index': float(frame_count - 1), 'unk1': 1, 'unk2': 3, 'name': name,
             'groups': [{'type': 1, 'nodes': nodes}] + ([{'type': 2, 'nodes': vis_nodes}] if vis_nodes else []),
             'buffer': bytes(buf), 'unk_a': [], 'unk_b': []}
    return ssbh.build(b'MINA', value)


def quat_rotate(q, v):
    x, y, z, w = q
    # v + 2 * cross(q.xyz, cross(q.xyz, v) + w * v)
    cx = (y * v[2] - z * v[1] + w * v[0], z * v[0] - x * v[2] + w * v[1], x * v[1] - y * v[0] + w * v[2])
    return (v[0] + 2 * (y * cx[2] - z * cx[1]), v[1] + 2 * (z * cx[0] - x * cx[2]), v[2] + 2 * (x * cx[1] - y * cx[0]))


def pose_positions(model, written, frame):
    """World positions of the package's bones at one frame, from the WRITTEN file's tracks (rigid chain: scale 1)."""
    _m, _v, anim = ssbh.parse(written)
    tr = {}
    for node in anim['groups'][0]['nodes']:
        t = node['tracks'][0]
        data = anim['buffer'][t['data_offset']:t['data_offset'] + t['data_size']]
        fr, _info = nuanmb.decode(data, t['flags'], t['frame_count'])
        tr[node['name']] = fr[min(frame, len(fr) - 1)]
    world = {}
    for b in model.bones:
        if b.name in tr:
            _s, q, t = tr[b.name]
        else:
            q, t = nuanmb.quat_from_euler_deg(b.rot), b.trans
        if b.parent < 0:
            world[b.index] = (q, t)
        else:
            pq, pt = world[b.parent]
            rt = quat_rotate(pq, t)
            x1, y1, z1, w1 = pq
            x2, y2, z2, w2 = q
            world[b.index] = ((w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2, w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
                               w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2, w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2),
                              (pt[0] + rt[0], pt[1] + rt[1], pt[2] + rt[2]))
    return {b.name: world[b.index][1] for b in model.bones}, tr.get(NATIVE_ROOT)


def convert(args):
    motion_pac, costume_pac, native_dir, out = args[:4]
    only, scale, root = None, 1.0, 'TransN'
    moveset = None
    fill, fill_data, written = None, None, set()
    own = False
    i = 4
    while i < len(args):
        if args[i] == '--moveset':           # the main fighter archive: mesh visibility per motion from its scripts
            from brawl import psa_listing as pl
            from brawl.psa import Psa
            from brawl.convert_model import switched_meshes
            for e in arc.load(args[i + 1]).entries:
                if e.type == 1 and e.size > 0x1000:
                    moveset = Psa(e.data)
        elif args[i] == '--own':             # also write the animations of motion_list.OWN under their own names
            own = True
            i += 1
            continue
        elif args[i] == '--names-min':       # the skeleton of convert_model --names-min (package limb names)
            from brawl.convert_model import set_names_min
            set_names_min(True)
            i += 1
            continue
        elif args[i] == '--fill':            # every motion file of the folder that got no animation gets this one
            fill = args[i + 1]
        elif args[i] == '--only':
            only = set(x.lower() for x in args[i + 1].split(','))
        elif args[i] == '--scale':
            scale = float(args[i + 1])
        elif args[i] == '--root':
            root = args[i + 1]
        else:
            raise SystemExit('unknown option ' + args[i])
        i += 2
    model = load_model(costume_pac)
    vis_meshes, by_name, switches, defaults = {}, {}, [], {}
    if moveset is not None:
        vis_meshes = switched_meshes(model)
        switches, defaults = pl.model_switches(moveset)
        for s in moveset.subactions:
            by_name.setdefault(s['name'], s)
        print('mesh visibility: %d switched meshes, %d switches in the moveset file' % (len(vis_meshes), len(switches)))
    unmatched = []
    extra = 0
    have ={f.lower() for f in os.listdir(native_dir)}
    os.makedirs(out, exist_ok=True)
    a = arc.load(motion_pac)
    chr_at = {}
    for e in a.entries:
        if e.kind() == 'BRRES':
            for nm_, off_ in brres.Brres(e.data).chr():
                chr_at[nm_] = (e.data, off_)
    done = skipped = 0
    for e in a.entries:
        if e.kind() != 'BRRES':
            continue
        b = brres.Brres(e.data)
        for name, off in b.chr():
            if only is not None and name.lower() not in only:
                continue
            targets = native_names(name, have)
            if not targets and own and name in OWN:
                targets = [own_file(name)]        # a motion of the fighter's OWN name (row: motion_list.py add)
            fn, hashed = targets[0] if targets else (None, None)
            if fn is None:
                skipped += 1
                unmatched.append(name)
                if only is not None:
                    print('%-22s no file of that motion name in the native folder: not converted' % name)
                continue
            c = chr0.Chr0(e.data, off)
            tracks = sample_animation(c, model, scale, root)
            n_frames = c.frames

            def vis_of(anim_name, count):
                if not vis_meshes:
                    return None, 0
                sub = by_name.get(anim_name)
                line = pl.visibility_timeline(moveset, sub, switches, defaults) if sub else [(0.0, {})]
                v_ = {}
                for mesh, bone in vis_meshes.items():
                    vals = []
                    k = 0
                    cur = line[0][1].get(bone, True)
                    for f in range(count):
                        while k + 1 < len(line) and line[k + 1][0] <= f:
                            k += 1
                            cur = line[k][1].get(bone, True)
                        vals.append(cur)
                    v_[mesh] = vals
                return v_, len(line) - 1
            vis, changes = vis_of(name, c.frames)
            note = ''
            # a smash is ONE native motion (start-up + swing); the package has the start-up as its own animation
            if name in COMBINE and COMBINE[name] in chr_at:
                cs = chr0.Chr0(*chr_at[COMBINE[name]])
                first = sample_animation(cs, model, scale, root)
                vis0, _c0 = vis_of(COMBINE[name], cs.frames)
                for bone in tracks:
                    a_, b_ = first[bone], tracks[bone]
                    if sum(x * y for x, y in zip(a_[-1][1], b_[0][1])) < 0:
                        b_ = [(s_, tuple(-x for x in q_), t_) for s_, q_, t_ in b_]
                    if bone == NATIVE_ROOT:              # the swing's travel continues where the start-up ended
                        end = a_[-1][2]
                        b_ = [(s_, q_, (t_[0] + end[0], t_[1] + end[1], t_[2] + end[2])) for s_, q_, t_ in b_]
                    tracks[bone] = a_ + b_
                if vis is not None:
                    vis = {m: vis0[m] + vis[m] for m in vis}
                n_frames = cs.frames + c.frames
                note = '; COMBINED: %s (%d frames) + %s (%d): the swing starts at frame %d' % (
                    COMBINE[name], cs.frames, name, c.frames, cs.frames)
            # a donor motion that moves only PART of the body (hand poses, thrown poses, faces): the same part only
            dn = donor_nodes(native_dir, hashed)
            if dn is not None and len(dn) < FULL_BODY:
                entered = {en.name for en in c.entries}
                plan_, _nn = bone_plan_cached(model)
                keep = {}
                for bone, chain, _p in plan_:
                    package_named = len(chain) == 1 and chain[0] == bone
                    if bone in tracks and (bone in dn or (package_named and bone in entered)):
                        keep[bone] = tracks[bone]
                note += '; PARTIAL donor motion (%d nodes): %d of %d bones written' % (len(dn), len(keep), len(tracks))
                tracks = keep
            vis_note = note
            if vis is not None:
                vis_note += '; visible at frame 0: %s; %d switch change(s)' % (
                    ','.join(sorted({model.bones[vis_meshes[m]].name for m, v in vis.items() if v[0]})) or '-',
                    changes)
            data = build_file(fn[:1].upper() + fn[1:3] + name + '.nuanmx', tracks, n_frames, vis)
            with open(os.path.join(out, hashed), 'wb') as f:
                f.write(data)
            for fn2, hashed2 in targets[1:]:       # the same motion under a second native name (left / right)
                with open(os.path.join(out, hashed2), 'wb') as f:
                    f.write(build_file(fn2[:1].upper() + fn2[1:3] + name + '.nuanmx', tracks, n_frames, vis))
                print('%-22s -> %-28s %s  (the same animation under a second native name)' % (name, fn2, hashed2))
                extra += 1
            root_track = tracks.get('Trans')
            travel = (root_track[-1][2][2] - root_track[0][2][2]) if root_track else 0.0
            print('%-22s -> %-28s %s  %3d frames, %2d bones, %6d bytes; root travel %.2f' % (
                name, fn, hashed, n_frames, len(tracks), len(data), travel) + vis_note)
            done += 1
            written.update(h for _f, h in targets)
            if name == fill:
                fill_data = (tracks, c.frames, vis)
    print('%d animation(s) converted (%d files with the second names) to %s; %d without a native file of that '
          'name' % (done, done + extra, out, skipped))
    if fill_data is not None:
        left = 0
        for fn in sorted(have):
            if not fn.endswith('.nuanmb') or fn in written:
                continue
            with open(os.path.join(native_dir, fn), 'rb') as f:
                donor_name = ssbh.parse(f.read())[2]['name']
            dn = donor_nodes(native_dir, fn)
            if dn is not None and len(dn) < FULL_BODY:
                # a partial motion (faces, upper body, hand poses) is laid OVER the body's motion by the engine:
                # a whole-body idle in its place would overwrite the root and the pose of whatever plays
                with open(os.path.join(native_dir, fn), 'rb') as f:
                    raw = f.read()
                with open(os.path.join(out, fn), 'wb') as f:
                    f.write(raw)
                print('KEPT    %-34s %s  the donor\'s own file: a PARTIAL motion (%d nodes), not filled' % (
                    donor_name, fn, len(dn)))
                continue
            with open(os.path.join(out, fn), 'wb') as f:
                f.write(build_file(donor_name, *fill_data))
            print('FILLED  %-34s %s  <- %s (no animation of the package was given this motion)' % (
                donor_name, fn, fill))
            left += 1
        print('%d motion file(s) of the folder filled with %s' % (left, fill))
    if only is None and unmatched:
        print('WITHOUT A NATIVE MOTION OF THAT NAME: ' + ' '.join(unmatched))


def check(folder):
    n = fit = unit_bad = files = unread = 0
    for fn in sorted(os.listdir(folder)):
        if not fn.endswith('.nuanmb'):
            continue
        with open(os.path.join(folder, fn), 'rb') as f:
            raw = f.read()
        try:
            _m, _v, v = ssbh.parse(raw)
        except ValueError:
            unread += 1
            continue
        files += 1
        for g in v['groups']:
            if g['type'] != 1:
                continue
            for node in g['nodes']:
                for t in node['tracks']:
                    if t['flags'] != nuanmb.F_PACKED:
                        continue
                    data = v['buffer'][t['data_offset']:t['data_offset'] + t['data_size']]
                    frames, info = nuanmb.decode(data, t['flags'], t['frame_count'])
                    n += 1
                    if info['computed_bits'] == info['bits_per_frame'] and info['expected_size'] == t['data_size']:
                        fit += 1
                    unit_bad += sum(1 for _s, q, _t in frames if q[0] ** 2 + q[1] ** 2 + q[2] ** 2 > 1.0005)
    print('%d files read (%d of another version not read); packed transform tracks: %d, fitting the description '
          '(bit width and size): %d; frames with a non-unit rotation: %d' % (files, unread, n, fit, unit_bad))


def close_loops(args):
    """close-loops <motion folder> <motion list .bin>

    The game's looping motions are CLOSED: their last frame repeats the first (the donor's walk_fast: Hip at
    frame 32 = frame 0), and the root's travel at that last frame is one full cycle. The package's loops are
    open (its CHR0 loop period is the frame count: the frame after the last is the first again), so a looping
    row's file got a pose jump at the wrap and one step of travel too little per cycle. For every row with the
    loop flag (0x02) whose file is open: one frame is appended = frame 0 of every bone, with Trans moved on by
    one more step of its own travel (last + (last - previous)); visibility tracks get their first value."""
    from brawl.motion_list import MotionList
    folder, list_path = args
    with open(list_path, 'rb') as f:
        ml = MotionList(f.read())
    done = set()
    n_fixed = 0
    for r in ml.rows:
        if not (r.flags & 0x02) or not r.anims:
            continue
        fn = '0x%010x.nuanmb' % (r.anims[0] & 0xFFFFFFFFFF)
        path = os.path.join(folder, fn)
        if fn in done or not os.path.exists(path):
            continue
        done.add(fn)
        with open(path, 'rb') as f:
            magic, ver, v = ssbh.parse(f.read())
        tracks, vis, opened = {}, {}, 0.0
        count = int(v['final_frame_index']) + 1
        for g in v['groups']:
            for node in g['nodes']:
                t = node['tracks'][0]
                data = v['buffer'][t['data_offset']:t['data_offset'] + t['data_size']]
                if g['type'] == 1:
                    fr = nuanmb.decode(data, t['flags'], t['frame_count'])[0]
                    fr = fr + [fr[-1]] * (count - len(fr))
                    tracks[node['name']] = fr
                    if node['name'] != NATIVE_ROOT:
                        a, b = fr[0], fr[-1]
                        opened = max(opened, 100 * (1 - abs(sum(x * y for x, y in zip(a[1], b[1])))) +
                                     max(abs(x - y) for x, y in zip(a[2], b[2])))
                elif g['type'] == 2:
                    vals = read_bool_track(data, t['flags'], t['frame_count'])
                    vis[node['name']] = vals + [vals[-1]] * (count - len(vals))
                else:
                    opened = -1.0                       # other groups: not handled, leave the file alone
        if opened <= 0.05 or count < 3:
            continue
        for name, fr in tracks.items():
            if name == NATIVE_ROOT:
                s0, q0, t0 = fr[0]
                last, prev = fr[-1][2], fr[-2][2]
                fr.append((s0, q0, tuple(l_ + (l_ - p_) for l_, p_ in zip(last, prev))))
            else:
                fr.append(fr[0])
        for name in vis:
            vis[name].append(vis[name][0])
        data = build_file(v['name'], tracks, count + 1, vis or None)
        ssbh.parse(data)
        with open(path, 'wb') as f:
            f.write(data)
        n_fixed += 1
        print('closed %-28s %s  %d -> %d frames (it ended %.3f away from its first pose)' % (
            v['name'], fn, count, count + 1, opened))
    print('%d looping motion file(s) closed' % n_fixed)


def fill_bones(args):
    """fill-bones <motion folder> <skeleton .nusktb>

    The game's whole-body motions carry a track for every bone of the skeleton (the donor's: 113 nodes, LegC and
    ClavicleC included); a bone without a track is not re-posed by the engine with the motion's travel, and the
    chains below it stay behind. Every file with at least 50 transform nodes (a whole-body motion) gets a
    constant track at the bone's rest value for each skeleton bone it lacks. Partial sets are left alone."""
    folder, skel_path = args
    s = ssbh.load(skel_path)[2]
    rest = {}
    for b, loc in zip(s['bone_entries'], s['local']):
        m = ssbh.mat4_rows(loc)                         # rows = local axes, translation in the last row
        sc = tuple(math.sqrt(sum(m[k][c] ** 2 for c in range(3))) for k in range(3))
        r = [[m[k][c] / sc[k] for c in range(3)] for k in range(3)]         # row-vector rotation
        # column-vector rotation matrix R = r^T
        R = [[r[c][k] for c in range(3)] for k in range(3)]
        tr = R[0][0] + R[1][1] + R[2][2]
        if tr > 0:
            w = math.sqrt(1 + tr) / 2
            q = ((R[2][1] - R[1][2]) / (4 * w), (R[0][2] - R[2][0]) / (4 * w), (R[1][0] - R[0][1]) / (4 * w), w)
        else:
            i = max(range(3), key=lambda k: R[k][k])
            j, k = (i + 1) % 3, (i + 2) % 3
            x = math.sqrt(max(0.0, 1 + R[i][i] - R[j][j] - R[k][k])) / 2
            qv = [0.0, 0.0, 0.0]
            qv[i] = x
            qv[j] = (R[j][i] + R[i][j]) / (4 * x)
            qv[k] = (R[k][i] + R[i][k]) / (4 * x)
            q = (qv[0], qv[1], qv[2], (R[k][j] - R[j][k]) / (4 * x))
        if q[3] < 0:
            q = tuple(-c for c in q)
        rest[b['name']] = (sc, q, tuple(m[3][:3]))
    n_files = n_tracks = 0
    for fn in sorted(os.listdir(folder)):
        if not fn.endswith('.nuanmb'):
            continue
        path = os.path.join(folder, fn)
        with open(path, 'rb') as f:
            try:
                magic, ver, v = ssbh.parse(f.read())
            except ValueError:
                continue
        nodes = [n for g in v['groups'] if g['type'] == 1 for n in g['nodes']]
        if len(nodes) < 50 or any(g['type'] not in (1, 2) for g in v['groups']):
            continue
        have = {n['name'] for n in nodes}
        missing = [b for b in rest if b not in have]
        if not missing:
            continue
        count = int(v['final_frame_index']) + 1
        tracks, vis = {}, {}
        for g in v['groups']:
            for node in g['nodes']:
                t = node['tracks'][0]
                data = v['buffer'][t['data_offset']:t['data_offset'] + t['data_size']]
                if g['type'] == 1:
                    fr = nuanmb.decode(data, t['flags'], t['frame_count'])[0]
                    tracks[node['name']] = fr + [fr[-1]] * (count - len(fr))
                else:
                    vals = read_bool_track(data, t['flags'], t['frame_count'])
                    vis[node['name']] = vals + [vals[-1]] * (count - len(vals))
        for b in missing:
            tracks[b] = [rest[b]] * count
        data = build_file(v['name'], tracks, count, vis or None)
        ssbh.parse(data)
        with open(path, 'wb') as f:
            f.write(data)
        n_files += 1
        n_tracks += len(missing)
    print('%d whole-body motion file(s) completed with %d rest-value track(s)' % (n_files, n_tracks))


LAYER = {0: 'whole body', 1: 'UPPER BODY set', 2: 'hand set', 3: 'face / shared set'}
THROWN_FROM = ('ThrownB', 'ThrownF', 'ThrownHi', 'ThrownLw')
ROOT_CHAIN = ('Trans', 'Rot', 'Hip')


def neutral(args):
    """neutral <donor motion folder> <delivery motion folder> <skeleton .nusktb of the delivery> <motion list .bin>

    The donor's motion files that the delivery still has BYTE FOR BYTE and that move only part of the body are made
    harmless for a fighter whose bones have other axes and proportions than the donor's:
    - a set the motion list marks as laid over another motion (the row's animation byte 1 upper body, 2 hands,
      3 face / shared) keeps its file, minus the transform nodes of bones that EXIST in this skeleton (those would
      take the donor's values); nodes of bones this skeleton does not have, and the other groups, stay;
    - a "thrown" pose of the donor (Big / Girl variants, no package animation) becomes the delivery's own converted
      ThrownB / F / Hi / Lw of the same direction.
    Every partial file of the delivery is then listed with its bones and the rows that use it."""
    from brawl.motion_list import MotionList
    donor, mine, skel_path, list_path = args[:4]
    with open(skel_path, 'rb') as f:
        have = {b['name'] for b in ssbh.parse(f.read())[2]['bone_entries']}
    with open(list_path, 'rb') as f:
        ml = MotionList(f.read())
    layer = {}
    rows = {}
    for r in ml.rows:
        for a, b in zip(r.anims, r.anim_bytes):
            layer[a & 0xFFFFFFFFFF] = b
            rows.setdefault(a & 0xFFFFFFFFFF, []).append(r)
    by_name = {}
    for fn in sorted(os.listdir(mine)):
        if fn.endswith('.nuanmb'):
            with open(os.path.join(mine, fn), 'rb') as f:
                try:
                    by_name[ssbh.parse(f.read())[2]['name'][3:].split('.')[0]] = fn
                except ValueError:
                    pass
    changed = 0
    for fn in sorted(os.listdir(donor)):
        mp = os.path.join(mine, fn)
        if not fn.endswith('.nuanmb') or not os.path.exists(mp):
            continue
        with open(os.path.join(donor, fn), 'rb') as f:
            raw = f.read()
        with open(mp, 'rb') as f:
            mine_raw = f.read()
        try:
            magic, ver, v = ssbh.parse(mine_raw)
        except ValueError:
            continue
        nodes = [n['name'] for g in v['groups'] if g['type'] == 1 for n in g['nodes']]
        try:
            donor_count = sum(len(g['nodes']) for g in ssbh.parse(raw)[2]['groups'] if g['type'] == 1)
        except ValueError:
            continue
        if donor_count >= FULL_BODY:
            continue                                       # a whole-body motion of the donor: not this step's
        h = int(fn[:12], 16)
        kind = layer.get(h)
        what = 'the delivery\'s converted file'
        if mine_raw == raw:
            short = v['name'][3:].split('.')[0]
            src = next((t for t in THROWN_FROM if short.startswith('Thrown') and short.endswith(t[6:])
                        and short != t), None)
            if kind in (1, 2, 3) or (kind is None and not nodes):
                dropped = [n for n in nodes if n in have]
                for g in v['groups']:
                    if g['type'] == 1:
                        g['nodes'] = [n for n in g['nodes'] if n['name'] not in have]
                out = ssbh.build(magic, v, ver)
                ssbh.parse(out)
                with open(mp, 'wb') as f:
                    f.write(out)
                nodes = [n for n in nodes if n not in have]
                what = 'the donor\'s file MINUS %d node(s) of bones this skeleton has (%s)' % (
                    len(dropped), ' '.join(dropped) or '-')
                changed += 1 if dropped else 0
            elif src and src in by_name:
                with open(os.path.join(mine, by_name[src]), 'rb') as f:
                    m2, ver2, v2 = ssbh.parse(f.read())
                v2['name'] = v['name']
                with open(mp, 'wb') as f:
                    f.write(ssbh.build(m2, v2, ver2))
                nodes = [n['name'] for g in v2['groups'] if g['type'] == 1 for n in g['nodes']]
                what = 'the delivery\'s own converted %s under this name' % src
                changed += 1
            else:
                what = 'the donor\'s file, UNCHANGED'
        root = [n for n in nodes if n in ROOT_CHAIN]
        print('%-28s %s layer %s; %2d transform nodes%s; rows %s: %s' % (
            v['name'][:-7], fn[:12], '%s (%s)' % (kind, LAYER.get(kind, '?')) if kind is not None else 'no row',
            len(nodes), ' (ROOT CHAIN: %s)' % ' '.join(root) if root else '',
            ', '.join('%#x flags %#x' % (r.key, r.flags) for r in rows.get(h, [])[:2]) or '-', what))
    print('%d file(s) rewritten' % changed)


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'fill-bones':
        fill_bones(sys.argv[2:])
    elif len(sys.argv) == 4 and sys.argv[1] == 'close-loops':
        close_loops(sys.argv[2:])
    elif len(sys.argv) == 6 and sys.argv[1] == 'neutral':
        neutral(sys.argv[2:])
    elif len(sys.argv) >= 6 and sys.argv[1] == 'convert':
        convert(sys.argv[2:])
    elif len(sys.argv) >= 3 and sys.argv[1] == 'check':
        check(sys.argv[2])
    else:
        print(__doc__)
