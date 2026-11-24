#!/usr/bin/env python3
"""OpenSmash releases (v0.5.0 on): the pieces .github/workflows/release.yml puts together. Nothing here comes from
the game, and the audit checks that every file of a release holds none of it either.

    python tools/release/make_release.py version vX.Y.Z[-pre]       the tag's version, checked (prints it; exit 1 if not one)
    python tools/release/make_release.py service OUT.json           the accounts service for the launcher's build, from the
                                                                    environment (OPENSMASH_SUPABASE_URL, OPENSMASH_SUPABASE_KEY,
                                                                    OPENSMASH_SIGNIN_PAGE, OPENSMASH_ACCOUNT_PAGE); never committed
    python tools/release/make_release.py audit FILE...              no Switch game file, key or key line in any file or zip entry
    python tools/release/make_release.py manifest DIST --version V --commit SHA [--channel stable|testing]
                                        [--lobby HOST:PORT] [--min-client-level N]
                                                                    (--lobby: an override, every install of the release
                                                                    plays on that one server; without it the players'
                                                                    server region decides, runtime/net_region.c)
                                                                    DIST/opensmash-release.json and DIST/SHA256SUMS from the
                                                                    files in DIST (OpenSmash.exe, the Linux and macOS
                                                                    launchers OpenSmash-<os>-<arch>.tar.gz,
                                                                    opensmash-runtime-*.zip)

The manifest is what the launcher reads (OpenSmash.Core/Releases.cs): the release's version and commit, every file
with its SHA-256, the official lobby and the lowest online level it lets in (runtime/net.h NET_CLIENT_LEVEL).
Standard library only."""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import sys
import tarfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MANIFEST = "opensmash-release.json"
SEMVER = re.compile(r"^v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-[0-9A-Za-z]+(\.[0-9A-Za-z]+)*)?$")
# what a release may hold, by its file name: the launchers, the runtime's zips (tools/runtime_fetch.py's names).
# The launcher's key in the manifest: OpenSmash.Core/Releases.cs reads "windows-x64"; tools/launcher_fetch.py the others
# (v0.5.0: OpenSmash.Avalonia for Linux and macOS, tools/publish_avalonia.py's archives).
LAUNCHERS = {"OpenSmash.exe": "windows-x64", "OpenSmash-linux-x64.tar.gz": "linux-x64",
             "OpenSmash-macos-arm64.tar.gz": "macos-arm64", "OpenSmash-macos-x64.tar.gz": "macos-x64"}
RUNTIME = re.compile(r"^opensmash-runtime-(windows|linux|macos)-([a-z0-9-]+)\.zip$")


def _audit_rules():
    """SWITCH_TYPES, KEY_NAME, KEY_LINE of .github/scripts/check_no_game_data.py (one list of what is game data)."""
    path = os.path.join(ROOT, ".github", "scripts", "check_no_game_data.py")
    spec = importlib.util.spec_from_file_location("check_no_game_data", path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m.SWITCH_TYPES, m.KEY_NAME, m.KEY_LINE


def version(tag):
    m = SEMVER.match(tag.strip())
    if not m:
        raise SystemExit(f"{tag!r} is not a release version (vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-pre)")
    return tag.strip().lstrip("v")


def audit(files):
    """Problems found: a game file type, a key file, a key line, in a file or in any entry of a zip."""
    types, key_name, key_line = _audit_rules()
    bad = []

    def look(name, data):
        low = name.lower().replace("\\", "/")
        if os.path.splitext(low)[1] in types:
            bad.append(f"{name}: a Switch game file type")
        if key_name.search(low):
            bad.append(f"{name}: a key or ticket file")
        if data is not None and key_line.search(data):
            bad.append(f"{name}: console key lines")

    for f in files:
        with open(f, "rb") as h:
            data = h.read()
        look(os.path.basename(f), data)
        if zipfile.is_zipfile(f):
            with zipfile.ZipFile(f) as z:
                for e in z.infolist():
                    if e.is_dir():
                        continue
                    if ".." in e.filename.replace("\\", "/").split("/") or e.filename.startswith(("/", "\\")):
                        bad.append(f"{f}!{e.filename}: an unsafe path")
                    look(f"{os.path.basename(f)}!{e.filename}", z.read(e) if e.file_size <= 64 << 20 else None)
        elif f.endswith((".tar.gz", ".tgz")):   # (the Linux and macOS launchers)
            try:
                with tarfile.open(f, "r:gz") as t:
                    for e in t.getmembers():
                        name = e.name.replace("\\", "/")
                        if name.startswith("/") or ".." in name.split("/"):
                            bad.append(f"{f}!{e.name}: an unsafe path")
                        if e.issym() or e.islnk() or not (e.isfile() or e.isdir()):
                            bad.append(f"{f}!{e.name}: not a plain file")
                        if e.isfile():
                            with t.extractfile(e) as h:
                                look(f"{os.path.basename(f)}!{e.name}", h.read() if e.size <= 64 << 20 else None)
            except tarfile.TarError as err:
                bad.append(f"{f}: not a readable .tar.gz ({err})")
    return bad


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def manifest(dist, ver, commit, channel="stable", lobby=None, min_level=0):
    if not re.fullmatch(r"[0-9a-f]{40}", commit or ""):
        raise SystemExit("--commit: the release's full commit (40 hex)")
    ver = version(ver)
    if channel == "stable" and "-" in ver:
        raise SystemExit(f"{ver} is a pre-release: it can only go to the testing channel")
    if lobby is not None and not re.fullmatch(r"[A-Za-z0-9.-]{1,200}:\d{1,5}", lobby):
        raise SystemExit("--lobby: HOST:PORT")
    if lobby is not None and (re.fullmatch(r"[0-9.]+", lobby.rsplit(":", 1)[0]) or "." not in lobby):
        raise SystemExit("--lobby: a DNS name, not an address in numbers (the server behind the name can move)")
    m = {"schema": 1, "version": ver, "commit": commit, "channel": channel, "launcher": {}, "runtime": {}}
    if lobby:
        m["lobby"] = lobby
    if min_level:
        m["min_client_level"] = int(min_level)
    sums = []
    for name in sorted(os.listdir(dist)):
        path = os.path.join(dist, name)
        if not os.path.isfile(path) or name in (MANIFEST, "SHA256SUMS") or name.endswith(".sha256"):
            continue
        digest = sha256(path)
        sums.append(f"{digest}  {name}")
        if name in LAUNCHERS:
            m["launcher"][LAUNCHERS[name]] = {"file": name, "sha256": digest}
        elif RUNTIME.match(name):
            os_, isa = RUNTIME.match(name).groups()
            m["runtime"][f"{os_}-{isa}"] = {"file": name, "sha256": digest}
        else:
            raise SystemExit(f"{name}: not a file a release holds (the launchers, the runtime's zips)")
    if "windows-x64" not in m["launcher"]:
        raise SystemExit(f"{dist}: no launcher (OpenSmash.exe)")
    problems = audit([os.path.join(dist, n) for n in sorted(os.listdir(dist)) if os.path.isfile(os.path.join(dist, n))])
    if problems:
        raise SystemExit("game data in the release, not made:\n  " + "\n  ".join(problems))
    with open(os.path.join(dist, MANIFEST), "w", encoding="utf-8", newline="\n") as f:
        json.dump(m, f, indent=2)
        f.write("\n")
    with open(os.path.join(dist, "SHA256SUMS"), "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(sums) + "\n")
    return m


def service(out):
    """The accounts service for the launcher's build (OpenSmash.Core/account_service.json): the values committed in
    that file (the shared OpenSmash / SSBU Arena accounts service; its publishable key is a client key, Row Level
    Security decides what it can do, so every launcher built from source can sign in), each one replaced by its
    OPENSMASH_SUPABASE_URL / _KEY / OPENSMASH_SIGNIN_PAGE / OPENSMASH_ACCOUNT_PAGE variable when that is set.
    A release without an address and a key is refused: its launcher could not sign in."""
    try:
        with open(out, encoding="utf-8") as f:
            s = json.load(f)
    except (OSError, ValueError):
        s = {}
    for k, env in (("url", "OPENSMASH_SUPABASE_URL"), ("publishable_key", "OPENSMASH_SUPABASE_KEY"),
                   ("sign_in_page", "OPENSMASH_SIGNIN_PAGE"), ("account_page", "OPENSMASH_ACCOUNT_PAGE")):
        v = os.environ.get(env, "").strip()
        s[k] = v or str(s.get(k) or "").strip()
    s = {k: s[k] for k in ("url", "publishable_key", "sign_in_page", "account_page")}
    if not s["url"] or not s["publishable_key"]:
        raise SystemExit("accounts service: no address or no publishable key (account_service.json or the repository's variables)")
    if not re.fullmatch(r"https://[A-Za-z0-9.-]+(:\d+)?/?", s["url"]):
        raise SystemExit("OPENSMASH_SUPABASE_URL: an https:// address")
    if s["publishable_key"].startswith(("sb_secret_", "eyJ")):   # (a secret key, or a legacy JWT key that may be service_role)
        raise SystemExit("OPENSMASH_SUPABASE_KEY: that is a secret key, never in a launcher")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(s, f, indent=2)
        f.write("\n")
    return s


def main(argv=None):
    a = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = a.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("version")
    v.add_argument("tag")
    s = sub.add_parser("service")
    s.add_argument("out")
    au = sub.add_parser("audit")
    au.add_argument("files", nargs="+")
    m = sub.add_parser("manifest")
    m.add_argument("dist")
    m.add_argument("--version", required=True)
    m.add_argument("--commit", required=True)
    m.add_argument("--channel", choices=("stable", "testing"), default="stable")
    m.add_argument("--lobby")
    m.add_argument("--min-client-level", type=int, default=0)
    o = a.parse_args(argv)
    if o.cmd == "version":
        print(version(o.tag))
    elif o.cmd == "service":
        s = service(o.out)
        print("accounts service: set")
    elif o.cmd == "audit":
        bad = audit(o.files)
        for b in bad:
            print(b)
        if bad:
            return 1
        print(f"audit: {len(o.files)} file(s), no game data")
    elif o.cmd == "manifest":
        r = manifest(o.dist, o.version, o.commit, o.channel, o.lobby, o.min_client_level)
        print(json.dumps(r, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
