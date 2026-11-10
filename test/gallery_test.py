#!/usr/bin/env python3
"""Checks the gallery sharing of js/editor-actions.js (__shareSketch, with fetch) against a local mock of the
gallery server; nothing goes to the real server. Run with `make check` (it needs build/hydra-native)."""
import base64
import json
import http.server
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = pathlib.Path(os.environ.get("HYDRA_NATIVE_BUILD", ROOT / "build"))
# The sketch shares a fake PNG and prints the answer that src/main.c would show.
SKETCH = """
native.shareDone = (ok, text) => { console.log('RESULT ' + JSON.stringify([ok, text])); native.exit(0) }
__shareSketch('%s', 'osc(10, 0.1, 1.2).out() // \\u00e4', new Uint8Array([0x89, 80, 78, 71, 32, 109, 111, 99, 107]).buffer, 'a name')
setTimeout(() => native.exit(1), 20000)
"""

MOCK_SKETCH_ID = "mock_sketch_42"
requests = []


class MockGallery(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        requests.append((self.path, self.headers.get("Content-Type", ""), self.rfile.read(length)))
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "https://hydra.ojack.xyz") # as the cors options of hydra-server
        self.end_headers()
        self.wfile.write(MOCK_SKETCH_ID.encode() if self.path.startswith("/sketch") else b"ok")

    def log_message(self, *arguments):
        pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MockGallery)
threading.Thread(target=server.serve_forever, daemon=True).start()
code = "osc(10, 0.1, 1.2).out() // ä"
with tempfile.TemporaryDirectory() as directory:
    sketch = pathlib.Path(directory, "share.js")
    sketch.write_text(SKETCH % f"http://127.0.0.1:{server.server_address[1]}", encoding="utf-8")
    run = subprocess.run([str(BUILD / "hydra-native"), "--hidden", "--no-vsync", str(sketch)],
                         capture_output=True, text=True, timeout=60)
results = [line for line in (run.stdout + run.stderr).splitlines() if line.startswith("RESULT ")]
ok, url = json.loads(results[0][len("RESULT "):]) if results else (False, run.stdout + run.stderr)
if not ok:
    server.shutdown()
    sys.exit(f"gallery_test: sharing failed: {url}")
server.shutdown()

errors = []
sketch_path, _, _ = requests[0]
query = urllib.parse.parse_qs(urllib.parse.urlparse(sketch_path).query, keep_blank_values=True)
decoded = urllib.parse.unquote(base64.b64decode(query["code"][0]).decode())
if not sketch_path.startswith("/sketch") or decoded != code:
    errors.append(f"sketch request {sketch_path}")
image_path, content_type, body = requests[1]
image_query = urllib.parse.parse_qs(urllib.parse.urlparse(image_path).query)
if not image_path.startswith("/image") or image_query["sketch_id"] != [MOCK_SKETCH_ID] or image_query["name"] != ["a name"]:
    errors.append(f"image request {image_path}")
if "multipart/form-data" not in content_type or b'name="previewImage"' not in body or b"\x89PNG mock" not in body:
    errors.append("the image is not the multipart field previewImage")
if not url.startswith(f"https://hydra.ojack.xyz/?sketch_id={MOCK_SKETCH_ID}&code="):
    errors.append(f"URL {url}")
if errors:
    sys.exit("gallery_test: " + "; ".join(errors))
print("gallery_test: ok")
