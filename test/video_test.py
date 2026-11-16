#!/usr/bin/env python3
"""Checks the controls of initVideo (s0.src) on a 3 second video of ffmpeg: red, green and blue for one second each.
The duration comes from ffprobe, a seek shows the color of its second, pause() keeps the frame, and the end without
loop fires 'ended'. Then it checks the colors of the YUV conversion on the GPU: a video tagged BT.709 and one without
tags (BT.601, as ffmpeg assumes) at an odd size give the RGB of ffmpeg's own conversion.
Run with `make check` (it needs build/hydra-native and ffmpeg)."""
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
TEST_COLOR = "0xC86432"
COLOR_TOLERANCE = 3 # of 255: the rounding of 8-bit YUV
TAGS = {"bt709": ["-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-color_range", "tv"],
        "untagged": []}

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

COLOR_SCRIPT = """
const LIMIT_MS = 5000
const SETTLE_MS = 300
s0.initVideo('%s', { width: 15, height: 9 })
s1.initVideo('%s', { width: 15, height: 9 })
src(s0).out(o0)
src(s1).out(o1)
const frame = () => new Promise((resolve) => requestAnimationFrame(resolve))
const center = (output) =>
  native.readPixel(output.getCurrent().id, Math.floor(width / 2), Math.floor(height / 2)).slice(0, 3)
;(async () => {
  const start = Date.now()
  while (!(s0.src.duration > 0 && s1.src.duration > 0) && Date.now() - start < LIMIT_MS) await frame()
  const settled = Date.now()
  while (Date.now() - settled < SETTLE_MS) await frame()
  console.log('RESULT ' + JSON.stringify({ bt709: center(o0), untagged: center(o1) }))
  native.exit(0)
})()
"""


def run_sketch(directory, name, code):
    sketch = pathlib.Path(directory, name)
    sketch.write_text(code, encoding="utf-8")
    run = subprocess.run([str(BUILD / "hydra-native"), "--hidden", "--socket", str(pathlib.Path(directory, "socket")),
                          str(sketch)], capture_output=True, text=True, timeout=60)
    lines = [line for line in run.stdout.splitlines() + run.stderr.splitlines() if line.startswith("RESULT ")]
    if not lines:
        sys.exit(f"video_test: no result of {name}\n{run.stdout}\n{run.stderr}")
    return json.loads(lines[0][len("RESULT "):])


# The RGB of the first frame of `video` as ffmpeg converts it.
def ffmpeg_rgb(video):
    pixel = subprocess.run(["ffmpeg", "-v", "error", "-i", str(video), "-frames:v", "1", "-vf", "scale=1:1",
                            "-pix_fmt", "rgb24", "-f", "rawvideo", "-"], capture_output=True, check=True).stdout
    return list(pixel[:3])


with tempfile.TemporaryDirectory() as directory:
    video = pathlib.Path(directory, "colors.mp4")
    inputs = [argument for name in COLORS
              for argument in ("-f", "lavfi", "-i", f"color={name}:size=64x64:duration=1:rate={FRAME_RATE}")]
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", *inputs, "-filter_complex",
                    "concat=n=3", "-g", str(FRAME_RATE), str(video)], check=True)
    result = run_sketch(directory, "video.js", SCRIPT % video)
    expected = {}
    for name, tags in TAGS.items():
        path = pathlib.Path(directory, f"{name}.mp4")
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                        f"color={TEST_COLOR}:size=64x64:duration=1:rate={FRAME_RATE}", "-pix_fmt", "yuv420p", *tags,
                        str(path)], check=True)
        expected[name] = ffmpeg_rgb(path)
    colors = run_sketch(directory, "colors.js",
                        COLOR_SCRIPT % (pathlib.Path(directory, "bt709.mp4"), pathlib.Path(directory, "untagged.mp4")))

print(f"video_test: {result}")
print(f"video_test: colors {colors}, ffmpeg {expected}")
pause, end = result["pause"], result["end"]
errors = [message for failed, message in [
    (abs(result["duration"] - 3) > 0.1, "duration is not 3 s"),
    (result["startColor"] != 0, "currentTime = 0.5 does not show red within 500 ms"),
    (result["seekColor"] != 2, "currentTime = 2 does not show blue within 500 ms"),
    (not pause["paused"] or pause["color"] != 2 or pause["moved"] != 0, "pause() does not keep the frame and time"),
    (not result["playing"], "play() does not play"),
    (end["ended"] != 2 or not end["paused"], "no 'ended' to onended and the listener without loop"),
    (abs(end["currentTime"] - 3) > 0.1, "currentTime at the end is not the duration"),
    *[(max(abs(a - b) for a, b in zip(colors[name], expected[name])) > COLOR_TOLERANCE,
       f"the {name} video shows {colors[name]} instead of {expected[name]}") for name in TAGS],
] if failed]
if errors:
    sys.exit("video_test: " + "; ".join(errors))
print("video_test: ok")
