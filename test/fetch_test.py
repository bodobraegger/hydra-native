#!/usr/bin/env python3
"""Checks fetch() and XMLHttpRequest of hydra-native against a local mock server: status, headers, redirects,
request bodies and the CORS checks of a page at https://hydra.ojack.xyz. Nothing leaves 127.0.0.1. Run with `make check` (it needs build/hydra-native)."""
import http.server
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = pathlib.Path(os.environ.get("HYDRA_NATIVE_BUILD", ROOT / "build"))


class MockServer(http.server.BaseHTTPRequestHandler):
    def reply(self, status, body, headers=()):
        self.send_response(status)
        if self.path != "/private":
            self.send_header("Access-Control-Allow-Origin", "*")
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/ok":
            self.reply(200, "hällo".encode(), [("X-Mock", "yes"), ("X-Hidden", "no"), ("Access-Control-Expose-Headers", "X-Mock"),
                                               ("Content-Type", "text/plain; charset=utf-8")])
        elif self.path == "/redirect":
            self.reply(302, b"", [("Location", "/ok")])
        else:
            self.reply(404, b"not here")

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        answer = {"method": self.command, "type": self.headers.get("Content-Type"), "body": body.decode()}
        self.reply(201, json.dumps(answer).encode(), [("Content-Type", "application/json")])

    do_PUT = do_POST

    # The preflight of a request that is not simple: PUT and the Content-Type header are allowed, DELETE not.
    def do_OPTIONS(self):
        origin = self.headers.get("Origin")
        self.reply(204, b"", [("Access-Control-Allow-Methods", "GET, POST, PUT"), ("Access-Control-Allow-Headers", "Content-Type"),
                              ("X-Origin", origin or "")])

    def log_message(self, *arguments):
        pass


SCRIPT = """
const base = '%s'
const results = {}
const xhr = (method, path, body) => new Promise((resolve) => {
  const request = new XMLHttpRequest()
  request.open(method, base + path)
  request.setRequestHeader('Content-Type', 'text/plain')
  request.onload = () => resolve({ status: request.status, text: request.responseText, header: request.getResponseHeader('content-type') })
  request.onerror = () => resolve({ error: true })
  request.send(body)
})
Promise.all([
  fetch(base + '/ok').then(async (r) => ({ status: r.status, ok: r.ok, header: r.headers.get('x-mock'), text: await r.text() })),
  fetch(base + '/missing').then(async (r) => ({ status: r.status, ok: r.ok, text: await r.text() })),
  fetch(base + '/redirect').then(async (r) => ({ status: r.status, redirected: r.redirected, text: await r.text() })),
  fetch(base + '/echo', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ a: 1 }) })
    .then(async (r) => ({ status: r.status, json: await r.json() })),
  fetch(base + '/echo', { method: 'PUT', body: new Uint8Array([104, 105]) }).then((r) => r.json()),
  xhr('POST', '/echo', 'from xhr'),
  xhr('GET', '/missing'),
  fetch(base + '/private').then(() => 'allowed', (error) => error.name),
  fetch(base + '/echo', { method: 'DELETE' }).then(() => 'allowed', (error) => error.name),
  fetch('http://example.invalid/insecure').then(() => 'allowed', (error) => error.name),
  fetch(base + '/private', { mode: 'no-cors' }).then((r) => [r.type, r.status]),
  fetch(base + '/ok').then((r) => r.headers.get('x-hidden')),
  xhr('GET', '/private')
]).then((values) => {
  console.log('RESULT ' + JSON.stringify(values))
  native.exit(0)
}, (error) => {
  console.log('RESULT ' + JSON.stringify({ error: String(error) }))
  native.exit(1)
})
"""

server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MockServer)
threading.Thread(target=server.serve_forever, daemon=True).start()
with tempfile.TemporaryDirectory() as directory:
    sketch = pathlib.Path(directory, "fetch.js")
    sketch.write_text(SCRIPT % f"http://127.0.0.1:{server.server_address[1]}", encoding="utf-8")
    run = subprocess.run([str(BUILD / "hydra-native"), "--hidden", "--no-vsync", str(sketch)],
                         capture_output=True, text=True, timeout=60)
server.shutdown()

lines = [line for line in run.stdout.splitlines() + run.stderr.splitlines() if line.startswith("RESULT ")]
if not lines:
    sys.exit(f"fetch_test: no result\n{run.stdout}\n{run.stderr}")
result = json.loads(lines[0][len("RESULT "):])
if not isinstance(result, list):
    sys.exit(f"fetch_test: {result}")
ok, missing, redirect, post, put, xhr_post, xhr_missing, private, delete, insecure, opaque, hidden, xhr_private = result
expected = [
    (ok, {"status": 200, "ok": True, "header": "yes", "text": "hällo"}),
    (missing, {"status": 404, "ok": False, "text": "not here"}),
    (redirect, {"status": 200, "redirected": True, "text": "hällo"}),
    (post, {"status": 201, "json": {"method": "POST", "type": "application/json", "body": '{"a":1}'}}),
    (put, {"method": "PUT", "type": None, "body": "hi"}),
    (xhr_post, {"status": 201, "text": '{"method": "POST", "type": "text/plain", "body": "from xhr"}',
                "header": "application/json"}),
    (xhr_missing, {"status": 404, "text": "not here", "header": None}),
    (private, "TypeError"),
    (delete, "TypeError"),
    (insecure, "TypeError"),
    (opaque, ["opaque", 0]),
    (hidden, None),
    (xhr_private, {"error": True}),
]
errors = [f"got {actual}, expected {wanted}" for actual, wanted in expected if actual != wanted]
if errors:
    sys.exit("fetch_test: " + "\n".join(errors))
print("fetch_test: ok")
