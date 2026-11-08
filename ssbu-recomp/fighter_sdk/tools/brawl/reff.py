"""The Wii predecessor's particle effects: an effect archive's EFLS list, REFF emitters and REFT textures.

First stage of the converter to the native effect container (.eff): it reads the old files and writes what a
native effect is rebuilt from - each texture as PNG and as a native texture (.nutexb, RGBA8), and a JSON report of
the effect list (index -> emitter set, model archive) and of every emitter (its size, the textures it names).
Writing the native container (template emitter sets with these textures) is the next stage.

Layouts as publicly documented by the community (NW4R effect files, as read by BrawlBox / BrawlCrate and described
on the Custom Brawl Modding wiki); nothing here is copied from those programs. Checked on the files on this PC.

  file header 0x10 : magic, byte-order mark FEFF, version (7), file size, header size 0x10, block count 1
  block       0x10 : magic again, block size; + 0x08 offset of the entry table (from + 0x08), + 0x14 name length,
                     + 0x18 name (the archive's own: e.g. "ganon")
  entry table      : u32 table size, u16 count, u16 padding; per entry u16 name length, the name (with its 0),
                     u32 offset of the data (from the table), u32 data size - entries are not aligned
  REFT texture     : (the entry's size is the image's) 0x20 header (u32 0, u16 width, u16 height, u32 image size, u8 GX format, u8 palette format,
                     u16 palette colours, u32 palette size, u8 mip levels, filters, f32 LOD bias), then the image
                     in the GX block layout (the same as a TEX0's), then the palette
  EFLS             : magic, u16 entry count, u16 model archive count, 8 bytes; per entry 0x10 bytes: u16 model
                     archive (0xFFFF none), u16 model, u32 offset of the emitter set's name, 8 bytes; then names

    python -I reff.py list <archive.pac>
    python -I reff.py colours <archive.pac>
        each emitter-drawn effect's colour (r, g, b in 0-1, set_colour), for vfxb.py plans ("color")
    python -I reff.py export <archive.pac> <out folder> [--effects 0x0e,0x12]
        archive.pac: a fighter's main archive (its effect archive "ef_*" inside) or an effect archive itself.
        --effects: only the emitter sets of these effect-list indices (the low 16 bits of an effect id such as
        0x17d000e), their child emitters and the textures they name.

Run with  python -I  when the input is a downloaded package.
"""
import json
import os
import re
import struct
import sys

if __package__ in (None, ''):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from brawl import arc, nutexb, tex0          # noqa: E402
else:
    from . import arc, nutexb, tex0


def _cstr(d, p):
    e = d.index(b'\0', p)
    return d[p:e].decode('ascii', 'replace')


def nw4r_table(data, magic, extra=0):
    """-> (archive name, [(entry name, data bytes)]) of a REFF / REFT file. extra: bytes kept past each entry's
    stated size (a REFT entry's size is its image's: its 0x20 header and palette lie around it)."""
    if data[:4] != magic or data[0x10:0x14] != magic or data[4:6] != b'\xfe\xff':
        raise ValueError('not a %s file' % magic.decode())
    table_rel, = struct.unpack_from('>I', data, 0x18)
    name_len, = struct.unpack_from('>H', data, 0x24)
    name = data[0x28:0x28 + name_len].split(b'\0')[0].decode('ascii', 'replace')
    t = 0x18 + table_rel
    size, count = struct.unpack_from('>IH', data, t)
    if not (0 < count < 4096 and t + size <= len(data)):
        raise ValueError('implausible entry table (%d entries, %#x bytes)' % (count, size))
    out, p = [], t + 8
    for _ in range(count):
        n, = struct.unpack_from('>H', data, p)
        ename = data[p + 2:p + 2 + n].split(b'\0')[0].decode('ascii', 'replace')
        off, sz = struct.unpack_from('>II', data, p + 2 + n)
        if t + off + sz > len(data):
            raise ValueError('entry %s beyond the end of the file' % ename)
        out.append((ename, data[t + off:t + off + sz + extra]))
        p += 2 + n + 8
    return name, out


class _Image(tex0.Tex0):
    """A REFT image, decoded with the TEX0 block decoder (same GX layouts)."""
    def __init__(self, d):   # noqa (no TEX0 header to read)
        (_z, self.width, self.height, self.image_size, self.format, self.palette_format, self.colours,
         self.palette_size, self.levels) = struct.unpack_from('>IHHIBBHIB', d, 0)
        if self.format not in tex0.FORMATS:
            raise ValueError('unknown texture format %d' % self.format)
        self.data, self.off, self.header_len = d, 0, 0x20
        self.size = 0x20 + self.image_size

    def palette(self):
        if self.format not in (8, 9, 10):
            return None
        conv = {0: tex0._ia8, 1: tex0._565, 2: tex0._5a3}[self.palette_format]
        vals = struct.unpack_from('>%dH' % self.colours, self.data, 0x20 + self.image_size)
        return [conv(v) for v in vals]


def textures(reft):
    _n, entries = nw4r_table(reft, b'REFT', extra=0x20 + 0x200)   # header + the largest palette (CI8: 256 colours)
    out = []
    for name, d in entries:
        im = _Image(d)
        w, h, rgba = im.decode(palette=im.palette())
        out.append({'name': name, 'width': w, 'height': h, 'format': tex0.FORMATS[im.format], 'levels': im.levels,
                    'rgba': rgba})
    return out


def effect_list(efls):
    if efls[:4] != b'EFLS':
        raise ValueError('not an effect list')
    count, models = struct.unpack_from('>HH', efls, 4)
    out = []
    for i in range(count):
        arch, model, name_off = struct.unpack_from('>HHI', efls, 0x10 + i * 0x10)
        out.append({'index': i, 'emitter_set': _cstr(efls, name_off) if name_off else None,
                    'model_archive': None if arch == 0xFFFF else arch, 'model': model})
    return out, models


def emitters(reff, texture_names):
    """Each emitter: name, size, the textures it names (found by name in its data), and its children: the emitters
    listed after an emitter set ("Ptc..." names) up to the next set."""
    _n, entries = nw4r_table(reff, b'REFF')
    out, current = [], None
    for name, d in entries:
        tex = sorted({t for t in texture_names if t.encode() + b'\0' in d})
        # textures of the base game's effect archives (the fighter's own, the common one) named by an emitter
        ext = sorted({m.decode() for m in re.findall(rb'(ptc_[A-Za-z0-9_]+)\0', d)} - set(tex))
        e = {'name': name, 'size': len(d), 'textures': tex, 'external_textures': ext}
        if name.startswith('Ptc'):
            current = e
            e['children'] = []
        elif current is not None:
            current['children'].append(name)
        out.append(e)
    return out


def effect_archive(path):
    """-> (name, EFLS, REFF, REFT bytes, model archives) from a fighter archive or an effect archive."""
    a = arc.load(path)
    for cand in [a] + [arc.Arc(e.data) for e in a.entries if e.data[:4] == b'ARC\0']:
        kinds = [e.data[:4] for e in cand.entries]
        if b'EFLS' in kinds and b'REFF' in kinds:
            get = lambda m: next(e.data for e in cand.entries if e.data[:4] == m)
            models = [e.data for e in cand.entries if e.data[:4] == b'bres' and e.type == 2]
            return cand.name, get(b'EFLS'), get(b'REFF'), get(b'REFT') if b'REFT' in kinds else b'', models
    raise ValueError('no effect archive (EFLS + REFF) in %s' % path)


def report(path, only=None):
    name, efls, reff, reft, models = effect_archive(path)
    texs = textures(reft) if reft else []
    lst, nmodels = effect_list(efls)
    ems = emitters(reff, [t['name'] for t in texs])
    by = {e['name']: e for e in ems}
    if only is not None:
        lst = [x for x in lst if x['index'] in only]
    for x in lst:
        s = by.get(x['emitter_set'])
        names = [x['emitter_set']] + (s['children'] if s else []) if x['emitter_set'] else []
        x['emitters'] = [{'name': n, 'size': by[n]['size'], 'textures': by[n]['textures'],
                          'external_textures': by[n]['external_textures']} for n in names if n in by]
        x['external_textures'] = sorted({t for n in names if n in by for t in by[n]['external_textures']})
        x['textures'] = sorted({t for n in names if n in by for t in by[n]['textures']})
    return {'archive': name, 'model_archives': nmodels, 'effects': lst,
            'textures': [{k: t[k] for k in ('name', 'width', 'height', 'format', 'levels')} for t in texs]}, texs


# An emitter's data: u32 0, u32 size of its emitter description (0x140 here), the description, then the particle
# parameters: u32 their size, four RGBA8 colours (colour 1 primary / secondary, colour 2 primary / secondary), f32
# size x, y, scale x, y ... (the particle part as the community documents it; checked on Phantom's file: its dark
# portal aura is black / 40 00 88, the red aura ff 00 00, the yellow sparks e0 d7 11).
def emitter_colours(d):
    """-> (colour 1 primary, colour 1 secondary, size x, size y) of one emitter's data (colours as RGBA tuples)."""
    esize, = struct.unpack_from('>I', d, 4)
    p = 8 + esize
    c = [tuple(d[p + 4 + 4 * i:p + 8 + 4 * i]) for i in range(2)]
    sx, sy = struct.unpack_from('>2f', d, p + 20)
    return c[0], c[1], sx, sy


def set_colour(datas):
    """The colour an emitter set draws in, from its emitters' data: per emitter the mean of its colour 1 primary and
    secondary (the texture's light and dark parts), emitters drawn only in greys (containers, white flashes) left
    out; -> (r, g, b) in 0-1, or None when every emitter is grey."""
    acc, n = [0.0, 0.0, 0.0], 0
    for d in datas:
        pri, sec, _sx, _sy = emitter_colours(d)
        if pri[0] == pri[1] == pri[2] and sec[0] == sec[1] == sec[2]:
            continue
        for k in range(3):
            acc[k] += (pri[k] + sec[k]) / 2.0 / 255.0
        n += 1
    return [round(a / n, 3) for a in acc] if n else None


def colours(path):
    """-> {effect-list index: (r, g, b)} for the effects drawn by emitters (not the model ones)."""
    _name, efls, reff, _reft, _models = effect_archive(path)
    lst, _n = effect_list(efls)
    _a, entries = nw4r_table(reff, b'REFF')
    data = dict(entries)
    ems = emitters(reff, [])
    by = {e['name']: e for e in ems}
    out = {}
    for x in lst:
        s = by.get(x['emitter_set'])
        if not s:
            continue
        c = set_colour([data[n] for n in [s['name']] + s['children'] if n in data])
        if c:
            out[x['index']] = c
    return out


def _pow2(n):
    p = 16
    while p < n:
        p *= 2
    return p


def export(path, out_dir, only=None):
    rep, texs = report(path, only)
    os.makedirs(out_dir, exist_ok=True)
    want = None if only is None else {t for x in rep['effects'] for t in x['textures']}
    for t in texs:
        if want is not None and t['name'] not in want:
            continue
        w, h, rgba = t['width'], t['height'], t['rgba']
        tex0.write_png(os.path.join(out_dir, t['name'] + '.png'), w, h, bytes(rgba))
        nw, nh = _pow2(w), _pow2(h)
        if (nw, nh) != (w, h):
            rgba = nutexb.scale_nearest(rgba, w, h, nw, nh)
        with open(os.path.join(out_dir, t['name'] + '.nutexb'), 'wb') as f:
            f.write(nutexb.build(t['name'], nw, nh, bytes(rgba), srgb=False))
    with open(os.path.join(out_dir, 'effects.json'), 'w', encoding='utf-8') as f:
        json.dump(rep, f, indent=1)
    return rep


def main(argv):
    if len(argv) >= 2 and argv[0] == 'list':
        rep, _t = report(argv[1])
        print('effect archive %s: %d effects, %d textures, %d model archives' % (
            rep['archive'], len(rep['effects']), len(rep['textures']), rep['model_archives']))
        for x in rep['effects']:
            print('  %#06x %-36s model archive %-4s textures %s%s' % (
                x['index'], x['emitter_set'] or '-', x['model_archive'], ','.join(x['textures']) or '-',
                ('  (base game: %s)' % ','.join(x['external_textures'])) if x['external_textures'] else ''))
        for t in rep['textures']:
            print('  texture %-24s %4dx%-4d %s' % (t['name'], t['width'], t['height'], t['format']))
        return 0
    if len(argv) >= 2 and argv[0] == 'colours':
        print(json.dumps({'%#06x' % k: v for k, v in sorted(colours(argv[1]).items())}, indent=1))
        return 0
    if len(argv) >= 3 and argv[0] == 'export':
        only = None
        if '--effects' in argv:
            only = {int(v, 0) & 0xFFFF for v in argv[argv.index('--effects') + 1].split(',')}
        rep = export(argv[1], argv[2], only)
        print('%d effects, textures written to %s' % (len(rep['effects']), argv[2]))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
