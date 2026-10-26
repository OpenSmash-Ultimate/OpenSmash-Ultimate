"""Matchmaking for SSBU's Online: Matchmake Extension (109), Match Making (21) and NAT Traversal (3),
which Pia's session jobs call (Quickplay is Pia's random matchmake: AutoMatchmakeWithParam_Postpone).

A matchmake session (a "gathering") is what the game server keeps of a match: owner, host,
participants, the game's rules attributes and application buffer. Players in one gathering may
exchange game traffic through the lobby (NexServer.talk_rules).

Clients are not trusted: only a gathering's participants may act on it, only its owner (or host)
may change it, a player joins only gatherings of its own game build and lobby code that are open
and have room, and nobody brings other players in with them."""
import secrets
import time

from . import rmc
from .streams import StreamIn, StreamOut, clean, datetime_now

# notification types (Notification protocol 14, ProcessNotificationEvent)
N_PARTICIPANT_JOINED, N_PARTICIPANT_LEFT, N_PARTICIPANT_DISCONNECTED, N_PARTICIPATION_ENDED = 3001, 3002, 3007, 3008
N_OWNER_CHANGED, N_GATHERING_UNREGISTERED, N_HOST_CHANGED = 4000, 109000, 110000
RATING_WINDOW = 150              # ranked matching: rating difference allowed at once...
RATING_WIDENING = 25             # ...plus this much for every second the session has waited
MAX_PARTICIPANTS = 16            # players in a gathering at most, whatever it asks for (SSBU: 8 at most)
MAX_ATTRIBS = 16                 # game attributes a gathering may have (NEX's own sessions: 6)
MAX_CRITERIA = 16                # search criteria, and target gatherings, in one AutoMatchmake
MAX_JOIN_MESSAGE = 256           # characters of a join message passed on to the others
MAX_URL = 512                    # a station URL's length
NO_GATHERING = 0x8006000A        # (RendezVous::SessionVoid-like)


def read_levels(s):
    """A structure's class levels: [(version, content bytes)], up to the end of the stream."""
    out = []
    while not s.eof():
        out.append(s.struct())
    return out


class Session:
    """A MatchmakeSession (Gathering + MatchmakeSession fields, NEX 4)."""

    def __init__(self):
        self.gid = 0
        self.owner = self.host = 0
        self.min_participants, self.max_participants = 1, 2
        self.policy = self.policy_arg = self.flags = self.state = 0
        self.description = ""
        self.game_mode = 0
        self.attribs = []
        self.open = True
        self.system_type = 0
        self.app_buffer = b""
        self.progress = 0
        self.session_key = b""
        self.option0 = 0
        self.mm_param = b"\0\4\0\0\0\0\0\0\0"   # MatchmakeParam (structure, empty map), kept as the client sent it
        self.started = 0
        self.user_password = ""
        self.refer_gid = 0
        self.user_password_enabled = self.system_password_enabled = False
        self.codeword = ""
        self.versions = (0, 0)         # version bytes of the Gathering and MatchmakeSession levels
        self.data_level = None         # the nn::nex::Data level's version, when the client sends one
        self.participants = []         # PIDs, in order of joining
        self.part = None               # (game build, lobby code): who may meet
        self.created = 0.0
        self.extra = b""               # MatchmakeSession fields after the codeword (unknown)
        self.system_password = ""

    def __repr__(self):
        return (f"<gathering {self.gid} owner {self.owner} mode {self.game_mode} attribs {self.attribs} "
                f"{len(self.participants)}/{self.max_participants}{'' if self.open else ' closed'}>")

    # ---- parsing -------------------------------------------------------------------
    @classmethod
    def parse(cls, s):
        """From a stream at a MatchmakeSession (as a structure inside another): the Data level (if
        any), Gathering, MatchmakeSession."""
        ms = cls()
        first = s.struct()
        if not first[1]:          # an empty level: nn::nex::Data
            ms.data_level = first[0]
            first = s.struct()
        gv, g = first[0], StreamIn(first[1])
        mv, m = s.struct()
        m = StreamIn(m)
        ms.versions = (gv, mv)
        ms.gid, ms.owner, ms.host = g.u32(), g.pid(), g.pid()
        ms.min_participants, ms.max_participants = g.u16(), g.u16()
        ms.policy, ms.policy_arg, ms.flags, ms.state = g.u32(), g.u32(), g.u32(), g.u32()
        ms.description = g.string()
        ms.game_mode = m.u32()
        ms.attribs = m.list(StreamIn.u32)
        if len(ms.attribs) > MAX_ATTRIBS:
            raise rmc.NexError(rmc.CORE_INVALID_ARGUMENT, f"{len(ms.attribs)} attributes")
        ms.open = m.bool()
        ms.system_type = m.u32()
        ms.app_buffer = m.buffer()
        m.u32()                    # participation count
        ms.progress = m.u8()
        ms.session_key = m.buffer()
        ms.option0 = m.u32()
        start = m.p
        m.struct()                 # MatchmakeParam: a structure holding Map<String, Variant>
        ms.mm_param = m.d[start:m.p]
        ms.started = m.datetime()
        ms.user_password = m.string()
        ms.refer_gid = m.u32()
        ms.user_password_enabled, ms.system_password_enabled = m.bool(), m.bool()
        ms.codeword = m.string() if not m.eof() else ""
        ms.extra = m.read(m.left())   # (fields of a later revision: sent back as they came)
        return ms

    def encode(self, out, now, private=True):
        """private: with the session key and password, which only participants get."""
        if self.data_level is not None:
            out.struct(lambda o: None, self.data_level)

        def gathering(o):
            o.u32(self.gid)
            o.pid(self.owner)
            o.pid(self.host)
            o.u16(self.min_participants)
            o.u16(self.max_participants)
            o.u32(self.policy)
            o.u32(self.policy_arg)
            o.u32(self.flags)
            o.u32(self.state)
            o.string(self.description)

        def session(o):
            o.u32(self.game_mode)
            o.list(self.attribs, StreamOut.u32)
            o.bool(self.open)
            o.u32(self.system_type)
            o.buffer(self.app_buffer)
            o.u32(len(self.participants))
            o.u8(self.progress)
            o.buffer(self.session_key if private else b"")
            o.u32(self.option0)
            o.write(self.mm_param)
            o.datetime(self.started or now)
            o.string(self.user_password if private else "")
            o.u32(self.refer_gid)
            o.bool(self.user_password_enabled)
            o.bool(self.system_password_enabled)
            o.string(self.codeword)
            o.write(self.extra)
        out.struct(gathering, self.versions[0])
        out.struct(session, self.versions[1])


def room(ms):
    """How many players a gathering takes."""
    return min(ms.max_participants, MAX_PARTICIPANTS)


def url_address(url):
    """The address field of a station URL ("prudp:/address=...;port=..."), or None."""
    for field in url.partition(":/")[2].split(";"):
        key, _, value = field.partition("=")
        if key == "address":
            return value
    return None


def read_variant(s):
    t = s.u8()
    return {0: lambda: None, 1: s.s64, 2: s.f64, 3: s.bool, 4: s.string, 5: s.datetime, 6: s.u64}[t]()


class Criteria:
    """A MatchmakeSessionSearchCriteria (NEX 4): attribute filters are strings, "" (any), "v" or "lo,hi"."""

    def __init__(self, levels):
        s = StreamIn(levels[-1][1])
        self.attribs = [self.rule(a) for a in s.list(StreamIn.string)[:MAX_ATTRIBS]]
        self.game_mode = self.rule(s.string())
        self.min_participants = self.rule(s.string())
        self.max_participants = self.rule(s.string())
        self.system_type = self.rule(s.string())
        self.vacant_only = s.bool()
        self.exclude_locked = s.bool()
        self.exclude_non_host = s.bool()
        self.selection = s.u32()
        self.vacant = s.u16() if not s.eof() else 1

    @staticmethod
    def rule(text):
        """A filter as (lo, hi), parsed once; None for any value (also what a filter that is not
        numbers means)."""
        if text == "" or len(text) > 40:
            return None
        lo, _, hi = text.partition(",")
        try:
            return int(lo), int(hi or lo)
        except ValueError:
            return None

    @staticmethod
    def fits(rule, value):
        return rule is None or rule[0] <= value <= rule[1]

    def matches(self, ms, joining):
        if not self.fits(self.game_mode, ms.game_mode) or not self.fits(self.system_type, ms.system_type):
            return False
        if not self.fits(self.min_participants, ms.min_participants) or not self.fits(self.max_participants, ms.max_participants):
            return False
        for rule, value in zip(self.attribs, ms.attribs):
            if not self.fits(rule, value):
                return False
        return len(ms.participants) + max(joining, self.vacant) <= room(ms)

    def __repr__(self):
        return (f"<criteria mode {self.game_mode!r} system {self.system_type!r} attribs {self.attribs} "
                f"vacant {self.vacant} selection {self.selection}>")


class Matchmaking:
    def __init__(self, nex):
        self.nex = nex
        self.sessions = {}       # gid -> Session
        self.of_pid = {}         # pid -> the Session it is in (one at a time)
        self.next_gid = 0x10000
        r = nex.register
        r(109, 40, self.auto_matchmake)
        r(109, 38, self.create_with_param)
        r(109, 39, self.join_with_param)
        r(109, 1, self.close_participation)
        r(109, 2, self.open_participation)
        r(109, 8, self.modify_attribute)
        r(109, 11, self.update_app_buffer)
        r(109, 12, self.update_attributes)
        r(109, 34, self.update_progress)
        r(109, 36, self.generate_system_password)
        r(109, 37, self.clear_system_password)
        r(109, 41, self.find_by_gid)
        r(109, 49, self.find_by_gid)
        r(109, 44, self.update_part)
        r(109, 58, self.mme_58)
        r(21, 41, self.get_session_urls)
        r(21, 42, self.update_session_host)
        r(21, 44, self.migrate_ownership)
        r(21, 2, self.unregister_gathering)
        r(3, 3, self.request_probe_initiation_ext)
        r(3, 4, self.report_nat_result)
        r(3, 5, self.report_nat_properties)
        r(3, 6, self.get_relay_signature_key)
        r(3, 7, self.report_nat_result_detail)
        r(50, 1, self.end_participation)
        r(50, 2, self.get_participants)
        nex.on_disconnect.append(self.gone)
        nex.talk_rules.append(self.together)

    # ---- helpers -------------------------------------------------------------------
    def info(self, *what):
        self.nex.info(*what)

    def session(self, gid):
        ms = self.sessions.get(gid)
        if ms is None:
            raise rmc.NexError(NO_GATHERING, f"no gathering {gid}")
        return ms

    def member(self, conn, gid):
        """The gathering gid, which conn must be in."""
        ms = self.session(gid)
        if conn.pid not in ms.participants:
            raise rmc.NexError(rmc.RENDEZVOUS_NOT_PARTICIPATED, f"not in gathering {gid}")
        return ms

    def managed(self, conn, gid):
        """The gathering gid, which conn must own or host."""
        ms = self.member(conn, gid)
        if conn.pid not in (ms.owner, ms.host):
            raise rmc.NexError(rmc.RENDEZVOUS_PERMISSION_DENIED, f"not the owner of gathering {gid}")
        return ms

    def rating_of(self, pid):
        ranked = getattr(self.nex, "ranked", None)
        return ranked.rating(pid) if ranked else 1500

    def session_rating(self, ms):
        return sum(self.rating_of(p) for p in ms.participants) / len(ms.participants) if ms.participants else 1500

    def together(self, a, b):
        ms = self.of_pid.get(a.pid)
        return ms is not None and ms is self.of_pid.get(b.pid)

    def encode_session(self, ms, private=True):
        out = StreamOut()
        ms.encode(out, datetime_now(), private)
        return out.get()

    def notify(self, pids, kind, source, p1=0, p2=0, text="", p3=0):
        body = StreamOut()

        def event(o):
            o.pid(source)
            o.u32(kind)
            o.u64(p1)
            o.u64(p2)
            o.string(text)
            o.u64(p3)
        body.struct(event)
        for pid in pids:
            conn = self.nex.by_pid.get(pid)
            if conn is not None:
                self.nex.call(conn, 14, 1, body.get())

    def new_session(self, conn, ms):
        ms.gid = self.next_gid
        self.next_gid += 1
        ms.owner = ms.host = conn.pid
        ms.participants = []
        ms.session_key = secrets.token_bytes(32)
        ms.started = datetime_now()
        ms.created = time.monotonic()
        ms.part = conn.user.get("part")
        self.sessions[ms.gid] = ms
        return ms

    def join(self, conn, ms, extra, message=""):
        """conn into ms (and out of any other gathering). The additional participants a client
        names are not taken: nobody brings others into a gathering without their asking."""
        if extra:
            self.info(f"{self.nex.name(conn)} named {len(extra)} more participants for gathering {ms.gid}: ignored")
        self.leave_others(conn, ms)
        if conn.pid not in ms.participants:
            ms.participants.append(conn.pid)
        self.of_pid[conn.pid] = ms
        message = message[:MAX_JOIN_MESSAGE]
        others = [p for p in ms.participants if p != conn.pid]
        # Everyone in hears of the join - the joiner too (Pia's matchmake jobs wait for that
        # notification before they go on) - and the joiner of everyone already in: Pia only
        # connects its mesh to the stations of its gathering's participants, and it knows them
        # from these notifications alone.
        count = len(ms.participants)
        self.notify(ms.participants, N_PARTICIPANT_JOINED, conn.pid, ms.gid, conn.pid, message, count)
        for pid in others:
            self.notify([conn.pid], N_PARTICIPANT_JOINED, pid, ms.gid, pid, "", count)
        self.info(f"{self.nex.name(conn)} joined {ms} (with {others})")

    def leave(self, pid, ms, kind=N_PARTICIPANT_LEFT):
        if pid not in ms.participants:
            return
        ms.participants.remove(pid)
        if self.of_pid.get(pid) is ms:
            del self.of_pid[pid]
        if not ms.participants:
            self.sessions.pop(ms.gid, None)
            self.info(f"gathering {ms.gid} closed")
            return
        if ms.owner != pid:
            self.notify([ms.owner], kind, pid, ms.gid, pid)
        else:
            new = ms.participants[0]
            self.notify(ms.participants, N_OWNER_CHANGED, pid, ms.gid, new)
            ms.owner = new
            if ms.host == pid:
                ms.host = new

    def gone(self, conn):
        ms = self.of_pid.get(conn.pid)
        if ms is not None:
            self.leave(conn.pid, ms, N_PARTICIPANT_DISCONNECTED)

    def leave_others(self, conn, keep=None):
        """A player is in one matchmake session at a time (Pia leaves the old one itself, mostly)."""
        ms = self.of_pid.get(conn.pid)
        if ms is not None and ms is not keep:
            self.leave(conn.pid, ms)

    # ---- Matchmake Extension (109) --------------------------------------------------
    def parse_param_head(self, s):
        """The common start of Auto/CreateMatchmakeSessionParam: (session, additional PIDs, gid for
        participation check, option, join message, participation count, rest of the stream)."""
        _v, raw = s.struct()
        p = StreamIn(raw)
        ms = Session.parse(p)
        extra = p.list(StreamIn.pid)
        check_gid, option, message, count = p.u32(), p.u32(), p.string(), p.u16()
        return ms, extra, check_gid, option, message, count, p

    def auto_matchmake(self, conn, s):
        source, extra, _check, option, message, count, p = self.parse_param_head(s)
        criteria = p.list(lambda q: Criteria(read_levels_one(q)))
        targets = p.list(StreamIn.u32)
        if len(criteria) > MAX_CRITERIA or len(targets) > MAX_CRITERIA:
            raise rmc.NexError(rmc.CORE_INVALID_ARGUMENT, f"{len(criteria)} criteria, {len(targets)} targets")
        joining = max(count, 1 + len(extra))
        self.info(f"AutoMatchmake from {self.nex.name(conn)}: {source} option {option} count {count} "
                  f"criteria {criteria} targets {targets}; buffer {len(source.app_buffer)} B (not shown)")
        part = conn.user.get("part")
        mine = self.rating_of(conn.pid)
        now = time.monotonic()
        candidates = []
        for ms in self.sessions.values():
            if ms.part != part or not ms.open or conn.pid in ms.participants:
                continue
            if targets and ms.gid not in targets:
                continue
            if ms.game_mode != source.game_mode and not criteria:
                continue
            if criteria and not any(c.matches(ms, joining) for c in criteria):
                continue
            if not criteria and len(ms.participants) + joining > room(ms):
                continue
            # ranked: the closest rating, within a window that widens while a session waits
            diff = abs(self.session_rating(ms) - mine)
            if diff > RATING_WINDOW + RATING_WIDENING * (now - ms.created):
                continue
            candidates.append((diff, ms.gid, ms))
        found = min(candidates)[2] if candidates else None
        self.leave_others(conn, found)   # (before a new gathering: the old one may be the best place)
        if found is None:
            found = self.new_session(conn, source)
            self.info(f"{self.nex.name(conn)} (rating {mine}) opened {found}")
        self.join(conn, found, extra, message)
        return self.encode_session(found)

    def create_with_param(self, conn, s):
        source, extra, _check, option, message, count, _p = self.parse_param_head(s)
        self.leave_others(conn)
        ms = self.new_session(conn, source)
        self.info(f"{self.nex.name(conn)} created {ms} (option {option}, count {count})")
        self.join(conn, ms, extra, message)
        return self.encode_session(ms)

    def join_with_param(self, conn, s):
        _v, raw = s.struct()
        p = StreamIn(raw)
        gid = p.u32()
        extra = p.list(StreamIn.pid)
        p.u32()   # gid for participation check
        p.u32()   # option
        p.u8()    # behaviour
        user_password, system_password, message = p.string(), p.string(), p.string()
        ms = self.session(gid)
        if conn.pid not in ms.participants:
            if ms.part != conn.user.get("part"):   # (another build or lobby code: as if it were not there)
                raise rmc.NexError(NO_GATHERING, f"no gathering {gid} for this player")
            if not ms.open:
                raise rmc.NexError(rmc.RENDEZVOUS_SESSION_CLOSED, f"gathering {gid} is closed")
            if len(ms.participants) + 1 > room(ms):
                raise rmc.NexError(rmc.RENDEZVOUS_SESSION_FULL, f"gathering {gid} is full")
        if ms.system_password and system_password != ms.system_password:
            raise rmc.NexError(0x8006000B, "wrong system password")
        if ms.user_password_enabled and user_password != ms.user_password:
            raise rmc.NexError(0x8006000B, "wrong password")
        self.join(conn, ms, extra, message)
        return self.encode_session(ms)

    def close_participation(self, conn, s):
        self.managed(conn, s.u32()).open = False
        return b""

    def open_participation(self, conn, s):
        self.managed(conn, s.u32()).open = True
        return b""

    def modify_attribute(self, conn, s):
        ms, index, value = self.managed(conn, s.u32()), s.u32(), s.u32()
        if index >= MAX_ATTRIBS:
            raise rmc.NexError(rmc.CORE_INVALID_ARGUMENT, f"attribute {index}")
        while len(ms.attribs) <= index:
            ms.attribs.append(0)
        ms.attribs[index] = value
        return b""

    def update_app_buffer(self, conn, s):
        ms = self.managed(conn, s.u32())
        ms.app_buffer = s.buffer()
        return b""

    def update_attributes(self, conn, s):
        ms = self.managed(conn, s.u32())
        attribs = s.list(StreamIn.u32)
        if len(attribs) > MAX_ATTRIBS:
            raise rmc.NexError(rmc.CORE_INVALID_ARGUMENT, f"{len(attribs)} attributes")
        ms.attribs = attribs
        return b""

    def update_progress(self, conn, s):
        ms = self.member(conn, s.u32())
        ms.progress = s.u8()
        return b""

    def generate_system_password(self, conn, s):
        ms = self.managed(conn, s.u32())
        ms.system_password = f"{secrets.randbelow(10 ** 8):08d}"
        ms.system_password_enabled = True
        out = StreamOut()
        out.string(ms.system_password)
        return out.get()

    def clear_system_password(self, conn, s):
        ms = self.managed(conn, s.u32())
        ms.system_password, ms.system_password_enabled = "", False
        return b""

    def find_by_gid(self, conn, s):
        ms = self.session(s.u32())
        return self.encode_session(ms, conn.pid in ms.participants)

    def update_part(self, conn, s):
        _v, raw = s.struct()
        p = StreamIn(raw)
        ms = self.managed(conn, p.u32())
        flags = p.u32()
        attribs, open_, buf, progress = p.list(StreamIn.u32), p.bool(), p.buffer(), p.u8()
        if flags & 0x1 and len(attribs) > MAX_ATTRIBS:
            raise rmc.NexError(rmc.CORE_INVALID_ARGUMENT, f"{len(attribs)} attributes")
        if flags & 0x1:
            ms.attribs = attribs
        if flags & 0x2:
            ms.open = open_
        if flags & 0x4:
            ms.app_buffer = buf
        if flags & 0x8:
            ms.progress = progress
        self.info(f"UpdateMatchmakeSessionPart {ms.gid} flags {flags:#x}")
        return b""

    # SSBU's own method 58 (u32) -> u32 (reply parser main+0x253f30), the first call of Quickplay's
    # "TaskMatchmake" once a fighter is ready (seen: 1). Meaning unknown yet: 0 lets it go on.
    MME_58 = 0

    def mme_58(self, conn, s):
        arg = s.u32()
        self.info(f"{self.nex.name(conn)}: MatchmakeExtension 58 ({arg}) -> {self.MME_58}")
        out = StreamOut()
        out.u32(self.MME_58)
        return out.get()

    # ---- Match Making (21) ----------------------------------------------------------
    def get_session_urls(self, conn, s):
        """The session host's station: one public URL (Pia takes one, or a private and a public
        one, and fails on anything else)."""
        ms = self.session(s.u32())
        host = self.nex.by_pid.get(ms.host)
        urls = [self.nex.station_url(host)] if host else []
        self.nex.debug(f"{self.nex.name(conn)} gets the station of {self.nex.name(host) if host else ms.host}: {urls}")
        out = StreamOut()
        out.list(urls, StreamOut.string)
        return out.get()

    def update_session_host(self, conn, s):
        """(Pia's host migration: the participant that becomes the host says so, and may take the
        ownership with it.)"""
        ms, migrate = self.member(conn, s.u32()), s.bool()
        ms.host = conn.pid
        if migrate:
            ms.owner = conn.pid
        self.notify([p for p in ms.participants if p != conn.pid], N_HOST_CHANGED, conn.pid, ms.gid)
        return b""

    def migrate_ownership(self, conn, s):
        ms = self.managed(conn, s.u32())
        candidates = s.list(StreamIn.pid)
        new = next((p for p in candidates if p in ms.participants), None)
        if new is not None and new != ms.owner:
            old, ms.owner = ms.owner, new
            self.notify(ms.participants, N_OWNER_CHANGED, old, ms.gid, new)
        return b""

    def unregister_gathering(self, conn, s):
        gid = s.u32()
        if gid in self.sessions:   # (one already gone: done)
            ms = self.managed(conn, gid)
            del self.sessions[gid]
            for pid in ms.participants:
                if self.of_pid.get(pid) is ms:
                    del self.of_pid[pid]
            self.notify([p for p in ms.participants if p != conn.pid], N_GATHERING_UNREGISTERED, conn.pid, gid)
        out = StreamOut()
        out.bool(True)
        return out.get()

    # ---- Match Making Ext (50) -----------------------------------------------------
    def end_participation(self, conn, s):
        gid, message = s.u32(), s.string()
        ms = self.sessions.get(gid)
        if ms is not None:
            self.info(f"{self.nex.name(conn)} ends its participation in gathering {gid}"
                      f"{f' ({clean(message)})' if message else ''}")
            self.leave(conn.pid, ms, N_PARTICIPATION_ENDED)
        out = StreamOut()
        out.bool(ms is not None)
        return out.get()

    def get_participants(self, conn, s):
        ms, _only_active = self.session(s.u32()), s.bool()
        out = StreamOut()
        out.list(ms.participants, StreamOut.pid)
        return out.get()

    # ---- NAT Traversal (3) ----------------------------------------------------------
    def request_probe_initiation_ext(self, conn, s):
        """Asks the stations at targets to probe source_url: passed on only to players of the
        caller's own gathering, each once, and only for a source that is the caller's own station."""
        targets = s.list(StreamIn.string)
        source_url = s.string()
        self.nex.debug(f"{self.nex.name(conn)} asks for probes to {targets[:MAX_PARTICIPANTS]}, "
                       f"from {clean(source_url, MAX_URL)}")
        if len(targets) > MAX_PARTICIPANTS or len(source_url) > MAX_URL:
            raise rmc.NexError(rmc.CORE_INVALID_ARGUMENT, f"{len(targets)} probe targets")
        mine = {conn.addr[0]} | {url_address(u) for u in conn.user.get("urls", [])}
        if url_address(source_url) not in mine:
            raise rmc.NexError(rmc.RENDEZVOUS_PERMISSION_DENIED, "probes for another station")
        ms = self.of_pid.get(conn.pid)
        if ms is None:
            return b""
        wanted = {url_address(u) for u in targets} - {None}
        body = StreamOut()
        body.string(source_url)
        for pid in ms.participants:
            other = self.nex.by_pid.get(pid)
            if other is None or other is conn:
                continue
            theirs = {other.addr[0]} | {url_address(u) for u in other.user.get("urls", [])}
            if wanted & theirs:
                self.nex.call(other, 3, 2, body.get())   # InitiateProbe(url to probe)
        return b""

    def report_nat_result(self, conn, s):
        cid, ok, rtt = s.u32(), s.bool(), s.u32()
        self.nex.debug(f"{self.nex.name(conn)} NAT traversal to station {cid}: {'ok' if ok else 'FAILED'}, rtt {rtt}")
        return b""

    def report_nat_result_detail(self, conn, s):
        cid, ok, detail, rtt = s.u32(), s.bool(), s.s32(), s.u32()
        self.nex.debug(f"{self.nex.name(conn)} NAT traversal to station {cid}: {'ok' if ok else 'FAILED'} ({detail}), rtt {rtt}")
        return b""

    def report_nat_properties(self, conn, s):
        mapping, filtering, rtt = s.u32(), s.u32(), s.u32()
        conn.user["natm"], conn.user["natf"] = mapping, filtering
        self.nex.debug(f"{self.nex.name(conn)} NAT mapping {mapping} filtering {filtering} rtt {rtt}")
        return b""

    def get_relay_signature_key(self, conn, s):
        out = StreamOut()
        out.s32(0)                  # relay mode: none (the virtual LAN connects everyone directly)
        out.datetime(datetime_now())
        out.string("")
        out.u16(0)
        out.s32(0)
        out.u32(0x23380901)
        return out.get()


def read_levels_one(s):
    """The class levels of one structure in a list: its own header only (search criteria are a
    single class)."""
    return [s.struct()]
