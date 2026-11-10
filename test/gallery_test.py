#!/usr/bin/env python3
"""Checks tools/gallery_share.py against a local mock of the gallery server; nothing goes to the real server."""
import base64
import http.server
import pathlib
import subprocess
import sys
import tempfile
import threading
import urllib.parse

MOCK_SKETCH_ID = "mock_sketch_42"
requests = []


class MockGallery(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        requests.append((self.path, self.headers.get("Content-Type", ""), self.rfile.read(length)))
        self.send_response(200)
        self.end_headers()
        self.wfile.write(MOCK_SKETCH_ID.encode() if self.path.startswith("/sketch") else b"ok")

    def log_message(self, *arguments):
        pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MockGallery)
threading.Thread(target=server.serve_forever, daemon=True).start()
code = "osc(10, 0.1, 1.2).out() // ä"
with tempfile.TemporaryDirectory() as directory:
    code_file, image_file = pathlib.Path(directory, "code.js"), pathlib.Path(directory, "image.png")
    code_file.write_text(code, encoding="utf-8")
    image_file.write_bytes(b"\x89PNG mock")
    script = pathlib.Path(__file__).resolve().parent.parent / "tools" / "gallery_share.py"
    url = subprocess.run([sys.executable, str(script), f"http://127.0.0.1:{server.server_address[1]}", str(code_file),
                          str(image_file), "a name"], capture_output=True, text=True, check=True).stdout.strip()
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
