#!/usr/bin/env python3
"""Checks "render to file" (src/render.c): a sketch of 1 second at 30 fps and 320x180 gives a video with 30 frames of
that size, each frame has the color of `time` at that frame, and the window size returns after the render. An audio
file drives `a`; its sound goes into the video with "renderAudio": "file", and not with "muted" or "sources" without
a video source with sound. With "sources", the sound of test videos with tones goes into the video: one video for the
whole render, two videos in equal shares, and a seek during the render that changes the tone at that frame.
Run with `make check` (it needs build/hydra-native and ffmpeg)."""
import array
import json
import math
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = pathlib.Path(os.environ.get("HYDRA_NATIVE_BUILD", ROOT / "build"))
WIDTH, HEIGHT, FPS, SECONDS = 320, 180, 30, 1
COLOR_TOLERANCE = 3  # of 255, for the color conversion of H.264
SAMPLE_RATE = 48000
VIDEO_SECONDS = 2
LOW, HIGH = 440, 880  # Hz of the test tones
DURATION_TOLERANCE = 0.1  # seconds, for the padding of the audio codec
TONE_RATIO = 10  # the tone of a span is this much louder than the other one
SHARE_RATIO = 3  # in a mix of two tones, neither is this much louder than the other
# The gray level follows `time`, which starts at 0 and advances 1/fps per frame. The sketch starts the render as
# the command palette does, and logs the loudest `a.vol` of the render and the size after it.
SKETCH = """
solid(() => time, () => time, () => time).out()
let loudest = 0
update = () => { if (native.rendering()) { loudest = Math.max(loudest, a.vol); RENDER_UPDATE } }
const report = () => console.log('RESULT ' + JSON.stringify({ loudest, width, height }))
const wait = () => native.rendering() ? setTimeout(wait, 50) : (report(), native.exit(0))
setTimeout(() => { RENDER_SETUP; native.renderToFile(); setTimeout(wait, 50) }, 500)
"""


def render(directory, name, audio_file="", audio="sources", setup="", update="", seconds=SECONDS):
    video = pathlib.Path(directory, name)
    config = pathlib.Path(directory, name + "-config")
    (config / "hydra-native").mkdir(parents=True)
    settings = {"renderFile": str(video), "renderLength": seconds, "renderFps": FPS,
                "renderSize": f"{WIDTH}x{HEIGHT}", "renderAudio": audio, "renderAudioFile": audio_file,
                "audioSource": "none"}
    (config / "hydra-native" / "settings.json").write_text(json.dumps(settings), encoding="utf-8")
    sketch = pathlib.Path(directory, "sketch.js")
    sketch.write_text(SKETCH.replace("RENDER_SETUP", setup).replace("RENDER_UPDATE", update), encoding="utf-8")
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


def tone_video(path, expression):
    """A video of VIDEO_SECONDS with the sound sin(2 PI f t), where `expression` gives f at the time t."""
    picture = f"color=size=64x64:rate={FPS}:duration={VIDEO_SECONDS}"
    tone = f"aevalsrc='sin(2*PI*({expression})*t)':s={SAMPLE_RATE}:d={VIDEO_SECONDS}"
    subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", picture, "-f", "lavfi", "-i", tone, "-shortest",
                    str(path)], check=True)
    return path


def audio_duration(video):
    output = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a", "-show_entries", "stream=duration",
                             "-of", "csv=p=0", str(video)], capture_output=True, text=True, check=True).stdout
    return float(output) if output.strip() else 0.0


def sound(video):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(video), "-vn", "-f", "s16le", "-ac", "1", "-ar",
                          str(SAMPLE_RATE), "-"], capture_output=True, check=True).stdout
    return array.array("h", raw)


def power(samples, start, end, frequency):
    """The power of `frequency` from `start` to `end` seconds (Goertzel)."""
    coefficient = 2 * math.cos(2 * math.pi * frequency / SAMPLE_RATE)
    previous = before = 0.0
    for sample in samples[int(start * SAMPLE_RATE):int(end * SAMPLE_RATE)]:
        previous, before = sample + coefficient * previous - before, previous
    return previous * previous + before * before - coefficient * previous * before


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

    low = tone_video(pathlib.Path(directory, "low.mp4"), LOW)
    high = tone_video(pathlib.Path(directory, "high.mp4"), HIGH)
    video, result = render(directory, "sources.mp4", setup=f"s0.initVideo({json.dumps(str(low))}); src(s0).out()",
                           seconds=VIDEO_SECONDS)
    duration = audio_duration(video)
    print(f"render_test: sources: audio {duration} s, sketch {result}")
    if abs(duration - VIDEO_SECONDS) > DURATION_TOLERANCE:
        errors.append(f"sources: the audio lasts {duration} s, expected {VIDEO_SECONDS} s")
    if result["loudest"] <= 0:
        errors.append("sources: a.vol stays 0 with a video with sound")

    video, result = render(directory, "mix.mp4", setup=f"s0.initVideo({json.dumps(str(low))}); "
                           f"s1.initVideo({json.dumps(str(high))}); src(s0).add(s1).out()")
    samples = sound(video)
    shares = power(samples, 0.1, 0.9, LOW), power(samples, 0.1, 0.9, HIGH)
    print(f"render_test: mix: power of {LOW} and {HIGH} Hz {shares}")
    if min(shares) * SHARE_RATIO < max(shares):
        errors.append(f"mix: the powers of {LOW} and {HIGH} Hz are {shares}, expected equal shares")

    # The tone changes from LOW to HIGH at 1 s of the video; the render seeks to 1.5 s at its time 0.5 s.
    steps = tone_video(pathlib.Path(directory, "steps.mp4"), f"if(lt(t,1),{LOW},{HIGH})")
    seek = "if (time >= 0.5 && !window.seeked) { window.seeked = true; s0.src.currentTime = 1.5 }"
    video, result = render(directory, "seek.mp4", setup=f"s0.initVideo({json.dumps(str(steps))}); src(s0).out()",
                           update=seek)
    samples = sound(video)
    spans = {"before": (0.05, 0.45, LOW, HIGH), "after": (0.55, 0.95, HIGH, LOW)}
    for name, (start, end, loud, quiet) in spans.items():
        powers = power(samples, start, end, loud), power(samples, start, end, quiet)
        print(f"render_test: seek: {name} the seek, power of {loud} and {quiet} Hz {powers}")
        if powers[0] < TONE_RATIO * powers[1]:
            errors.append(f"seek: {name} the seek, {loud} Hz is not the tone ({powers})")
if errors:
    sys.exit("render_test: " + "\n".join(errors))
print("render_test: ok")
