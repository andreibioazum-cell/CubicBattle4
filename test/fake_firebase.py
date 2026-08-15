#!/usr/bin/env python3
"""Fake Firebase Realtime Database REST server for the Linux test harness.

Implements just enough of the REST API that net.c uses:
  GET    <path>.json            (+ X-Firebase-ETag -> ETag header)
  PUT    <path>.json            (if-match: "null" or etag -> 200/412)
  PATCH  <path>.json            (deep merge of flat "a/b": value keys)
  DELETE <path>.json
  POST   <path>/chat.json       (push key -> {"name": "-KEY"})
  GET    .../chat.json?orderBy="$key"&limitToLast=20
"""
import json
import re
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DB = {"rooms": {"main": {}}}
LOCK = threading.Lock()
ETAG_COUNTER = [0]

# Seed: a remote player in slot 1 so the game exercises read_remotes/draw nick.
DB["rooms"]["main"]["players"] = {
    "1": {"uid": "00000000000000aa", "nick": "BotRival", "x": 0.5, "y": 0.5,
          "angle": 1.2, "hp": 10, "alive": 1, "seq": 100},
}
DB["rooms"]["main"]["bullets"] = {}


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
        path = self.path.split("?", 1)[0]
        if not path.endswith(".json"):
            self._send(404, b'{"error":"not found"}')
            return
        parts = [p for p in path[:-5].split("/") if p]
        if not parts:
            self._send(404, b'{"error":"not found"}')
            return
        method = self.command
        body = self._read_body()
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


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18765
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print("fake firebase listening on %d" % port, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
