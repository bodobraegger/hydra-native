#!/usr/bin/env bash
# Alternates native and Brave runs of the same sketches, so that GPU clock changes hit both.
# usage: bench/compare.sh [runs] [width] [height] [sketch.js ...]
set -euo pipefail
cd "$(dirname "$0")/.."
runs=${1:-3}; width=${2:-1920}; height=${3:-1080}; shift $(( $# < 3 ? $# : 3 ))
sketches=("$@")
[ ${#sketches[@]} -eq 0 ] && sketches=(../../2609-artrio.js ../../25.11.08-daz-kunsthaus.js)
for run in $(seq "$runs"); do
  for sketch in "${sketches[@]}"; do
    native=$(timeout 60 build/hydra-native --hidden --no-vsync --size "${width}x${height}" --bench 8 bench/prelude.js "$sketch" 2>/dev/null | grep -o '"fps":[0-9.]*' | cut -d: -f2)
    brave=$(timeout 90 node bench/brave-bench.mjs "$sketch" "$width" "$height" 2>/dev/null | grep -o '"fps":[0-9.]*' | cut -d: -f2)
    printf '%s\t%s\t%sx%s\tnative %.0f\tbrave %.0f\n' "$run" "$(basename "$sketch")" "$width" "$height" "$native" "$brave"
  done
done
