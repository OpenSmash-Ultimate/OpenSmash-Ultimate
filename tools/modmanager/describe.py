"""What a mod is, in a line the mod list shows (My mods): from the files in its folder - which
fighter and costume slots, whether its plugin adds a character of its own to the select screen (the
CSK Collection's API) or changes a fighter's moves, an Assist Trophy, a stage, music... - and which
mods are parts of one download (a moveset and its Assist Trophy, a pack's characters), so the list
can show them together."""
import os
import re

import mods

# the game's names for its fighters (fighter/<name>/...) -> the names people know
FIGHTERS = {
    "mario": "Mario", "donkey": "Donkey Kong", "link": "Link", "samus": "Samus", "samusd": "Dark Samus",
    "yoshi": "Yoshi", "kirby": "Kirby", "fox": "Fox", "pikachu": "Pikachu", "luigi": "Luigi", "ness": "Ness",
    "captain": "Captain Falcon", "purin": "Jigglypuff", "peach": "Peach", "daisy": "Daisy", "koopa": "Bowser",
    "ice_climber": "Ice Climbers", "popo": "Ice Climbers", "nana": "Ice Climbers", "sheik": "Sheik", "zelda": "Zelda",
    "mariod": "Dr. Mario", "pichu": "Pichu", "falco": "Falco", "marth": "Marth", "lucina": "Lucina",
    "younglink": "Young Link", "ganon": "Ganondorf", "mewtwo": "Mewtwo", "roy": "Roy", "chrom": "Chrom",
    "gamewatch": "Mr. Game & Watch", "metaknight": "Meta Knight", "pit": "Pit", "pitb": "Dark Pit",
    "szerosuit": "Zero Suit Samus", "wario": "Wario", "snake": "Snake", "ike": "Ike", "ptrainer": "Pokemon Trainer",
    "pzenigame": "Squirtle", "pfushigisou": "Ivysaur", "plizardon": "Charizard", "diddy": "Diddy Kong",
    "lucas": "Lucas", "sonic": "Sonic", "dedede": "King Dedede", "pikmin": "Olimar", "lucario": "Lucario",
    "robot": "R.O.B.", "toonlink": "Toon Link", "wolf": "Wolf", "murabito": "Villager", "rockman": "Mega Man",
    "wiifit": "Wii Fit Trainer", "rosetta": "Rosalina & Luma", "littlemac": "Little Mac", "gekkouga": "Greninja",
    "palutena": "Palutena", "pacman": "Pac-Man", "reflet": "Robin", "shulk": "Shulk", "koopajr": "Bowser Jr.",
    "duckhunt": "Duck Hunt", "ryu": "Ryu", "ken": "Ken", "cloud": "Cloud", "kamui": "Corrin", "bayonetta": "Bayonetta",
    "inkling": "Inkling", "ridley": "Ridley", "simon": "Simon", "richter": "Richter", "krool": "King K. Rool",
    "shizue": "Isabelle", "gaogaen": "Incineroar", "packun": "Piranha Plant", "jack": "Joker", "brave": "Hero",
    "buddy": "Banjo & Kazooie", "dolly": "Terry", "master": "Byleth", "tantan": "Min Min", "pickel": "Steve",
    "edge": "Sephiroth", "eflame": "Pyra", "elight": "Mythra", "element": "Pyra/Mythra", "demon": "Kazuya", "trail": "Sora",
    "miifighter": "Mii Brawler", "miiswordsman": "Mii Swordfighter", "miigunner": "Mii Gunner",
}
SLOT = re.compile(r"^c(\d{2,3})$")
KIRBY_HAT = "kirby copy hat"
FIGHTERS[KIRBY_HAT] = "Kirby's copy-ability hat"


def slots_text(slots):
    """[0, 1, 2, 120, 121] -> "c00-c02, c120-c121"."""
    runs, out = [], []
    for s in sorted(slots):
        if runs and s == runs[-1][1] + 1:
            runs[-1][1] = s
        else:
            runs.append([s, s])
    for a, b in runs:
        out.append(f"c{a:02d}" if a == b else f"c{a:02d}-c{b:02d}")
    return ", ".join(out)


def scan(folder):
    """{fighter: {costume slots}} of the costumes a mod's files are for, and the top folders it has."""
    costumes, tops = {}, set()
    for root in mods.find_roots(folder) or [folder]:
        for top in os.listdir(root):
            if os.path.isdir(os.path.join(root, top)):
                tops.add(top.lower())
        base = os.path.join(root, "fighter")
        if not os.path.isdir(base):
            continue
        for kind in os.listdir(base):
            for part in ("model", "motion"):   # (fighter/<kind>/model/<body or a weapon>/cNN)
                p = os.path.join(base, kind, part)
                if not os.path.isdir(p):
                    continue
                for sub in os.listdir(p):
                    q = os.path.join(p, sub)
                    if os.path.isdir(q):
                        # (Kirby's copy_<fighter>_* folders: his hat when he copies that fighter)
                        who = KIRBY_HAT if kind == "kirby" and sub.startswith("copy_") else kind
                        for c in os.listdir(q):
                            m = SLOT.match(c)
                            if m:
                                costumes.setdefault(who, set()).add(int(m.group(1)))
    return costumes, tops


def describe(folder):
    """A line saying what the mod is."""
    costumes, tops = scan(folder)
    plugin_files = mods.plugins(folder)
    adds_character = any(_reads(os.path.join(folder, p), b"add_chara_db_entry_info") for p in plugin_files)
    assists = sorted(os.listdir(os.path.join(folder, "assist"))) if os.path.isdir(os.path.join(folder, "assist")) else []
    if not assists:
        for root in mods.find_roots(folder):
            if os.path.isdir(os.path.join(root, "assist")):
                assists = sorted(os.listdir(os.path.join(root, "assist")))
    named = [(FIGHTERS.get(k, k.title()), slots) for k, slots in sorted(costumes.items())]
    on = "; ".join(f"{n} {slots_text(s)}" for n, s in named)
    if adds_character and named:
        return (f"A new character of its own on the select screen, with its own moves (its plugin). It is "
                f"{on}: those costumes, not the fighter's usual ones.")
    if plugin_files and named:
        new = any(s >= 8 for _n, slots in named for s in slots)
        return (f"A moveset: new moves for {', '.join(n for n, _s in named)} (its plugin), on "
                f"{'its own new costumes, ' if new else 'the costumes '}{on}.")
    if assists and not named:
        return (f"An Assist Trophy: changes the {', '.join(a.replace('_', ' ').title() for a in assists)} Assist "
                f"Trophy (model, sounds, art). Not a playable character.")
    if plugin_files:
        return "Code only (its plugin): changes how the game plays, not a fighter's look."
    if named:
        new = any(s >= 8 for _n, slots in named for s in slots)
        return f"{'A skin on new costume slots' if new else 'A skin'}: {on}."
    if tops <= mods.PLUGIN_DATA and tops:
        return "An option for a plugin (the CSK Collection reads it): no files of the game's."
    kinds = [t for t in ("stage", "sound", "stream;", "ui", "effect", "param", "camera", "item") if t in tops]
    words = {"stage": "a stage", "sound": "sounds", "stream;": "music", "ui": "menus and art", "effect": "effects",
             "param": "game parameters", "camera": "cameras", "item": "items"}
    return "Changes " + ", ".join(words[k] for k in kinds) + "." if kinds else ""


def _reads(path, needle):
    try:
        with open(path, "rb") as f:
            return needle in f.read()
    except OSError:
        return False


def groups(entries):
    """{mod name: group key} for mods that are parts of one download: the same GameBanana mod, or (no
    record of where it came from) a shared "<pack> - <part>" name. Only groups of 2+."""
    keys = {}
    for m in entries:
        if m.get("gamebanana"):
            keys[m["name"]] = f"gb:{m['gamebanana']}"
        elif " - " in m["name"]:
            keys[m["name"]] = "name:" + m["name"].split(" - ", 1)[0]
    count = {}
    for k in keys.values():
        count[k] = count.get(k, 0) + 1
    return {n: k for n, k in keys.items() if count[k] > 1}


def part_label(name, title):
    """A part's own name: "Shadow The Hedgehog (Moveset) - Assist Trophy_ Shadow Android" -> "Assist
    Trophy: Shadow Android" (a ":" in a folder name was made "_")."""
    part = name.split(" - ", 1)[1] if " - " in name else (title or name)
    return part.replace("_ ", ": ").replace("_", " ")
