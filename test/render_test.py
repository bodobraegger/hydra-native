#!/usr/bin/env python3
"""Checks "render to file" (src/render.c): a sketch of 1 second at 30 fps and 320x180 gives a video with 30 frames of
that size, each frame has the color of `time` at that frame, and the window size returns after the render. An audio
file drives `a`; its sound goes into the video with "renderAudio": "file", and not with "muted" or "sources" without
a video source with sound. With "sources", the sound of test videos with tones goes into the video: one video for the
whole render, two videos in equal shares, and a seek during the render that changes the tone at that frame. The
picture of a video follows the render clock: a video of four colors shows each color at its time, also at
playbackRate 2, in a render faster than real time, and a change of color and tone comes at the same frame.
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
COLORS = {"black": (0, 0, 0), "red": (255, 0, 0), "lime": (0, 255, 0), "blue": (0, 0, 255),
          "white": (255, 255, 255)}
FRAME_TOLERANCE = 1  # frames between the change of color and of tone
# The gray level follows `time`, which starts at 0 and advances 1/fps per frame. The sketch starts the render as
# the command palette does, once ffprobe has given the duration of a video in s0, and logs the loudest `a.vol` of the
# render, the size after it and the seconds that it took.
SKETCH = """
solid(() => time, () => time, () => time).out()
let loudest = 0, begin = 0
update = () => { if (native.rendering()) { loudest = Math.max(loudest, a.vol); RENDER_UPDATE } }
const report = () => console.log('RESULT ' + JSON.stringify({ loudest, width, height,
  seconds: (native.now() - begin) / 1000 }))
const wait = () => native.rendering() ? setTimeout(wait, 50) : (report(), native.exit(0))
const start = () => {
  if (s0.src && Number.isNaN(s0.src.duration)) return setTimeout(start, 10)
  begin = native.now()
  native.renderToFile()
  setTimeout(wait, 50)
}
setTimeout(() => { RENDER_SETUP; start() }, 500)
"""
# Starts the video at 0 in the first frame of the render, without the frames that it played in real time before.
REWIND = "if (!window.rewound) { window.rewound = true; s0.src.currentTime = 0 }"


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


def center_pixels(video, pixel_format="gray", channels=1):
    pixels = subprocess.run(["ffmpeg", "-v", "error", "-i", str(video), "-f", "rawvideo", "-pix_fmt", pixel_format,
                             "-"], capture_output=True, check=True).stdout
    size, center = WIDTH * HEIGHT * channels, (HEIGHT // 2 * WIDTH + WIDTH // 2) * channels
    return [tuple(pixels[start + center:start + center + channels]) for start in range(0, len(pixels), size)]


def center_colors(video):
    """The name of the color of COLORS that is nearest to the center of each frame."""
    def nearest(pixel):
        return min(COLORS, key=lambda name: sum((a - b) ** 2 for a, b in zip(COLORS[name], pixel)))
    return [nearest(pixel) for pixel in center_pixels(video, "rgb24", 3)]


def tone_video(path, expression, colors=("black",)):
    """A video of VIDEO_SECONDS with the sound sin(2 PI f t), where `expression` gives f at the time t, and the
    `colors` one after the other in equal spans."""
    span = VIDEO_SECONDS / len(colors)
    pictures = [argument for color in colors
                for argument in ("-f", "lavfi", "-i", f"color={color}:size=64x64:rate={FPS}:duration={span}")]
    tone = f"aevalsrc='sin(2*PI*({expression})*t)':s={SAMPLE_RATE}:d={VIDEO_SECONDS}"
    subprocess.run(["ffmpeg", "-v", "error", *pictures, "-f", "lavfi", "-i", tone, "-filter_complex",
                    f"concat=n={len(colors)}[picture]", "-map", "[picture]", "-map", f"{len(colors)}:a", "-shortest",
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
    grays, expected = [pixel[0] for pixel in center_pixels(video)], [round(frame / FPS * 255) for frame in range(FPS * SECONDS)]
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
    steps = tone_video(pathlib.Path(directory, "steps.mp4"), f"if(lt(t,1),{LOW},{HIGH})", ("red", "blue"))
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

    # Each color lasts 0.5 s; the frames in the middle of each span show it.
    colors = tone_video(pathlib.Path(directory, "colors.mp4"), LOW, ("red", "lime", "blue", "white"))
    setup = f"s0.initVideo({json.dumps(str(colors))}); src(s0).out()"
    video, result = render(directory, "colors.mp4", audio="muted", setup=setup, update=REWIND, seconds=VIDEO_SECONDS)
    found, speed = center_colors(video), VIDEO_SECONDS / result["seconds"]
    print(f"render_test: colors: {found}, {result['seconds']:.2f} s for {VIDEO_SECONDS} s ({speed:.1f}x real time)")
    for time, color in [(0.25, "red"), (0.75, "lime"), (1.25, "blue"), (1.75, "white")]:
        if found[int(time * FPS)] != color:
            errors.append(f"colors: the frame at {time} s is {found[int(time * FPS)]}, expected {color}")
    if result["seconds"] >= VIDEO_SECONDS:
        errors.append(f"colors: the render took {result['seconds']} s, not faster than real time")

    setup = f"s0.initVideo({json.dumps(str(colors))}); s0.src.playbackRate = 2; src(s0).out()"
    video, result = render(directory, "rate.mp4", audio="muted", setup=setup, update=REWIND)
    found = center_colors(video)
    print(f"render_test: playbackRate 2: {found}")
    if found[int(0.5 * FPS)] != "blue":
        errors.append(f"playbackRate 2: the frame at 0.5 s is {found[int(0.5 * FPS)]}, expected blue (1 s)")

    # The color and the tone of `steps` change at 1 s of the video.
    setup = f"s0.initVideo({json.dumps(str(steps))}); src(s0).out()"
    video, result = render(directory, "sync.mp4", setup=setup, update=REWIND, seconds=VIDEO_SECONDS)
    picture = center_colors(video).index("blue")
    samples, span = sound(video), 1 / FPS
    tone = next(frame for frame in range(FPS * VIDEO_SECONDS)
                if power(samples, frame * span, (frame + 1) * span, HIGH) > power(samples, frame * span,
                                                                                    (frame + 1) * span, LOW))
    print(f"render_test: sync: the color changes at frame {picture}, the tone at frame {tone}")
    if abs(picture - tone) > FRAME_TOLERANCE:
        errors.append(f"sync: the color changes at frame {picture}, the tone at frame {tone}")
if errors:
    sys.exit("render_test: " + "\n".join(errors))
print("render_test: ok")
