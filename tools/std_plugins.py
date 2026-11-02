#!/usr/bin/env python3
"""The standard Skyline plugins: the community plugins GameBanana's moveset and new-character mods
list as requirements, fetched from their official releases into <tree>/skyline/plugins, where
tools/skyline.py runs them as they are (recompiled like any plugin, before the mods' own) instead
of the copies mods bundle.

Usage:
  std_plugins.py list  [--tree <dir>]            each plugin: version, whether it is fetched, whether it runs here
  std_plugins.py fetch [--tree <dir>] [names...]  download (the ones that run here, or the named ones) and check them

They are downloaded, not shipped in WindowSmash: none of the GitHub ones declares a licence, and
GameBanana's pages don't allow re-uploads. Each download is checked against the SHA-256 recorded
here, so a changed release is noticed (update the entry after checking the new one runs)."""
import hashlib
import io
import os
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# name: (version, url, file in the zip (None: the url is the .nro), sha256 of the .nro, source, status here)
# status: "runs" (tools/skyline.py runs it; the CSK Collection ported to 13.0.5 first, PORTS), "native" (the runtime
# provides it: not fetched by default; lib_paramconfig: its main hangs here, and movesets link its code in anyway),
# "not yet" (doesn't run here yet: docs/PLUGINS-REPORT.md)
PLUGINS = {
    "libone_slot_eff.nro": ("13.0.5 build (2026-09)", "https://gamebanana.com/dl/1806494",
                            "one_slot_eff_13-0-5/atmosphere/contents/01006A800016E000/romfs/skyline/plugins/libone_slot_eff.nro",
                            "ee27ab9735b06b372ae4e6f8cd208bceb42ee488b02bdf3ff1b62b72fa9a0327",
                            "gamebanana.com/mods/549058 (One Slot Effects [Updated])", "runs"),
    "libthe_csk_collection.nro": ("5.0.6", "https://gamebanana.com/dl/1745532",
                                  "atmosphere/contents/01006A800016E000/romfs/skyline/plugins/libthe_csk_collection.nro",
                                  "4d91c3d771b5c846a58ba31e123ff2aca5847614493235e754930a621c19c892",
                                  "gamebanana.com/mods/499008 (The CSK Collection)", "runs"),
    "libsmashline_plugin.nro": ("1.6.8", "https://github.com/HDR-Development/smashline/releases/download/v1.6.8/libsmashline_plugin.nro",
                                None, "a83636e565be0729271aa7c21817da9aa3b8b3e9b3abed7499c1eeae4cb57d5c",
                                "github.com/HDR-Development/smashline", "runs"),
    "libparam_config.nro": ("6.5", "https://github.com/CSharpM7/lib_paramconfig/releases/download/6.5/libparam_config.nro",
                            None, "f014f3143ca8ff0b9b2dcbacdb07acc598c2a76cacd7f754097ea2a8f896889a",
                            "github.com/CSharpM7/lib_paramconfig", "native"),
    "libnro_hook.nro": ("0.4.0", "https://github.com/ultimate-research/nro-hook-plugin/releases/download/v0.4.0/libnro_hook.nro",
                        None, "ca7aec03aee49fea539dcb92569fea5bc5389f77377ba2a30e4d0b7ebd0255e6",
                        "github.com/ultimate-research/nro-hook-plugin", "runs"),
    "libarcropolis.nro": ("4.1.1", "https://github.com/Raytwo/ARCropolis/releases/download/v4.1.1/release.zip",
                          "atmosphere/contents/01006A800016E000/romfs/skyline/plugins/libarcropolis.nro",
                          "92171b2f4dbd7afe9361a55922f01de7f46ffae09f05185c1cea4ab79dba9a1c",
                          "github.com/Raytwo/ARCropolis (GPL-3.0)", "runs"),
}


# plugins built for an older game version: the address map that ports each to 13.0.5 (tools/ports/)
PORTS = {"libthe_csk_collection.nro": "the_csk_collection-5.0.6.txt"}


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def folder(tree):
    return os.path.join(tree, "skyline", "plugins")


def download(url):
    req = urllib.request.Request(url, headers={"User-Agent": "WindowSmash"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def fetch(tree, name, log=print):
    version, url, member, sha, _source, _status = PLUGINS[name]
    data = raw = download(url)
    if member:
        with zipfile.ZipFile(io.BytesIO(raw)) as z:
            data = z.read(member)
    got = hashlib.sha256(data).hexdigest()
    if got != sha:
        raise SystemExit(f"{name}: the download isn't the release checked here ({got[:16]}..., expected {sha[:16]}...): left out")
    ported = ""
    if name in PORTS:   # (built for an older game version: its addresses moved to 13.0.5's, tools/port_plugin.py)
        import port_plugin
        table, _sha = port_plugin.load_map(os.path.join(HERE, "ports", PORTS[name]))
        data, done, missing = port_plugin.port(data, table)
        if missing:
            raise SystemExit(f"{name}: the port's addresses aren't all in it ({', '.join(hex(v) for v in missing)}): left out")
        ported = f", ported to 13.0.5 ({len(done)} addresses)"
    os.makedirs(folder(tree), exist_ok=True)
    with open(os.path.join(folder(tree), name), "wb") as f:
        f.write(data)
    if name == "libarcropolis.nro":   # (its in-game menu's files: sd:/ultimate/arcropolis/resources, without which it stops)
        with zipfile.ZipFile(io.BytesIO(raw)) as z:
            for m in z.namelist():
                if m.startswith("ultimate/arcropolis/resources/") and not m.endswith("/"):
                    out = os.path.join(tree, "dump", "skyline", "sd", *m.split("/"))
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    with open(out, "wb") as f:
                        f.write(z.read(m))
    log(f"{name} {version}: fetched{ported}")


def main():
    tree = os.path.abspath(arg("--tree", ROOT))
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    if cmd == "list":
        for name, (version, _u, _m, _s, source, status) in PLUGINS.items():
            have = os.path.exists(os.path.join(folder(tree), name))
            print(f"{name:28} {version:24} {'fetched' if have else '-':8} {status:8} {source}")
    elif cmd == "fetch":
        named = [a for a in sys.argv[2:] if not a.startswith("--") and a != tree and a in PLUGINS]
        names = named or [n for n, p in PLUGINS.items() if p[5] == "runs"]
        for n in names:
            fetch(tree, n)
        for n in set(os.listdir(folder(tree)) if os.path.isdir(folder(tree)) else []) - set(names):
            if n in PLUGINS and not named and "--keep" not in sys.argv:   # (the set that runs: one no longer listed as running goes)
                os.remove(os.path.join(folder(tree), n))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
