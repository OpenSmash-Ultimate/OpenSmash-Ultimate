"""Catalogue of move mechanics, mined from the fighters' own status sources (fighters/<name>/status/*.cpp).

Part 2 of the "mechanics encyclopedia" (part 1: hitcat.py, hit properties). Every status function of every
fighter and weapon is read into one table (what it calls, which work variables it reads and writes, where it
can go, which motions / articles / kinetic calls / input checks / situation checks it has). A MOVE is one
(agent, status kind): the functions bound to it in install.cpp plus the loops and helpers they reach. MECHANICS
are recognition rules over a move (the table MECHANICS below: name, topic, rule text, test).

    python fighter_sdk/tools/mechcat.py              writes fighters/catalog/status_table.csv, moves.csv,
                                                      move_mechanics.json, MOVE_MECHANICS.md
    python fighter_sdk/tools/mechcat.py --find <mechanic> [--fighter <name>] [--simplest]
                                                      lists the moves having a mechanic (smallest first with
                                                      --simplest)
    python fighter_sdk/tools/mechcat.py --move <agent> <status>
                                                      which mechanics a move has, and its row of facts
    python fighter_sdk/tools/mechcat.py --list        the mechanics and their rules

The rules are heuristics over source text; the catalogue says how each one was checked. This file holds rules
and the names of public constants only. Everything it writes is made from the game's scripts and stays under
fighters/ (not published). Hand-written parts of the catalogue are read from fighters/catalog/ when they exist:
mechanic_notes.md (templates), mechanic_checks.md (what reading hits showed), mechanic_limits.md, one_click_design.md.
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
FIGHTERS = os.path.join(STUDY, 'fighters')
OUT = os.path.join(FIGHTERS, 'catalog')
SKIP_DIRS = {'common', 'catalog', 'sdk_generated', 'sdk_generated_s8', 'tools_out', 'item'}

HEAD = re.compile(r'^L2C_(?:VOID_)?FUNCTION\d*\(\s*(\w+)\s*,\s*(\w+)')
HEAD_LOCAL = re.compile(r'^(?:L2CValue|void)\s+local::(\w+)\(')
CALL = re.compile(r'\b((?:\w+::)+\w+)\(')
CONST = re.compile(r'\b[A-Z][A-Z0-9]*_[A-Z0-9_]+\b')
HASH = re.compile(r'hash40\("([^"]*)"\)')
BIND = re.compile(r'sv_set_status_func\(agent,\s*(\w+),\s*LUA_SCRIPT_STATUS_FUNC_(\w+),\s*status_function\((\w+)\)')
REF = re.compile(r'status_function\((\w+)\)|local::(\w+)\(')
GLOBAL = re.compile(r'global_table\[(\d+)\]')
PLAIN = re.compile(r'^[A-Z][A-Z0-9_]*$')

# indices of the agent's global table (community names)
G_PREV_STATUS, G_STATUS, G_SITUATION, G_PREV_SITUATION = 10, 11, 22, 23
G_STICK_X, G_STICK_Y, G_FLICK_X, G_FLICK_Y, G_PAD, G_CAT1, G_CAT4 = 26, 27, 28, 29, 31, 32, 35

WORK_READ = {'is_flag', 'get_int', 'get_float', 'get_int64'}
WORK_WRITE = {'on_flag', 'off_flag', 'set_flag', 'set_int', 'set_float', 'set_int64', 'turn_off_flag'}
WORK_COUNT = {'inc_int', 'dec_int', 'add_int', 'sub_int', 'add_float', 'sub_float', 'mul_float', 'mul_int',
              'count_down_int', 'count_down', 'div_float'}
ENDERS = ('FIGHTER_STATUS_KIND_WAIT', 'FIGHTER_STATUS_KIND_FALL', 'FIGHTER_STATUS_KIND_FALL_SPECIAL',
          'FIGHTER_STATUS_KIND_LANDING', 'FIGHTER_STATUS_KIND_LANDING_FALL_SPECIAL',
          'FIGHTER_STATUS_KIND_LANDING_LIGHT', 'FIGHTER_STATUS_KIND_FALL_AERIAL')
CANCEL_TARGETS = ('FIGHTER_STATUS_KIND_JUMP_SQUAT', 'FIGHTER_STATUS_KIND_JUMP_AERIAL', 'FIGHTER_STATUS_KIND_ESCAPE',
                  'FIGHTER_STATUS_KIND_ESCAPE_F', 'FIGHTER_STATUS_KIND_ESCAPE_B', 'FIGHTER_STATUS_KIND_ESCAPE_AIR',
                  'FIGHTER_STATUS_KIND_GUARD_ON', 'FIGHTER_STATUS_KIND_ATTACK', 'FIGHTER_STATUS_KIND_ATTACK_AIR',
                  'FIGHTER_STATUS_KIND_JUMP', 'FIGHTER_STATUS_KIND_DASH', 'FIGHTER_STATUS_KIND_TURN')


def call_args(text, i):
    """arguments of a call; i = index right after the opening parenthesis"""
    out, start, depth, quote = [], i, 0, False
    n = len(text)
    while i < n:
        ch = text[i]
        if quote:
            quote = ch != '"'
        elif ch == '"':
            quote = True
        elif ch == '(' or ch == '{':
            depth += 1
        elif ch == ')' or ch == '}':
            if depth == 0:
                out.append(text[start:i].strip())
                return out
            depth -= 1
        elif ch == ',' and depth == 0:
            out.append(text[start:i].strip())
            start = i + 1
        i += 1
    return out


def consts_of(arg):
    return CONST.findall(arg)


def key_of(arg):
    """the constant an argument stands for: the plain name, the one constant inside a wrapper, or '?'"""
    if PLAIN.match(arg):
        return arg
    c = consts_of(arg)
    return c[-1] if c else '?'


class Fn(object):
    def __init__(self, module, name, path, body):
        self.module, self.name, self.file = module, name, os.path.basename(path)
        self.lines = len(body)
        self.text = t = ''.join(body)
        self.binds = []            # (agent, status kind, line)
        self.refs = set(a or b for a, b in REF.findall(t)) - {name}
        self.calls = collections.Counter()
        self.keycalls = set()      # "Module::func(CONST, ...)"
        self.work_r, self.work_w, self.counter = set(), set(), set()
        self.work_on = set()       # ids switched ON or given a value (on_flag, set_flag, set_int ...; not off_flag)
        self.trans, self.motions, self.kinetic = [], [], set()
        self.art_gen, self.art_other = set(), set()
        self.inputs, self.sit = set(), set()
        self.params = set()
        self.init = None           # init_settings: situation, kinetic type, ground correct, cliff check
        self.consts = set(CONST.findall(t))
        self.globals = set(int(g) for g in GLOBAL.findall(t))
        for m in CALL.finditer(t):
            full = m.group(1)
            if full.startswith('local::'):
                continue
            mod, _, fn = full.rpartition('::')
            args = call_args(t, m.end())
            self.calls[full] += 1
            ks = [key_of(a) for a in args[1:] if consts_of(a) and 'hash40' not in a]
            if ks and mod not in ('L2CAgentBase',):
                self.keycalls.add('%s(%s)' % (full, ', '.join(ks)))
            if mod == 'WorkModule':
                ids = [a for a in args[1:] if consts_of(a)]
                wid = key_of(ids[-1]) if ids else '?'
                if fn in WORK_READ:
                    self.work_r.add(wid)
                elif fn in WORK_WRITE:
                    self.work_w.add(wid)
                    if fn != 'off_flag' and fn != 'turn_off_flag':
                        self.work_on.add(wid)
                elif fn in WORK_COUNT:
                    self.work_w.add(wid)
                    self.counter.add(wid)
                elif fn in ('get_param_int', 'get_param_float', 'get_param_int64'):
                    hs = HASH.findall(','.join(args))
                    if hs:
                        self.params.add('/'.join(hs))
            elif fn in ('change_status', 'change_status_request', 'change_status_request_from_script',
                        'set_status_kind_interrupt') and mod in ('L2CFighterBase', 'StatusModule'):
                if len(args) > 1:
                    self.trans.append(key_of(args[1]))
            elif mod in ('MotionModule', 'FighterMotionModuleImpl') and fn.startswith('change_motion'):
                hs = HASH.findall(args[1]) if len(args) > 1 else []
                self.motions.append(hs[0] if hs else '?')
            elif mod == 'ArticleModule' or full == 'sv_module_access::article':
                a = key_of(args[1]) if len(args) > 1 else '?'
                if 'generate' in fn or (full == 'sv_module_access::article' and 'GENERATE' in a):
                    self.art_gen.add(a if mod == 'ArticleModule' else key_of(args[2]) if len(args) > 2 else a)
                else:
                    extra = [key_of(x) for x in args[2:] if consts_of(x)] + HASH.findall(','.join(args[2:]))
                    self.art_other.add('%s(%s%s)' % (fn, a, ''.join(', ' + e for e in extra)))
            elif mod in ('KineticModule', 'sv_kinetic_energy', 'KineticUtility'):
                ks2 = [key_of(a) for a in args[1:] if consts_of(a)]
                self.kinetic.add('%s(%s)' % (fn, ', '.join(ks2)))
            elif mod == 'ControlModule':
                if fn.startswith('check_button'):
                    b = key_of(args[1]).replace('CONTROL_PAD_BUTTON_', '') if len(args) > 1 else '?'
                    kind = {'check_button_on': 'on', 'check_button_off': 'off', 'check_button_trigger': 'trigger',
                            'check_button_release': 'release'}.get(fn, fn.replace('check_button_', ''))
                    self.inputs.add('%s:%s' % (kind, b))
                elif fn in ('get_stick_x', 'get_stick_y', 'get_stick_dir', 'get_stick_angle', 'get_stick_prev_y',
                            'get_flick_x', 'get_flick_y', 'get_flick_no_reset_x', 'get_flick_no_reset_y'):
                    self.inputs.add(fn[4:].replace('_no_reset', '').replace('_prev', ''))
            elif full == 'StatusModule::init_settings' and len(args) >= 5:
                self.init = [key_of(a) for a in args[1:5]]
        for c in self.consts:
            if c.startswith('FIGHTER_PAD_FLAG_'):
                self.inputs.add('pad:' + c[len('FIGHTER_PAD_FLAG_'):])
            elif c.startswith('FIGHTER_PAD_CMD_CAT'):
                self.inputs.add('cmd:' + c.split('_FLAG_', 1)[-1])
        for g, tag in ((G_STICK_X, 'stick_x'), (G_STICK_Y, 'stick_y'), (G_FLICK_X, 'flick_x'), (G_FLICK_Y, 'flick_y')):
            if g in self.globals:
                self.inputs.add(tag)
        situ = G_SITUATION in self.globals or 'StatusModule::situation_kind' in self.calls
        if situ and 'SITUATION_KIND_GROUND' in self.consts:
            self.sit.add('ground?')
        if situ and 'SITUATION_KIND_AIR' in self.consts:
            self.sit.add('air?')
        if G_PREV_SITUATION in self.globals:
            self.sit.add('changed?')
        for c in self.calls:
            if c.endswith('_by_situation') or c.endswith('_by_situation_kirby_copy'):
                self.sit.add('by_situation')
            if c.endswith('sub_transition_group_check_air_cliff') or c.endswith('sub_fighter_cliff_check'):
                self.sit.add('cliff')
            if c in ('GroundModule::is_touch', 'GroundModule::is_wall_touch_line', 'GroundModule::is_floor_touch_line',
                     'GroundModule::is_ottotto', 'GroundModule::get_touch_flag', 'GroundModule::is_touch_id'):
                self.sit.add('touch')
        if any(c.startswith('GROUND_CLIFF_CHECK_KIND_') and not c.endswith('_NONE') for c in self.consts):
            self.sit.add('cliff')
        self.commons = sorted(c.split('::', 1)[1] for c in self.calls if c.startswith('L2CFighterCommon::')
                              or c.startswith('L2CWeaponCommon::') or c.startswith('L2CFighterBase::'))


def read_module(module):
    """functions of one module, bindings filled in; returns (dict name -> Fn, names referenced but not source)"""
    fns = {}
    for path in sorted(glob.glob(os.path.join(FIGHTERS, module, 'status', '*.cpp'))):
        name, body = None, []
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                if name is None:
                    m = HEAD.match(line) or HEAD_LOCAL.match(line)
                    if m:
                        name, body = m.group(m.lastindex), [line]
                    continue
                body.append(line)
                if line.startswith('}'):
                    fns[name] = Fn(module, name, path, body)
                    name = None
    return fns, install_tables(fns)


SUB_INSTALL = re.compile(r'local::(\w+_install)\(')


def install_tables(fns):
    """agent -> [(status kind, line, function)]. An install function called from another one (a table shared
    by two fighters of a family, or the fighter half of a class) belongs to the agent that calls it."""
    installs = dict((n, f) for n, f in fns.items() if n.endswith('_install'))
    called = set(c for f in installs.values() for c in SUB_INSTALL.findall(f.text))
    out = collections.OrderedDict()
    for name, f in installs.items():
        if name in called:
            continue
        rows, seen, todo = [], set(), [name]
        while todo:
            n = todo.pop(0)
            if n in seen or n not in installs:
                continue
            seen.add(n)
            rows.extend(BIND.findall(installs[n].text))
            todo.extend(SUB_INSTALL.findall(installs[n].text))
        out[name[:-len('_install')]] = rows
    return out


class Move(object):
    """one (agent, status kind): the bound functions and everything they reach"""

    def __init__(self, module, agent, status):
        self.module, self.agent, self.status = module, agent, status
        self.fns, self.lines_bound, self.missing = [], {}, set()
        self.tags = []

    def finish(self):
        fs = self.fns
        self.calls = set(c for f in fs for c in f.calls)
        self.keycalls = set(c for f in fs for c in f.keycalls)
        self.consts = set(c for f in fs for c in f.consts)
        self.work_r = set(c for f in fs for c in f.work_r)
        self.work_w = set(c for f in fs for c in f.work_w)
        self.counter = set(c for f in fs for c in f.counter)
        self.trans = set(c for f in fs for c in f.trans)
        self.motions = set(c for f in fs for c in f.motions)
        self.kinetic = set(c for f in fs for c in f.kinetic)
        self.art_gen = set(c for f in fs for c in f.art_gen)
        self.art_other = set(c for f in fs for c in f.art_other)
        self.inputs = set(c for f in fs for c in f.inputs)
        self.sit = set(c for f in fs for c in f.sit)
        self.size = sum(f.lines for f in fs)
        # the status kind's name says what the agent is (an agent's name does not: a fighter class can be nameless,
        # and a module can name a weapon like itself)
        self.weapon = self.status.startswith('WEAPON_') or (not self.status.startswith('FIGHTER_')
                                                            and self.agent != self.module)
        self.own = set()           # filled by read_all: status kinds this agent binds
        self.agent_moves = {}

    def where(self):
        return '%s %s' % (self.agent, self.status)

    def own_trans(self):
        return set(t for t in self.trans if t in self.own and t != self.status)


def read_all(only=None):
    fns_all, moves = [], collections.OrderedDict()
    mods = [d for d in sorted(os.listdir(FIGHTERS)) if d not in SKIP_DIRS
            and os.path.isdir(os.path.join(FIGHTERS, d, 'status'))]
    for module in mods:
        if only and module != only:
            continue
        real, tables = read_module(module)
        for agent, rows in tables.items():
            for kind, line, fn in rows:
                line = line.lower()
                mv = moves.setdefault((module, agent, kind), Move(module, agent, kind))
                if fn not in real:             # named by the install table but not source (stays original)
                    mv.missing.add(fn)
                    # its loops can be source all the same (named <function>_loop...): they belong to the move
                    for n, g in real.items():
                        if n.startswith(fn + '_loop') and g not in mv.fns:
                            mv.fns.append(g)
                            mv.fns.extend(real[r] for r in g.refs if r in real and real[r] not in mv.fns
                                          and not r.endswith('_install'))
                    continue
                real[fn].binds.append((agent, kind, line))
                mv.lines_bound[line] = fn
                seen, todo = set(), [fn]
                while todo:
                    n = todo.pop()
                    if n in seen:
                        continue
                    seen.add(n)
                    g = real.get(n)
                    if g is None:
                        mv.missing.add(n)
                        continue
                    if g not in mv.fns:
                        mv.fns.append(g)
                    todo.extend(r for r in g.refs if not r.endswith('_install'))
        fns_all.extend(v for v in real.values())
    by_agent = collections.defaultdict(dict)
    for (module, agent, kind), mv in moves.items():
        mv.finish()
        by_agent[(module, agent)][kind] = mv
    for (module, agent), d in by_agent.items():
        for mv in d.values():
            mv.own = set(d)
            mv.agent_moves = d
    used = collections.defaultdict(set)
    for mv in moves.values():
        for f in mv.fns:
            used[id(f)].add((mv.agent, mv.status))
    return fns_all, list(moves.values()), used


# ---------------------------------------------------------------------------------------------- rule helpers

def is_instance(w):
    return '_INSTANCE_WORK_ID_' in w


def button_hold(f):
    return any(i.startswith(('on:', 'off:', 'release:')) for i in f.inputs) or \
        any(i.startswith('pad:') and i.endswith('_RELEASE') for i in f.inputs)


def button_press(f):
    return any(i.startswith('trigger:') for i in f.inputs) or \
        any(i.startswith('pad:') and i.endswith('_TRIGGER') for i in f.inputs)


def stick_aim(f):
    return bool(f.inputs & {'stick_y', 'stick_dir', 'stick_angle'})


def strip_air(m):
    return m.replace('_air_', '_').replace('air_', '').replace('_air', '')


def air_pair(motions):
    ms = set(m for m in motions if m != '?')
    return any(('air' in m) and strip_air(m) in ms and strip_air(m) != m for m in ms)


def variants(motions):
    """distinct motions once the ground / air pairing is removed"""
    return set(strip_air(m) for m in motions if m != '?')


def kin(mv, *words):
    return any(all(w in k for w in words) for k in mv.kinetic)


def chain(mv):
    """own statuses reached in one or two steps, and own statuses leading here"""
    if not chainable(mv.status):
        return set(), set(), set()
    nxt = set(t for t in mv.own_trans() if chainable(t))
    nxt2 = set(t for n in nxt for t in mv.agent_moves[n].own_trans() if t != mv.status and chainable(t)) - nxt
    prev = set(k for k, o in mv.agent_moves.items() if mv.status in o.trans and k != mv.status and chainable(k))
    return prev, nxt, nxt2


def chainable(kind):
    """a status kind that can be a part of a move: the agent's own kinds, and of the shared kinds only the
    specials, the final smash and the attacks (not WAIT, FALL, LANDING ... that a fighter happens to override)"""
    if not kind.startswith('FIGHTER_STATUS_KIND_'):
        return True
    return any(x in kind for x in ('SPECIAL_', 'FINAL', 'ATTACK')) and 'FALL_SPECIAL' not in kind


def readers(mv, wid):
    return [o for k, o in mv.agent_moves.items() if k != mv.status and wid in o.work_r]


def instance_written(mv):
    return set(w for w in mv.work_w if is_instance(w))


def own_instance(w):
    """an instance work id of this fighter or weapon, not one of the shared machine's (FIGHTER_INSTANCE_WORK_ID_*)"""
    return is_instance(w) and not w.startswith(('FIGHTER_INSTANCE_WORK_ID_', 'WEAPON_INSTANCE_WORK_ID_'))


def bound_lines(m, f):
    return set(b[2] for b in f.binds if b[1] == m.status)


def shield_calls(m, kind, state):
    """keyed calls that switch a shield of a collision kind ('SHIELD', 'REFLECTOR', 'ABSORBER') to a state"""
    out = []
    for k in m.keycalls:
        if state not in k:
            continue
        if k.startswith('sv_module_access::shield(') and 'COLLISION_KIND_' + kind in k:
            out.append(k)
        elif kind == 'SHIELD' and k.startswith('ShieldModule::set_status'):
            out.append(k)
        elif kind == 'REFLECTOR' and k.startswith('ReflectorModule::set_status'):
            out.append(k)
        elif kind == 'ABSORBER' and k.startswith('AbsorberModule::set_status'):
            out.append(k)
    return out


def mode_flags(m):
    """own INSTANCE flags this move switches ON outside its end / exit functions"""
    out = set()
    for f in m.fns:
        if bound_lines(m, f) & {'status_end', 'exit_status'}:
            continue
        out |= set(w for w in f.work_on if own_instance(w) and '_FLAG_' in w
                   and not any(x in w for x in ('STRANS', 'MTRANS', '_INIT', 'INHERIT')))
    return out


# name, topic, the rule in words, test(move)
MECHANICS = [
    ('charge_hold', 'charge',
     'one function of the move both tests a held / released button (check_button_on, _off, _release, or a '
     'PAD_FLAG_*_RELEASE bit) and calls change_status, and the move counts in a work variable (inc_int, add_float, '
     'dec_int, count_down_int ... ; the value is usually read by the NEXT status)',
     lambda m: bool(m.counter) and any(button_hold(f) and f.trans for f in m.fns)),
    ('charge_stored', 'charge',
     'a FIGHTER status counts in an INSTANCE work id of its own (it outlives the status: the value is kept between '
     'uses) and one function tests a button (held, released or pressed) and calls change_status',
     lambda m: not m.weapon and any(own_instance(w) for w in m.counter)
     and any((button_hold(f) or button_press(f)) and f.trans for f in m.fns)),
    ('timed_press_window', 'charge',
     'the move counts in a work variable it reads, a function that counts also reads a parameter (get_param_*), and '
     'one function tests a button PRESS (check_button_trigger or PAD_FLAG_*_TRIGGER) and calls change_status; no '
     'held-button test: a count runs by itself and a press or the limit moves on (self-filling charges, timed '
     'windows for a follow-up press)',
     lambda m: 'charge_hold' not in m.tags and bool(m.counter & m.work_r)
     and any(button_press(f) and f.trans for f in m.fns) and any(f.params and f.counter for f in m.fns)),
    ('stick_aim', 'aim',
     'one function reads the stick vertically or as a direction (global stick_y, ControlModule::get_stick_y, '
     'get_stick_dir, get_stick_angle) and in the same function selects among two or more motions, or sets a '
     'speed / angle (sv_kinetic_energy::set_speed, set_angle, a float work variable)',
     lambda m: any(stick_aim(f) and (len(variants(f.motions)) >= 2 or any('set_speed' in k or 'angle' in k
                   for k in f.kinetic) or any('_FLOAT_' in w for w in f.work_w)) for f in m.fns)),
    ('ground_air_motion', 'situation',
     'one function tests the situation (global situation kind against SITUATION_KIND_GROUND / _AIR) and sets a '
     'motion pair "x" / "air x", or calls sub_change_motion_by_situation',
     lambda m: any((f.sit & {'ground?', 'air?'} and air_pair(f.motions)) or 'by_situation' in f.sit
                   and any('change_motion_by_situation' in c for c in f.calls) for f in m.fns)),
    ('ground_air_status', 'situation',
     'one function tests the situation and can change to two or more different statuses that are the agent\'s own '
     'kinds (not kinds of the shared machine: WAIT, FALL, LANDING ... do not count): the move splits into a ground '
     'status and an air status',
     lambda m: any(f.sit & {'ground?', 'air?'} and len(set(t for t in f.trans if t in m.own and t != m.status
                   and not t.startswith('FIGHTER_STATUS_KIND_'))) >= 2 for f in m.fns)),
    ('article_spawn', 'projectile',
     'status code calls ArticleModule::generate_article* (or the article GENERATE script command): the article '
     'constant is the argument. Spawns written in the ACMD script are counted in the ACMD supplement, not here',
     lambda m: bool(m.art_gen)),
    ('article_one_at_a_time', 'projectile',
     'the function that generates the article also asks ArticleModule::is_exist / get_active_num / is_generatable / '
     'get_num for it',
     lambda m: any(f.art_gen and any(c in f.calls for c in ('ArticleModule::is_exist', 'ArticleModule::get_active_num',
                   'ArticleModule::is_generatable', 'ArticleModule::get_num')) for f in m.fns)),
    ('article_driven', 'projectile',
     'the owner drives an article it has from status code: ArticleModule::change_status / change_status_exist / '
     'change_motion / shoot / set_flag / set_int / set_float',
     lambda m: any(a.split('(')[0] in ('change_status', 'change_status_exist', 'change_motion', 'shoot',
                   'shoot_exist', 'set_flag', 'set_int', 'set_float', 'change_motion_from_no') for a in m.art_other)),
    ('article_work_shared', 'projectile',
     'a fighter status reads or writes a work variable of one of its articles (a WEAPON_*_INSTANCE_WORK_ID_* id in a '
     'fighter function, or ArticleModule::set_float / set_int / set_int64 / set_flag): the value (charge, mode, '
     'target) lives in the projectile',
     lambda m: not m.weapon and (any(w.startswith('WEAPON_') and is_instance(w) and not w.startswith('WEAPON_INSTANCE_')
                                     for w in m.work_r | m.work_w)
                                 or any(a.split('(')[0] in ('set_float', 'set_int', 'set_int64', 'set_flag')
                                        for a in m.art_other))),
    ('hitbox_lerp_by_work', 'projectile',
     'status code calls AttackModule::set_lerp_ratio: the hitbox is interpolated between two definitions of the '
     'animation script (ATTACK id a, ATTACK id b, attack command SET_LERP a b) by a ratio, usually the charge',
     lambda m: 'AttackModule::set_lerp_ratio' in m.calls),
    ('weapon_life_timer', 'projectile',
     'a weapon status decrements a *_INSTANCE_WORK_ID_INT_LIFE work variable (dec_int / count_down_int)',
     lambda m: m.weapon and any(w.endswith('_INT_LIFE') for w in m.counter)),
    ('weapon_owner_link', 'projectile',
     'a weapon status talks to its owner or follows it: LinkModule::*, or WorkModule calls on the owner through '
     'sv_battle_object::module_accessor / get_founder_id / get_owner_id',
     lambda m: m.weapon and any(c.startswith('LinkModule::') or c.endswith('get_founder_id')
                                or c.endswith('get_owner_id') for c in m.calls)),
    ('remote_controlled_projectile', 'projectile',
     'a WEAPON status reads the controller itself: a ControlModule::check_button_* call or a stick read '
     '(the projectile is steered, kept alive or set off by the owner\'s input)',
     lambda m: m.weapon and any(i.startswith(('on:', 'off:', 'trigger:', 'release:', 'stick', 'flick'))
                                for i in m.inputs)),
    ('command_grab', 'grab',
     'a status that is not one of the shared grab / throw kinds (FIGHTER_STATUS_KIND_CATCH*, _THROW*, _CAPTURE*) '
     'calls CatchModule::* or the _catch script command (set catch, is_catch, catch_cut, cling, check_damage, '
     'capture_object_id)',
     lambda m: any(c.startswith('CatchModule::') or c == 'sv_module_access::_catch' for c in m.calls)
     and not (m.status.startswith('FIGHTER_STATUS_KIND_') and any(x in m.status for x in ('CATCH', 'THROW', 'CAPTURE')))
     and not (m.weapon and 'THROW' in m.status)),
    ('counter_shield', 'counter',
     'status code switches a counter shield ON: the shield script command MA_MSC_SHIELD_SET_STATUS with '
     'COLLISION_KIND_SHIELD, or ShieldModule::set_status, with SHIELD_STATUS_NORMAL',
     lambda m: bool(shield_calls(m, 'SHIELD', 'SHIELD_STATUS_NORMAL'))),
    ('reflector', 'counter',
     'status code switches a reflector ON: MA_MSC_SHIELD_SET_STATUS with COLLISION_KIND_REFLECTOR, or '
     'ReflectorModule::set_status, with SHIELD_STATUS_NORMAL',
     lambda m: bool(shield_calls(m, 'REFLECTOR', 'SHIELD_STATUS_NORMAL'))),
    ('absorber', 'counter',
     'status code switches an absorber ON: AbsorberModule::set_status with SHIELD_STATUS_NORMAL',
     lambda m: bool(shield_calls(m, 'ABSORBER', 'SHIELD_STATUS_NORMAL'))),
    ('shield_reset_only', 'counter',
     'status code only switches a counter shield, reflector or absorber OFF (SHIELD_STATUS_NONE; also '
     'ReflectorModule::set_status_all): the status belongs to a counter / reflector move whose shield is switched on '
     'by the animation script or by another status',
     lambda m: not any(shield_calls(m, k, 'SHIELD_STATUS_NORMAL') for k in ('SHIELD', 'REFLECTOR', 'ABSORBER'))
     and (any(shield_calls(m, k, 'SHIELD_STATUS_NONE') for k in ('SHIELD', 'REFLECTOR', 'ABSORBER'))
          or any(k.startswith('ReflectorModule::set_status_all') and 'SHIELD_STATUS_NONE' in k for k in m.keycalls))),
    ('free_fall', 'recovery',
     'the move can change to FIGHTER_STATUS_KIND_FALL_SPECIAL (change_status / set_status_kind_interrupt), or '
     'stores that kind in a work variable as "what follows" (the shared up-special machine)',
     lambda m: 'FIGHTER_STATUS_KIND_FALL_SPECIAL' in m.trans
     or any(k.startswith('WorkModule::set_int(') and 'FIGHTER_STATUS_KIND_FALL_SPECIAL' in k for k in m.keycalls)),
    ('shared_super_jump_punch', 'recovery',
     'the move calls L2CFighterCommon::super_jump_punch: the whole up special is the shared machine, configured by '
     'FIGHTER_STATUS_SUPER_JUMP_PUNCH_WORK_* values (motions, stick influence, landing frames, what follows)',
     lambda m: 'L2CFighterCommon::super_jump_punch' in m.calls),
    ('ledge_grab_window', 'recovery',
     'the move allows the ledge from status code: sub_transition_group_check_air_cliff / sub_fighter_cliff_check, '
     'a GROUND_CLIFF_CHECK_KIND_* other than NONE (init_settings or the cliff-check event)',
     lambda m: 'cliff' in m.sit),
    ('landing_lag_status', 'landing',
     'the move can change to a landing status: FIGHTER_STATUS_KIND_LANDING* or a status of its own whose name has '
     'LANDING / _LAND, or (not being a landing status itself) it writes FIGHTER_INSTANCE_WORK_ID_FLOAT_LANDING_FRAME',
     lambda m: any('LANDING' in t or t.endswith('_LAND') or '_LAND_' in t for t in m.trans)
     or ('FIGHTER_INSTANCE_WORK_ID_FLOAT_LANDING_FRAME' in m.work_w and 'LANDING' not in m.status)),
    ('multi_part_chain', 'multi-part',
     'the status is part of a chain of at least three statuses of its own agent linked by change_status '
     '(it leads to one that leads to a third, or it sits in the middle or at the end of such a chain)',
     lambda m: (lambda p, n, n2: bool(n and n2) or bool(p and n) or
                bool(p and any(m.agent_moves[q].status != m.status and
                               any(m.agent_moves[q].status in o.trans and k not in (m.status, q)
                                   for k, o in m.agent_moves.items()) for q in p)))(*chain(m))),
    ('mash_extend', 'multi-part',
     'one function tests a button PRESS and in the same function counts in a work variable (inc_int, add_*, '
     'dec_int ...) : presses add to / reset a counter the move reads',
     lambda m: any(button_press(f) and f.counter for f in m.fns) and bool(m.counter & m.work_r)),
    ('cancel_terms_enabled', 'cancel',
     'status code enables actions of the shared machine as interruptions: WorkModule::enable_transition_term with a '
     'FIGHTER_STATUS_TRANSITION_TERM_ID_CONT_* term, or enable_transition_term_group with a guard / escape / jump / '
     'attack / special group (terms like _WAIT, _FALL, _LANDING and the cliff / landing groups, which moves use as '
     'their own "how do I end" switches, do not count)',
     lambda m: any((k.startswith('WorkModule::enable_transition_term(') and 'TRANSITION_TERM_ID_CONT_' in k)
                   or (k.startswith('WorkModule::enable_transition_term_group(')
                       and any(x in k for x in ('GUARD', 'ESCAPE', 'JUMP', 'ATTACK', 'SPECIAL')))
                   for k in m.keycalls)),
    ('cancel_direct', 'cancel',
     'one function tests a command / pad bit and calls change_status to a jump, escape, guard, dash, turn or '
     'attack status of the shared state machine',
     lambda m: any(any(i.startswith(('cmd:', 'pad:')) for i in f.inputs)
                   and any(t in CANCEL_TARGETS for t in f.trans) for f in m.fns)),
    ('cancel_frame_standard', 'cancel',
     'the standard ending: CancelModule::is_enable_cancel then sub_wait_ground_check_common / '
     'sub_air_check_fall_common (the cancel frame of the animation opens every normal action)',
     lambda m: 'CancelModule::is_enable_cancel' in m.calls and any('check_common' in c or 'check_fall_common' in c
                                                                   for c in m.calls)),
    ('super_armour', 'armour',
     'status code switches armour ON: DamageModule::set_no_reaction_mode_status with a mode other than '
     'DAMAGE_NO_REACTION_MODE_NORMAL, or set_no_reaction_damage_power / set_no_reaction_no_effect. (A status that '
     'only resets to NORMAL in its end function has its armour from the animation script: see the ACMD supplement)',
     lambda m: any(k.startswith('DamageModule::set_no_reaction_mode_status') and 'DAMAGE_NO_REACTION_MODE_' in k
                   and 'DAMAGE_NO_REACTION_MODE_NORMAL' not in k for k in m.keycalls)
     or any(c in m.calls for c in ('DamageModule::set_no_reaction_damage_power', 'DamageModule::set_no_reaction_no_effect'))),
    ('intangible', 'armour',
     'status code calls HitModule::set_whole / set_status_all / set_status_joint with HIT_STATUS_XLU or '
     '_INVINCIBLE, or set_xlu_frame_global / set_invincible_frame_global (HIT_STATUS_OFF, which mostly removes an '
     'extra hurtbox or a weapon\'s, is left out)',
     lambda m: any(k.startswith('HitModule::') and ('HIT_STATUS_XLU' in k or 'HIT_STATUS_INVINCIBLE' in k)
                   for k in m.keycalls)
     or any(c in m.calls for c in ('HitModule::set_xlu_frame_global', 'HitModule::set_invincible_frame_global'))),
    ('damage_scaled_by_status', 'armour',
     'status code sets the power of the hitboxes: AttackModule::set_power_mul_status / set_power_mul / set_power / '
     'set_power_up (damage is not only what the animation script says)',
     lambda m: any(c.startswith('AttackModule::set_power') for c in m.calls)),
    ('kinetic_stop', 'movement',
     'the kinetic type FIGHTER_KINETIC_TYPE_GROUND_STOP / _AIR_STOP, by KineticModule::change_kinetic or '
     'init_settings (the move brakes to a halt; the default of most specials)',
     lambda m: any(c in m.consts for c in ('FIGHTER_KINETIC_TYPE_GROUND_STOP', 'FIGHTER_KINETIC_TYPE_AIR_STOP'))),
    ('gravity_off', 'movement',
     'KineticModule::unable_energy(FIGHTER_KINETIC_ENERGY_ID_GRAVITY) or unable_energy_all, with no '
     'enable_energy(GRAVITY) anywhere in the move (the set-up idiom "switch all energies off, then on again the '
     'ones wanted" is left out that way; a move that stalls and later falls again is left out too)',
     lambda m: (kin(m, 'unable_energy(', 'ENERGY_ID_GRAVITY') or kin(m, 'unable_energy_all('))
     and not kin(m, 'enable_energy(', 'ENERGY_ID_GRAVITY') and not m.weapon),
    ('air_drift_control', 'movement',
     'the stick moves the fighter during the move: KineticModule::enable_energy(FIGHTER_KINETIC_ENERGY_ID_CONTROL), '
     'or sv_kinetic_energy::controller_set_accel_x_mul / _add',
     lambda m: kin(m, 'enable_energy(', 'ENERGY_ID_CONTROL') or kin(m, 'controller_set_accel')),
    ('speed_set_by_status', 'movement',
     'status code writes the speed itself: sv_kinetic_energy::set_speed / reset_energy / set_accel on the STOP, '
     'GRAVITY or MOTION energy (launch speeds, dashes, custom arcs)',
     lambda m: any(k.startswith(('set_speed(', 'reset_energy(', 'set_accel(')) and 'ENERGY_ID_' in k
                   and 'ENERGY_ID_CONTROL' not in k for k in m.kinetic)),
    ('speed_limit_scaled', 'movement',
     'sv_kinetic_energy::set_speed_mul / set_limit_speed / set_stable_speed / set_brake / mul_x_speed_max / '
     'mul_x_accel_mul (speed caps and multipliers while the move runs)',
     lambda m: any(k.startswith(('set_speed_mul', 'set_limit_speed', 'set_stable_speed', 'set_brake', 'mul_x_'))
                   for k in m.kinetic)),
    ('motion_driven_movement', 'movement',
     'KineticModule::change_kinetic to a *_KINETIC_TYPE_MOTION* type, or init_settings with one (the animation '
     'moves the fighter); the falling type MOTION_FALL is left out',
     lambda m: any('KINETIC_TYPE_MOTION' in c and 'MOTION_FALL' not in c for c in m.consts)),
    ('special_start_shared_kinetic', 'movement',
     'the move calls L2CFighterCommon::sub_set_special_start_common_kinetic_setting / '
     'sub_exec_special_start_common_kinetic_setting with a parameter group: the shared "a special slows the fall at '
     'its start" block',
     lambda m: any('special_start_common_kinetic_setting' in c for c in m.calls)),
    ('turn_at_start', 'turn',
     'set_fighter_status_data with FIGHTER_STATUS_ATTR_START_TURN (the shared code turns the fighter to the stick at '
     'the start: the "B-reverse" of the shared machine)',
     lambda m: 'FIGHTER_STATUS_ATTR_START_TURN' in m.consts),
    ('turn_in_status', 'turn',
     'status code turns the fighter itself: PostureModule::reverse_lr (a turn) or set_stick_lr (face the stick), '
     'or, in a fighter status, set_lr',
     lambda m: any(c in m.calls for c in ('PostureModule::reverse_lr', 'PostureModule::set_stick_lr'))
     or (not m.weapon and 'PostureModule::set_lr' in m.calls)),
    ('instance_timer', 'resource',
     'the move counts an INSTANCE work variable of its own agent down or up (dec_int / count_down_int / inc_int / '
     'add_* on a *_INSTANCE_WORK_ID_* id that is not one of the shared machine\'s and not a weapon\'s LIFE): a timer '
     'or use counter that outlives the status',
     lambda m: any(own_instance(w) and not w.endswith('_INT_LIFE') for w in m.counter)),
    ('resource_gate', 'resource',
     'a PRE or MAIN function (not its loop) reads an INSTANCE int / float of its own agent and can change status, '
     'or reads an INSTANCE flag and can change to a status whose name has FAIL: the move is refused or rerouted by '
     'a meter, a counter or a "not available" flag',
     lambda m: any((any(own_instance(w) and ('_INT_' in w or '_FLOAT_' in w) for w in f.work_r) and f.trans)
                   or (any(own_instance(w) and '_FLAG_' in w for w in f.work_r) and any('FAIL' in t for t in f.trans))
                   for f in m.fns if bound_lines(m, f) & {'status_pre', 'status_main'})),
    ('mode_switch', 'stance',
     'outside its end / exit functions the move switches ON (on_flag / set_flag) an INSTANCE flag of its own agent '
     'that at least three OTHER statuses of the agent read (names with STRANS / MTRANS / INIT / INHERIT, the '
     'transition bookkeeping, left out): it turns on a mode the rest of the fighter looks at',
     lambda m: any(len(readers(m, w)) >= 3 for w in mode_flags(m))),
    ('flag_variant', 'stance',
     'one function reads an INSTANCE flag of its own agent and in the same function chooses among two or more '
     'motions (beyond the ground / air pair): an alternate or powered-up version of the move',
     lambda m: any(any(own_instance(w) and '_FLAG_' in w for w in f.work_r) and len(variants(f.motions)) >= 2
                   for f in m.fns)),
    ('on_hit_followup', 'extra',
     'one function asks AttackModule::is_infliction / is_infliction_status / is_attack_occur and changes status or '
     'writes a work variable, or the status has a CHECK_ATTACK line bound: the move continues differently when it '
     'connected',
     lambda m: any(any(c.startswith('AttackModule::is_infliction') or c == 'AttackModule::is_attack_occur'
                       for c in f.calls) and (f.trans or f.work_w) for f in m.fns) or 'check_attack' in m.lines_bound),
    ('wall_or_floor_touch', 'extra',
     'one function asks GroundModule::is_touch / is_wall_touch_line / is_floor_touch_line and then changes status '
     'or motion (bounce, stick, stop at a wall)',
     lambda m: any('touch' in f.sit and (f.trans or f.motions) for f in m.fns)),
    ('motion_rate_by_status', 'extra',
     'status code sets the animation speed: MotionModule::set_rate (often from a parameter or a charge value)',
     lambda m: 'MotionModule::set_rate' in m.calls),
    ('scale_by_status', 'extra',
     'status code sets the size of the object: PostureModule::set_scale (projectiles that grow with charge)',
     lambda m: 'PostureModule::set_scale' in m.calls),
    ('kirby_copy_status', 'extra',
     'the status changes motion with the *_kirby_copy calls: it is Kirby\'s own copy of another fighter\'s neutral '
     'special (FIGHTER_KIRBY_STATUS_KIND_<FIGHTER>_SPECIAL_N*), separate code in the kirby module',
     lambda m: any('change_motion' in c and 'kirby_copy' in c for c in m.calls)),
    ('one_shot', 'extra',
     'the baseline: a fighter status that leaves only to WAIT / FALL / the common landings, tests '
     'MotionModule::is_end, has no status of its own to go to and no status of its own agent leading to it',
     lambda m: not m.weapon and bool(m.trans) and all(t in ENDERS for t in m.trans)
     and 'MotionModule::is_end' in m.calls and not chain(m)[0]),
]
NAMES = [n for n, _, _, _ in MECHANICS]


def classify(moves):
    by = collections.OrderedDict((n, []) for n in NAMES)
    for m in moves:
        for name, _, _, test in MECHANICS:
            if test(m):
                m.tags.append(name)
                by[name].append(m)
    return by


def examples(ms, n=5):
    seen, out = set(), []
    for m in sorted(ms, key=lambda m: (m.module == 'kirby', m.weapon, m.module)):
        if m.module not in seen:
            seen.add(m.module)
            out.append(m.where())
            if len(out) == n:
                break
    return out


# ------------------------------------------------------------------------------------------- ACMD supplement

ACMD_HEAD = re.compile(r'^ACMD\w*\(\s*(\w+)\s*,\s*(\w+)\s*\)')
ACMD_PATTERNS = [
    ('article spawned (ArticleModule::generate_article*)', re.compile(r'ArticleModule::generate_article\w*\(boma,\s*(\w+)')),
    ('reflector box (shield command MA_MSC_CMD_SHIELD_ON with COLLISION_KIND_REFLECTOR)',
     re.compile(r'sv_module_access::shield\(agent,\s*MA_MSC_CMD_SHIELD_ON,\s*COLLISION_KIND_REFLECTOR')),
    ('shield box of a counter (shield command MA_MSC_CMD_SHIELD_ON with COLLISION_KIND_SHIELD)',
     re.compile(r'sv_module_access::shield\(agent,\s*MA_MSC_CMD_SHIELD_ON,\s*COLLISION_KIND_SHIELD')),
    ('hitbox interpolation declared (attack command MA_MSC_CMD_ATTACK_SET_LERP)',
     re.compile(r'sv_module_access::attack\(agent,\s*MA_MSC_CMD_ATTACK_SET_LERP')),
    ('grab box (CATCH)', re.compile(r'\bCATCH\(agent')),
    ('super armour (damage command MA_MSC_DAMAGE_DAMAGE_NO_REACTION)',
     re.compile(r'sv_module_access::damage\(agent,\s*MA_MSC_DAMAGE_DAMAGE_NO_REACTION')),
    ('intangible / invincible (WHOLE_HIT or HIT_NODE with HIT_STATUS_XLU / _INVINCIBLE)',
     re.compile(r'\b(?:WHOLE_HIT|HIT_NODE)\(agent,.*HIT_STATUS_(?:XLU|INVINCIBLE)')),
    ('status flag set (WorkModule::on_flag / off_flag / set_flag on a *_STATUS_* id): the script talks to the status',
     re.compile(r'WorkModule::(?:on_flag|off_flag|set_flag)\(boma,.*?(\w*_STATUS_\w+)')),
    ('turn (REVERSE_LR)', re.compile(r'\bREVERSE_LR\(|PostureModule::reverse_lr\(')),
    ('ledge grab window opened (cliff-check event with a GROUND_CLIFF_CHECK_KIND_*)',
     re.compile(r'notify_event_msc_cmd\(agent,\s*Hash40\{0x2127e37c07ULL\}')),
    ('speed set (SET_SPEED_EX / ADD_SPEED_NO_LIMIT)', re.compile(r'\b(?:SET_SPEED_EX|ADD_SPEED_NO_LIMIT)\(')),
    ('cancel opened (cancel command MA_MSC_CMD_CANCEL_ENABLE_CANCEL)',
     re.compile(r'sv_module_access::cancel\(agent,\s*MA_MSC_CMD_CANCEL_ENABLE_CANCEL')),
]


def acmd_supplement(only=None):
    res = collections.OrderedDict((n, {'scripts': set(), 'modules': set(), 'args': collections.Counter()})
                                  for n, _ in ACMD_PATTERNS)
    for path in sorted(glob.glob(os.path.join(FIGHTERS, '*', 'acmd', 'game.cpp'))):
        module = os.path.basename(os.path.dirname(os.path.dirname(path)))
        if module in SKIP_DIRS or (only and module != only):
            continue
        script = None
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = ACMD_HEAD.match(line)
                if m:
                    script = m.group(2)
                    continue
                for name, pat in ACMD_PATTERNS:
                    h = pat.search(line)
                    if h:
                        res[name]['scripts'].add((module, script))
                        res[name]['modules'].add(module)
                        if h.lastindex:
                            res[name]['args'][h.group(1)] += 1
    return res


# ---------------------------------------------------------------------------------------------------- output

def j(s):
    return ' '.join(sorted(s))


def fact_lines(m):
    out = ['%s %s  (%s, %d functions, %d lines%s)' % (m.agent, m.status, 'weapon' if m.weapon else 'fighter',
                                                      len(m.fns), m.size,
                                                      ', NOT source: ' + j(m.missing) if m.missing else '')]
    out.append('  lines bound : ' + ', '.join('%s=%s' % kv for kv in sorted(m.lines_bound.items())))
    for label, s in (('transitions', m.trans), ('motions', m.motions), ('work read', m.work_r),
                     ('work written', m.work_w), ('counted', m.counter), ('articles made', m.art_gen),
                     ('article calls', m.art_other), ('kinetic', m.kinetic), ('inputs', m.inputs),
                     ('situation', m.sit)):
        if s:
            out.append('  %-12s: %s' % (label, ', '.join(sorted(s))))
    init = [f.init for f in m.fns if f.init]
    if init:
        out.append('  init        : ' + ', '.join(init[0]))
    out.append('  mechanics   : ' + (' '.join(m.tags) or '(none)'))
    return out


def find_move(moves, agent, status):
    s = status.upper()
    hits = [m for m in moves if m.agent == agent and (m.status == s or m.status.endswith('_KIND_' + s))]
    return hits or [m for m in moves if m.agent == agent and s in m.status]


def notes(name='mechanic_notes.md'):
    """hand-written sections '### <mechanic>' of a file in fighters/catalog"""
    path, out, cur = os.path.join(OUT, name), {}, None
    if os.path.exists(path):
        with open(path, encoding='utf-8') as f:
            for line in f:
                if line.startswith('### '):
                    cur = line[4:].strip()
                    out[cur] = []
                elif cur:
                    out[cur].append(line.rstrip('\n'))
    return dict((k, '\n'.join(v).strip()) for k, v in out.items())


def main():
    argv = sys.argv[1:]
    known = ('--list', '--find', '--fighter', '--simplest', '--sample', '--move')
    if any(a.startswith('-') and a not in known for a in argv):     # --help and typos: never the writing run
        print(__doc__)
        return
    if '--list' in argv:
        for n, topic, rule, _ in MECHANICS:
            print('%-24s [%s] %s' % (n, topic, rule))
        return
    fighter = argv[argv.index('--fighter') + 1] if '--fighter' in argv else None
    fns, moves, used = read_all()
    by = classify(moves)
    if '--find' in argv:
        name = argv[argv.index('--find') + 1]
        if name not in by:
            print('no mechanic %r; known: %s' % (name, ' '.join(NAMES)))
            return
        hits = [m for m in by[name] if not fighter or m.module == fighter]
        if '--simplest' in argv:
            hits.sort(key=lambda m: m.size)
        if '--sample' in argv:                 # N hits spread evenly over the list, the copy abilities left out
            n = int(argv[argv.index('--sample') + 1])
            pool = [m for m in hits if m.module != 'kirby'] or hits
            hits = [pool[(i * len(pool)) // n] for i in range(n)] if len(pool) > n else pool
        for m in hits:
            print('%-12s %-58s %4d lines  %s' % (m.module, m.where(), m.size, ' '.join(t for t in m.tags if t != name)))
        print('%d moves, %d agents, %d modules' % (len(hits), len({(m.module, m.agent) for m in hits}),
                                                    len({m.module for m in hits})))
        return
    if '--move' in argv:
        i = argv.index('--move')
        hits = find_move(moves, argv[i + 1], argv[i + 2] if len(argv) > i + 2 else '')
        for m in hits:
            print('\n'.join(fact_lines(m)))
            print('  functions   : ' + ', '.join('%s (%s)' % (f.name, f.file) for f in m.fns))
        if not hits:
            print('no such move')
        return
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'status_table.csv'), 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f)
        w.writerow(['module', 'function', 'file', 'role', 'agent', 'status', 'line', 'used_by_statuses', 'lines',
                    'init_settings', 'transitions', 'motions', 'work_read', 'work_written', 'work_counted',
                    'articles_generated', 'article_calls', 'kinetic', 'inputs', 'situation', 'params',
                    'shared_subs', 'calls'])
        for fn in fns:
            if fn.name.endswith('_install'):
                continue
            us = sorted(used.get(id(fn), ()))
            rows = fn.binds or [('', '', '')]
            for agent, kind, line in rows:
                role = 'bound' if line else ('loop' if fn.name.endswith(tuple('_loop_%d' % k for k in range(2, 30)))
                                             or fn.name.endswith('_loop') else 'helper')
                w.writerow([fn.module, fn.name, fn.file, role, agent or (us[0][0] if us else ''), kind, line,
                            ' '.join(sorted({u[1] for u in us})), fn.lines, ' '.join(fn.init or []),
                            j(set(fn.trans)), j(set(fn.motions)), j(fn.work_r), j(fn.work_w), j(fn.counter),
                            j(fn.art_gen), '; '.join(sorted(fn.art_other)), '; '.join(sorted(fn.kinetic)),
                            j(fn.inputs), j(fn.sit), j(fn.params), j(fn.commons), '; '.join(sorted(fn.keycalls))])
    with open(os.path.join(OUT, 'moves.csv'), 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f)
        w.writerow(['module', 'agent', 'kind', 'status', 'functions', 'lines', 'not_source', 'transitions', 'motions',
                    'articles_generated', 'mechanics'])
        for m in moves:
            w.writerow([m.module, m.agent, 'weapon' if m.weapon else 'fighter', m.status, len(m.fns), m.size,
                        j(m.missing), j(m.trans), j(m.motions), j(m.art_gen), ' '.join(m.tags)])
    sup = acmd_supplement()
    hand = notes()
    checks = notes('mechanic_checks.md')
    nmoves = len(moves)
    blocks = collections.Counter()
    block_mods = collections.defaultdict(set)
    for m in moves:
        for c in m.calls:
            if c.startswith(('L2CFighterCommon::', 'L2CWeaponCommon::', 'L2CFighterBase::')):
                blocks[c] += 1
                block_mods[c].add(m.module)
    data = {
        'functions': len([f for f in fns if not f.name.endswith('_install')]),
        'moves': nmoves,
        'agents': len({(m.module, m.agent) for m in moves}),
        'modules': len({m.module for m in moves}),
        'moves_with_a_function_not_source': len([m for m in moves if m.missing]),
        'moves_without_mechanic': len([m for m in moves if not m.tags]),
        'mechanics': [{'name': n, 'topic': topic, 'rule': rule, 'moves': len(by[n]),
                       'agents': len({(m.module, m.agent) for m in by[n]}),
                       'modules': len({m.module for m in by[n]}),
                       'fighter_moves': len([m for m in by[n] if not m.weapon]),
                       'weapon_moves': len([m for m in by[n] if m.weapon]),
                       'examples': examples(by[n]), 'checked': checks.get(n, ''), 'notes': hand.get(n, '')}
                      for n, topic, rule, _ in MECHANICS],
        'shared_building_blocks': [{'function': c, 'moves': k, 'modules': len(block_mods[c])}
                                   for c, k in blocks.most_common(60)],
        'acmd_supplement': [{'what': n, 'scripts': len(v['scripts']), 'modules': len(v['modules']),
                             'special_scripts': len([s for s in v['scripts'] if 'special' in (s[1] or '')])}
                            for n, v in sup.items()],
        'articles_generated_in_status': sorted(collections.Counter(a for m in moves for a in m.art_gen).items()),
        'articles_generated_in_acmd': sorted(sup[ACMD_PATTERNS[0][0]]['args'].items()),
    }
    with open(os.path.join(OUT, 'move_mechanics.json'), 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=1)
    md = ['# Move mechanics found in the game\n',
          'Made by `fighter_sdk/tools/mechcat.py` from %d status functions of %d modules (%d agents: fighters and '
          'their weapons), grouped into %d moves (a move = one agent + one status kind: the functions bound to it '
          'in `install.cpp` and the loops / helpers they reach). Game-derived: not published. Tables: '
          '`status_table.csv` (one row per function and binding), `moves.csv` (one row per move with its '
          'mechanics), `move_mechanics.json`.\n' % (data['functions'], data['modules'], data['agents'], nmoves),
          '`python fighter_sdk/tools/mechcat.py --find <mechanic> [--fighter <name>] [--simplest]` lists the moves; '
          '`--move <agent> <status>` prints the facts and mechanics of one move; `--list` prints the rules.\n',
          '**How far to trust it.** The status sources are a reading of the game\'s code that equals the original '
          'only in an offline synthetic test (plan S1 section 13). %d of the %d moves have at least one function '
          'that is not source (it stays original: its calls are invisible here), so every count below is a LOWER '
          'bound. A rule is a heuristic over the text of the functions; "Checked" under each mechanic says what '
          'reading 10 hits showed. %d moves got no mechanic at all.\n'
          % (data['moves_with_a_function_not_source'], nmoves, data['moves_without_mechanic']),
          '## Overview\n',
          '| Mechanic | Topic | Moves | of which weapon | Agents | Modules | Examples (agent status) |',
          '|---|---|---|---|---|---|---|']
    for p in data['mechanics']:
        md.append('| %s | %s | %d | %d | %d | %d | %s |' % (p['name'], p['topic'], p['moves'], p['weapon_moves'],
                                                             p['agents'], p['modules'],
                                                             '; '.join(x.replace('_STATUS_KIND', '') for x in p['examples'][:3])))
    md += ['\n## What the animation scripts add (ACMD supplement)\n',
           'Several mechanics are written in the animation script (`acmd/game.cpp`), not in status code; the rules '
           'above do not see them. Counted by text pattern over the game scripts:\n',
           '| What | Scripts | of which named *special* | Modules |', '|---|---|---|---|']
    md += ['| %s | %d | %d | %d |' % (x['what'], x['scripts'], x['special_scripts'], x['modules'])
           for x in data['acmd_supplement']]
    md += ['\n## Shared building blocks the fighters call\n',
           'Functions of the shared state machine (`fighters\\common\\status\\`) called from the fighters\' own status '
           'code, by number of moves. A mechanic that is one of these is the most data-driven kind: the fighter only '
           'sets work values and calls it.\n',
           '| Function | Moves | Modules |', '|---|---|---|']
    md += ['| %s | %d | %d |' % (x['function'], x['moves'], x['modules']) for x in data['shared_building_blocks']]
    limits = os.path.join(OUT, 'mechanic_limits.md')
    if os.path.exists(limits):
        with open(limits, encoding='utf-8') as f:
            md += ['', f.read().rstrip()]
    md += ['\n## The mechanics\n']
    for p in data['mechanics']:
        md += ['### %s\n' % p['name'],
               '- **Topic:** %s' % p['topic'],
               '- **Rule:** %s.' % p['rule'],
               '- **Found:** %d moves (%d fighter, %d weapon) of %d agents in %d modules.'
               % (p['moves'], p['fighter_moves'], p['weapon_moves'], p['agents'], p['modules']),
               '- **Examples:** %s' % '; '.join('`%s`' % x for x in p['examples']),
               '- **Checked:** %s' % (p['checked'].replace('\n', ' ') or 'not checked by reading.'), '']
        if p['notes']:
            md += [p['notes'], '']
    extra = os.path.join(OUT, 'one_click_design.md')
    if os.path.exists(extra):
        with open(extra, encoding='utf-8') as f:
            md.append(f.read().rstrip())
    with open(os.path.join(OUT, 'MOVE_MECHANICS.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(md) + '\n')
    print('%d functions, %d moves of %d agents in %d modules -> %s' % (data['functions'], nmoves, data['agents'],
                                                                        data['modules'], OUT))
    print('moves with a function that is not source: %d; without any mechanic: %d'
          % (data['moves_with_a_function_not_source'], data['moves_without_mechanic']))
    for p in data['mechanics']:
        print('  %-24s %5d moves %4d agents %3d modules' % (p['name'], p['moves'], p['agents'], p['modules']))
    for x in data['acmd_supplement']:
        print('  acmd: %-58s %5d scripts %3d modules' % (x['what'], x['scripts'], x['modules']))


if __name__ == '__main__':
    main()
