"""The native effect container (.eff): read it, clone emitter sets, add textures, write it back.

Second stage of the effect converter (reff.py reads the Wii predecessor's effects): a new fighter's effects are
made from the emitter sets of an effect file it already has (its donor's, or a skin's), each cloned under a new
effect name and, where wanted, pointed at other textures - the file's own or new ones added from PNG files (for
example the old game's textures that reff.py exported). Emitter bodies are copied as they are (their life, rate,
scale and colour stay the template's; the effect scripts scale and tint them: EFFECT size, LAST_EFFECT_SET_COLOR).

Layout, as read from the files on this PC (no other program's code used):

  EFFN       "EFFN", u32 0x20000, u32 effect count, u32 0x10000; per effect 16 bytes (u32 flags, u32 emitter set
             index + 1, u64 0); then the effect names (upper case, each ended by 0); the VFXB starts at 0x1000.
  VFXB       "VFXB    ", u16 0, u16 version, u16 byte-order mark, u8 alignment log2 (12), u8 0x40, u32 0x20,
             u16 0, u16 first section (0x40), u32 0, u32 VFXB size.
  section    magic, u32 size, u32 first child (from the section), u32 next section (from the section, ~0 = last),
             u32 attribute block (~0 none), u32 binary (from the section), u32 0, u32 child count (0x20 bytes).
             ESTA (emitter sets) > ESET (emitter set: binary + 0x10 = its name, 64 bytes) > EMTR (emitters,
             binary + 0x10 = name); GRTF (textures) > GTNT (texture ids and names), its binary = a BNTX;
             PRMA, G3PR (primitives), GRSN / GRSC (shaders). Binaries are 4 KB aligned in the file, so every size
             change is made in whole 4 KB steps.
  GTNT       entries (as many as fit in the section size): u64 texture id, u32 entry size, u32 name size with
             its 0, the name and at least two zero bytes, 8-aligned.
             An emitter refers to its textures by these ids.
  BNTX       written again whole (header, NX, texture pointer array, _STR with the _DIC name tree, the BRTI
             texture records with their mip offset arrays, BRTD data, _RLT relocation table).

    python -I vfxb.py info <file.eff>
    python -I vfxb.py build <file.eff> <plan.json> <out.eff>
        plan.json: {"textures": [{"name": "pg_fire", "png": "ptc_ganon_fire.png"}],
                    "sets": [{"from": "GANON_ENTRY_AURA", "name": "GANON_PG_ENTRY_AURA",
                              "textures": {"ef_ganon_fire00": "pg_fire"}}]}
        (paths relative to the plan; "from" is an effect name or an emitter set name)
    python -I vfxb.py check <file.eff>
        parses everything the writer touches (sections, sets, GTNT, BNTX pointers) and reports.
"""
import json
import os
import struct
import sys

if __package__ in (None, ''):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from brawl import bntx as bntx_png, nutexb          # noqa: E402
else:
    from . import bntx as bntx_png, nutexb

NONE = 0xFFFFFFFF
PAGE = 0x1000


def _up(n, a):
    return (n + a - 1) // a * a


def fnv1(name):
    h = 0x811C9DC5
    for c in name.encode():
        h = ((h * 0x01000193) & 0xFFFFFFFF) ^ c
    return h


# ------------------------------------------------------------------ sections
def sec(d, o):
    magic = bytes(d[o:o + 4])
    size, child, nxt, attr, binary, _z, count = struct.unpack_from('<IIIIIII', d, o + 4)
    return {'magic': magic, 'at': o, 'size': size, 'child': child, 'next': nxt, 'attr': attr, 'binary': binary,
            'count': count}


def chain(d, o):
    while True:
        s = sec(d, o)
        yield s
        if s['next'] == NONE:
            return
        o += s['next']


# ------------------------------------------------------------------ BNTX
class Texture:
    """One BRTI record: its 0xA0-byte description (pointers cleared), mip offsets (from its data), data bytes."""

    def __init__(self, name, desc, mips, data):
        self.name, self.desc, self.mips, self.data = name, bytearray(desc), list(mips), bytes(data)

    @property
    def align(self):
        return struct.unpack_from('<I', self.desc, 0x44)[0] or 0x200


def bntx_read(b):
    if b[:4] != b'BNTX':
        raise ValueError('not a BNTX')
    name_off = struct.unpack_from('<I', b, 0x10)[0]
    fname = b[name_off:b.index(b'\0', name_off)].decode()
    count = struct.unpack_from('<I', b, 0x24)[0]
    arr = struct.unpack_from('<Q', b, 0x28)[0]
    tex = []
    for i in range(count):
        t = struct.unpack_from('<Q', b, arr + 8 * i)[0]
        if b[t:t + 4] != b'BRTI':
            raise ValueError('texture %d: no BRTI at %x' % (i, t))
        nptr, _parent, mip_arr = struct.unpack_from('<QQQ', b, t + 0x60)
        ln = struct.unpack_from('<H', b, nptr)[0]
        name = b[nptr + 2:nptr + 2 + ln].decode()
        nmips = struct.unpack_from('<H', b, t + 0x16)[0]
        size = struct.unpack_from('<I', b, t + 0x50)[0]
        offs = [struct.unpack_from('<Q', b, mip_arr + 8 * m)[0] for m in range(nmips)]
        base = offs[0]
        desc = bytearray(b[t + 0x10:t + 0xA0])            # from + 0x10 (after the block header) to the pointers' end
        tex.append(Texture(name, desc, [o - base for o in offs], b[base:base + size]))
    return fname, tex


class Rel:
    """Pointer positions for the _RLT, by section (0 = the file's structures, 1 = the BRTD data)."""

    def __init__(self):
        self.p = {0: [], 1: []}

    def add(self, pos, section=0):
        self.p[section].append(pos)


def _dic(keys):
    """The name tree (_DIC): nodes of (reference bit, left, right, key). Bits are counted from the key's end."""
    def bit(k, n):
        i = n >> 3
        return (k[len(k) - 1 - i] >> (n & 7)) & 1 if i < len(k) else 0

    nodes = [[-1, 0, 0, b'']]
    for key in keys:
        # the closest key in the tree
        p, q = 0, nodes[0][1]
        while nodes[q][0] > nodes[p][0]:
            p, q = q, (nodes[q][2] if bit(key, nodes[q][0]) else nodes[q][1])
        other = nodes[q][3]
        b = 0
        while bit(key, b) == bit(other, b):
            b += 1
            if b > 8 * max(len(key), len(other)) + 8:
                raise ValueError('duplicate texture name %r' % key)
        p, q = 0, nodes[0][1]
        while nodes[q][0] > nodes[p][0] and nodes[q][0] < b:
            p, q = q, (nodes[q][2] if bit(key, nodes[q][0]) else nodes[q][1])
        n = len(nodes)
        nodes.append([b, n, q] if not bit(key, b) else [b, q, n])
        nodes[-1].append(key)
        if p == 0:
            nodes[0][1] = n
        elif bit(key, nodes[p][0]):
            nodes[p][2] = n
        else:
            nodes[p][1] = n
    return nodes


def bntx_write(fname, tex):
    out = bytearray()
    rel = Rel()

    def put(b, at=None):
        if at is None:
            out.extend(b)
        else:
            out[at:at + len(b)] = b

    count = len(tex)
    put(b'\0' * 0x198)                                    # header, NX, memory pool area
    arr = len(out)
    put(b'\0' * 8 * count)
    # _STR: count, the empty string, the names; then the _DIC
    str_at = _up(len(out), 8)
    put(b'\0' * (str_at - len(out)))
    put(b'_STR' + b'\0' * 12)
    names = [fname] + [t.name for t in tex]
    put(struct.pack('<I', len(names)))
    empty = len(out)
    put(b'\0\0\0\0')
    spos = {}
    for n in names:
        spos[n] = len(out)
        e = n.encode()
        put(struct.pack('<H', len(e)) + e + b'\0')
        if len(out) % 2:
            put(b'\0')
    dic_at = _up(len(out), 8)
    put(b'\0' * (dic_at - len(out)))
    nodes = _dic([t.name.encode() for t in tex])
    put(b'_DIC' + struct.pack('<I', count))
    for k, (ref, left, right, key) in enumerate(nodes):
        rel.add(len(out) + 8)
        put(struct.pack('<iHHQ', ref, left, right, empty if not key else spos[key.decode()]))
    brti_at = _up(len(out), 8)
    put(b'\0' * (brti_at - len(out)))
    struct.pack_into('<4sIII', out, str_at, b'_STR', brti_at - str_at, brti_at - str_at, 0)
    # BRTI records
    at = []
    for t in tex:
        o = len(out)
        at.append(o)
        nm = len(t.mips)
        size = 0x2A0 + 8 * nm
        put(b'\0' * size)
        struct.pack_into('<4sIII', out, o, b'BRTI', size, size, 0)
        out[o + 0x10:o + 0xA0] = t.desc
        struct.pack_into('<QQQQQQQQ', out, o + 0x60, spos[t.name], 0x20, o + 0x2A0, 0, o + 0xA0, o + 0x1A0, 0, 0)
        for p in (0x60, 0x68, 0x70, 0x80, 0x88):
            rel.add(o + p)
    for i, o in enumerate(at):
        struct.pack_into('<Q', out, arr + 8 * i, o)
        rel.add(arr + 8 * i)
    # BRTD: data 4 KB aligned
    brtd = _up(len(out) + 0x10, PAGE) - 0x10
    put(b'\0' * (brtd - len(out)))
    put(b'BRTD' + b'\0' * 12)
    for t, o in zip(tex, at):
        start = _up(len(out), max(t.align, 0x200))
        put(b'\0' * (start - len(out)))
        for m, off in enumerate(t.mips):
            struct.pack_into('<Q', out, o + 0x2A0 + 8 * m, start + off)
            rel.add(o + 0x2A0 + 8 * m, 1)
        put(t.data)
    rlt = _up(len(out), PAGE)
    put(b'\0' * (rlt - len(out)))
    struct.pack_into('<4sIQ', out, brtd, b'BRTD', 0, rlt - brtd)
    # header and NX
    struct.pack_into('<4sIIHBBIHHII', out, 0, b'BNTX', 0, 0x40000, 0xFEFF, 12, 0x40, spos[fname] + 2, 0, str_at, rlt, 0)
    struct.pack_into('<4sIQQQQ', out, 0x20, b'NX  ', count, arr, brtd, dic_at, 0x58)
    for p in (0x28, 0x38, 0x40):
        rel.add(p)
    rel.add(0x30, 1)
    # _RLT: two sections (the structures; the BRTD as its own memory), entries of runs of adjacent pointers
    entries, secs = [], []
    for s, (lo, hi) in enumerate(((0, brtd), (brtd, rlt))):
        first = len(entries)
        ps = sorted(set(rel.p[s]))
        i = 0
        while i < len(ps):
            j = i
            while j + 1 < len(ps) and ps[j + 1] == ps[j] + 8 and j + 1 - i < 255:
                j += 1
            entries.append((ps[i], 1, j - i + 1, 0))
            i = j + 1
        secs.append((0, lo, hi - lo, first, len(entries) - first))
    r = bytearray(b'_RLT' + struct.pack('<III', rlt, len(secs), 0))
    for s in secs:
        r += struct.pack('<QIIII', *s)
    for e in entries:
        r += struct.pack('<IHBB', *e)
    put(bytes(r))
    struct.pack_into('<I', out, 0x1C, len(out))
    return bytes(out)


def rgba8_texture(name, png, like):
    """A new RGBA8 texture (one mip) from a PNG, its description taken from `like` (another texture's)."""
    w, h, rgba = bntx_png.read_png(png)
    # (the old game's textures are 16 - 64 texels: drawn on an emitter at that size, a 16 x 16 one showed as hard
    # squares in a test run, the same picture at 128 x 128 as soft sprites; small ones are scaled up to 128 on the
    # short side, bilinear)
    k = 1
    while min(w, h) * k < 128:
        k *= 2
    if k > 1:
        rgba = bntx_png.fit(w, h, rgba, w * k, h * k, 'cover')
        w, h = w * k, h * k
    data = nutexb.swizzle(bytes(rgba), w, h, 4)
    bh = nutexb.block_height(h)
    desc = bytearray(like.desc)                     # flags, dimension, layout words as the template's
    struct.pack_into('<HHHI', desc, 0x02, 0, 0, 1, 1)   # block-linear, no swizzle field, one mip, one sample
    struct.pack_into('<IIiiiii', desc, 0x0C, 0x0B01, struct.unpack_from('<I', like.desc, 0x10)[0], w, h, 1, 1,
                     bh.bit_length() - 1)
    struct.pack_into('<II', desc, 0x40, len(data), 0x200)
    desc[0x48:0x4C] = bytes((2, 3, 4, 5))            # channels R, G, B, A
    return Texture(name, desc, [0], data)


# ------------------------------------------------------------------ the .eff
class Eff:
    def __init__(self, path):
        d = open(path, 'rb').read()
        if d[:4] != b'EFFN':
            raise ValueError('not an effect file')
        self.head = bytearray(d[:0x10])
        n = struct.unpack_from('<I', d, 8)[0]
        self.entries = [list(struct.unpack_from('<IIQ', d, 0x10 + 16 * i)) for i in range(n)]
        p = 0x10 + 16 * n
        self.names = []
        for _ in range(n):
            e = d.index(b'\0', p)
            self.names.append(d[p:e].decode())
            p = e + 1
        self.vfx_at = d.index(b'VFXB', p)
        self.v = bytearray(d[self.vfx_at:])
        v = self.v
        self.esta = sec(v, struct.unpack_from('<H', v, 0x16)[0])
        if self.esta['magic'] != b'ESTA':
            raise ValueError('first section is %r' % self.esta['magic'])
        self.sets = list(chain(v, self.esta['at'] + self.esta['child']))
        for s in self.sets:
            s['name'] = v[s['at'] + s['binary'] + 0x10:s['at'] + s['binary'] + 0x50].split(b'\0')[0].decode()
        self.grtf = sec(v, self.esta['at'] + self.esta['next'])
        if self.grtf['magic'] != b'GRTF':
            raise ValueError('after the emitter sets: %r' % self.grtf['magic'])
        self.gtnt = sec(v, self.grtf['at'] + self.grtf['child'])
        self.tex_ids = []
        o = self.gtnt['at'] + self.gtnt['binary']
        while o < self.gtnt['at'] + self.gtnt['size']:            # (the section's size ends the list)
            tid, esz, nl = struct.unpack_from('<QII', v, o)
            self.tex_ids.append((tid, v[o + 16:o + 15 + nl].decode()))
            if not esz:
                break
            o += esz
        b = v[self.grtf['at'] + self.grtf['binary']:self.grtf['at'] + self.grtf['binary'] + self.grtf['size']]
        self.bntx_name, self.textures = bntx_read(bytes(b))
        self.new_sets = []                               # (effect name, flags, bytes)
        self.new_textures = []

    # -- queries
    def set_index(self, name):
        u = name.upper()
        if u in self.names:
            return self.entries[self.names.index(u)][1] - 1
        for i, s in enumerate(self.sets):
            if s['name'] == name:
                return i
        raise KeyError('no effect or emitter set %r' % name)

    def emitters(self, i):
        s = self.sets[i]
        out = []
        if s['count']:
            for e in chain(self.v, s['at'] + s['child']):
                e['name'] = self.v[e['at'] + e['binary'] + 0x10:e['at'] + e['binary'] + 0x50].split(b'\0')[0].decode()
                out.append(e)
        return out

    def texture_refs(self, body):
        ids = {t: n for t, n in self.tex_ids}
        return [(o, ids[struct.unpack_from('<Q', body, o)[0]]) for o in range(0, len(body) - 7, 4)
                if struct.unpack_from('<Q', body, o)[0] in ids]

    # -- edits
    def add_texture(self, name, png):
        if any(n == name for _t, n in self.tex_ids):
            raise ValueError('texture %s exists' % name)
        like = next(t for t in self.textures if len(t.mips) >= 1)
        # the texture list and the GTNT go in step (entry i of the GTNT is texture i of the BNTX; a BNTX may hold one
        # more, unnamed in the GTNT, after them): the new texture goes in at the GTNT's length
        self.textures.insert(len(self.tex_ids), rgba8_texture(name, png, like))
        tid = fnv1(name)
        while any(t == tid for t, _n in self.tex_ids):
            tid = (tid + 1) & 0xFFFFFFFF
        self.tex_ids.append((tid, name))
        self.new_textures.append(name)
        return tid

    def clone_set(self, src, effect_name, set_name=None, textures=None):
        i = self.set_index(src)
        s = self.sets[i]
        body = bytearray(self.v[s['at']:s['at'] + s['size']])
        nm = (set_name or 'P_' + effect_name.title().replace('_', '')).encode()[:63]
        body[s['binary'] + 0x10:s['binary'] + 0x50] = nm + b'\0' * (64 - len(nm))
        struct.pack_into('<I', body, 0x0C, NONE)
        retarget = 0
        if textures:
            ids = {n: t for t, n in self.tex_ids}
            for old, new in textures.items():
                if old not in ids or new not in ids:
                    raise KeyError('texture %s -> %s: unknown name' % (old, new))
                a, b = struct.pack('<Q', ids[old]), struct.pack('<Q', ids[new])
                k = body.find(a)
                while k >= 0:
                    body[k:k + 8] = b
                    retarget += 1
                    k = body.find(a, k + 8)
        flags = self.entries[self.names.index(src.upper())][0] if src.upper() in self.names else 0x100
        self.new_sets.append((effect_name.upper(), flags, bytes(body), s['at']))
        return retarget

    # -- write
    def save(self, path):
        v = self.v
        es, gr = self.esta, self.grtf
        # emitter sets: the old ones, the clones after them, whole 4 KB steps
        sets = bytearray(v[es['at'] + es['child']:es['at'] + es['next']])
        base = es['at'] + es['child']
        last = self.sets[-1]['at'] - base
        for _n, _f, body, src in self.new_sets:
            # at the same place within a 4 KB page as the set it copies: its emitters' binaries keep their alignment
            # (they are 0x200-aligned in the file; a copy one 0x100 off stopped the game in the effect loader)
            at = len(sets) + (src - base - len(sets)) % PAGE
            sets += b'\0' * (at - len(sets))
            struct.pack_into('<I', sets, last + 0x0C, at - last)
            last = at
            sets += body
        # (whole 4 KB steps: the BNTX after it is a GPU memory pool; a texture 0xd80 off stopped the texture set-up)
        grow = _up(max(0, len(sets) - (es['next'] - es['child'])), PAGE)
        sets += b'\0' * (es['next'] - es['child'] + grow - len(sets))
        esta = bytearray(v[es['at']:es['at'] + es['child']])
        struct.pack_into('<III', esta, 4, es['size'] + grow, es['child'], es['next'] + grow)
        struct.pack_into('<I', esta, 0x1C, len(self.sets) + len(self.new_sets))
        # textures: GTNT entries, the BNTX written again
        g = self.gtnt
        ent = bytearray()
        for k, (tid, n) in enumerate(self.tex_ids):
            e = n.encode() + b'\0'
            size = 16 + _up(len(e) + 1, 8)             # (the name and at least two zero bytes, 8-aligned)
            ent += struct.pack('<QII', tid, size, len(e)) + e + b'\0' * (size - 16 - len(e))
        gtnt_end = g['at'] - gr['at'] + g['binary'] + len(ent)
        if gtnt_end > gr['binary']:
            raise ValueError('the texture names no longer fit before the BNTX (%x > %x)' % (gtnt_end, gr['binary']))
        grtf = bytearray(v[gr['at']:gr['at'] + gr['binary']])
        go = g['at'] - gr['at']
        grtf[go + g['binary']:] = ent + b'\0' * (gr['binary'] - go - g['binary'] - len(ent))
        struct.pack_into('<I', grtf, go + 4, g['binary'] + len(ent))
        struct.pack_into('<I', grtf, go + 12, _up(g['binary'] + len(ent), 0x10) + 0x20)
        if self.new_textures:
            b = bntx_write(self.bntx_name, self.textures)
        else:
            b = bytes(v[gr['at'] + gr['binary']:gr['at'] + gr['binary'] + gr['size']])
        need = _up(gr['binary'] + len(b), 0x10)
        gnext = gr['next'] + max(0, _up(need - gr['next'], PAGE))
        struct.pack_into('<II', grtf, 4, len(b), gr['child'])
        struct.pack_into('<I', grtf, 12, gnext)
        grtf += b + b'\0' * (gnext - gr['binary'] - len(b))
        rest = v[gr['at'] + gr['next']:]
        nv = bytearray(v[:es['at']]) + esta + sets + grtf + rest
        struct.pack_into('<I', nv, 0x1C, len(nv))
        # the effect list
        names = self.names + [t[0] for t in self.new_sets]
        entries = self.entries + [[t[1], len(self.sets) + k + 1, 0] for k, t in enumerate(self.new_sets)]
        head = bytearray(self.head)
        struct.pack_into('<I', head, 8, len(names))
        for e in entries:
            head += struct.pack('<IIQ', *e)
        for n in names:
            head += n.encode() + b'\0'
        if len(head) > self.vfx_at:
            raise ValueError('the effect names no longer fit before the VFXB')
        head += b'\0' * (self.vfx_at - len(head))
        with open(path, 'wb') as f:
            f.write(head + nv)


def check(path):
    e = Eff(path)
    print('%s: %d effects, %d emitter sets, %d texture ids, BNTX "%s" with %d textures' % (
        os.path.basename(path), len(e.names), len(e.sets), len(e.tex_ids), e.bntx_name, len(e.textures)))
    bad = [n for _t, n in e.tex_ids if n not in {t.name for t in e.textures}]
    if bad:
        print('  texture ids without a BNTX texture:', bad)
    if [n for _t, n in e.tex_ids] != [t.name for t in e.textures][:len(e.tex_ids)]:
        print('  the GTNT and the BNTX do not list the textures in the same order')
    return e


def main(argv):
    if len(argv) >= 2 and argv[0] in ('info', 'check'):
        e = check(argv[1])
        if argv[0] == 'info':
            for k, n in enumerate(e.names):
                i = e.entries[k][1] - 1
                s = e.sets[i] if 0 <= i < len(e.sets) else None
                print('  %-34s flags %08x  set %-32s' % (n, e.entries[k][0], s['name'] if s else '-'))
                if s:
                    for m in e.emitters(i):
                        body = e.v[m['at']:m['at'] + m['size']]
                        print('      %-22s textures %s' % (m['name'], ', '.join(sorted({t for _o, t in e.texture_refs(body)})) or '-'))
            print('  textures:', ', '.join('%s %dx%d' % (t.name, struct.unpack_from('<i', t.desc, 0x14)[0],
                                                          struct.unpack_from('<i', t.desc, 0x18)[0]) for t in e.textures))
        return 0
    if len(argv) >= 4 and argv[0] == 'build':
        e = Eff(argv[1])
        plan = json.load(open(argv[2]))
        here = os.path.dirname(os.path.abspath(argv[2]))
        for t in plan.get('textures', []):
            e.add_texture(t['name'], os.path.join(here, t['png']))
            print('texture %s added from %s' % (t['name'], t['png']))
        for s in plan.get('sets', []):
            n = e.clone_set(s['from'], s['name'], s.get('set_name'), s.get('textures'))
            print('effect %s: emitter set of %s cloned, %d texture references changed' % (s['name'], s['from'], n))
        e.save(argv[3])
        check(argv[3])
        return 0
    print(__doc__)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
