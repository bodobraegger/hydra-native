# hydra-native manual

The reference of hydra-native: the editor, the settings, the keys and the inputs.
[README.md](../README.md) tells how to install, run and build it.

## Run

```sh
hydra-native sketch.js                             # window, vsync, code overlay, code socket
hydra-native setup.js sketch.js                    # evaluates setup.js, then shows and evaluates sketch.js
hydra-native 'https://hydra.ojack.xyz/?code=...'  # a sketch URL of the web editor
```

Ctrl+, opens the settings menu, which saves to settings.json. `hydra-native --help` lists the flags
for scripts and tests; `--socket PATH` sets the code socket for one run.

The program evaluates the files in the given order, in the global scope, like the browser console.
Top-level `const`, `let` and `class` declarations of sketch code become `var` before each evaluation. Thus the same
code can run again without a redeclaration error, and later evaluations see the names. Declarations in blocks and
functions stay as they are. Error messages keep their line numbers, and also their columns, except on the line of
a `class` declaration.
`loadScript("name.js")` with a relative path loads the file from the folder of the open sketch. During the
evaluation of the setup file or of another file of the command line, the path is relative to the folder of that
file. URLs, file:// URLs and absolute paths stay as they are.
Without a file, it starts like the web editor: with a random example and the info panel.
The last file opens in the code overlay, and Ctrl+S saves the overlay text to it.
If the file does not exist yet, the overlay starts empty and Ctrl+S creates the file.
Click the "file" row of the settings menu to open another .js file; without an open file, the row shows a dim
"choose a file...". The file replaces the code and runs; Ctrl+Z brings the old code back. Without an open file,
Ctrl+S asks for a file in a save dialog. The window title shows the name of the open file.
With an open file and unsaved changes, the bar asks before an action loses them: reset, clear, an example, another
file, a URL from the clipboard and quit (Ctrl+Q or the close button of the window). Press s to save the changes and
continue, d to discard them and continue, or Escape to cancel the action. Without an open file, nothing asks, as in
the web editor.
Two seconds after the last edit, hydra-native writes the unsaved code to a recovery file in the folder of
settings.json (see "Settings file"). Each run writes its own file, recovery-YYMMDD-HHMM.js with the local date and
time of its first unsaved edit, and "-2", "-3" and so on when another run has that name. Saving, discarding and a
quit with Ctrl+Q or the close button remove the file of the run. After a crash, a start with the same file restores
the newest recovery file of that file, and a start without a file restores the newest of all. The code opens with
its file, as unsaved changes, and the bar says so. Saving or discarding these changes removes the restored file.
The other recovery files stay, and the bar gives their number. The recovery.js of older versions counts as one more
recovery file. A start with a sketch URL keeps the restored file and names it in the bar.
With the setting "reload on change" (watch), a change on disk replaces the overlay text and evaluates it,
but only when the overlay has no unsaved edits. Otherwise the error bar says that the file changed, and the edits
stay.
The "setup file" row of the settings menu sets a script that runs once at the start, before the files of the
command line, for example the extensions and the MIDI helpers of your sketches. A new setup file runs at once, and
"clear" removes the setting. Errors of the setup file show in the error bar with the prefix `setup:`.
While a setup file is set, the overlay shows the dim line `await loadScript("<path>")` above the code. The path is
relative to the folder of the open sketch when the setup file is in that folder. The line is not part of the code:
hydra-native does not save it, does not evaluate it with the code, and does not put it into the URL of the code.
A click on the line runs the setup file again. After shared code, the setup file runs in the sandbox (see "Shared
sketches").

## Code overlay

The overlay shows the code on a dark, partly transparent background behind each line, with syntax colors
and bracket pair colors. Long lines wrap with a hanging indent of 2 (Alt+Z toggles the wrap).
Evaluated lines flash for 300 ms.
An evaluation error, a console warning during the evaluation, or a shader compile error shows in the bar at the bottom.
New shaders compile in the background, and each output shows its old shader until the new one is ready.
Thus an evaluation does not stop the visuals, and a shader compile error comes some frames after the evaluation.
The next successful evaluation clears the bar.
Ctrl+Shift+H hides and shows the overlay; the visuals keep running, and the evaluation keys keep working.
The default font is DejaVu Sans Mono at 20 pixels (settings menu: font size and font; Ctrl+plus and Ctrl+minus).
The toolbar at the top right has the buttons of the web editor: run, clear, extensions, example, mutate
(Shift: undo, Ctrl: change a function), share (with a server in the settings) and info. With an open file,
"clear" becomes "reset", which opens a list with two entries: "reset" brings back the saved file and runs it, and
"clear" closes the file and empties the code. Up, Down and Enter choose an entry, and Escape or a click outside
closes the list. The command "clear all and close the file" of the command palette does the same as "clear".
"clear", an example or a sketch URL closes the file, so that Ctrl+S cannot overwrite it; Ctrl+S then asks for a
file. The info panel has the
languages of the web editor except Arabic (from hydra-synth/l10n, cached in ~/.cache/hydra-native/l10n).
The extensions panel lists the catalog of hydra-synth/hydra-extensions; "+" adds the load code to the top.
The window title shows the hydra.ojack.xyz URL of the code after each run of all code, as the address bar of
the web editor does. Ctrl+Shift+L copies that URL, and Ctrl+Shift+O opens the URL in the clipboard.

The table in "Settings file" lists each row of the settings menu. A change applies at once and goes into the
settings file, except "code at start", which applies at the next start.
"input gain" multiplies the audio input before the analysis of `a` (0 to 400 %, default 100 %); the system
output, which has no automatic gain, often needs more than 100 %. The server is "off" (the
default; `pb` is undefined then), hydra.ojack.xyz (https://api.hydrasynth.xyz) or a custom URL; a change starts or
stops the patch bay and the "share" button without a restart. While the patch bay is connected, the panel shows
the stream id, which other peers pass to `s0.initStream` and which `pb.list()` shows; its "copy" button and
Ctrl+Alt+I copy it.

Click a number of the settings menu to type a value, also with its unit ("45 %", "20px"). Enter applies it,
Escape cancels. Up, Down and the wheel change it by 1, or by 10 with Shift.

### Settings file

All settings live in one JSON file: ~/.config/hydra-native/settings.json on Linux,
~/Library/Application Support/hydra-native/settings.json on macOS, %APPDATA%\hydra-native\settings.json on
Windows, or $XDG_CONFIG_HOME/hydra-native/settings.json when that variable is set. settings.example.json shows
every key. hydra-native reads the file at the start. Keys that hydra-native does not know stay in the file.
When the file is no valid JSON, the defaults apply, the error bar shows the error with its line, and nothing overwrites
the file.

| Key                   | Default          | Settings menu        | Meaning                    |
| --------------------- | ---------------- | -------------------- | -------------------------- |
| font                  | DejaVu Sans Mono | font (list, file...) | monospace font file        |
| fontSize              | 20               | font size            | font size of the overlay   |
| theme                 | "hydra"          | theme                | built-in or custom theme   |
| renderWhitespace      | "all"            | whitespace           | none to all                |
| autocompleteStyle     | "list"           | autocomplete         | list or ghost              |
| autocompleteTrigger   | "manual"         | trigger              | manual or typing           |
| signatureHelp         | "manual"         | signatures           | manual or paren            |
| autocompleteOpacity   | 60               | list opacity         | 0 to 100 percent           |
| codeBackgroundOpacity | theme alpha      | background opacity   | 0 to 100 percent           |
| codeBackgroundBlend   | "normal"         | background blend     | normal or difference       |
| codeTextBlend         | "normal"         | text blend           | normal or difference       |
| wordWrap              | true             | line wrap, Alt+Z     | soft wrap of long lines    |
| codeVisible           | true             | code at start        | the code at the start      |
| fullscreen            | false            | window               | fullscreen                 |
| vsync                 | true             | window               | wait for the refresh       |
| watch                 | false            | reload on change     | reload the sketch file     |
| audioSource           | "" (default)     | audio input          | input of `a`, or "none"    |
| audioGain             | 100              | input gain           | 0 to 400 percent           |
| language              | "" ($LANG)       | language             | language of the info panel |
| galleryServer         | "" (none)        | server               | gallery and patch bay      |
| streamRate            | 60               | stream rate out      | 1 to 60 fps of the output  |
| streamMaxRate         | 60               | stream rate in (max) | 1 to 60 fps of a stream    |
| l10nUrl               | "" (l10n)        | translation URL...   | editor.json of a language  |
| setupFile             | "" (none)        | setup file           | runs before the sketch     |
| renderFile            | "" (Videos)      | render: file         | the video file             |
| renderLength          | 30               | render: length       | 1 to 3600 seconds          |
| renderFps             | 60               | render: fps          | 1 to 240 frames per second |
| renderSize            | "1920x1080"      | render: size         | WxH, even, up to 8192      |
| renderAudio           | "sources"        | render: audio        | sources, file or muted     |
| renderAudioFile       | "" (none)        | render: audio file   | drives `a` in the render   |
| socket                | "" (runtime dir) |                      | the code socket (scripts)  |
| themes                | {}               |                      | custom themes, see below   |
| keybindings           | {}               |                      | keys, see below            |

"codeBackgroundOpacity" sets the opacity of the box behind each code line; without it, the theme sets it (72 %
in "hydra"). With "difference", the box or the text inverts the visuals below it, so the code stays readable
over bright and over dark visuals.


A custom theme in "themes" has a name, an optional "base" (a built-in theme, "hydra" by default) and colors as
"#rrggbb" or "#rrggbbaa": lineBackground, selection, flash, bracket, cursor, text, keyword, number, string,
comment, function, barBackground, error, info, toolbar, popupBackground, popupSelected, helpBackground,
helpKeys, bookmark, match, matchActive, matchText, ghost, whitespace and "brackets" (a list of three colors).
The built-in themes stay available by name.

"keybindings" maps a command name to a key, such as "Ctrl+Shift+Enter", a two-key command such as
"Ctrl+K Ctrl+U", a list of keys, or "" to unbind it, like keybindings.json of VS Code. The keys replace the
default keys of that command and win over other commands with the same key. F1 shows each command with its
name in brackets and its keys after these changes.

The editor has multiple cursors, find and replace, go to line, a command palette and autocomplete of hydra
names and of the words of the file. Autocomplete opens with Ctrl+Space, or while typing with the trigger "while
typing" of the settings menu. The style "list" shows the candidates below the word, or above its line when
there is no room below. "autocompleteOpacity" sets how much its box covers the code below it. The style "ghost
text" shows the rest of the best candidate after the cursor, like the inline suggestions of VS Code: Tab accepts
it, Escape dismisses it, Alt+] and Alt+[ show the other candidates.

The completions follow the type of the place. After a "." behind a chain, the list has the transforms of a chain
(color, coord, combine and combineCoord functions, and `out`), and no source functions. After the name of an
object of the running JavaScript, such as `Math.`, `JSON.` or a user object such as `midi.`, the list has the
properties of that object; a function goes in as a call with the cursor inside the parentheses. In a texture
argument (`src()`, the first argument of combine and combineCoord functions), it has o0 to o3, s0 to s3, `src()`
and the source functions. In a number argument, it has the default value, `() => `, `[ ]`, `time`,
`Math.sin(time)`, and the helpers of a setup file when they exist at run time: `cc[]`, `hit()`, `midi.beat`,
`midi.bpm` and `a.fft[]`. A helper goes into an arrow function, because a plain value would not change. A function
from these lists comes with its arguments and their defaults, for example `modulate(o0, 0.1)`. Tab and Shift+Tab
select the next and the previous argument, the last Tab goes behind the call, and Escape ends the tab stops.

Ctrl+Shift+Space shows the signature help of the hydra function around the cursor, as in VS Code: the call with
its parameters and defaults, the current parameter marked, and its type and default on a second line. It follows
the cursor between the commas, also in nested calls, and closes when the cursor leaves the call or with Escape.
With "signatureHelp": "paren" (settings menu: "on ("), it also opens after typing "(" behind a hydra function. It
has the look of the autocomplete style. The definitions come from the running hydra-synth, so functions of
`setFunction` and of extensions appear after their evaluation.

Typing an opening bracket or quote adds the closing one. Find works like
the find bar of a browser: it ignores case and accents, highlights all matches and shows their count.

## Send code from an editor

hydra-native listens on the Unix socket `$XDG_RUNTIME_DIR/hydra-native.sock`, or `%TEMP%\hydra-native.sock` on
Windows.
A client sends JavaScript, closes its write side and gets `ok` or the error with its stack.
Code from the socket does not enter the overlay text, because the overlay shows the sketch file.
Errors of socket code show in the error bar with the prefix `socket:`.
The reply comes before the new shaders are compiled, so shader compile errors show only in the error bar and on stderr.

```sh
echo 'osc(20, 0.1, 1.2).kaleid(5).out()' | hydra-native --send
socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/hydra-native.sock < block.js
```

From Neovim or Vim, send the visual selection with `:'<,'>w !hydra-native --send`.

## Keys

These are the default keys. Change any of them in the "keybindings" section of settings.json (see "Settings file");
F1 shows every command with its name and its current keys, and Ctrl+Shift+P finds a command by name.
Keys marked "always" also work while the overlay is hidden.

| Key                                            | Action                                                   |
| ---------------------------------------------- | -------------------------------------------------------- |
| Ctrl+Enter, Shift+Enter                        | evaluate the line or the selection (always)              |
| Alt+Enter                                      | evaluate the block between blank lines (always)          |
| Ctrl+Alt+Enter, Ctrl+Shift+Enter               | evaluate all (always)                                    |
| Ctrl+Shift+H                                   | hide or show the code (always)                           |
| Ctrl+S; Ctrl+Shift+S                           | save; save a screenshot and the code (always)            |
| F1; Ctrl+Shift+P; Ctrl+,                       | help; command palette; settings (always)                 |
| Ctrl+Alt+I                                     | copy the stream id of the patch bay (always)             |
| Ctrl+Shift+L; Ctrl+Shift+O; Ctrl+Shift+G       | copy the URL; open a URL; share with the gallery         |
| F7; F8, Shift+F8, Ctrl+F8; Ctrl+Shift+Delete   | example; mutate, undo, change a function; clear or reset |
| F11; Ctrl+Q                                    | fullscreen; quit (always)                                |
| Ctrl+Shift+F, Shift+Alt+F                      | format: indent by brackets and chains, with tabs         |
| Ctrl+/ (Ctrl+Shift+7 on the ch layout)         | toggle comment                                           |
| Alt+Up, Alt+Down; Shift+Alt+Down, Ctrl+Shift+D | move lines; duplicate lines                              |
| Ctrl+L; Ctrl+Shift+K; Ctrl+J                   | select line; delete line; join lines                     |
| Ctrl+D                                         | select the word, then add the next occurrence            |
| Ctrl+Shift+Up/Down, Ctrl+Alt+Up/Down           | add a cursor above or below                              |
| Alt+click; Shift+Alt+drag; Shift+Alt+I         | add a cursor; column selection; a cursor per line        |
| Alt+Left, Alt+Right                            | move by subword (camelCase)                              |
| Ctrl+F2, F2, Shift+F2; Ctrl+F3, Alt+F3         | bookmarks; find or select the word under the cursor      |
| Ctrl+K, then Ctrl+U, L, K, Backspace, D        | the two-key commands of the sublime keymap               |
| Ctrl+F, F3, Shift+F3; Ctrl+H; Ctrl+G           | find; replace (Tab, Enter, Ctrl+Alt+Enter); go to line   |
| Ctrl+Space; Tab, Escape; Alt+], Alt+[          | autocomplete; accept, dismiss; other ghost candidates    |
| Ctrl+Shift+Space                               | signature help of the hydra function around the cursor   |
| Tab, Shift+Tab; Escape (after a completion)    | next, previous argument of the call; end the tab stops   |
| Ctrl+], Ctrl+[; Tab, Shift+Tab                 | indent, outdent                                          |
| Ctrl+M; Ctrl+Shift+M                           | go to the bracket; select inside the brackets            |
| Escape                                         | close a popup, keep one cursor, leave fullscreen         |

Sketches add keys with the browser API, for example
`window.addEventListener('keydown', (event) => { if (event.key === '1') speed = 2 })`.
As in the browser, sketches get every key event, also the keys that the overlay uses.

## Inputs

All `init*` calls accept `{ width, height }` for the capture size (default 1280x720) and the regl texture options (`min`, `mag`, `wrap`).

| Call                            | Source                                                     |
| ------------------------------- | ---------------------------------------------------------- |
| `s0.initCam(index)`             | webcam number `index` (GStreamer on Linux, else SDL)       |
| `s0.initScreen()`               | a window or monitor that you pick in the system dialog     |
| `s0.initMonitor(index)`         | monitor number `index`, no dialog                          |
| `s0.initArea(x, y, w, h)`       | an area of the screen, no dialog                           |
| `s0.initWindow(delay)`          | the window that has the focus after `delay` seconds        |
| `s0.initVideo(url, {loop})`     | a file or stream (ffmpeg); YouTube links need `yt-dlp`     |
| `s0.initImage(url)`             | an image (ffmpeg)                                          |
| `s0.initCommand(shell, {w, h})` | any command that writes raw RGBA frames of w x h to stdout |

`initScreen()` opens the picker of xdg-desktop-portal, as `getDisplayMedia()` does in the browser.
`initMonitor`, `initArea` and `initWindow` use the Mutter ScreenCast API directly and open no dialog.
On macOS, ScreenCaptureKit and its picker do the same, and on Windows Windows.Graphics.Capture.
initVideo, initImage and vidRecorder need ffmpeg in PATH; the error bar names a missing program.
A relative path of initVideo and initImage is relative to the folder of the open sketch, as for `loadScript`.
A monitor stream sends no frame until something on that monitor repaints.
Window and area streams start at once.

`initVideo` plays in a loop unless you give `{ loop: false }`.
As in the web editor, `s0.src` controls the video like a `<video>` element:

```js
s0.src.currentTime = 12      // seek, in seconds
s0.src.pause(); s0.src.play()
s0.src.playbackRate = 0.5    // 0.25 to 4
s0.src.loop = false          // at the end, paused is true and an 'ended' event fires
s0.src.onended = () => s0.src.play()
```

`currentTime`, `duration` (from ffprobe, `NaN` until it is known) and `paused` give the state.
A seek, `play()` and a new `playbackRate` or `loop` start a new ffmpeg at the position, so the image changes after
about 0.2 seconds; meanwhile the texture keeps the last frame.
YouTube links play once and have no controls.
hydra-native plays no sound. The sound of a video goes only into a render to file, with the render audio
"sources" (see [Render to file](#render-to-file)).

`fetch()` and `XMLHttpRequest` read local files and send HTTP and HTTPS requests with curl.
The CORS and mixed-content rules of a web page do not apply, so a request can succeed here and fail in the web
editor. `vidRecorder.start()` and `vidRecorder.stop()` record the visuals to a WebM file
(VP9, 25 fps) in ~/Downloads. `navigator.requestMIDIAccess()` gives MIDI input and output. The `timeStamp` of a
MIDI message is its arrival time, in the time of `performance.now()`.

With a gallery server (settings menu: server), `pb` joins the room 'iclc' of the signalling server at that URL, as the web editor
does with its server, and publishes the output of hydra-native. `pb.list()` shows the peers, and
`s0.initStream(id)` receives a peer, such as a web editor in the same room. The patch bay sends and receives
VP8 video, and it needs ffmpeg in PATH for the video. It sends the output with the frames per second of "stream
rate out" and at most at 1280x720, and it scales a larger window down with the same aspect ratio. "stream rate in (max)" is the
most frames per second that hydra-native takes from a received stream: it drops the frames above it, also of a web
editor. `s0.initStream(id, { fps: 10 })` sets this maximum for one stream; the other params of `initStream` stay the
texture options. A receiving hydra-native tells a hydra-native sender its maximum. The sender then sends at the
highest maximum of its receivers when that is lower than its stream rate out; a web editor counts as 60.

The audio object `a` is the one of hydra-synth (`a.show()`, `a.setBins()`, `a.setSmooth()`, `a.fft`).
hydra-native processes the input with a noise suppression and an automatic gain control, as getUserMedia() does
in Chrome. It does not change the volume of the capture source, which Chrome does on Linux. Thus `a.setScale()`
and `a.setCutoff()` might need other values than in the browser.

On macOS, the first use of the microphone, the camera and the screen capture asks for a permission (System
Settings, Privacy and Security). `initMonitor`, `initArea`, `initWindow` and the system output as audio input
need the Screen Recording permission; start the capture again after you allow it. `initScreen` uses the picker
of the system (macOS 14 or newer). A program that a terminal starts uses the permissions of the terminal.

On Windows, `initCommand` runs its command with cmd.exe, so the command uses double quotes and `%NAME%`
variables. The microphone needs "Let desktop apps access your microphone" in the privacy settings. Windows draws
a yellow border around a captured window or monitor. The system output is not available as audio input.

## Render to file

The command "render to file" (Ctrl+Shift+P, or "start" in the "render" group of the settings menu) renders the
running sketch into a video file. It needs ffmpeg in PATH. It renders frame by frame at a fixed rate and size, as
fast as the GPU and the encoder allow, and it drops no frame. The "render" group of the settings menu sets the
options:

- file: the video file. Without a file, the render writes hydra-native-<date>-<time>.mp4 into the videos folder
  of the system (~/Videos). The extension sets the format: .mp4 gives H.264 (crf 18), .mov gives ProRes 422 HQ,
  .webm gives VP9.
- length, fps: the duration in seconds (30) and the frames per second (60).
- size: the size of the video in pixels as WxH (1920x1080), two even numbers up to 8192. The size can be larger
  than the window.
- audio: the sound of the video. "file" puts the sound of the audio file into the video, and the video ends with
  the shorter of the two. "muted" gives a video without sound. "sources" is the sound of the video sources of the
  sketch (see below). Without a video source with sound in the whole render, the video has no sound and the bar
  tells it at the end.
- audio file: a sound file that drives `a` during the render, also with "muted", and with "sources" until a video
  source sounds.

During the render, `time` starts at 0 and advances exactly 1/fps per frame. The timers (setTimeout, setInterval,
requestAnimationFrame), Date.now() and performance.now() follow the same frame time. The sketch draws into an
offscreen framebuffer of the render size, and `width` and `height` have that size. The window shows the frames,
without the code, and the bar shows the progress ("render 312 / 1800 frames"). Escape cancels the render and
deletes the partial file. At the end, the bar names the file. Then the real clock, the audio input and the size of
the window return.

With an audio file, ffmpeg decodes the file into mono samples at 48 kHz. Before each frame, the analysis of the
audio input gets the samples up to the time of that frame, so `a.fft` has the values that a live run with the
same sound has. Without an audio file and without a video source with sound, `a.fft` stays 0 during the render.

With "sources", each video source of `initVideo` with a file or an HTTP(S) URL that has an audio stream plays its
sound in the time of the render: at the start of the render, each playing video starts again at its position. A
seek, a new `playbackRate` or `loop`, `play()` and an `initVideo` during the render start its sound at the position
from that frame on. A pause, the end of a video without loop, `clear()` and `hush()` stop it. A `playbackRate`
changes the speed and the pitch together, as on tape. YouTube links and images give no sound. The sources mix in
equal shares: each source gets 1/n, where n is the most sources that sounded at the same time so far in the
render. Two sources play at 50/50; when one stops, the other keeps its share, so the loudness does not jump. The
analysis gets this mix, so `a.fft` follows it frame by frame, and the video file gets the same mix. The render
writes the video without sound and the mix next to the file (`<file>-video<extension>`, `<file>-sound.f32`), and
at the end ffmpeg puts them together into the file. The keys, the mouse and the code socket
do not reach the sketch until the end of the render.

The render does not replay MIDI. Camera, screen, video and stream sources deliver their frames in real time, not
in the time of the render. So the picture of a video drifts from its sound when the render is faster or slower
than real time.

## Text

`text(str, config)` gives a source with the text, for example `osc().diff(text("hydra\nnative")).out()`.
`strokeText` draws the outline, `fillStrokeText` the text with its outline, and `strokeFillText` the outline
below the text. These functions have the interface of the hydra-text extension, so sketches that use the
extension run without it; loading the extension changes nothing.

`config` is a font name or an object, and the global `hydraText` holds the defaults:

| Key           | Default      | Meaning                                                              |
| ------------- | ------------ | -------------------------------------------------------------------- |
| font          | "sans-serif" | a font family of the system, or serif, sans-serif or monospace       |
| fontStyle     | "normal"     | normal, bold, italic or "bold italic"                                |
| fontSize      | "auto"       | auto fills the width; "50%" is a part of that size; "80px" is a size |
| textAlign     | "center"     | left, center or right                                                |
| fillStyle     | "white"      | a color: a hex value, rgb() or a basic color name                    |
| strokeStyle   | "white"      | the color of the outline                                             |
| lineWidth     | "2%"         | the width of the outline, in pixels or as a part of the font size    |
| canvasResize  | 2            | the size of the texture, as a multiple of the output width           |
| interpolation | "linear"     | linear or nearest                                                    |

The text is as wide as the output, and "\n" starts a new line. `lineJoin` of the extension has no effect: the
corners of an outline are round. A character that the font lacks shows as the missing-glyph box of the font.
fontconfig (`fc-match`) finds the font; on a system without it, the text uses the code font.

P5 and the canvas APIs of the browser do not exist here; README.md gives the reasons.

## Shared sketches

Code that you do not write or open from a file runs in a sandbox: a sketch URL (also from the clipboard), a
sketch of the gallery, the load code and the examples of an extension, and a script that `loadScript` gets from
the network. From that moment until the program ends, sketches cannot run `initCommand`, cannot open local files
with `initVideo`, `initImage`, `fetch` or `loadScript`, and cannot reach the host functions (`native`). Cameras,
screen capture, network URLs, audio, MIDI and the patch bay keep working. The sandbox stays on after the shared
code, because that code can leave timers and changed globals behind. The examples of the toolbar come with the
program and start no sandbox. To use local files and commands again, restart hydra-native.
