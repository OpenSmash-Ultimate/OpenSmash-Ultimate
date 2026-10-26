"""Where the fighter SDK's tools find what they read, and where they put what they make.

    SDK    fighter_sdk/ (include/, src/, these tools)
    TREE   the folder fighter_sdk/ is in: ssbu-recomp/ in the repository, build/work in the layout
           scripts/setup.py stages. Its own files (tests/, the mods, the NRO tools) are read from there.
    WORK   where the generated sources and the builds go (fighters/, obj_fighters/, fighters_bin/,
           tests/logs/): SSBU_FIGHTER_WORK, else TREE, as before
    REPO   the repository: the first folder above the tools with tools/cc.py and tools/isa.py, or the
           one SSBU_REPO names; None when neither is found

Nothing the tools read from the game, or from public projects about it, is part of the SDK. README.md
says what each tool reads and where it is looked for. A missing input stops the tool with Missing,
which says what was looked for and where."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(HERE)
INCLUDE = os.path.join(SDK, 'include')
SRC = os.path.join(SDK, 'src')
TREE = os.path.dirname(SDK)
VERSIONS = ('13.0.4', '13.0.5')     # the game builds a converted dump can be (scripts/setup.py's binaries/<version>)


class Missing(SystemExit):
    """An input that is not there: the message says which, where it was looked for, and what to do."""


def find_repo(start=HERE, env=None):
    env = os.environ if env is None else env
    if env.get('SSBU_REPO'):
        return os.path.abspath(env['SSBU_REPO'])
    d = os.path.abspath(start)
    while True:
        if all(os.path.isfile(os.path.join(d, 'tools', f)) for f in ('cc.py', 'isa.py')):
            return d
        up = os.path.dirname(d)
        if up == d:
            return None
        d = up


def work_dir(env=None):
    env = os.environ if env is None else env
    return os.path.abspath(env['SSBU_FIGHTER_WORK']) if env.get('SSBU_FIGHTER_WORK') else TREE


REPO = find_repo()
WORK = work_dir()


def missing(what, tried, hint=''):
    lines = [what + ' was not found. Looked for:'] + ['  ' + t for t in tried]
    if hint:
        lines.append(hint)
    lines.append('(fighter_sdk/README.md, "Reference data", says what the tools read and where it goes)')
    return Missing('\n'.join(lines))


def first(what, candidates, hint=''):
    """The first of the candidate paths that exists (None entries are skipped), else Missing."""
    tried = [c for c in candidates if c]
    for c in tried:
        if os.path.exists(c):
            return c
    raise missing(what, tried, hint)


def main_elf(given=None, env=None, work=None):
    """The game's main program as an ELF (scripts/setup.py's step convert makes it from your dump):
    the path given, else RECOMP_BIN/main.elf (what setup.py sets), else ../binaries/<version>/main.elf
    next to the work folder."""
    env = os.environ if env is None else env
    work = WORK if work is None else work
    if given:
        return first('main.elf', [given])
    cands = [os.path.join(env['RECOMP_BIN'], 'main.elf') if env.get('RECOMP_BIN') else None]
    cands += [os.path.join(os.path.dirname(work), 'binaries', v, 'main.elf') for v in VERSIONS]
    return first('main.elf (the game\'s main program, converted from your dump)', cands,
                 'Run scripts/setup.py (step convert), or name the folder holding main.elf in RECOMP_BIN.')


def program(name, variable, work=None):
    """A game program built in the work folder (variable: the environment variable that names another)."""
    work = WORK if work is None else work
    return first('The game program %s' % name, [os.path.join(work, name)],
                 'Build it with scripts/setup.py, or name another one in %s.' % variable)


def reference(*parts, env=None, work=None, repo=None):
    """A file of a public project the tools read (reference/skyline-smash/..., reference/param-labels/...):
    under SSBU_FIGHTER_REFERENCE, else the repository's deps/reference, else ../reference next to the
    work folder."""
    env = os.environ if env is None else env
    work = WORK if work is None else work
    repo = REPO if repo is None else repo
    roots = [env.get('SSBU_FIGHTER_REFERENCE'), os.path.join(repo, 'deps', 'reference') if repo else None,
             os.path.join(os.path.dirname(work), 'reference')]
    return first('reference/' + '/'.join(parts), [os.path.join(r, *parts) for r in roots if r],
                 'Clone the public project into one of these folders, or name the folder in SSBU_FIGHTER_REFERENCE.')


def userdata(env=None, work=None):
    """A save folder the probe copies before each run (your own, from your console)."""
    env = os.environ if env is None else env
    work = WORK if work is None else work
    return first('A save folder (userdata)', [env.get('SSBU_FIGHTER_USERDATA'), os.path.join(os.path.dirname(work), 'userdata')],
                 'Name your save folder in SSBU_FIGHTER_USERDATA.')


def tree_file(*candidates, tree=None):
    """A file of the SDK's own tree, which setup.py's staging may have moved (mods/x -> x, tools/nro -> ro_study)."""
    tree = TREE if tree is None else tree
    return first(candidates[0], [os.path.join(tree, c) for c in candidates])


def nro_tools(tree=None):
    """The folder of ssbu-recomp's NRO tools (arc_extract.py): tools/nro, staged as ro_study."""
    return os.path.dirname(tree_file(os.path.join('tools', 'nro', 'arc_extract.py'), os.path.join('ro_study', 'arc_extract.py'), tree=tree))


def nro_modules(env=None, work=None, repo=None):
    """[{'name': 'lua2cpp_<x>', 'base': hex text, 'file': path}] of the game's script modules, from the first
    of: SSBU_FIGHTER_NRO, the work folder's ro_study/ (setup.py's layout: nro_map.json, the files in nro/),
    the repository's dump/nro/ (tools/build_game.py's: bases.txt beside the files). The modules come from
    your own copy of the game."""
    env = os.environ if env is None else env
    work = WORK if work is None else work
    repo = REPO if repo is None else repo
    folders = [env.get('SSBU_FIGHTER_NRO'), os.path.join(work, 'ro_study'), os.path.join(repo, 'dump', 'nro') if repo else None]
    tried = []
    for d in [f for f in folders if f]:
        sr, ws = os.path.join(d, 'nro_map.json'), os.path.join(d, 'bases.txt')
        tried += [sr, ws]
        if os.path.isfile(sr):
            with open(sr) as f:
                return [dict(e, file=os.path.join(d, 'nro', e['file'])) for e in json.load(f)]
        if os.path.isfile(ws):
            return bases_modules(d)
    raise missing("The game's script modules (lua2cpp_*.nro)", tried,
                  'Build the game first (tools/build_game.py puts them in dump/nro), or name their folder in SSBU_FIGHTER_NRO.')


def bases_modules(folder):
    """nro_modules() of a folder in tools/build_game.py's layout: <name>.nro files, bases.txt rows
    "<name> <base> <end> <build id>". A module not placed yet has base 0."""
    rows = []
    path = os.path.join(folder, 'bases.txt')
    if os.path.isfile(path):
        with open(path) as f:
            rows = [l.split() for l in f if len(l.split()) == 4]
    out, seen = [], set()
    for name, base, _end, _bid in rows:
        if os.path.isfile(os.path.join(folder, name + '.nro')) and name not in seen:
            out.append({'name': name, 'base': base, 'file': os.path.join(folder, name + '.nro')})
            seen.add(name)
    for fn in sorted(os.listdir(folder)):
        name = fn[:-4]
        if fn.endswith('.nro') and name.startswith('lua2cpp_') and name not in seen:
            out.append({'name': name, 'base': '0', 'file': os.path.join(folder, fn)})
            seen.add(name)
    return out
