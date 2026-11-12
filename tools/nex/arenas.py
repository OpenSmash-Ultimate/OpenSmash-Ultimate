"""Battle Arenas (Online > Smash > Battle Arenas): SSBU's own MatchmakeExtension (109) methods around a
gathering the game makes with CreateMatchmakeSessionWithParam (game mode 7, matchmaking.py).

An arena is a matchmake session like Quickplay's, with an arena ID: a short code players type to find it.
Reply layouts read from the game's own response parsers (13.0.4, the protocol's handler table
main+0x52a6b78 is filled by each method's call function with the parser's address):
  55 (u32 gid) -> structure {String arena ID, DateTime}        parser main+0x253d50
  56 (?) -> u32                                                 parser main+0x253e90
  52 BrowseMatchmakeSessionNoHolderNoResultRange (search criteria) -> List<MatchmakeSession>   parser main+0x253bb0
     (Battle Arenas > Find an arena > Advanced search)

Clients are not trusted: only a gathering's participants get its arena ID, and an ID names a gathering only while
it exists and is open to the asker's build and lobby code."""
import secrets

from . import rmc
from .matchmaking import NO_GATHERING, Criteria
from .streams import StreamIn, StreamOut, clean, datetime_now

ID_LETTERS = "0123456789ABCDEFGHJKLMNPQRSTUVWXY"   # (no I, O, Z: read as 1, 0, 2)
ID_LENGTH = 5
MAX_ID_TEXT = 32
MAX_FOUND = 50     # arenas in one search's answer at most (the game asks for 50)


class Arenas:
    def __init__(self, nex, mm):
        self.nex, self.mm = nex, mm
        self.id_of = {}    # gid -> arena ID
        self.gid_of = {}   # arena ID -> gid
        r = nex.register
        r(109, 55, self.arena_id)
        r(109, 56, self.mme_56)
        r(109, 52, self.browse)
        r(109, 51, self.friends)

    def name(self, conn):
        return self.nex.name(conn)

    def new_id(self, gid):
        if len(self.id_of) > 4096:   # (the IDs of gatherings gone)
            for old in [g for g in self.id_of if g not in self.mm.sessions]:
                self.forget(old)
        for _ in range(1000):
            code = "".join(secrets.choice(ID_LETTERS) for _ in range(ID_LENGTH))
            old = self.gid_of.get(code)
            if old is None or old not in self.mm.sessions:
                break
        self.forget(gid)
        self.id_of[gid], self.gid_of[code] = code, gid
        return code

    def forget(self, gid):
        code = self.id_of.pop(gid, None)
        if code is not None and self.gid_of.get(code) == gid:
            del self.gid_of[code]

    def gid_for(self, code, part):
        """The gathering an arena ID names for a player of that game build and lobby code, or None (gone, or of
        another build or lobby code)."""
        gid = self.gid_of.get(code.strip().upper())
        ms = self.mm.sessions.get(gid)
        if ms is None or ms.part != part:
            return None
        return gid

    # 55: the arena ID of a gathering its participant is in (asked right after creating an arena).
    def arena_id(self, conn, s):
        gid = s.u32()
        ms = self.mm.member(conn, gid)
        code = self.id_of.get(gid)
        if code is None or self.gid_of.get(code) != gid:
            code = self.new_id(gid)
        self.nex.info(f"{self.name(conn)}: arena ID of gathering {gid} ({ms}): {code}")
        out = StreamOut()
        out.struct(lambda o: (o.string(code), o.datetime(datetime_now())))
        return out.get()

    # 56 (SSBU's own) -> u32: logged; the request's layout is not read yet (an arena ID -> its gathering, it seems).
    def mme_56(self, conn, s):
        raw = s.read(s.left())
        gid = 0
        try:
            text = StreamIn(raw).string()
            found = self.gid_for(text[:MAX_ID_TEXT], conn.user.get("part"))
            gid = found or 0
        except Exception:   # (another layout: logged below)
            text = None
        self.nex.info(f"{self.name(conn)}: MatchmakeExtension 56 {self.nex.shown(conn, raw, 64, 109)}"
                      f"{f' (arena ID {clean(text, MAX_ID_TEXT)!r})' if text is not None else ''} -> {gid}")
        if text is not None and not gid:
            raise rmc.NexError(NO_GATHERING, f"no arena {clean(text, MAX_ID_TEXT)!r}")
        out = StreamOut()
        out.u32(gid)
        return out.get()

    # 51 FindMatchmakeSessionByParticipant: the arenas of the asker's friends (Battle Arenas, the friends' list). There are no friends here:
    # an empty list (the parser, main+0x253ae0, reads List<a wrapped MatchmakeSession>); the request is not read.
    def friends(self, conn, s):
        s.read(s.left())
        out = StreamOut()
        out.u32(0)
        return out.get()

    # 52: the arenas a search finds - open gatherings of the asker's build and lobby code that fit the criteria and
    # have room, the fullest first (where a match is likelier soon); without their session keys or passwords.
    def browse(self, conn, s):
        _v, raw = s.struct()
        criteria = Criteria([(0, raw)])
        part = conn.user.get("part")
        found = [ms for ms in self.mm.sessions.values()
                 if ms.part == part and ms.open and conn.pid not in ms.participants and criteria.matches(ms, 1)]
        found.sort(key=lambda ms: (-len(ms.participants), ms.gid))
        found = found[:MAX_FOUND]
        self.nex.info(f"{self.name(conn)} searches arenas: {criteria} -> {[ms.gid for ms in found]}")
        out = StreamOut()
        out.u32(len(found))
        for ms in found:
            out.write(self.mm.encode_session(ms, private=False))
        return out.get()
