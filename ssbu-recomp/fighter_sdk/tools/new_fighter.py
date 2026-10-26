"""Start a NEW fighter as a copy of an existing one: one folder per fighter.

    python fighter_sdk\\tools\\new_fighter.py <name> --donor <fighter> [options]
    python fighter_sdk\\tools\\new_fighter.py <name> --build            (rebuild the DLL of an existing folder)

Creates <characters>\\<name>\\ (default characters folder: ..\\characters next to the work folder):
    fighter.json          the manifest the registry reads (name, donor, dll, kind auto, own_name)
    fighter\\<name>\\...    the donor's files under the new name, extracted from the archive on this PC:
    effect\\fighter\\<name>   the model description, every animation and the motion list, the two parameter files, the
    sound\\...              effect file, the computer player's parameters, the sound banks and the sequence file
    clone_files.json      every cloned file with its donor path hash and its new path hash (the archive stores hashes,
                          not names: a file whose name is unknown is written as 0x<name hash>.<ext>)
    src\\<name>\\acmd, status, generated
                          the donor's generated C++ script sources (move scripts, status functions, bindings) with the
                          fighter's name in the source macros and in the names of its own functions
    build.cmd             ONE command: builds src\\ into <name>.dll in the folder
    <name>.dll            the script DLL (built at the end unless --no-build; <name>.so on Linux, .dylib on macOS)
    README.txt            which file to replace to change what

Options:
    --donor <fighter>     the existing fighter to copy (required when creating)
    --characters <dir>    the characters folder
    --source-root <dir>   folder under recomp-study\\fighters with the generated sources (default work_s14); only READ
    --display-name <text> the fighter's own name text on the screens (manifest "display_name"; without it the donor's)
    --model donor|<fighter>[:cNN]
                          also put the WHOLE body model into the folder as the fighter's own files (mesh, materials,
                          skeleton, helper bones, every texture). "donor" = the donor's (everything then fits: its
                          animations, parameters and scripts were made for that skeleton; edit the textures / mesh
                          from there). Another fighter's model loads and draws too, but is moved by the donor's
                          animations and sized by the donor's parameters (wrong unless the two are of one build)
    --no-files            do not extract the donor's files (the fighter then uses the donor's through the fallback)
    --no-status           clone the move scripts only (no status sources)
    --no-build            do not build the DLL
    --force               write into an existing folder (files are overwritten; nothing is deleted)

What is copied is generated from the game or extracted from it: it stays under the characters folder and in
recomp-study work folders on this PC. This tool itself contains nothing of the game.
"""
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fighter_cc  # noqa: E402
import sdk_paths  # noqa: E402

STUDY = sdk_paths.WORK
SDK_ROOT = 'work_new'                 # recomp-study\fighters\work_new\sdk_generated: a frozen copy of the generated SDK headers

TOKEN = re.compile(r'"(?:\\.|[^"\\\n])*"|[A-Za-z_][A-Za-z0-9_]*')
PREFIXES = ('FS_OFF_', 'fs_fn_', 'fs_acmd_', 'fs_modified_')


def opt(args, key, default=None):
    if key in args:
        i = args.index(key)
        v = args[i + 1]
        del args[i:i + 2]
        return v
    return default


def flag(args, key):
    if key in args:
        args.remove(key)
        return True
    return False


# ---------------------------------------------------------------------------------------------- files from the archive
def clone_files(name, donor, out_root, dry=False):
    """The donor's files under the new name. The list of paths is what the engine asks for with a fighter's name
    (seen in the registry's log for the test fighters): three single files, three folders, the sound files."""
    sys.path.insert(0, os.path.dirname(sdk_paths.tree_file(os.path.join('tests', 'n5_clone_files.py'))))
    sys.path.insert(0, sdk_paths.nro_tools())
    import n5_clone_files as cf
    import arc_extract
    arc = arc_extract.Arc()
    files = ['fighter/%s/model/body/c00/model.numdlb', 'fighter/%s/param/vl.prc', 'fighter/%s/param/expression_vl.prc',
             'sound/sequence/fighter/%s.sqb']
    for bank in ('sound/bank/fighter/se_%s_c00', 'sound/bank/fighter_voice/vc_%s_c00', 'sound/bank/fighter_voice/vc_%s_cheer_c00'):
        files += [bank + '.' + e for e in ('nus3audio', 'nus3bank', 'tonelabel')]
    folders = ['fighter/%s/motion/body/c00', 'effect/fighter/%s', 'fighter/%s/ai/param']
    ext_names = {cf.hash40(e): e for e in cf.EXTS}
    want = {}
    for p in folders:
        want[cf.hash40(p % donor)] = p
        want[cf.hash40(p % donor + '/')] = p
    by_parent = {}
    for i in range(arc.n_path):
        (ph, _), (xh, _), (parent, _), (nm, _) = arc.file_path(i)
        if parent in want:
            by_parent.setdefault(want[parent], []).append((i, ph, xh, nm))
    rows, missing, n_bytes, n_bad = [], [], 0, 0
    unread = []

    def put(rel, data):
        dst = os.path.join(out_root, rel.replace('/', os.sep))
        if not dry:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as f:
                f.write(data)

    # the fighter's pictures (portrait, stock icon, select-screen images): a file of the package keeps its own bytes,
    # a missing one is the donor's; either way it gets a row, which is what lets the loose-file layer serve it (2026-10-21:
    # without these rows a new fighter's own HUD portrait was "not in the archive and no donor file: NOT served")
    for n in range(8):
        tpl = 'ui/replace/chara/chara_%d/chara_%d_' % (n, n) + '%s_00.bntx'
        new, old = tpl % name, tpl % donor
        try:
            arc.get(old)
        except Exception:                     # noqa: BLE001  a DLC donor's pictures are under ui/replace_patch/
            old = old.replace('ui/replace/', 'ui/replace_patch/')
        own = os.path.join(out_root, new.replace('/', os.sep))
        if os.path.exists(own):
            rows.append({'path': new, 'donor': old, 'donor_hash': '0x%x' % cf.hash40(old), 'new_hash': '0x%x' % cf.hash40(new), 'size': os.path.getsize(own)})
        else:
            files.append(tpl)
    for p in files:
        new, old = p % name, p % donor
        data = None
        for region in range(0, 15):          # a regional file has one data row per region: the first that reads
            try:
                data = arc.get(old, region)
                break
            except Exception:
                continue
        if data is None:
            missing.append(new)
            continue
        put(new, data)
        rows.append({'path': new, 'donor': old, 'donor_hash': '0x%x' % cf.hash40(old), 'new_hash': '0x%x' % cf.hash40(new), 'size': len(data)})
        n_bytes += len(data)
    per_folder = {}
    for p in folders:
        new, old = p % name, p % donor
        kids = by_parent.get(p, [])
        if not kids:
            missing.append(new + '/')
            continue
        for (i, ph, xh, nm) in kids:
            if cf.child_hash(old, nm) != ph:
                n_bad += 1
                continue
            fn = '0x%010x.%s' % (nm, ext_names.get(xh, 'bin'))
            data = None
            for region in range(0, 15):      # N11: a folder file can be regional too (first region that reads)
                try:
                    data = arc.read(arc.file_record(i, region))
                    break
                except Exception:            # noqa: BLE001
                    continue
            if data is None:                 # N11: one unreadable record no longer stops the tool: the donor's stays in use
                rec = arc.file_record(i)
                unread.append('%s/%s (info flags %x, data flags %x, %d -> %d bytes)' % (new, fn, rec['info_flags'], rec['data_flags'], rec['comp_size'], rec['decomp_size']))
                continue
            put(new + '/' + fn, data)
            rows.append({'path': new + '/' + fn, 'donor_hash': '0x%x' % ph, 'new_hash': '0x%x' % cf.child_hash(new, nm), 'name_hash': '0x%x' % nm, 'size': len(data)})
            n_bytes += len(data)
            per_folder[new] = per_folder.get(new, 0) + 1
    if not dry:
        with open(os.path.join(out_root, 'clone_files.json'), 'w') as f:
            json.dump({'name': name, 'donor': donor, 'files': rows, 'not_cloned': missing}, f, indent=1)
    print('files: %d cloned from the archive (%d bytes): %s' % (len(rows), n_bytes, ', '.join('%s %d' % kv for kv in sorted(per_folder.items()))))
    if missing:
        print('files: the donor has no such file / folder (not cloned): ' + ', '.join(missing))
    for u in unread:
        print('files: NOT cloned (the archive record could not be read; the donor\'s file stays in use): ' + u)
    if n_bad:
        print('files: %d skipped because the computed path hash did not match the archive\'s' % n_bad)
    return len(rows)


# ---------------------------------------------------------------------------------------------- sources
def rename_text(text, donor, name, own):
    """The fighter's name in the source macros, and in the names of the fighter's own functions (the names the binding
    files give them). Text inside string literals is never touched (those are names of the game's resources)."""
    def tok(m):
        t = m.group(0)
        if t[0] == '"':
            return t
        if t in own:
            return own[t]
        for pre in PREFIXES:
            if t.startswith(pre) and t[len(pre):] in own:
                return pre + own[t[len(pre):]]
        return t
    text = TOKEN.sub(tok, text)
    return re.sub(r'\b(ACMD|ACMD_BIND|L2C_BIND|L2C_(?:VOID_)?FUNCTION\d?)\(\s*%s\s*,' % re.escape(donor), lambda m: '%s(%s,' % (m.group(1), name), text)


def clone_sources(name, donor, sroot, src_dir, with_status):
    src = os.path.join(STUDY, 'fighters', sroot, donor)
    if not os.path.isdir(src):
        print('sources: no generated sources for the donor: %s' % src)
        return 0
    own = {}
    for inc in ('bindings.inc', 'status_bindings.inc'):
        p = os.path.join(src, 'generated', inc)
        if os.path.exists(p):
            for n in re.findall(r'_BIND\(\s*\w+\s*,\s*(\w+)\s*,', open(p, errors='replace').read()):
                if n.startswith(donor + '_'):
                    own[n] = name + n[len(donor):]
    p = os.path.join(src, 'generated', 'status_local.h')        # functions without source have a name there too
    if os.path.exists(p):
        for t in TOKEN.findall(open(p, errors='replace').read()):
            for pre in ('',) + PREFIXES:
                if t.startswith(pre + donor + '_'):
                    n = t[len(pre):]
                    own[n] = name + n[len(donor):]
    dst = os.path.join(src_dir, name)
    count = renamed = 0
    for sub in ('acmd', 'status', 'generated'):
        if sub == 'status' and not with_status:
            continue
        s = os.path.join(src, sub)
        if not os.path.isdir(s):
            continue
        for root, _, fs in os.walk(s):
            for fn in fs:
                if not with_status and fn.startswith('status_'):
                    continue
                rel = os.path.relpath(os.path.join(root, fn), src)
                text = open(os.path.join(root, fn), 'r', errors='replace', newline='').read()
                new = rename_text(text, donor, name, own) if fn.endswith(('.cpp', '.h', '.inc')) else text
                renamed += new != text
                os.makedirs(os.path.dirname(os.path.join(dst, rel)), exist_ok=True)
                with open(os.path.join(dst, rel), 'w', newline='') as f:
                    f.write(new)
                count += 1
    with open(os.path.join(dst, 'clone.json'), 'w') as f:
        json.dump({'name': name, 'donor': donor, 'source_root': sroot, 'files': count, 'module': 'lua2cpp_' + name, 'status_sources': with_status,
                   'own_function_names_renamed': len(own)}, f, indent=1)
    sdk_dst = os.path.join(STUDY, 'fighters', SDK_ROOT, 'sdk_generated')
    if not os.path.isdir(sdk_dst):
        shutil.copytree(os.path.join(STUDY, 'fighters', sroot, 'sdk_generated'), sdk_dst)
        with open(os.path.join(STUDY, 'fighters', SDK_ROOT, 'sdk_source.txt'), 'w') as f:
            f.write('copied from fighters\\%s\\sdk_generated by new_fighter.py; delete this folder to copy it again\n' % sroot)
    print('sources: %d files from fighters\\%s\\%s to %s (%d changed by the renaming; %d own function names renamed); status sources: %s'
          % (count, sroot, donor, dst, renamed, len(own), 'yes' if with_status else 'no'))
    return count


def row_table():
    """N12: name -> type, offset of the parameter row (a file of ssbu-recomp's mods, staged without mods/)"""
    return sdk_paths.tree_file(os.path.join('mods', 'mods_n12_src', 'fighter_param_row.txt'), os.path.join('mods_n12_src', 'fighter_param_row.txt'))


def donor_article_count(donor):
    """N12: the number of rows of the donor's article list, read from the game image (the record returned by the
    engine's per-kind description lookup 0x710064b730: record + 8 -> { rows, count }; see plan\\phases\\N12)."""
    import struct
    sys.path.insert(0, os.path.dirname(sdk_paths.tree_file(os.path.join('tools', 'lifter', 'recomp.py'), 'recomp.py')))
    import recomp      # noqa: E402  (the game image on this PC; nothing of it is written anywhere)
    import luaconst    # noqa: E402
    kind = dict((n, v) for n, v in luaconst.table_names() if n).get('FIGHTER_KIND_' + donor.upper())
    img = recomp.IMG
    phoff, = struct.unpack_from('<Q', img, 0x20)
    phnum, = struct.unpack_from('<H', img, 0x38)
    segs = []
    for i in range(phnum):
        t, _, off, va, _, fs, ms, _ = struct.unpack_from('<IIQQQQQQ', img, phoff + 56 * i)
        if t == 1:
            segs.append((va + recomp.BASE, off, fs, ms))

    def u(a, n=4):
        for va, off, fs, ms in segs:
            if va <= a < va + fs:
                return int.from_bytes(img[off + a - va: off + a - va + n], 'little')
        return 0

    def rel(a):        # R_AARCH64_RELATIVE entry for the word at a (the pointers of the data are relocations)
        key = struct.pack('<QQ', a - recomp.BASE, 1027)
        i = img.find(key)
        return struct.unpack_from('<q', img, i + 16)[0] + recomp.BASE if i >= 0 else 0

    tab = 0x71044bc18c
    pc = tab + struct.unpack('<i', struct.pack('<I', u(tab + 4 * kind)))[0]
    w1, w2 = u(pc), u(pc + 4)
    if w1 == 0xd65f03c0:
        rec = 0x7104fe35d0                       # the lookup's preset answer (kind 0)
    else:
        imm = recomp.sext((((w1 >> 5) & 0x7ffff) << 2) | ((w1 >> 29) & 3), 21)
        rec = (pc & ~0xfff) + (imm << 12) + ((w2 >> 10) & 0xfff)
    hdr = rel(rec + 8)
    return u(hdr + 8, 8) if hdr else 0


def manifest_products(name, folder):
    """N12: what the manifest asks of the build. (1) "parameter_row": {"set": {...}}: every name must be in the row
    table (the registry writes them at run time). (2) "articles": [{name, template, max}]: the build product
    src\\<name>\\generated\\fs_articles.h with FIGHTER_<NAME>_GENERATE_ARTICLE_<ARTICLE> = donor's count + index."""
    try:
        man = json.load(open(os.path.join(folder, 'fighter.json')))
    except (OSError, ValueError):
        return 0
    row = man.get('parameter_row', {}).get('set', {}) if isinstance(man.get('parameter_row'), dict) else {}
    if row:
        with open(row_table()) as f:
            known = set(l.split()[0] for l in f if l.strip() and not l.startswith('#'))
        bad = [k for k in row if k not in known]
        print('parameter_row.set: %d values, %s' % (len(row), ('UNKNOWN names (not in %s): %s' % (row_table(), ' '.join(bad))) if bad else 'all names in the row table'))
        if bad:
            return 1
    arts = man.get('articles')
    if isinstance(arts, list) and arts:
        base = man.get('articles_base')
        if base is None:
            base = donor_article_count(man['donor'])
        text = '// build product (new_fighter.py, N12): generate-article ids of the articles of fighter.json\n#pragma once\n#include <stdint.h>\n'
        for i, a in enumerate(arts):
            text += 'static const int32_t FIGHTER_%s_GENERATE_ARTICLE_%s = %d;      // template %s, max %s\n' % (
                name.upper(), a['name'].upper(), base + i, a.get('template'), a.get('max', 1))
        gen = os.path.join(folder, 'src', name, 'generated')
        os.makedirs(gen, exist_ok=True)
        path = os.path.join(gen, 'fs_articles.h')
        if not os.path.exists(path) or open(path).read() != text:
            with open(path, 'w', newline='\n') as f:
                f.write(text)
        print('articles: %d own (ids from %d, after the donor\'s %d): %s' % (len(arts), base, base, path))
    return 0


def build(name, folder):
    if manifest_products(name, folder):
        return 1
    src_dir = os.path.join(folder, 'src')
    meta = {}
    cj = os.path.join(src_dir, name, 'clone.json')
    if os.path.exists(cj):
        meta = json.load(open(cj))
    env = dict(os.environ)
    env['SSBU_FIGHTERS_ROOT'] = os.path.relpath(src_dir, os.path.join(STUDY, 'fighters'))
    env['SSBU_FIGHTER_BIN'] = os.path.relpath(folder, STUDY)
    # B2: NEW_FIGHTER_SDK=<folder under fighters\> picks another SDK (e.g. "sdk_generated", the one the INSTALLED
    # sources were built with, for a folder cloned with --source-root .); unset = the frozen copy as before
    env['SSBU_FIGHTER_SDK'] = os.environ.get('NEW_FIGHTER_SDK') or (SDK_ROOT + '/sdk_generated')
    if meta.get('status_sources', True):
        env['SSBU_FIGHTER_STATUS'] = '1'
    else:
        env.pop('SSBU_FIGHTER_STATUS', None)
    rc = subprocess.call([sys.executable, os.path.join(HERE, 'build_fighter.py'), name], cwd=STUDY, env=env)
    dll = os.path.join(folder, fighter_cc.library(name))
    print('build: exit code %d; %s %s' % (rc, dll, ('%d bytes' % os.path.getsize(dll)) if os.path.exists(dll) else 'MISSING'))
    return rc


README = """{name}: a fighter folder (made by fighter_sdk\\tools\\new_fighter.py from the donor "{donor}")

Everything here was copied from the donor. Replace a piece to make the fighter differ; what you do not replace
stays the donor's. A file is replaced by a file of the SAME NAME in the engine's own format.

  fighter.json                       the manifest: name, donor, dll
  build.cmd                          rebuilds {name}.dll from src\\ (run it after every change in src\\)

  MOVES (damage, angle, hitbox sizes, timing, effects and sounds per frame)
      src\\{name}\\acmd\\game.cpp       the hit data and timing of every move
      src\\{name}\\acmd\\effect.cpp     which effect at which frame
      src\\{name}\\acmd\\sound.cpp      which sound at which frame
      src\\{name}\\acmd\\expression.cpp rumble, slope behaviour, flashes
  STATE MACHINE (what a special does, what cancels into what)
      src\\{name}\\status\\*.cpp        one file per status; a function that exists only here is a new function
  ANIMATIONS
      fighter\\{name}\\motion\\body\\c00\\  one animation file per motion + the motion list (names are stored as hashes:
                                      the file 0x<hash of the motion file's name>.nuanmb is that motion's file)
  MODEL
      fighter\\{name}\\model\\body\\c00\\   the model description; put the mesh, material, skeleton and texture files it
                                      names beside it to replace the donor's
  PARAMETERS
      fighter\\{name}\\param\\vl.prc, expression_vl.prc   the fighter's own values (the donor's layout)
  EFFECTS
      effect\\fighter\\{name}\\          the effect container
  SOUNDS
      sound\\bank\\fighter\\, sound\\bank\\fighter_voice\\   the sound banks;  sound\\sequence\\fighter\\  the sequence file
  COMPUTER PLAYER
      fighter\\{name}\\ai\\param\\        its parameter files (its code is the donor's)
  PORTRAIT AND NAME
      see docs\\NEW_FIGHTER.md (what is read from this folder today and what is not)

Run (from recomp-study):   set SSBU_CHARACTERS=..\\characters   and start the game; docs\\NEW_FIGHTER.md has the test command.
The copied files and sources come from the game on this PC: do not publish this folder.
"""


def main():
    args = sys.argv[1:]
    donor = opt(args, '--donor')
    chars = opt(args, '--characters', os.path.join(STUDY, '..', 'characters'))
    sroot = opt(args, '--source-root', 'work_s14')
    display = opt(args, '--display-name')
    model = opt(args, '--model')
    no_files, no_status, no_build = flag(args, '--no-files'), flag(args, '--no-status'), flag(args, '--no-build')
    force, only_build = flag(args, '--force'), flag(args, '--build')
    if len(args) != 1 or args[0].startswith('-'):
        print(__doc__)
        return 2
    name = args[0]
    folder = os.path.normpath(os.path.join(chars, name))
    if only_build:
        return build(name, folder)
    if not donor:
        print('--donor <fighter> is required')
        return 2
    if not re.fullmatch(r'[a-z][a-z0-9]{1,15}', name):
        print('the name must be 2..16 lower-case letters / digits and start with a letter')
        return 2
    if os.path.isdir(os.path.join(STUDY, 'fighters', sroot, name)) or name == donor:
        print('%s is an existing fighter\'s name' % name)
        return 2
    if os.path.exists(folder) and not force:
        print('%s exists already: not touched (--force writes into it)' % folder)
        return 1
    if len(donor) > len(name):
        print('the name must be at least as long as the donor\'s (%d characters): a path without a row of its own falls back to the donor\'s by '
              'shortening the text in place, and that cannot lengthen it' % len(donor))
        return 2
    os.makedirs(folder, exist_ok=True)
    manifest = {'name': name, 'kind': 'auto', 'donor': donor, 'dll': name + '.dll', 'own_name': True,
                'roster': {'id': 'ui_chara_' + name, 'order': 'last'},
                'comment': 'made by fighter_sdk\\tools\\new_fighter.py as a copy of the donor'}
    if display:         # the fighter's own name text on the screens (without it: the donor's name and pictures)
        manifest['display_name'] = display
    with open(os.path.join(folder, 'fighter.json'), 'w') as f:
        json.dump(manifest, f, indent=2)
    with open(os.path.join(folder, 'README.txt'), 'w') as f:
        f.write(README.format(name=name, donor=donor))
    with open(os.path.join(folder, 'build.cmd'), 'w') as f:
        f.write('@echo off\r\nrem rebuilds %s.dll from src\\ (fighter_sdk\\tools\\new_fighter.py)\r\npython "%s" %s --characters "%s" --build\r\n'
                % (name, os.path.join(HERE, 'new_fighter.py'), name, os.path.abspath(chars)))
    print('%s: folder %s, manifest written (donor %s)' % (name, folder, donor))
    if not no_files:
        clone_files(name, donor, folder)
    if model:
        # N11: the WHOLE body model as files of the fighter's own (mesh, materials, skeleton, helper bones, textures:
        # tests\n10_model.py swap), from the donor ("donor", the consistent choice: its animations and parameters fit
        # that skeleton) or from another fighter / costume ("<fighter>" or "<fighter>:cNN")
        src, _, cc = (donor if model == 'donor' else model).partition(':')
        rc = subprocess.call([sys.executable, sdk_paths.tree_file(os.path.join('tests', 'n10_model.py')), 'swap', chars, name, src, cc or 'c00'], cwd=STUDY)
        shutil.rmtree(os.path.join(folder, '_model_before'), ignore_errors=True)     # the runtime would scan it as fighter files
        print('model: the whole body model of %s (%s) is in the folder (exit code %d)%s' % (src, cc or 'c00', rc,
              '' if src == donor else '; NOTE: animations, parameters and scripts are the donor\'s: they fit only a skeleton of the donor\'s build'))
    n = clone_sources(name, donor, sroot, os.path.join(folder, 'src'), not no_status)
    if n and not no_build:
        return build(name, folder)
    return 0


if __name__ == '__main__':
    sys.exit(main())
