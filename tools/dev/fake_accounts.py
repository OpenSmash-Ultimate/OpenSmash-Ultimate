#!/usr/bin/env python3
"""A stand-in for the accounts service on 127.0.0.1 (tests of the launcher's sign-in, lobby ticket, crash report and
performance summary; never a real service, no real account, nothing leaves this PC).

Usage: python tools/dev/fake_accounts.py [--port 54321] [--secret-file <lobby ticket secret>] [--name Tester] [--log <file>]

The launcher is pointed at it with OPENSMASH_SUPABASE_URL=http://127.0.0.1:<port> and OPENSMASH_SUPABASE_KEY=test-key.
What it answers (the calls OpenSmash.Core makes):
  GET  /auth/v1/authorize?provider=discord&redirect_to=..&code_challenge=..   -> 302 to redirect_to?code=<code>
       (the provider's page is skipped: the "player" is always the test account)
  POST /auth/v1/token?grant_type=pkce            {auth_code, code_verifier}  -> a session (the verifier is checked, S256)
  POST /auth/v1/token?grant_type=refresh_token   {refresh_token}             -> a new session (the old token stops working)
  POST /auth/v1/logout
  GET  /rest/v1/profiles?...                                                  -> [{"username": <name>, "tag": 1234}]
  POST /rest/v1/rpc/lobby_ticket                                              -> "<uid>|<name>|<expiry>.<hmac>" signed with
       --secret-file (the same file tools/ssbu_relay.py --ticket-secret-file reads: its tickets are accepted there)
  POST /rest/v1/rpc/crash_report_add, /rest/v1/rpc/perf_session_add           -> 1 (the body goes to the log)
  POST /storage/v1/object/crashes/<name>                                      -> {"Key": ...} (its size goes to the log)
Every request needs the header apikey; the calls of a signed-in player their bearer token."""
import argparse
import base64
import hashlib
import hmac
import json
import os
import sys
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UID = "00000000-0000-4000-8000-0000000000aa"
state = {"challenges": {}, "refresh": set(), "access": set(), "n": 0}
opts = None


def log(text):
    line = time.strftime("%H:%M:%S ") + text
    print(line, flush=True)
    if opts.log:
        with open(opts.log, "a", encoding="utf-8") as f: f.write(line + "\n")


def session():
    state["n"] += 1
    a, r = "access-%d-%s" % (state["n"], os.urandom(6).hex()), "refresh-%d-%s" % (state["n"], os.urandom(6).hex())
    state["access"].add(a); state["refresh"].add(r)
    return {"access_token": a, "refresh_token": r, "expires_in": 3600, "token_type": "bearer",
            "user": {"id": opts.uid, "email": "", "identities": [{"provider": "discord", "id": "123456789012345678"}]}}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def send(self, code, body, ctype="application/json", headers=None):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        for k, v in (headers or {}).items(): self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def bearer_ok(self):
        return self.headers.get("Authorization", "")[7:] in state["access"]

    def do_GET(self):
        u = urllib.parse.urlparse(self.path); q = urllib.parse.parse_qs(u.query)
        if u.path == "/auth/v1/authorize":
            to, ch = q.get("redirect_to", [""])[0], q.get("code_challenge", [""])[0]
            if not to.startswith("http://127.0.0.1:") or not ch or q.get("provider", [""])[0] != "discord":
                log("authorize REFUSED: " + self.path[:200]); return self.send(400, {"error": "bad authorize"})
            code = os.urandom(12).hex()
            state["challenges"][code] = ch
            log("authorize: provider discord, PKCE challenge given, back to the launcher's loopback address")
            return self.send(302, b"", "text/plain", {"Location": to + "?code=" + code})
        if not self.headers.get("apikey"): return self.send(401, {"error": "no apikey"})
        if u.path == "/rest/v1/profiles":
            if not self.bearer_ok(): return self.send(401, {"error": "no session"})
            log("profile read")
            return self.send(200, [{"username": opts.name, "tag": 1234}])
        self.send(404, {"error": "not found"})

    def do_POST(self):
        u = urllib.parse.urlparse(self.path); q = urllib.parse.parse_qs(u.query)
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        if not self.headers.get("apikey"): return self.send(401, {"error": "no apikey"})
        if u.path == "/auth/v1/token":
            try: body = json.loads(raw or b"{}")
            except ValueError: body = {}
            grant = q.get("grant_type", [""])[0]
            if grant == "pkce":
                ch = state["challenges"].pop(body.get("auth_code", ""), None)
                want = base64.urlsafe_b64encode(hashlib.sha256(body.get("code_verifier", "").encode()).digest()).rstrip(b"=").decode()
                if ch is None or ch != want: log("token (pkce) REFUSED: the verifier does not match"); return self.send(400, {"error": "invalid_grant"})
                log("token (pkce): the verifier matches, session given")
                return self.send(200, session())
            if grant == "refresh_token":
                rt = body.get("refresh_token", "")
                if rt not in state["refresh"]: log("token (refresh) REFUSED"); return self.send(400, {"error": "invalid_grant"})
                state["refresh"].discard(rt)
                log("token (refresh): session renewed")
                return self.send(200, session())
            return self.send(400, {"error": "unsupported grant"})
        if not self.bearer_ok(): return self.send(401, {"error": "no session"})
        if u.path == "/auth/v1/logout":
            log("logout"); return self.send(204, b"")
        if u.path == "/rest/v1/rpc/lobby_ticket":
            secret = open(opts.secret_file, "rb").read().strip() if opts.secret_file else b"test-secret-not-a-real-one"
            body = "%s|%s|%d" % (opts.uid, opts.name, int(time.time()) + 600) + ("|d=" + opts.discord if opts.discord else "")
            log("lobby ticket given (10 min)")
            return self.send(200, body + "." + hmac.new(secret, body.encode(), hashlib.sha256).hexdigest())
        if u.path in ("/rest/v1/rpc/crash_report_add", "/rest/v1/rpc/perf_session_add"):
            log("%s: %s" % (u.path.rsplit("/", 1)[1], raw.decode("utf-8", "replace")[:4000]))
            return self.send(200, 1)
        if u.path.startswith("/storage/v1/object/crashes/"):
            log("upload %s: %d bytes" % (u.path[len("/storage/v1/object/"):], len(raw)))
            return self.send(200, {"Key": u.path[len("/storage/v1/object/"):]})
        self.send(404, {"error": "not found"})


def main():
    global opts
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=54321)
    ap.add_argument("--secret-file")
    ap.add_argument("--name", default="Tester")
    ap.add_argument("--uid", default=UID, help="the test account's id (two instances: two accounts, e.g. a game pair)")
    ap.add_argument("--discord", help="a Discord id put in the ticket as |d=<id> (the board links the account to it)")
    ap.add_argument("--log")
    opts = ap.parse_args()
    srv = ThreadingHTTPServer(("127.0.0.1", opts.port), H)
    log("fake accounts service on http://127.0.0.1:%d (test only)" % opts.port)
    try: srv.serve_forever()
    except KeyboardInterrupt: pass


if __name__ == "__main__":
    sys.exit(main())
