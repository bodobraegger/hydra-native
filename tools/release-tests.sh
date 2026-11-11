#!/usr/bin/env bash
# The manual tests before a release (README.md), with the build of `make`: each stage starts hydra-native with a
# sketch, says what to do, and asks for pass or fail. The results go to build/release-test-results.env; a second
# run offers them as defaults.
set -euo pipefail

if [[ -t 1 ]] && command -v tput >/dev/null 2>&1 && [[ "$(tput colors 2>/dev/null || echo 0)" -ge 8 ]]; then
  BOLD=$(tput bold); DIM=$(tput dim); RESET=$(tput sgr0)
  BLUE=$(tput setaf 4); GREEN=$(tput setaf 2); YELLOW=$(tput setaf 3)
else
  BOLD=""; DIM=""; RESET=""; BLUE=""; GREEN=""; YELLOW=""
fi

STAGE_INDEX=0

# Only the current stage is on the screen; piped output stays as it is.
clear_screen() {
  [[ -t 1 ]] || return 0
  tput clear 2>/dev/null || printf '\033[2J\033[3J\033[H'
}

banner() {
  clear_screen
  printf '\n%s%s  %s%s\n' "$BOLD" "$BLUE" "$1" "$RESET"
  printf '%s  %s stages. Stop any time with Ctrl-C; a later run remembers the saved results.%s\n\n' "$DIM" "$TOTAL_STAGES" "$RESET"
  pause "Ready to start?"
}

stage() {
  clear_screen
  STAGE_INDEX=$((STAGE_INDEX + 1))
  printf '\n%s%sStage %s/%s: %s%s\n' "$BOLD" "$BLUE" "$STAGE_INDEX" "$TOTAL_STAGES" "$1" "$RESET"
}

say()  { printf '  %s\n' "$1"; }
step() { printf '  %s-%s %s\n' "$BLUE" "$RESET" "$1"; }
note() { printf '  %s%s%s\n' "$DIM" "$1" "$RESET"; }
warn() { printf '  %s! %s%s\n' "$YELLOW" "$1" "$RESET"; }

pause() {
  printf '  %s%s%s ' "$DIM" "${1:-Press Enter to continue}" "$RESET"
  read -r _ || true
}

confirm() {
  local reply=""
  printf '  %s? %s [y/N] ' "$YELLOW" "$1"
  read -r reply || true
  [[ "$reply" =~ ^[Yy] ]]
}

# ask KEY "Prompt" reads a value into $KEY; Enter keeps the value of KEY in ENV_FILE.
ask() {
  local key="$1" prompt="$2" current="" input
  [[ -f "$ENV_FILE" ]] && current=$(grep -E "^${key}=" "$ENV_FILE" | tail -n1 | cut -d= -f2-) || true
  printf '  %s%s%s %s' "$BOLD" "$prompt" "$RESET" "${current:+${DIM}[Enter keeps current]${RESET} }"
  read -r input || true
  printf -v "$key" '%s' "${input:-$current}"
}

# write_env KEY VALUE sets KEY=VALUE in ENV_FILE and replaces an older line of KEY.
write_env() {
  local key="$1" value="$2" tmp
  touch "$ENV_FILE"
  tmp=$(mktemp)
  grep -vE "^${key}=" "$ENV_FILE" > "$tmp" || true
  printf '%s=%s\n' "$key" "$value" >> "$tmp"
  mv "$tmp" "$ENV_FILE"
}

TOTAL_STAGES=5

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD="${HYDRA_NATIVE_BUILD:-$REPO/build}"
APP="$BUILD/hydra-native"
ENV_FILE="$BUILD/release-test-results.env"
WORK=$(mktemp -d)
export XDG_CONFIG_HOME="$WORK/config" # a fresh settings file; your own settings stay as they are
trap 'kill $(jobs -p) 2>/dev/null || true' EXIT
[[ -x "$APP" ]] || { warn "no build at $APP (run make in $REPO)"; exit 1; }

# run_app NAME ARGS... starts hydra-native in the background, with its own code socket and log.
run_app() {
  local name="$1"; shift
  "$APP" --socket "$WORK/$name.sock" "$@" > "$WORK/$name.log" 2>&1 &
  APP_PID=$!
}

# record KEY asks for pass or fail and a note, and writes both to the results file.
record() {
  local key="$1" verdict
  if confirm "Did it pass?"; then verdict=pass; else verdict=fail; fi
  ask NOTE "Note (Enter for none):"
  write_env "$key" "$verdict"
  write_env "${key}_NOTE" "\"${NOTE}\""
}

banner "hydra-native manual release tests"

stage "Screen picker: mutate and quit while it is open"
cat > "$WORK/picker.js" <<'JS'
s0.initScreen()
osc(10, 0.1, 0.8).blend(src(s0), 0.5).out()
JS
run_app picker "$WORK/picker.js"
say "hydra-native starts and the system picker for initScreen opens."
step "Do NOT choose a window. Click into the hydra window."
step "Press F8 (mutate) three times. Each mutation evaluates again and must replace the picker."
note "Expected: no crash, no 'Wayland display connection closed', at most one picker at a time."
step "With a picker still open, press Ctrl+Q in the hydra window."
pause "Press Enter after Ctrl+Q."
for _ in 1 2 3 4 5 6; do kill -0 "$APP_PID" 2>/dev/null || break; sleep 0.5; done
if kill -0 "$APP_PID" 2>/dev/null; then warn "hydra-native still runs 3 s after Ctrl+Q"; else say "hydra-native ended."; fi
if pgrep -f hydra-native-capture >/dev/null; then warn "a hydra-native-capture process still runs: $(pgrep -af hydra-native-capture)"
else say "No capture helper is left."; fi
note "Log: $WORK/picker.log"
step "Check that no picker dialog is left on the screen."
record PICKER

stage "File dialogs, Ctrl+S and reset"
mkdir -p "$WORK/sketches"
printf 'osc(30, 0.1, 1.5).kaleid(4).out()\n' > "$WORK/sketches/open-me.js"
run_app files
say "hydra-native starts without a file (a random example). The sketches folder is:"
say "  $WORK/sketches"
step "Ctrl+, opens the settings. Click the empty field in the 'file' row: the Open dialog appears."
step "Open open-me.js. Expected: the kaleid sketch runs, and the window title shows open-me.js."
step "Toolbar: the button 'clear' is now 'reset'. Change a number, press reset: the saved code comes back."
step "Press F7 (example). The file closes. Press Ctrl+S: the Save dialog appears."
step "Save as saved.js in the sketches folder."
pause "Press Enter after the save."
if [[ -s "$WORK/sketches/saved.js" ]]; then say "saved.js exists ($(wc -c < "$WORK/sketches/saved.js") bytes)."
else warn "no saved.js in $WORK/sketches"; fi
step "Change the code and press Ctrl+S: no dialog this time, the file gets the new code."
pause "Press Enter when done, then quit hydra-native with Ctrl+Q."
grep -q . "$WORK/sketches/saved.js" 2>/dev/null && note "saved.js now starts with: $(head -n1 "$WORK/sketches/saved.js")"
record FILES

stage "Patch bay: two instances"
node "$REPO/test/signalling_server.cjs" 0 > "$WORK/server.log" &
for _ in 1 2 3 4 5 6 7 8 9 10; do [[ -s "$WORK/server.log" ]] && break; sleep 0.3; done
PORT=$(awk 'NR == 1 { print $2 }' "$WORK/server.log")
SERVER="http://127.0.0.1:$PORT"
say "A local signalling server runs at $SERVER."
printf 'osc(20, 0.1, 1.2).kaleid(5).out()\n' > "$WORK/a.js"
printf '// paste the stream id of A into initStream\ns0.initStream("")\nsrc(s0).out()\n' > "$WORK/b.js"
mkdir -p "$WORK/server-config/hydra-native" # the server is a setting
printf '{ "galleryServer": "%s" }\n' "$SERVER" > "$WORK/server-config/hydra-native/settings.json"
XDG_CONFIG_HOME="$WORK/server-config" run_app a "$WORK/a.js"
XDG_CONFIG_HOME="$WORK/server-config" run_app b "$WORK/b.js"
say "Two windows start: A shows a kaleid, B is empty."
step "In A, press Ctrl+Alt+I: it copies the stream id of A (Ctrl+, shows it too)."
step "In B, paste the id between the quotes of initStream(\"\") and press Ctrl+Shift+Enter."
note "Expected: the kaleid of A shows in B within some seconds, and it moves."
pause "Press Enter when you have checked it, then quit both with Ctrl+Q."
record PATCH_BAY

stage "Autocomplete box over bright visuals"
cat > "$WORK/bright.js" <<'JS'
osc(30, 0.1, 1.5).
  kaleid(4).color(1, 0.8, 0.2)
  .modulate(noise(3), 0.3)
  .out()
// a line of code under the list
// another line of code under the list
JS
run_app completion "$WORK/bright.js"
step "Put the cursor after the '.' at the end of the first line and press Ctrl+Space."
note "Expected (list opacity 60 %): the box inverts the visuals, the code under it shows only faintly, the text is readable."
step "Ctrl+, : set 'list opacity' to 20 % and then to 100 % (- and +, or click the value and type)."
step "Open the list again after each change: 20 % shows more code below, 100 % hides it."
step "Ctrl+Shift+Space inside osc( ... ): the signature help has the same look."
pause "Press Enter when done, then quit with Ctrl+Q."
record COMPLETION

stage "macOS app (on a Mac)"
say "The workflow 'build' packs the macOS app (a manual run, or the tag of the release)."
step "Download the zip of the newest run:  gh run download -p '*macos*' \$(gh run list -w build -L 1 --json databaseId --jq '.[0].databaseId')"
step "On the Mac: unzip, move hydra-native.app to Applications, start it."
step "macOS blocks it: System Settings > Privacy and Security > 'Open Anyway'."
step "Check: the random example runs; Ctrl+Enter evaluates; Ctrl+, opens the settings."
step "Check: a.show() (the microphone permission), s0.initCam() (camera), s0.initScreen() (screen recording)."
step "Check: Ctrl+Q quits."
if confirm "Did you test on a Mac now?"; then record MACOS
else write_env MACOS "not tested"; fi

clear_screen
say "Results: $ENV_FILE"
cat "$ENV_FILE"
