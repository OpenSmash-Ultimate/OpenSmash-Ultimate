#!/usr/bin/env python3
"""HDR's online pop-up (Arena: Battle Arenas - Smash / Options): its Smash starts Quickplay.

HewDraw Remix replaces the main menu's online layer (hdr's ui/script/menu/main/main_menu_layer_online.lua): its Smash
goes to Battle Arenas only (SEQUENCE_ONLINE_MELEE_ROOM), and the game's own Smash panel (Quickplay, Battle Arenas,
background matchmaking) is gone from its layout. On the console Quickplay would match HDR players with the game's own.
OpenSmash's lobbies part players by their mods (runtime/net_relay.c net_part_key: HDR mode's mod set is HDR's
folders), so HDR players meet HDR players of the same HDR only - and Battle Arenas do not work online in OpenSmash
yet (the Local Play menus' arenas do): Smash starts Quickplay (SEQUENCE_ONLINE_MELEE_ANYONE), its help line the
game's own Quickplay one.

(An earlier version added a third row to HDR's layout.arc: its Quickplay row decided Smash's button. Its changed
layout is put back from dump/hdr/menu_original when found.)

HDR's file is changed in place (an HDR update brings its own back: hdr.py does this again), the original kept in
dump/hdr/menu_original for `hdr.py quickplay off`. A file that is not the one this knows is left as it is."""
import os
import shutil

LAYOUT = ("hdr-assets", "ui", "layout", "menu", "main_menu", "main_menu", "layout.arc")
SCRIPT = ("hdr", "ui", "script", "menu", "main", "main_menu_layer_online.lua")
MARK = "-- OpenSmash: HDR's online Smash starts Quickplay (tools/hdr_menu.py)\n"
OLD_MARK = "-- OpenSmash: Quickplay in HDR's online pop-up (tools/hdr_menu.py)\n"
OLD_LAYOUT_PART = b"blyt/online_top_btn3_09.bflyt"   # (the earlier version's layout.arc)


class Error(Exception):
    pass


SCRIPT_EDITS = (
    ('self.button_table[button.ONLINE_MELEE_ROOM] = {\n        parts = "set_parts_btn_00",\n        sequence = SEQUENCE_ONLINE_MELEE_ROOM\n    }',
     'self.button_table[button.ONLINE_MELEE_ROOM] = {\n        parts = "set_parts_btn_00",\n        sequence = SEQUENCE_ONLINE_MELEE_ANYONE\n    }'),
    ('self.footer_table[button.ONLINE_MELEE_ROOM] = {\n        label = "mnu_onl_mel_top_help_room"\n    }',
     'self.footer_table[button.ONLINE_MELEE_ROOM] = {\n        label = "mnu_onl_mel_top_help_any"\n    }'),
)


def patch_script(text):
    """The online layer's script with Smash starting Quickplay, or None when it does already."""
    if text.startswith(MARK):
        return None
    out = text.replace("\r\n", "\n")
    for old, new in SCRIPT_EDITS:
        if out.count(old) != 1:
            raise Error("main_menu_layer_online.lua: not HDR's (" + old.split("\n")[0].strip() + ")")
        out = out.replace(old, new)
    return MARK + out


def _backup(root):
    return os.path.join(root, "dump", "hdr", "menu_original")


def _undo_layout(root, mods, log):
    """The earlier version's layout.arc: HDR's own back (from its backup). Whether it was."""
    lay, keep = os.path.join(mods, *LAYOUT), os.path.join(_backup(root), "layout.arc")
    if not (os.path.isfile(lay) and os.path.isfile(keep)):
        return False
    with open(lay, "rb") as f:
        head = f.read(1 << 16)
    if OLD_LAYOUT_PART not in head:
        return False
    shutil.copyfile(keep, lay)
    os.remove(keep)
    log("HDR's main menu layout: as HDR made it again (the earlier Quickplay row taken out)")
    return True


def apply(root, mods, log=print):
    """Smash starts Quickplay (the original kept). Whether anything changed. Raises Error on a file it doesn't know,
    leaving it as it was."""
    changed = _undo_layout(root, mods, log)
    lua = os.path.join(mods, *SCRIPT)
    if not os.path.isfile(lua):
        return changed
    keep = _backup(root)
    with open(lua, encoding="utf-8", newline="") as f:
        text = f.read()
    if text.startswith(OLD_MARK):   # (the earlier version's: from HDR's own again)
        with open(os.path.join(keep, "main_menu_layer_online.lua"), encoding="utf-8", newline="") as f:
            text = f.read()
        fresh = False
    else:
        fresh = True
    new = patch_script(text)
    if new is None:
        return changed
    os.makedirs(keep, exist_ok=True)
    if fresh:
        shutil.copyfile(lua, os.path.join(keep, "main_menu_layer_online.lua"))
    with open(lua, "w", encoding="utf-8", newline="\n") as f:
        f.write(new)
    log("HDR's online pop-up: Smash starts Quickplay (HDR players only)")
    return True


def restore(root, mods, log=print):
    """HDR's own files back (from before apply). Whether anything was restored."""
    keep = _backup(root)
    done = False
    for name, rel in (("layout.arc", LAYOUT), ("main_menu_layer_online.lua", SCRIPT)):
        src = os.path.join(keep, name)
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(mods, *rel))
            os.remove(src)
            done = True
    if done:
        log("HDR's online pop-up: as HDR made it again")
    return done
