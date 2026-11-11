"""The NEX game server for SSBU's own Online mode: authentication (Ticket Granting) and the secure
server (Secure Connection plus the game's services), behind the netplay lobby's virtual LAN.

The game logs in with its network service account's id token (runtime/hle_env.c:
"ssbu-port.1.<nsa id hex>.<name>"); the server trusts it - the lobby is the gate, and the NSA id
(each install's own random one) is the account's secret. Every call this server does not know is
logged with its parameters and answered "not implemented", which is how the game's real call
order is found.

The log never holds an NSA id or a token, at any verbosity: it names an account by its PID and
account_tag (a short hash of its NSA id), and shows no bytes of the authentication server's
messages or of Ticket Granting's (logging in carries the token) - nor any message that holds the
player's own NSA id.

The authentication server answers Ticket Granting alone, and only for the PID its connection
logged in as; the secure server answers the rest, and only on connections that came in with a
ticket."""
import hashlib
import json
import os
import re
import struct
import time

from . import rmc
from .crypto import TicketIssuer, kerberos_decrypt
from .prudp import FRAGMENT, PrudpServer
from .streams import StreamIn, StreamOut, clean, datetime_now, hexdump

ACCESS_KEY = "9587602b"          # SSBU's NEX access key (game server 0x23380901)
AUTH_PORT, SECURE_PORT = 60000, 60001
NAT_CHECK_PORTS = (10025, 10125)  # Pia's NAT check servers, nncs1 (this server) and nncs2:
NAT_CHECK_SECOND_IP = "10.13.255.254"  # Pia wants nncs2 at another address (runtime/hle_online.c)
PIA_KEEPALIVE_PORT = 33335        # Pia's NAT traversal keeps its socket's mapping open to nncs1 here
PIA_MAGIC = bytes.fromhex("32ab9864")   # the start of every Pia packet
SERVER_PID = 2                   # the secure server's "Quazal Rendez-Vous" principal
BUILD_NAME = "ssbu-port nex 1"
AUTH_PROTOCOLS = (10,)           # what the authentication server answers: Ticket Granting
NSA_ID = re.compile(r"[0-9a-f]{16}\Z")   # (runtime/hle_env.c: "%016llx")
MAX_NAME = 32                    # characters of a player's name kept
MAX_ACCOUNTS = 50000
NEW_ACCOUNTS_RATE, NEW_ACCOUNTS_BURST = 1.0, 60   # new accounts a second, and at once
MAX_URLS = 8                     # station URLs a client may register
MAX_RESPONSE = 255 * FRAGMENT - 64   # largest RMC response (PRUDP fragment ids are a byte)
SAVE_EVERY = 10.0                # seconds between writes of changed accounts and ratings
SAVE_REQUEST_EVERY = 10.0        # (debugging copies of failed requests: one a method this often)


def account_tag(nsa):
    """What the log calls an account: the first 8 hex digits of its NSA id's SHA-256 - enough to
    follow one install through the log, too little to log in with or to get the id back from."""
    return hashlib.sha256(nsa.encode()).hexdigest()[:8]


def nsa_forms(nsa):
    """The ways an NSA id (16 hex digits) can show up in a message's bytes: as text in hex (the
    token's) or decimal (the login's user name), or as a u64 either way round."""
    n = int(nsa, 16)
    return (nsa.encode(), nsa.upper().encode(), str(n).encode(), struct.pack("<Q", n), struct.pack(">Q", n))


class Bucket:
    """A token bucket: rate a second, up to burst saved."""

    def __init__(self, rate, burst):
        self.rate, self.burst, self.level, self.at = rate, burst, burst, None

    def take(self, now, n=1.0):
        if self.at is not None:
            self.level = min(self.burst, self.level + (now - self.at) * self.rate)
        self.at = now
        if self.level < n:
            return False
        self.level -= n
        return True


class JsonStore:
    """Data kept in a JSON file, written at most every SAVE_EVERY seconds while it changes."""

    def __init__(self, path):
        self.path = path
        self.dirty, self.saved = False, None

    def load(self):
        if self.path and os.path.exists(self.path):
            with open(self.path, encoding="utf-8") as f:
                return json.load(f)
        return None

    def flush(self, now, force=False):
        if not self.dirty or not (force or self.saved is None or now - self.saved >= SAVE_EVERY):
            return
        self.saved, self.dirty = now, False
        if not self.path:
            return
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.content(), f, indent=1)
        os.replace(tmp, self.path)


class Accounts(JsonStore):
    """NSA id -> PID and name, kept in a JSON file."""

    def __init__(self, path):
        super().__init__(path)
        self.data = {"next_pid": 1000001, "users": {}}
        self.data.update(self.load() or {})
        self.names = {u["pid"]: u["name"] for u in self.data["users"].values()}
        self.nsa_of = {}         # pid -> NSA id, of the accounts logged in since the server started (for shown)
        self.new_accounts = Bucket(NEW_ACCOUNTS_RATE, NEW_ACCOUNTS_BURST)

    def content(self):
        return self.data

    def login(self, nsa, name, now):
        """The PID of NSA id nsa: a new account if it has none, or None when no new one may be made now."""
        u = self.data["users"].get(nsa)
        if u is None:
            if len(self.data["users"]) >= MAX_ACCOUNTS or not self.new_accounts.take(now):
                return None
            u = self.data["users"][nsa] = {"pid": self.data["next_pid"], "name": name}
            self.data["next_pid"] += 1
            self.dirty = True
        if name and name != u["name"]:
            u["name"] = name
            self.dirty = True
        self.names[u["pid"]] = u["name"]
        self.nsa_of[u["pid"]] = nsa
        return u["pid"]

    def name(self, pid):
        n = self.names.get(pid)
        return clean(n, MAX_NAME) if n is not None else f"pid {pid}"


class NexServer:
    """send(dst_addr, src_port, bytes, src_ip=None) puts a datagram out from this server's address
    (or another virtual one)."""

    def __init__(self, send, host="10.13.0.1", state_dir=None, log=print, verbose=1):
        self.host, self.out, self.say, self.verbose = host, send, log, verbose
        self.state_dir = state_dir
        self.accounts = Accounts(os.path.join(state_dir, "nex_accounts.json") if state_dir else None)
        self.tickets = TicketIssuer()
        self.handlers = {}       # (protocol, method) -> fn(conn, StreamIn) -> bytes
        self.by_pid = {}         # pid -> secure Connection
        self.at_ip = {}          # virtual address -> a secure Connection from there
        self.saved_requests = {}  # (protocol, method) -> when its last failed request was kept
        self.call_id = 1
        self.pending_calls = {}  # call id -> callback(conn, Message) for our requests to clients
        self.now = time.monotonic()
        self.auth = PrudpServer("auth", ACCESS_KEY, lambda a, d: self.out(a, AUTH_PORT, d),
                                self.auth_connect, self.message, self.closed, secure=False, log=self.debug)
        self.secure = PrudpServer("secure", ACCESS_KEY, lambda a, d: self.out(a, SECURE_PORT, d),
                                  self.secure_connect, self.message, self.closed, secure=True, log=self.debug)
        # the same servers over streams (the game's NEX connects with TCP: PRUDP Lite)
        self.auth_lite = PrudpServer("auth/tcp", ACCESS_KEY, lambda a, d: self.stream_send(a, AUTH_PORT, d),
                                     self.auth_connect, self.message, self.closed, secure=False, log=self.debug, lite=True)
        self.secure_lite = PrudpServer("secure/tcp", ACCESS_KEY, lambda a, d: self.stream_send(a, SECURE_PORT, d),
                                       self.secure_connect, self.message, self.closed, secure=True, log=self.debug,
                                       lite=True)
        self.stream_writers = {}  # (addr, port) -> fn(bytes): a client's stream to that server port
        self.pia_ports = {}       # virtual address -> its Pia socket's port (see datagram())
        self.on_disconnect = []  # fn(conn) for services that keep per-player state
        self.talk_rules = []     # fn(conn, conn) -> whether these two may exchange game traffic
        self.part_of = lambda ip: None   # (the lobby's) who may meet whom: game build and lobby code
        self.register(10, 6, self.validate_and_request_ticket_with_param)
        self.register(10, 3, self.request_ticket)
        self.register(11, 1, self.register_urls)
        self.register(11, 4, self.register_urls_ex)
        self.register(11, 3, self.request_urls)
        self.register(11, 6, self.update_urls)
        self.register(11, 7, self.replace_url)
        self.register(11, 8, self.send_report)

    # ---- plumbing ------------------------------------------------------------------
    def debug(self, *what):
        if self.verbose > 1:
            self.say("[nex]", *what)

    def info(self, *what):
        if self.verbose:
            self.say("[nex]", *what)

    def shown(self, conn, data, limit=256, protocol=None):
        """A client's message bytes as they may go into the log: in hex, but only their size when
        they may carry the account's secret - anything to the authentication server or of Ticket
        Granting (logging in sends the token), or anything holding the player's own NSA id."""
        if not conn.server.secure or protocol in AUTH_PROTOCOLS:
            return f"({len(data)} B, not shown: logging in carries the account's token)"
        nsa = self.accounts.nsa_of.get(conn.pid)
        if nsa is not None and any(form in data for form in nsa_forms(nsa)):
            return f"({len(data)} B, not shown: they hold the account's NSA id)"
        return hexdump(data, limit)

    def register(self, protocol, method, fn):
        self.handlers[(protocol, method)] = fn

    def datagram(self, addr, port, data, now, to_ip=None):
        """A datagram from a client (addr: (virtual ip, port)) to this server's UDP port; to_ip is
        the address it was sent to when that is not the server's own (nncs2's)."""
        self.now = now
        if port == AUTH_PORT and to_ip is None:
            self.auth.datagram(addr, data, now)
        elif port == SECURE_PORT and to_ip is None:
            self.secure.datagram(addr, data, now)
        elif port in NAT_CHECK_PORTS:
            self.nat_check(addr, port, data, to_ip)
        elif port == PIA_KEEPALIVE_PORT and to_ip is None and data[:4] == PIA_MAGIC:
            if self.pia_ports.get(addr[0]) != addr[1]:
                self.debug(f"{addr[0]}'s Pia socket is port {addr[1]}")
            self.pia_ports[addr[0]] = addr[1]   # (on the virtual LAN, where it came from is where it is)
        else:
            self.debug(f"datagram from {addr[0]}:{addr[1]} to {to_ip or self.host}:{port}, {len(data)} bytes: {data.hex()}")

    def nat_check(self, addr, port, data, to_ip=None):
        """Pia's NAT check (nn::pia::nex::NatPropertyDetecter): 16-byte requests, a big-endian u32
        type and zeros, 101 and 102 to nncs1 and 103 to nncs2. The answer is the type, then the port
        and address the request came from (on the virtual LAN the player's own: no NAT) and a u32
        0. It comes from where the request went, except 102's, which comes from the other port
        (the test of the NAT's filtering)."""
        if len(data) < 16:
            return
        kind = struct.unpack_from(">I", data, 0)[0]
        ip, src_port = addr
        self.debug(f"NAT check {kind} from {ip}:{src_port} to {to_ip or self.host}:{port}")
        if kind not in (101, 102, 103):
            self.info(f"NAT check of an unknown kind {kind} from {ip}: {data.hex()}")
            return
        from_port = port
        if kind == 102:
            from_port = NAT_CHECK_PORTS[1] if port == NAT_CHECK_PORTS[0] else NAT_CHECK_PORTS[0]
        ip_n = struct.unpack(">I", bytes(int(x) for x in ip.split(".")))[0]
        self.out(addr, from_port, struct.pack(">IIII", kind, src_port, ip_n, 0), to_ip)

    def tick(self, now):
        self.now = now
        for server in self.servers():
            server.tick(now)
        self.flush(now)

    def flush(self, now, force=False):
        """Writes changed accounts and ratings (force: now, as when the server stops)."""
        self.accounts.flush(now, force)
        ranked = getattr(self, "ranked", None)
        if ranked is not None:
            ranked.tick(now)
            ranked.flush(now, force)

    def servers(self):
        return (self.auth, self.secure, self.auth_lite, self.secure_lite)

    # ---- streams (TCP / WebSocket, cut into Lite packets by the transport) -----------
    def stream_open(self, addr, port, write):
        """Whether a stream from addr to port may open: not while one is (a stream never takes
        another's place)."""
        if (addr, port) in self.stream_writers:
            self.info(f"stream from {addr} to port {port}: one is open already")
            return False
        self.stream_writers[(addr, port)] = write
        self.info(f"stream from {addr} to port {port}")
        return True

    def stream_packet(self, addr, port, data, now):
        self.now = now
        server = self.auth_lite if port == AUTH_PORT else self.secure_lite if port == SECURE_PORT else None
        if server is not None:
            server.datagram(addr, data, now)

    def stream_closed(self, addr, port):
        """(Only for a stream stream_open let open.)"""
        self.stream_writers.pop((addr, port), None)
        server = self.auth_lite if port == AUTH_PORT else self.secure_lite
        for conn in list(server.conns.values()):
            if conn.addr == addr:
                server.close(conn, "stream closed")

    def stream_send(self, addr, port, data):
        write = self.stream_writers.get((addr, port))
        if write is not None:
            write(data)

    def secure_url(self):
        return f"prudps:/address={self.host};port={SECURE_PORT};CID=1;PID={SERVER_PID};sid=1;stream=10;type=2"

    def name(self, conn):
        return f"{self.accounts.name(conn.pid)} (pid {conn.pid})" if conn.pid else str(conn.addr)

    # ---- the lobby's questions -----------------------------------------------------
    def may_talk(self, ip_a, ip_b):
        """Whether two players' games may exchange datagrams (Pia, between matched players)."""
        a, b = self.online_at(ip_a), self.online_at(ip_b)
        return a is not None and b is not None and any(fn(a, b) for fn in self.talk_rules)

    def online_at(self, ip):
        return self.at_ip.get(ip)

    def player_gone(self, ip):
        self.pia_ports.pop(ip, None)
        for server in self.servers():
            for conn in list(server.conns.values()):
                if conn.addr[0] == ip:
                    server.close(conn, "left the lobby")

    # ---- connections ---------------------------------------------------------------
    def auth_connect(self, conn, payload):
        return b""

    def secure_connect(self, conn, payload):
        try:
            s = StreamIn(payload)
            ticket, request = s.buffer(), s.buffer()
        except Exception as e:
            self.info(f"secure CONNECT from {conn.addr}: bad payload ({e}; {len(payload)} B, a ticket: not shown)")
            return None
        opened = self.tickets.open_inner(ticket)
        if opened is None:
            self.info(f"secure CONNECT from {conn.addr}: not one of our tickets")
            return None
        pid, session_key = opened
        req = kerberos_decrypt(session_key, request)
        if req is None or len(req) < 16:
            self.info(f"secure CONNECT from {conn.addr}: request does not decrypt")
            return None
        req_pid, _cid, check = struct.unpack_from("<QII", req, 0)
        if req_pid != pid:
            self.info(f"secure CONNECT from {conn.addr}: ticket for {pid}, request from {req_pid}")
            return None
        old = self.by_pid.get(pid)
        if old is not None and old is not conn:
            old.server.close(old, "logged in again")   # (over UDP or a stream, whichever it was)
        conn.session_key, conn.pid = session_key, pid
        conn.user["part"] = self.part_of(conn.addr[0])
        self.by_pid[pid] = conn
        self.at_ip[conn.addr[0]] = conn
        self.info(f"{self.name(conn)} connected to the secure server from {conn.addr}")
        return struct.pack("<II", 4, (check + 1) & 0xFFFFFFFF)

    def closed(self, conn):
        if not conn.server.secure or self.by_pid.get(conn.pid) is not conn:
            return   # (the authentication server keeps nothing of its connections)
        del self.by_pid[conn.pid]
        ip = conn.addr[0]
        if self.at_ip.get(ip) is conn:
            del self.at_ip[ip]
            other = next((c for c in self.by_pid.values() if c.addr[0] == ip), None)
            if other is not None:
                self.at_ip[ip] = other
        why = getattr(conn, "why", "")
        self.info(f"{self.name(conn)} left" + (f" ({why})" if why else ""))
        for fn in self.on_disconnect:
            fn(conn)

    # ---- RMC ------------------------------------------------------------------------
    def message(self, conn, data):
        try:
            m = rmc.decode(data)
        except Exception as e:
            self.info(f"{self.name(conn)}: undecodable RMC message ({e}): {self.shown(conn, data, 64)}")
            return
        if not m.request:
            cb = self.pending_calls.pop(m.call_id, None)
            if self.verbose > 1 or not m.success:
                self.info(f"{self.name(conn)} answered {m!r}")
            if cb:
                cb(conn, m)
            return
        fn = self.handlers.get((m.protocol, m.method))
        what = rmc.name_of(m.protocol, m.method)
        if self.verbose > 2:
            self.save_request(m)
        if conn.server.secure:   # the game's services, for a player who came in with a ticket
            served = m.protocol not in AUTH_PROTOCOLS and conn.pid and self.by_pid.get(conn.pid) is conn
        else:                    # logging in
            served = m.protocol in AUTH_PROTOCOLS
        if not served and fn is not None:
            self.info(f"{what} from {self.name(conn)} on the {conn.server.name} server: not served there")
            self.reply_error(conn, m, rmc.CORE_NOT_IMPLEMENTED)
            return
        if fn is None:
            self.info(f"UNHANDLED {what} from {self.name(conn)}: {self.shown(conn, m.body, 512, m.protocol)}")
            self.reply_error(conn, m, rmc.CORE_NOT_IMPLEMENTED)
            return
        try:
            body = fn(conn, StreamIn(m.body))
        except rmc.NexError as e:
            self.info(f"{what} from {self.name(conn)}: error {e}")
            self.reply_error(conn, m, e.code)
            return
        except Exception as e:
            self.info(f"{what} from {self.name(conn)} failed: {e!r}; parameters {self.shown(conn, m.body, 512, m.protocol)}")
            self.save_request(m)
            self.reply_error(conn, m, rmc.CORE_INVALID_ARGUMENT)
            return
        if body is None:   # (the handler answers later, itself)
            return
        if len(body) > MAX_RESPONSE:
            self.info(f"{what} from {self.name(conn)}: an answer of {len(body)} B is too large to send")
            self.reply_error(conn, m, rmc.CORE_BUFFER_OVERFLOW)
            return
        if self.verbose > 1:
            self.info(f"{what} from {self.name(conn)}: {len(m.body)} B in, {len(body)} B out")
        conn.server.send_message(conn, rmc.response(m.protocol, m.call_id, m.method, body), self.now)

    def save_request(self, m):
        """(debugging) The parameters of a request this server could not take, for replaying offline:
        <state>/requests/<protocol>-<method>.bin (one a method every SAVE_REQUEST_EVERY seconds).
        Never Ticket Granting's: logging in carries the account's token, and these get passed on."""
        if not self.state_dir or m.protocol in AUTH_PROTOCOLS:
            return
        key = (m.protocol, m.method)
        last = self.saved_requests.get(key)
        if last is not None and self.now - last < SAVE_REQUEST_EVERY or last is None and len(self.saved_requests) >= 256:
            return
        self.saved_requests[key] = self.now
        folder = os.path.join(self.state_dir, "requests")
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, f"{m.protocol}-{m.method}.bin"), "wb") as f:
            f.write(m.body)

    def reply_error(self, conn, m, code):
        conn.server.send_message(conn, rmc.error(m.protocol, m.call_id, code), self.now)

    def reply(self, conn, m, body):
        conn.server.send_message(conn, rmc.response(m.protocol, m.call_id, m.method, body), self.now)

    def call(self, conn, protocol, method, body, callback=None):
        """Our request to a client (notifications: the client never answers those)."""
        cid = self.call_id
        self.call_id = self.call_id % 0x7FFFFFFF + 1
        if callback:
            self.pending_calls[cid] = callback
        conn.server.send_message(conn, rmc.request(protocol, cid, method, body), self.now)

    # ---- Ticket Granting (10) -------------------------------------------------------
    def validate_and_request_ticket_with_param(self, conn, s):
        _v, raw = s.struct()
        p = StreamIn(raw)
        platform = p.u32()
        _username = p.string()   # (can be the NSA id itself - nex.selftest's is, in decimal - so never logged)
        holder, data = p.anydata()
        token = ""
        if holder == "AuthenticationInfo":
            d = StreamIn(data)
            levels = []
            while not d.eof():
                levels.append(d.struct()[1])
            a = StreamIn(levels[-1]) if levels else StreamIn(b"")
            token = a.string()
        parts = token.split(".", 3)
        if len(parts) != 4 or parts[0] != "ssbu-port" or not NSA_ID.match(parts[2]):
            # (its shape only: the token holds the secret, and so can the user name)
            what = ("not a port token" if parts[0] != "ssbu-port" else
                    f"a port token of {len(parts)} fields" if len(parts) != 4 else "a port token with a bad NSA id")
            self.info(f"login refused from {conn.addr}: {what} ({len(token)} characters; holder {clean(holder, 40)!r})")
            raise rmc.NexError(rmc.RENDEZVOUS_INVALID_USERNAME, "not a port token")
        nsa = parts[2]
        name = "".join(ch for ch in parts[3][:MAX_NAME] if ch.isprintable())
        pid = self.accounts.login(nsa, name, self.now)
        if pid is None:
            self.info(f"login refused from {conn.addr}: no new accounts now ({len(self.accounts.data['users'])} kept; "
                      f"account {account_tag(nsa)})")
            raise rmc.NexError(rmc.RENDEZVOUS_LIMIT_EXCEEDED, "no new accounts now")
        conn.pid = pid
        ticket, source_key, _session = self.tickets.issue(pid, SERVER_PID, datetime_now())
        rest = p.read(p.left()) if p.left() else b""
        self.info(f"login: {clean(name)} (account {account_tag(nsa)}, platform {platform}) -> pid {pid}; "
                  f"rest of the parameters {hexdump(rest, 64)}")
        out = StreamOut()

        def result(o):
            o.pid(pid)
            o.buffer(ticket)
            o.string(self.secure_url())
            o.datetime(datetime_now())
            o.string(BUILD_NAME)
            o.string(source_key)
        out.struct(result)
        return out.get()

    def request_ticket(self, conn, s):
        """A new ticket, for the PID this connection logged in as (and no other)."""
        source, target = s.pid(), s.pid()
        if not conn.pid or source != conn.pid:
            self.info(f"ticket for pid {source} refused to {self.name(conn)} at {conn.addr}")
            raise rmc.NexError(rmc.CORE_ACCESS_DENIED, "not logged in as that PID")
        ticket, source_key, _session = self.tickets.issue(source, target, datetime_now())
        out = StreamOut()
        out.result(rmc.SUCCESS)
        out.buffer(ticket)
        out.string(source_key)
        return out.get()

    # ---- Secure Connection (11) -----------------------------------------------------
    def station_url(self, conn):
        """A client's station as the server tells it, to itself and to the players it matches:
        its registered URL, from its virtual address (on the virtual LAN nobody is behind a NAT),
        marked public (type 3), with its PID and RVCID (the connection id Register gave it) and
        the NAT it reported. Not its CID: Pia makes that up for its station, and takes the one in
        the station's own probes when the URL has none (a wrong one fails the mesh join). The port
        is its Pia socket's, once Pia's NAT traversal has kept it open to nncs1 (NEX's own URL has
        no port: it talks over a stream)."""
        ip, port = conn.addr
        urls = conn.user.get("urls") or []
        fields = {"address": ip, "port": str(port), "natf": "0", "natm": "0", "pmp": "0", "sid": "15", "upnp": "0"}
        scheme = "prudp"
        if urls:
            scheme, _, rest = urls[0].partition(":/")
            fields.update(dict(f.split("=", 1) for f in rest.split(";") if "=" in f))
        cid = conn.user.get("cid") or conn.pid & 0xFFFFFFFF
        fields.update(address=ip, type="3", PID=str(conn.pid), RVCID=str(cid),
                      natm=str(conn.user.get("natm", 0)), natf=str(conn.user.get("natf", 0)))
        if ip in self.pia_ports:
            fields["port"] = str(self.pia_ports[ip])
        return f"{scheme}:/" + ";".join(f"{k}={v}" for k, v in fields.items())

    def _register(self, conn, urls):
        if len(urls) > MAX_URLS or any(len(u) > 512 for u in urls):
            raise rmc.NexError(rmc.CORE_INVALID_ARGUMENT, f"{len(urls)} URLs")
        conn.user["urls"] = urls
        conn.user["cid"] = conn.pid & 0xFFFFFFFF
        public = self.station_url(conn)
        conn.user["public_url"] = public
        self.info(f"{self.name(conn)} registered {urls} -> {public}")
        out = StreamOut()
        out.result(rmc.SUCCESS)
        out.u32(conn.user["cid"])
        out.string(public)
        return out.get()

    def register_urls(self, conn, s):
        return self._register(conn, s.list(StreamIn.string))

    def register_urls_ex(self, conn, s):
        urls = s.list(StreamIn.string)
        name, _data = s.anydata()
        self.debug(f"RegisterEx custom data {clean(name)}")
        return self._register(conn, urls)

    def request_urls(self, conn, s):
        _cid, pid = s.u32(), s.pid()
        target = self.by_pid.get(pid)
        out = StreamOut()
        urls = [self.station_url(target)] if target and target.user.get("public_url") else []
        out.bool(bool(urls))
        out.list(urls, StreamOut.string)
        return out.get()

    def update_urls(self, conn, s):
        urls = s.list(StreamIn.string)
        if len(urls) > MAX_URLS or any(len(u) > 512 for u in urls):
            raise rmc.NexError(rmc.CORE_INVALID_ARGUMENT, f"{len(urls)} URLs")
        conn.user["urls"] = urls
        self.debug(f"{self.name(conn)} URLs now {urls}")
        return b""

    def replace_url(self, conn, s):
        old, new = s.string(), s.string()
        self.debug(f"{self.name(conn)} replaces URL {clean(old, 512)} with {clean(new, 512)}")
        return b""

    def send_report(self, conn, s):
        return b""


def make_server(send, host="10.13.0.1", state_dir=None, log=print, verbose=1):
    """The game server with every service it has."""
    from .arenas import Arenas
    from .matchmaking import Matchmaking
    from .ranked import Ranked
    from .services import Services
    nex = NexServer(send, host, state_dir, log, verbose)
    nex.ranked = Ranked(nex, state_dir)
    nex.matchmaking = Matchmaking(nex)
    nex.arenas = Arenas(nex, nex.matchmaking)
    nex.services = Services(nex)
    return nex
