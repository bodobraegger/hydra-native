#!/usr/bin/env python3
"""Checks "render to file" (src/render.c): a sketch of 1 second at 30 fps and 320x180 gives a video with 30 frames of
that size, each frame has the color of `time` at that frame, and the window size returns after the render. An audio
file drives `a`; its sound goes into the video with "renderAudio": "file", and not with "muted" or "sources" (which
has no video source with sound here).
Run with `make check` (it needs build/hydra-native and ffmpeg)."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = pathlib.Path(os.environ.get("HYDRA_NATIVE_BUILD", ROOT / "build"))
WIDTH, HEIGHT, FPS, SECONDS = 320, 180, 30, 1
COLOR_TOLERANCE = 3  # of 255, for the color conversion of H.264
# The gray level follows `time`, which starts at 0 and advances 1/fps per frame. The sketch starts the render as
# the command palette does, and logs the loudest `a.vol` of the render and the size after it.
SKETCH = """
solid(() => time, () => time, () => time).out()
let loudest = 0
update = () => { if (native.rendering()) loudest = Math.max(loudest, a.vol) }
const report = () => console.log('RESULT ' + JSON.stringify({ loudest, width, height }))
const wait = () => native.rendering() ? setTimeout(wait, 50) : (report(), native.exit(0))
setTimeout(() => { native.renderToFile(); setTimeout(wait, 50) }, 500)
"""


def render(directory, name, audio_file="", audio="sources"):
    video = pathlib.Path(directory, name)
    config = pathlib.Path(directory, name + "-config")
    (config / "hydra-native").mkdir(parents=True)
    settings = {"renderFile": str(video), "renderLength": SECONDS, "renderFps": FPS,
                "renderSize": f"{WIDTH}x{HEIGHT}", "renderAudio": audio, "renderAudioFile": audio_file,
                "audioSource": "none"}
    (config / "hydra-native" / "settings.json").write_text(json.dumps(settings), encoding="utf-8")
    sketch = pathlib.Path(directory, "sketch.js")
    sketch.write_text(SKETCH, encoding="utf-8")
    environment = dict(os.environ, XDG_CONFIG_HOME=str(config))
    run = subprocess.run([str(BUILD / "hydra-native"), "--hidden", "--socket", str(config / "socket"), str(sketch)],
                         capture_output=True, text=True, timeout=60, env=environment)
    results = [line for line in (run.stdout + run.stderr).splitlines() if line.startswith("RESULT ")]
    if not video.exists() or not results:
        sys.exit(f"render_test: no video {video}\n{run.stdout}\n{run.stderr}")
    return video, json.loads(results[0][len("RESULT "):])


def streams(video):
    output = subprocess.run(["ffprobe", "-v", "error", "-count_frames", "-show_entries",
                             "stream=codec_type,width,height,nb_read_frames", "-of", "json", str(video)],
                            capture_output=True, text=True, check=True).stdout
    return json.loads(output)["streams"]


def center_grays(video):
    pixels = subprocess.run(["ffmpeg", "-v", "error", "-i", str(video), "-f", "rawvideo", "-pix_fmt", "gray", "-"],
                            capture_output=True, check=True).stdout
    size = WIDTH * HEIGHT
    return [pixels[start + HEIGHT // 2 * WIDTH + WIDTH // 2] for start in range(0, len(pixels), size)]


errors = []
with tempfile.TemporaryDirectory() as directory:
    video, result = render(directory, "silent.mp4")
    found = streams(video)
    print(f"render_test: streams {found}, sketch {result}")
    wanted = {"codec_type": "video", "width": WIDTH, "height": HEIGHT, "nb_read_frames": str(FPS * SECONDS)}
    if found != [wanted]:
        errors.append(f"got the streams {found}, expected {[wanted]}")
    grays, expected = center_grays(video), [round(frame / FPS * 255) for frame in range(FPS * SECONDS)]
    print(f"render_test: grays {grays}, expected {expected}")
    if len(grays) != len(expected) or any(abs(a - b) > COLOR_TOLERANCE for a, b in zip(grays, expected)):
        errors.append("the grays of the frames do not follow time")
    if result["loudest"] != 0:
        errors.append(f"a.vol is {result['loudest']} without an audio file, expected 0")
    if [result["width"], result["height"]] == [WIDTH, HEIGHT]:
        errors.append("the sketch keeps the render size after the render")

    tone = pathlib.Path(directory, "tone.wav")
    subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"sine=frequency=440:duration={SECONDS}",
                    str(tone)], check=True)
    for audio, wanted_types in [("file", ["video", "audio"]), ("muted", ["video"])]:
        video, result = render(directory, f"{audio}.mp4", str(tone), audio)
        found = streams(video)
        print(f"render_test: render audio {audio}: streams {found}, sketch {result}")
        if [stream["codec_type"] for stream in found] != wanted_types:
            errors.append(f"render audio {audio}: got the streams {found}, expected {wanted_types}")
        if result["loudest"] <= 0:
            errors.append(f"render audio {audio}: a.vol stays 0 with the audio file")
if errors:
    sys.exit("render_test: " + "\n".join(errors))
print("render_test: ok")
