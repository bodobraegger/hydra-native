#!/usr/bin/env python3
"""Checks the webRTC patch bay with two hydra-native instances and a local copy of the hydra-server signalling
(test/signalling_server.cjs on 127.0.0.1): instance A draws red and publishes it, instance B joins the same room,
receives A with s0.initStream(id, { fps }) and reads the color back. B then counts the new frames of the stream for a
second: at most about fps arrive, and A publishes at that rate. The public signalling server is never used.
Run with `make check` (it needs build/hydra-native and node_modules)."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = pathlib.Path(os.environ.get("HYDRA_NATIVE_BUILD", ROOT / "build"))
TIMEOUT_SECONDS = 60
HYDRA = [str(BUILD / "hydra-native"), "--hidden", "--size", "320x240"]
MAX_RATE = 10 # frames per second of the stream that B receives
COUNT_MS = 1000
MAX_COUNTED = MAX_RATE * COUNT_MS // 1000 + 2 # the tolerance of the receiver can let one frame more through
MIN_COUNTED = 3

PUBLISHER = """
solid(1, 0, 0).out()
const publish = native.patchBayPublish
native.patchBayPublish = (on, fps) => {
  console.log(`PUBLISH ${on} ${fps}`)
  return publish(on, fps)
}
setTimeout(() => native.exit(0), 40000)
"""

RECEIVER = """
const started = Date.now()
const wait = setInterval(() => {
  if (!pb.peers.length) return
  clearInterval(wait)
  s0.initStream(pb.peers[0], { fps: MAX_RATE })
  src(s0).out()
}, 200)
const check = setInterval(() => {
  const pixel = native.readPixel(o0.getCurrent().id, Math.floor(width / 2), Math.floor(height / 2))
  const [r, g, b] = pixel
  if (r > 200 && g < 60 && b < 60) {
    clearInterval(check)
    const seconds = (Date.now() - started) / 1000
    let frames = 0
    const upload = s0.src.upload.bind(s0.src)
    s0.src.upload = (texture) => {
      const fresh = upload(texture)
      if (fresh) frames++
      return fresh
    }
    setTimeout(() => {
      console.log('RESULT received ' + JSON.stringify({ pixel, seconds, frames, peers: pb.peers }))
      native.exit(0)
    }, COUNT_MS)
  } else if (Date.now() - started > 30000) {
    console.log('RESULT timeout ' + JSON.stringify({ pixel, peers: pb.peers, nicks: pb.nicks }))
    native.exit(1)
  }
}, 250)
"""

server = subprocess.Popen(["node", str(ROOT / "test" / "signalling_server.cjs"), "0"], stdout=subprocess.PIPE, text=True)
try:
    port = server.stdout.readline().split()[1]
    url = f"http://127.0.0.1:{port}"
    with tempfile.TemporaryDirectory() as directory:
        publisher_file, receiver_file = pathlib.Path(directory, "a.js"), pathlib.Path(directory, "b.js")
        publisher_file.write_text(PUBLISHER)
        receiver_file.write_text(RECEIVER.replace("MAX_RATE", str(MAX_RATE)).replace("COUNT_MS", str(COUNT_MS)))
        # The server is a setting: A and B read it from a settings file of their own.
        settings = pathlib.Path(directory, "config", "hydra-native", "settings.json")
        settings.parent.mkdir(parents=True)
        settings.write_text(json.dumps({"galleryServer": url}))
        with_server = dict(os.environ, XDG_CONFIG_HOME=str(settings.parent.parent))
        # Code sockets of their own, so that a running hydra-native of the user keeps its socket.
        publisher = subprocess.Popen(HYDRA + ["--socket", str(pathlib.Path(directory, "a.sock")), str(publisher_file)],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=with_server)
        receiver = subprocess.run(HYDRA + ["--socket", str(pathlib.Path(directory, "b.sock")), str(receiver_file)],
                                  capture_output=True, text=True, timeout=TIMEOUT_SECONDS, env=with_server)
        publisher.terminate() # SIGTERM: A leaves the room through its normal exit
        publisher_output = publisher.communicate(timeout=TIMEOUT_SECONDS)[0]
finally:
    server.terminate()
    server.wait()

output = receiver.stdout + receiver.stderr
results = [line for line in output.splitlines() if line.startswith("RESULT ")]
if not results or not results[0].startswith("RESULT received"):
    sys.exit(f"patch_bay_test: no stream\n--- receiver\n{output}\n--- publisher\n{publisher_output}")
if publisher.returncode != 0:
    sys.exit(f"patch_bay_test: the publisher ended with {publisher.returncode}\n{publisher_output}")
frames = json.loads(results[0][len("RESULT received "):])["frames"]
if not MIN_COUNTED <= frames <= MAX_COUNTED:
    sys.exit(f"patch_bay_test: {frames} frames in {COUNT_MS} ms, expected {MIN_COUNTED} to {MAX_COUNTED}\n{output}")
if f"PUBLISH true {MAX_RATE}" not in publisher_output:
    sys.exit(f"patch_bay_test: the publisher did not publish at {MAX_RATE} fps\n{publisher_output}")
print(f"patch_bay_test: ok, {results[0][len('RESULT received '):]}")
