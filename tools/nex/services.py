"""The Online menu's other services: Utility (110), DataStore (115), Screening (124), Tournament
(201) - each answered as simply as the game accepts (reply layouts read from the game's own
response parsers; 11.0.0 addresses in the comments).

Every call is logged; a call this file does not know falls through to the server's "not
implemented", which the game reports as error 2306-0103."""
import os

from .rmc import CORE_INVALID_ARGUMENT, NexError
from .server import JsonStore
from .streams import StreamIn, StreamOut

DATASTORE_NOT_FOUND = 0x80690004


def u32(v):
    out = StreamOut()
    out.u32(v)
    return out.get()


# Methods whose reply the game does not read (their response parsers are empty): logged, answered
# with success and nothing else.
NO_REPLY = {
    115: (3, 6, 11, 18, 22, 26, 31, 32, 37, 38, 41, 51, 56, 57, 58, 74, 76, 78, 79, 83, 86, 89, 91, 92, 94, 96, 97),
    124: (1,),
}


POWER_RECORD = 25      # a fighter record's bytes (DataStore 82 / 84): u8, u32, u32, u32, DateTime, u32
MAX_POWER_PLAYERS = 200000


class Powers(JsonStore):
    """Each player's fighter records (DataStore 82 posts one, 84 gives them all back): the GSP the
    game computes and shows, kept for it as the console's server keeps it. "pid" -> {"fighter": hex}."""

    def __init__(self, state_dir):
        super().__init__(os.path.join(state_dir, "nex_powers.json") if state_dir else None)
        self.of_pid = self.load() or {}

    def content(self):
        return self.of_pid

    def put(self, pid, record):
        mine = self.of_pid.get(str(pid))
        if mine is None:
            if len(self.of_pid) >= MAX_POWER_PLAYERS:
                return False
            mine = self.of_pid[str(pid)] = {}
        mine[str(record[0])] = record.hex()
        self.dirty = True
        return True

    def records(self, pid):
        """pid's records, by fighter."""
        return sorted((int(k), bytes.fromhex(v)) for k, v in self.of_pid.get(str(pid), {}).items())


class Services:
    def __init__(self, nex):
        self.nex = nex
        self.powers = Powers(getattr(nex, "state_dir", None))
        r = nex.register
        r(110, 7, self.get_integer_settings)
        r(110, 9, self.utility_9)
        r(110, 10, self.utility_10)
        r(110, 11, self.get_game_events)
        r(115, 8, self.datastore_get_meta)
        r(115, 61, self.datastore_61)
        r(115, 60, self.datastore_60)
        r(115, 84, self.datastore_84)
        r(115, 82, self.datastore_post_power)
        r(115, 21, self.datastore_post_meta_binary)
        r(115, 95, self.datastore_95)
        for protocol, methods in NO_REPLY.items():
            for method in methods:
                r(protocol, method, self.no_reply(protocol, method))

    def no_reply(self, protocol, method):
        from .rmc import name_of

        def handler(conn, s):
            self.nex.info(f"{name_of(protocol, method)} from {self.nex.name(conn)}: "
                          f"{self.nex.shown(conn, s.read(s.left()), 160, protocol)}")
            return b""
        return handler

    # Utility.GetIntegerSettings(u32 index) -> Map<u32, s32> (parser main+0x25ebf0). No settings:
    # the game keeps its defaults.
    def get_integer_settings(self, conn, s):
        index = s.u32()
        self.nex.info(f"{self.nex.name(conn)}: integer settings {index}")
        out = StreamOut()
        out.map({}, StreamOut.u32, StreamOut.s32)
        return out.get()

    # Utility method 9 (SSBU's own) {u32, u32, String country} -> structure {u32, u32, u32} (parser
    # main+0x25e710), asked when Quickplay is chosen (seen: 6, 3, "US").
    UTILITY_9 = (0, 0, 0)

    def utility_9(self, conn, s):
        _v, raw = s.struct()
        p = StreamIn(raw)
        a, b, country = p.u32(), p.u32(), p.string()
        self.nex.info(f"{self.nex.name(conn)}: Utility 9 ({a}, {b}, {country!r}) -> {self.UTILITY_9}")
        out = StreamOut()
        out.struct(lambda o: [o.u32(v) for v in self.UTILITY_9])
        return out.get()

    # Utility method 10 (SSBU's own) {method 9's structure, s32} -> a structure the game starts as
    # {u16 0, s32 -1} (parser main+0x25e8d0; field layout not confirmed - 6 bytes either way).
    def utility_10(self, conn, s):
        raw = s.read(s.left())
        self.nex.info(f"{self.nex.name(conn)}: Utility 10 {self.nex.shown(conn, raw, 64, 110)}")
        out = StreamOut()
        out.struct(lambda o: (o.u16(0), o.s32(-1)))
        return out.get()

    # Utility.GetGameEvents(param {u32, String locale}) -> List<GameEvent> (208-byte objects; parser
    # main+0x25ed70): online events and their banners - none.
    def get_game_events(self, conn, s):
        return u32(0)

    # DataStore.GetMeta(DataStoreGetMetaParam {u64 data id, PersistenceTarget {PID owner, u16 slot},
    # u32 result option, u64 access password}): nothing is stored yet - NotFound, as for a new player.
    def datastore_get_meta(self, conn, s):
        _v, raw = s.struct()
        p = StreamIn(raw)
        data_id = p.u64()
        _tv, target = p.struct()
        t = StreamIn(target)
        owner, slot = t.pid(), t.u16()
        self.nex.info(f"{self.nex.name(conn)}: DataStore.GetMeta data {data_id}, owner {owner} slot {slot}: not found")
        raise NexError(DATASTORE_NOT_FOUND, "no object")

    # DataStore method 61 (SSBU's own; no parameters) -> u32 (parser main+0x28e550), asked right
    # after logging in.
    def datastore_61(self, conn, s):
        return u32(0)

    # DataStore method 60 (no parameters) -> List of 56-byte structures (parser main+0x28e4b0): none.
    def datastore_60(self, conn, s):
        return u32(0)

    # DataStore.PostMetaBinary(DataStorePreparePostParam) -> u64 data id (13.0.4: request main+0x2679f0, its
    # parameter written by main+0x28b9b0; answer read by main+0x28cd10, a u64). Nothing is stored: the player's
    # PID as the id, an object GetMeta then does not find.
    def datastore_post_meta_binary(self, conn, s):
        s.struct()
        self.nex.info(f"{self.nex.name(conn)}: DataStore.PostMetaBinary: data {conn.pid}, not kept")
        out = StreamOut()
        out.u64(conn.pid)
        return out.get()

    # DataStore method 95 (SSBU's own; no parameters, asked at login) -> a structure {bool} (13.0.4: request
    # main+0x272a40, answer read by main+0x28f390): false.
    def datastore_95(self, conn, s):
        out = StreamOut()
        out.struct(lambda o: o.bool(False))
        return out.get()

    # DataStore method 82 (SSBU's own) {u8 fighter, u32, u32, u32, DateTime, u32} -> nothing (13.0.4:
    # request main+0x27255c, parameter written by main+0x284ee0, answer main+0x28ee70 reads nothing). The
    # game posts one of its fighters' records - its GSP (seen: fighter 0x2b, 1460, 123, 1460, 0, 1) - as a
    # match is set up and again after it; kept, and given back by method 84.
    def datastore_post_power(self, conn, s):
        _v, raw = s.struct()
        if len(raw) != POWER_RECORD:
            raise NexError(CORE_INVALID_ARGUMENT, f"a fighter record of {len(raw)} bytes")
        p = StreamIn(raw)
        fighter, a, b, c, when, d = p.u8(), p.u32(), p.u32(), p.u32(), p.u64(), p.u32()
        self.nex.info(f"{self.nex.name(conn)}: DataStore 82 fighter {fighter}: {a} {b} {c} {when:#x} {d}")
        self.powers.put(conn.pid, raw)
        return b""

    # DataStore method 84 (no parameters) -> Map<u8 fighter, the records method 82 posts> (13.0.4: answer
    # main+0x28ee90; the map main+0x298420, a record main+0x285010), then a structure {u32, u32, u32, u64}
    # (main+0x2852f0, not known yet: zeros). Asked at login: the player's GSP of each fighter, as posted.
    def datastore_84(self, conn, s):
        out = StreamOut()
        mine = self.powers.records(conn.pid)
        out.u32(len(mine))
        for fighter, raw in mine:
            out.u8(fighter)
            out.struct(lambda o, raw=raw: o.write(raw))
        out.struct(lambda o: (o.u32(0), o.u32(0), o.u32(0), o.u64(0)))
        return out.get()
