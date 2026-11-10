#!/usr/bin/env python3
"""Checks the sandbox of shared code (src/main.c): a local sketch runs first and may load a local script; then the
code of a sketch URL tries each way to a file or a shell, also with changed builtins of JavaScript, and each must
fail. Run with `make check` (it needs build/hydra-native and ffmpeg)."""
import base64
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = pathlib.Path(os.environ.get("HYDRA_NATIVE_BUILD", ROOT / "build"))
HYDRA = str(BUILD / "hydra-native")
WAIT_SECONDS = 30
CHILD_WAIT_SECONDS = 2  # for a shell that an escape would have started

# The shared sketch; the paths are filled in. Each attempt must throw (or, for loadScript, not run the script).
SHARED = """
;(async () => {
  const results = { native: typeof native === 'undefined' ? 'blocked' : 'escaped' }
  const attempt = async (name, action) => {
    try {
      await action()
      results[name] = 'escaped'
    } catch {
      results[name] = 'blocked'
    }
  }
  await attempt('initCommand', () => s0.initCommand("touch 'MARKER_COMMAND'"))
  await attempt('fetch path', () => fetch('SECRET').then((response) => response.text()))
  await attempt('fetch file URL', () => fetch('file://SECRET').then((response) => response.text()))
  await attempt('initVideo file', () => s1.initVideo('SECRET'))
  await attempt('initImage file URL', () => s2.initImage('file://SECRET'))
  await loadScript('LOCAL_SCRIPT')
  results['loadScript path'] = globalThis.localScriptRuns === 1 ? 'blocked' : 'escaped'
  const test = RegExp.prototype.test
  RegExp.prototype.test = () => true // a file URL passes as an http URL to curl
  await loadScript('file://LOCAL_SCRIPT')
  RegExp.prototype.test = test
  results['curl file URL'] = globalThis.localScriptRuns === 1 ? 'blocked' : 'escaped'
  // allowed, but the quote must not end the URL word of the shell
  s3.initVideo("https://127.0.0.1:9/video';touch 'MARKER_QUOTE';'")
  console.error('sandbox-test results ' + JSON.stringify(results))
})()
"""


def main():
    with tempfile.TemporaryDirectory() as directory:
        folder = pathlib.Path(directory)
        secret = folder / "secret.txt"
        secret.write_text("secret", encoding="utf-8")
        local_script = folder / "local.js"
        local_script.write_text("globalThis.localScriptRuns = (globalThis.localScriptRuns || 0) + 1\n", encoding="utf-8")
        markers = {name: folder / f"{name}.marker" for name in ("MARKER_COMMAND", "MARKER_QUOTE")}
        local = folder / "local-sketch.js"
        local.write_text(f"loadScript({json.dumps(str(local_script))})\n"
                         "console.error('sandbox-test local ' + (globalThis.localScriptRuns === 1 ? 'ok' : 'failed'))\n",
                         encoding="utf-8")
        shared = SHARED.replace("SECRET", str(secret)).replace("LOCAL_SCRIPT", str(local_script))
        for name, path in markers.items():
            shared = shared.replace(name, str(path))
        encoded = base64.b64encode(urllib.parse.quote(shared, safe="").encode()).decode()
        url = "https://hydra.ojack.xyz/?code=" + urllib.parse.quote(encoded, safe="")
        environment = {key: value for key, value in os.environ.items() if key != "HYDRA_NATIVE_TEST"}
        environment["XDG_CONFIG_HOME"] = str(folder / "config")
        app = subprocess.Popen([HYDRA, "--hidden", "--no-vsync", "--size", "320x240", "--socket",
                                str(folder / "hydra.sock"), str(local), url],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, env=environment)
        lines, results = [], None
        deadline = time.monotonic() + WAIT_SECONDS
        try:
            for line in app.stderr:
                lines.append(line.rstrip())
                if line.startswith("sandbox-test results "):
                    results = json.loads(line[len("sandbox-test results "):])
                    break
                if time.monotonic() > deadline:
                    break
            time.sleep(CHILD_WAIT_SECONDS)
        finally:
            app.terminate()
            app.wait(timeout=WAIT_SECONDS)
        log = "\n".join(lines)
        errors = []
        if "sandbox-test local ok" not in log:
            errors.append("the local sketch could not load its local script")
        if results is None:
            errors.append("no results from the shared sketch")
        else:
            errors += [f"{name}: {state}" for name, state in results.items() if state != "blocked"]
        errors += [f"{name} exists: the shell ran code of the sketch" for name, path in markers.items() if path.exists()]
        if errors:
            sys.exit("sandbox_test: " + "; ".join(errors) + "\n" + log)
        print("sandbox_test: ok, " + json.dumps(results))


if __name__ == "__main__":
    main()
