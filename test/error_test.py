#!/usr/bin/env python3
"""Checks that an evaluation error of hydra-synth reaches the error bar: a hydra function passed without
parentheses (hydra-synth #205) sends its error back through the code socket, which shows the same text in the
error bar ("socket: ..."). Run with `make check` (it needs build/hydra-native)."""
import os
import pathlib
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = pathlib.Path(os.environ.get("HYDRA_NATIVE_BUILD", ROOT / "build"))
HYDRA = str(BUILD / "hydra-native")
EXPECTED = "modulate() received the hydra function noise without parentheses"
SOCKET_WAIT_SECONDS = 15

with tempfile.TemporaryDirectory() as directory:
    socket = str(pathlib.Path(directory, "hydra.sock"))
    sketch = pathlib.Path(directory, "sketch.js")
    sketch.write_text("osc().out()\nsetTimeout(() => native.exit(0), 30000)\n", encoding="utf-8")
    app = subprocess.Popen([HYDRA, "--hidden", "--no-vsync", "--no-analog-agc", "--size", "320x240", "--socket", socket,
                            str(sketch)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + SOCKET_WAIT_SECONDS
        while not os.path.exists(socket) and time.monotonic() < deadline:
            time.sleep(0.1)
        sent = subprocess.run([HYDRA, "--send", "--socket", socket], input="osc().modulate(noise).out()",
                              capture_output=True, text=True, timeout=SOCKET_WAIT_SECONDS)
        working = subprocess.run([HYDRA, "--send", "--socket", socket], input="osc().modulate(noise()).out()",
                                 capture_output=True, text=True, timeout=SOCKET_WAIT_SECONDS)
    finally:
        app.terminate()
        app.wait(timeout=SOCKET_WAIT_SECONDS)

if sent.returncode != 1 or EXPECTED not in sent.stderr:
    sys.exit(f"error_test: expected the error '{EXPECTED}', got status {sent.returncode}: {sent.stdout}{sent.stderr}")
if working.returncode != 0:
    sys.exit(f"error_test: the call with parentheses failed: {working.stdout}{working.stderr}")
print(f"error_test: ok, {sent.stderr.strip()}")
