# SPDX-License-Identifier: GPL-3.0-or-later
"""The server region: OpenSmash's official lobbies, one a region, and which of them an install plays on.

The same setup and the same accounts on each (one lobby ticket works on all), but rooms and matchmaking are a lobby's
own: players on another region are not listed. The game's copy of the table and of the rules is runtime/net.h's
NET_REGION_TABLE and runtime/net_region.c, the Windows launcher's OpenSmash.Core/ServerRegion.cs
(tests/test_launcher_defaults.py compares the tables). The files are the same ones:

    config/server_region.txt        "auto" (the default: the region whose lobby answers soonest) or a region's id
    config/server_region_auto.txt   Auto's last pick, "<id> <ms> <unix time>": the game starts on it without waiting

An override (SSBU_OFFICIAL_LOBBY, config/official_lobby.txt) is that server: no regions.

    python3 tools/weblauncher/server_region.py [ROOT]      ask every region's lobby and print the times and the pick
"""
import os
import re
import secrets
import socket
import statistics
import sys
import threading
import time

# (id, the name shown, the lobby: a DNS name and its port, never an address in numbers); the first is the fallback
REGIONS = (
    ("eu", "EU", "eu.lobby.opensmashultimate.com:39421"),
    ("na-east", "NA East", "na.lobby.opensmashultimate.com:39421"),
    ("sa", "South America", "sa.lobby.opensmashultimate.com:39421"),
)
AUTO = "auto"
SETTING_FILE, AUTO_FILE = "server_region.txt", "server_region_auto.txt"
SAMPLES, SAMPLE_WAIT, WAIT = 3, 0.35, 2.5
KEEP_MARGIN_MS = 10   # Auto stays on its last pick unless another region answers this much sooner (no hopping)
IDS = tuple(r[0] for r in REGIONS)


def label(region):
    return next(r[1] for r in REGIONS if r[0] == region)


def lobby(region):
    return next(r[2] for r in REGIONS if r[0] == region)


def by_id(word):
    """The region's id as the table has it; None: not one ("auto" too)."""
    w = (word or "").strip().lower()
    return w if w in IDS else None


def of_lobby(host):
    """The region whose lobby this is ("host:port", or the host alone); None."""
    h = (host or "").lower()
    return next((r[0] for r in REGIONS if h and h in (r[2], r[2].rsplit(":", 1)[0])), None)


def _first_word(path):
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    return line.split()[0]
    except (OSError, UnicodeDecodeError):
        pass
    return ""


def override(root=None):
    """The server an override names (SSBU_OFFICIAL_LOBBY, else config/official_lobby.txt); None: the regions decide."""
    words = [os.environ.get("SSBU_OFFICIAL_LOBBY", "").strip()]
    if root:
        words.append(_first_word(os.path.join(root, "config", "official_lobby.txt")))
    for w in words:
        if re.fullmatch(r"[A-Za-z0-9.-]{1,190}:\d{1,5}", w) and 0 < int(w.rsplit(":", 1)[1]) < 65536:
            return w
    return None


def setting(root):
    """The saved choice: "auto" or a region's id (no file, a word that is no region: auto)."""
    return by_id(_first_word(os.path.join(root, "config", SETTING_FILE))) or AUTO if root else AUTO


def set_setting(root, value):
    value = by_id(value) or AUTO
    path = os.path.join(root, "config", SETTING_FILE)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        lines = []
    at = next((i for i, x in enumerate(lines) if x.strip() and not x.strip().startswith("#")), None)
    if at is None:
        lines.append(value)
    else:
        lines[at] = value
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def median(ms):
    """The median of the samples that are >= 0 (whole ms); -1: none."""
    v = sorted(x for x in ms if x >= 0)
    return -1 if not v else int(statistics.median(v))


def pick(ms, kept=None):
    """The region with the lowest time (ms: {id: ms}, under 0 or missing: no answer), the earlier of two the same;
    `kept` (Auto's last pick) stays unless another answers KEEP_MARGIN_MS sooner. None: none answered."""
    best = None
    for r in IDS:
        if ms.get(r, -1) >= 0 and (best is None or ms[r] < ms[best]):
            best = r
    if best is not None and kept in IDS and ms.get(kept, -1) >= 0 and ms[kept] - ms[best] < KEEP_MARGIN_MS:
        return kept
    return best


def parse_kept(line):
    """config/server_region_auto.txt's line: (the region or None, its time or -1)."""
    w = (line or "").split()
    ms = int(w[1]) if len(w) > 1 and w[1].isdigit() and int(w[1]) < 60000 else -1
    return (by_id(w[0]) if w else None), ms


def kept(root):
    if not root:
        return None, -1
    try:
        with open(os.path.join(root, "config", AUTO_FILE), encoding="utf-8") as f:
            return parse_kept(f.readline())
    except (OSError, UnicodeDecodeError):
        return None, -1


def keep(root, region, ms):
    try:
        os.makedirs(os.path.join(root, "config"), exist_ok=True)
        with open(os.path.join(root, "config", AUTO_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write(f"{region} {ms} {int(time.time())}\n")
    except OSError:
        pass


def ask(region, samples=SAMPLES):
    """One region's lobby asked `samples` times: each answer's time in ms. The question is a JOIN of netplay version 2
    with no lobby ticket ("SN", version, type 1, a nonce, the lobby code, the name "ping", no build): an official lobby
    lets nobody in without one and says so at once (REJECT, type 3), which is all that is timed. A lobby that does let
    it in (WELCOME, 2) is left (LEAVE, 8)."""
    got = []
    host, port = lobby(region).rsplit(":", 1)
    try:
        at = (socket.gethostbyname(host), int(port))
    except OSError:
        return got
    nonce = secrets.token_bytes(8)
    join = b"SN\x02\x01" + nonce + b"smash".ljust(32, b"\0") + b"ping".ljust(32, b"\0") + bytes(40)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        for i in range(samples):
            sent = time.monotonic()
            try:
                s.sendto(join, at)
                while True:
                    left = SAMPLE_WAIT - (time.monotonic() - sent)
                    if left <= 0:
                        break
                    s.settimeout(left)
                    p, src = s.recvfrom(2048)
                    if src[0] == at[0] and len(p) >= 12 and p[:2] == b"SN" and p[4:12] == nonce and p[3] in (2, 3):
                        got.append(round((time.monotonic() - sent) * 1000))
                        if p[3] == 2:
                            s.sendto(b"SN\x02\x08" + nonce, at)
                            return got
                        break
            except OSError:   # (no answer in time, "port unreachable")
                if not got and i:   # (nothing twice: not there)
                    break
    return got


def measure_all(wait=WAIT, asker=None):
    """Every region asked at once, at most `wait` seconds in all: {id: the median in ms, -1 no answer}."""
    asker = asker or ask
    out = {r: -1 for r in IDS}

    def one(r):
        try:
            out[r] = median(asker(r))
        except Exception:   # (one region's failure is "no answer")
            pass
    threads = [threading.Thread(target=one, args=(r,), daemon=True) for r in IDS]
    for t in threads:
        t.start()
    until = time.monotonic() + wait
    for t in threads:
        t.join(max(0.0, until - time.monotonic()))
    return dict(out)


def measure(root, wait=WAIT, asker=None):
    """Measures and keeps Auto's pick for the install (the game starts from it): (region, ms); (None, -1): no answer."""
    ms = measure_all(wait, asker)
    best = pick(ms, kept(root)[0])
    if best is None:
        return None, -1
    if root:
        keep(root, best, ms[best])
    return best, ms[best]


def current(root):
    """The region the install plays on (None: an override's server); Auto with nothing measured: the first."""
    if override(root):
        return None
    chosen = by_id(setting(root))
    return chosen or kept(root)[0] or IDS[0]


def official_lobby(root=None):
    """The official lobby of the install, as the game reads it (runtime/net_region.c)."""
    return override(root) or lobby(current(root))


def official_lobbies(root=None):
    """Every official lobby: the one played on first, then the other regions'; an override: that server alone."""
    other = override(root)
    if other:
        return [other]
    now = official_lobby(root)
    return [now] + [r[2] for r in REGIONS if r[2] != now]


def shown(root):
    """The choice as it is shown: "Auto (EU, 24 ms)", "Auto (EU)", "Auto", "NA East"; an override: its server."""
    other = override(root)
    if other:
        return other
    chosen = by_id(setting(root))
    if chosen:
        return label(chosen)
    region, ms = kept(root)
    return "Auto" if region is None else f"Auto ({label(region)}, {ms} ms)" if ms >= 0 else f"Auto ({label(region)})"


if __name__ == "__main__":
    folder = sys.argv[1] if len(sys.argv) > 1 else None
    times = measure_all()
    for rid in IDS:
        print(f"{label(rid)} ({lobby(rid)}): " + (f"{times[rid]} ms" if times[rid] >= 0 else "no answer"))
    best_region = pick(times, kept(folder)[0])
    print("Auto: " + (label(best_region) if best_region else f"no lobby answered ({label(IDS[0])})"))
    if folder:
        print("this install plays on " + official_lobby(folder) + " (" + shown(folder) + ")")
