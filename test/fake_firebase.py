#!/usr/bin/env python3
"""Fake Firebase Realtime Database REST server for the Linux test harness.

Implements just enough of the REST API that net.c uses:
  GET    <path>.json            (+ X-Firebase-ETag -> ETag header)
  PUT    <path>.json            (if-match: "null" or etag -> 200/412)
  PATCH  <path>.json            (deep merge of flat "a/b": value keys)
  DELETE <path>.json
  POST   <path>/chat.json       (push key -> {"name": "-KEY"})
  GET    .../chat.json?orderBy="$key"&limitToLast=32
  GET    .../chat.json?orderBy="$key"&limitToFirst=16

Plus the Firebase Auth endpoints the secured mode uses:
  POST   /v1/accounts:signUp?key=K           (email/password -> idToken)
  POST   /v1/accounts:signInWithPassword     (EMAIL_NOT_FOUND/INVALID_PASSWORD)
  POST   /v1/token?key=K                     (refresh_token -> id_token)
  GET    /__stats.json                       (счётчики для проверок в тестах)
"""
import json
import re
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

DB = {"rooms": {"main": {}}}
LOCK = threading.Lock()
ETAG_COUNTER = [0]

# Аккаунты Firebase Auth (email -> password) и выданные refresh-токены.
AUTH = {}
REFRESH = {}
TOKEN_COUNTER = [0]

# Счётчики: тест проверяет, что вход в Firebase был, а к базе клиент ходил
# только с ?auth= (иначе правила firebase.rules.json отрубили бы его).
STATS = {"sign_up": 0, "sign_in": 0, "token_refresh": 0,
         "rtdb_auth": 0, "rtdb_no_auth": 0}

# Seed: a remote player in slot 1 so the game exercises read_remotes/draw nick.
DB["rooms"]["main"]["players"] = {
    "1": {"uid": "00000000000000aa", "nick": "BotRival", "x": 0.5, "y": 0.5,
          "angle": 1.2, "hp": 10, "alive": 1, "seq": 100,
          "px": 0.5, "py": 0.5, "pdx": 1, "pdy": 0, "punch": 100, "cls": 0},
}


def _issue_tokens(email):
    TOKEN_COUNTER[0] += 1
    id_token = "idtok-%s-%d" % (email.split("@")[0], TOKEN_COUNTER[0])
    refresh = "refreshtok-%d" % TOKEN_COUNTER[0]
    REFRESH[refresh] = email
    return id_token, refresh


def deep_get(node, parts):
    for p in parts:
        if not isinstance(node, dict) or p not in node:
            return None
        node = node[p]
    return node


def deep_set(node, parts, value):
    for p in parts[:-1]:
        nxt = node.get(p)
        if not isinstance(nxt, dict):
            nxt = {}
            node[p] = nxt
        node = nxt
    node[parts[-1]] = value


def deep_del(node, parts):
    for p in parts[:-1]:
        if not isinstance(node, dict) or p not in node:
            return False
        node = node[p]
    if isinstance(node, dict) and parts[-1] in node:
        del node[parts[-1]]
        return True
    return False


import hashlib

def node_etag(node):
    digest = hashlib.sha1(repr(node).encode()).hexdigest()[:16]
    return '"etag-%s"' % digest


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("SVR %s\n" % (fmt % args))

    def _read_body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n > 0 else b""

    def _send(self, code, body=b"", ctype="application/json", etag=None):
        self.send_response(code)
        if etag is not None:
            self.send_header("ETag", etag)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _handle(self):
        path, _, query = self.path.partition("?")
        if path == "/__stats.json":
            with LOCK:
                self._send(200, json.dumps(STATS, separators=(", ", ": ")).encode())
            return
        parts_probe = [p for p in path.split("/") if p]
        if parts_probe and parts_probe[0] == "v1":
            body = self._read_body()
            self._auth(parts_probe, query, body)
            return
        if not path.endswith(".json"):
            self._send(404, b'{"error":"not found"}')
            return
        parts = [p for p in path[:-5].split("/") if p]
        if not parts:
            self._send(404, b'{"error":"not found"}')
            return
        method = self.command
        body = self._read_body()
        # К базе клиент в защищённом режиме ходит только с ?auth=.
        with LOCK:
            if "auth=" in query:
                STATS["rtdb_auth"] += 1
            else:
                STATS["rtdb_no_auth"] += 1
        with LOCK:
            if method == "GET":
                node = deep_get(DB, parts)
                if node is None:
                    self._send(200, b"null", etag=node_etag(None))
                else:
                    raw = json.dumps(node, separators=(",", ":")).encode()
                    self._send(200, raw, etag=node_etag(node))
            elif method == "DELETE":
                deep_del(DB, parts)
                self._send(200, b"null")
            elif method == "PUT":
                if_match = self.headers.get("If-Match")
                node = deep_get(DB, parts)
                cur_etag = node_etag(node)
                if if_match is not None:
                    want = if_match.strip()
                    if want == "null":
                        if node is not None:
                            self._send(412, b'{"error":"condition failed"}')
                            return
                    elif want != cur_etag:
                        self._send(412, b'{"error":"condition failed"}')
                        return
                try:
                    value = json.loads(body.decode()) if body else None
                except Exception:
                    self._send(400, b'{"error":"bad json"}')
                    return
                deep_set(DB, parts, value)
                raw = json.dumps(value, separators=(",", ":")).encode()
                self._send(200, raw, etag=node_etag(value))
            elif method == "PATCH":
                try:
                    patch = json.loads(body.decode()) if body else {}
                except Exception:
                    self._send(400, b'{"error":"bad json"}')
                    return
                if not isinstance(patch, dict):
                    self._send(400, b'{"error":"bad patch"}')
                    return
                for key, value in patch.items():
                    deep_set(DB, parts + key.split("/"), value)
                node = deep_get(DB, parts)
                raw = json.dumps(node, separators=(",", ":")).encode()
                self._send(200, raw, etag=node_etag(node))
            elif method == "POST":
                key = "-TESTKEY%06d" % ETAG_COUNTER[0]
                ETAG_COUNTER[0] += 1
                try:
                    value = json.loads(body.decode()) if body else {}
                except Exception:
                    self._send(400, b'{"error":"bad json"}')
                    return
                node = deep_get(DB, parts)
                if not isinstance(node, dict):
                    node = {}
                    deep_set(DB, parts, node)
                node[key] = value
                self._send(200, json.dumps({"name": key}).encode())
            else:
                self._send(405, b'{"error":"method"}')

    do_GET = _handle
    do_PUT = _handle
    do_PATCH = _handle
    do_DELETE = _handle
    do_POST = _handle

    def _fb_error(self, message):
        self._send(400, json.dumps({"error": {"code": 400, "message": message}}).encode())

    def _auth(self, parts, query, body):
        """Мини-Firebase Auth: e-mail/пароль + refresh-токены.

        Ошибки повторяют коды настоящего сервиса: EMAIL_NOT_FOUND,
        INVALID_PASSWORD, EMAIL_EXISTS — на них опирается fb_sign_in() в net.c.
        """
        endpoint = "/".join(parts)
        qs = parse_qs(query)
        if not qs.get("key", [""])[0]:
            self._fb_error("API_KEY_INVALID")
            return
        try:
            data = json.loads(body.decode()) if body else {}
        except Exception:
            # /v1/token может прислать form-encoded тело вместо JSON.
            data = {k: v[0] for k, v in parse_qs(body.decode()).items()} if body else {}
        if not isinstance(data, dict):
            data = {}
        with LOCK:
            if endpoint in ("v1/accounts:signUp", "v1/accounts:signInWithPassword"):
                email = data.get("email", "")
                password = data.get("password", "")
                if not email or not password:
                    self._fb_error("MISSING_EMAIL_OR_PASSWORD")
                    return
                if endpoint == "v1/accounts:signUp":
                    if email in AUTH:
                        self._fb_error("EMAIL_EXISTS")
                        return
                    AUTH[email] = password
                    STATS["sign_up"] += 1
                    idt, rt = _issue_tokens(email)
                else:
                    if email not in AUTH:
                        self._fb_error("EMAIL_NOT_FOUND")
                        return
                    if AUTH[email] != password:
                        self._fb_error("INVALID_PASSWORD")
                        return
                    STATS["sign_in"] += 1
                    idt, rt = _issue_tokens(email)
                self._send(200, json.dumps({
                    "idToken": idt, "refreshToken": rt,
                    "expiresIn": "3600", "localId": email.split("@")[0],
                }).encode())
                return
            if endpoint == "v1/token":
                rt = data.get("refresh_token", "")
                if rt not in REFRESH:
                    self._fb_error("INVALID_REFRESH_TOKEN")
                    return
                email = REFRESH[rt]
                STATS["token_refresh"] += 1
                idt, nrt = _issue_tokens(email)
                self._send(200, json.dumps({
                    "id_token": idt, "refresh_token": nrt,
                    "expires_in": "3600", "user_id": email.split("@")[0],
                }).encode())
                return
        self._fb_error("OPERATION_NOT_ALLOWED")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18765
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print("fake firebase listening on %d" % port, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
