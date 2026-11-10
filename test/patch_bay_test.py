#!/usr/bin/env python3
"""Checks the webRTC patch bay with two hydra-native instances and a local copy of the hydra-server signalling
(test/signalling_server.cjs on 127.0.0.1): instance A draws red and publishes it, instance B joins the same room,
receives A with s0.initStream and reads the color back. The public signalling server is never used.
Run with `make check` (it needs build/hydra-native and node_modules)."""
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = pathlib.Path(os.environ.get("HYDRA_NATIVE_BUILD", ROOT / "build"))
TIMEOUT_SECONDS = 60
HYDRA = [str(BUILD / "hydra-native"), "--hidden", "--no-vsync", "--size", "320x240"]

PUBLISHER = """
solid(1, 0, 0).out()
setTimeout(() => native.exit(0), 40000)
"""

RECEIVER = """
const started = Date.now()
const wait = setInterval(() => {
  if (!pb.peers.length) return
  clearInterval(wait)
  s0.initStream(pb.peers[0])
  src(s0).out()
}, 200)
const check = setInterval(() => {
  const pixel = native.readPixel(o0.getCurrent().id, Math.floor(width / 2), Math.floor(height / 2))
  const [r, g, b] = pixel
  if (r > 200 && g < 60 && b < 60) {
    console.log('RESULT received ' + JSON.stringify({ pixel, seconds: (Date.now() - started) / 1000, peers: pb.peers }))
    native.exit(0)
  } else if (Date.now() - started > 30000) {
    console.log('RESULT timeout ' + JSON.stringify({ pixel, peers: pb.peers, nicks: pb.nicks }))
    native.exit(1)
  }
}, 250)
"""

# The server of the settings menu switches at run time (src/ui.c set_server calls __setPatchBayServer): pb appears
# with a URL and goes away with "", and the helper process ends.
SWITCHER = """
const log = (text) => console.log('RESULT ' + text)
log('before ' + typeof pb)
__setPatchBayServer('%s')
const wait = setInterval(() => {
  if (typeof pb === 'undefined' || !pb.id) return
  clearInterval(wait)
  log('started ' + (__patchBayStreamId() === pb.streamId && pb.streamId.length > 0))
  __setPatchBayServer('')
  log('stopped ' + typeof pb + ' ' + JSON.stringify(__patchBayStreamId()))
  setTimeout(() => native.exit(0), 3000)
}, 200)
setTimeout(() => { log('timeout'); native.exit(1) }, 20000)
"""


def helper_running(pid):
    return subprocess.run(["pgrep", "-f", f"hydra-native-patch-bay-{pid}"], capture_output=True).returncode == 0


server = subprocess.Popen(["node", str(ROOT / "test" / "signalling_server.cjs"), "0"], stdout=subprocess.PIPE, text=True)
try:
    port = server.stdout.readline().split()[1]
    url = f"http://127.0.0.1:{port}"
    with tempfile.TemporaryDirectory() as directory:
        publisher_file, receiver_file = pathlib.Path(directory, "a.js"), pathlib.Path(directory, "b.js")
        publisher_file.write_text(PUBLISHER)
        receiver_file.write_text(RECEIVER)
        publisher = subprocess.Popen(HYDRA + ["--gallery-server", url, str(publisher_file)], stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True)
        receiver = subprocess.run(HYDRA + ["--gallery-server", url, str(receiver_file)], capture_output=True, text=True,
                                  timeout=TIMEOUT_SECONDS)
        publisher.terminate() # SIGTERM: A leaves the room through its normal exit
        publisher_output = publisher.communicate(timeout=TIMEOUT_SECONDS)[0]
        switcher_file = pathlib.Path(directory, "c.js")
        switcher_file.write_text(SWITCHER % url)
        switcher = subprocess.Popen(HYDRA + [str(switcher_file)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        switch_lines, helper_after_stop = [], None
        for line in switcher.stdout:
            switch_lines.append(line)
            if line.startswith("RESULT stopped"):
                helper_after_stop = helper_running(switcher.pid)
        switcher.wait(timeout=TIMEOUT_SECONDS)
finally:
    server.terminate()
    server.wait()

output = receiver.stdout + receiver.stderr
results = [line for line in output.splitlines() if line.startswith("RESULT ")]
if not results or not results[0].startswith("RESULT received"):
    sys.exit(f"patch_bay_test: no stream\n--- receiver\n{output}\n--- publisher\n{publisher_output}")
if publisher.returncode != 0:
    sys.exit(f"patch_bay_test: the publisher ended with {publisher.returncode}\n{publisher_output}")
switch_results = [line.strip() for line in switch_lines if line.startswith("RESULT ")]
expected_switch = ["RESULT before undefined", "RESULT started true", 'RESULT stopped undefined ""']
if switch_results != expected_switch or helper_after_stop:
    sys.exit(f"patch_bay_test: live start and stop: {switch_results}, helper after stop: {helper_after_stop}\n{''.join(switch_lines)}")
print(f"patch_bay_test: ok, {results[0][len('RESULT received '):]}, live start and stop")
