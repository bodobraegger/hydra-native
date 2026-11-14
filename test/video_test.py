#!/usr/bin/env python3
"""Checks the controls of initVideo (s0.src) on a 3 second video of ffmpeg: red, green and blue for one second each.
The duration comes from ffprobe, a seek shows the color of its second, pause() keeps the frame, and the end without
loop fires 'ended'. Run with `make check` (it needs build/hydra-native and ffmpeg)."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = pathlib.Path(os.environ.get("HYDRA_NATIVE_BUILD", ROOT / "build"))
COLORS = ["red", "lime", "blue"]
FRAME_RATE = 25

SCRIPT = """
const SEEK_LIMIT_MS = 500
const PAUSE_MS = 500
const END_LIMIT_MS = 3000
s0.initVideo('%s', { width: 16, height: 16 })
src(s0).out(o0)
render(o0)
const video = s0.src
const results = {}
const frame = () => new Promise((resolve) => requestAnimationFrame(resolve))
const color = () => {
  const pixel = native.readPixel(o0.getCurrent().id, Math.floor(width / 2), Math.floor(height / 2)).slice(0, 3)
  return pixel.indexOf(Math.max(...pixel))
}
const waitFor = async (condition, limit) => {
  const start = Date.now()
  while (!condition() && Date.now() - start < limit) await frame()
  return Date.now() - start
}
;(async () => {
  await waitFor(() => video.duration > 0 && !o0.pendingDraw, END_LIMIT_MS)
  results.duration = video.duration
  video.currentTime = 0.5
  await waitFor(() => color() === 0, SEEK_LIMIT_MS)
  results.startColor = color()
  video.currentTime = 2
  results.seekMs = await waitFor(() => color() === 2, SEEK_LIMIT_MS)
  results.seekColor = color()
  video.pause()
  const pausedTime = video.currentTime
  await waitFor(() => false, PAUSE_MS)
  results.pause = { paused: video.paused, color: color(), moved: video.currentTime - pausedTime }
  let ended = 0
  video.onended = () => ended++
  video.addEventListener('ended', () => ended++)
  video.loop = false
  video.play()
  results.playing = !video.paused
  await waitFor(() => ended, END_LIMIT_MS)
  results.end = { ended, paused: video.paused, currentTime: video.currentTime }
  console.log('RESULT ' + JSON.stringify(results))
  native.exit(0)
})()
"""

with tempfile.TemporaryDirectory() as directory:
    video = pathlib.Path(directory, "colors.mp4")
    inputs = [argument for name in COLORS
              for argument in ("-f", "lavfi", "-i", f"color={name}:size=64x64:duration=1:rate={FRAME_RATE}")]
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", *inputs, "-filter_complex",
                    "concat=n=3", "-g", str(FRAME_RATE), str(video)], check=True)
    sketch = pathlib.Path(directory, "video.js")
    sketch.write_text(SCRIPT % video, encoding="utf-8")
    run = subprocess.run([str(BUILD / "hydra-native"), "--hidden", "--socket", str(pathlib.Path(directory, "socket")),
                          str(sketch)], capture_output=True, text=True, timeout=60)

lines = [line for line in run.stdout.splitlines() + run.stderr.splitlines() if line.startswith("RESULT ")]
if not lines:
    sys.exit(f"video_test: no result\n{run.stdout}\n{run.stderr}")
result = json.loads(lines[0][len("RESULT "):])
print(f"video_test: {result}")
pause, end = result["pause"], result["end"]
errors = [message for failed, message in [
    (abs(result["duration"] - 3) > 0.1, "duration is not 3 s"),
    (result["startColor"] != 0, "currentTime = 0.5 does not show red within 500 ms"),
    (result["seekColor"] != 2, "currentTime = 2 does not show blue within 500 ms"),
    (not pause["paused"] or pause["color"] != 2 or pause["moved"] != 0, "pause() does not keep the frame and time"),
    (not result["playing"], "play() does not play"),
    (end["ended"] != 2 or not end["paused"], "no 'ended' to onended and the listener without loop"),
    (abs(end["currentTime"] - 3) > 0.1, "currentTime at the end is not the duration"),
] if failed]
if errors:
    sys.exit("video_test: " + "; ".join(errors))
print("video_test: ok")
