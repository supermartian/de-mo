#!/usr/bin/env bash
set -euo pipefail

mvstab_binary=$1
if ! command -v ffmpeg >/dev/null; then
    exit 77
fi
if ! ffmpeg -hide_banner -encoders 2>/dev/null | grep 'libx264' >/dev/null; then
    exit 77
fi
if ! ffmpeg -hide_banner -filters 2>/dev/null | grep 'vidstabtransform' >/dev/null; then
    exit 77
fi

test_directory=$(mktemp -d /tmp/mvstab-e2e.XXXXXX)
trap 'rm -rf -- "$test_directory"' EXIT

ffmpeg -hide_banner -loglevel error \
    -f lavfi -i "nullsrc=size=500x240" \
    -vf "geq=lum='mod(17*X+31*Y+mod(X*Y\,97)\,256)':cb=128:cr=128" \
    -frames:v 1 -y "$test_directory/base.png"

printf 'sentinel\n' >"$test_directory/rejected.csv"
if "$mvstab_binary" dump "$test_directory/base.png" \
    -o "$test_directory/rejected.csv" 2>/dev/null; then
    exit 1
fi
grep '^sentinel$' "$test_directory/rejected.csv" >/dev/null

ffmpeg -hide_banner -loglevel error \
    -loop 1 -framerate 30 -i "$test_directory/base.png" -t 1 \
    -vf "crop=320:180:x=40+2*n:y=30+n" \
    -c:v libx264 -preset medium -crf 12 -bf 0 -g 10 -refs 1 \
    -x264-params scenecut=0 -pix_fmt yuv420p \
    -y "$test_directory/input.mp4"

printf 'sentinel\n' >"$test_directory/vectors.json"
"$mvstab_binary" dump "$test_directory/input.mp4" \
    --format json -o "$test_directory/vectors.json"
grep '^\[' "$test_directory/vectors.json" >/dev/null
"$mvstab_binary" inspect "$test_directory/input.mp4" \
    >"$test_directory/inspection.txt"
metadata_only=0
if grep 'motion metadata only' "$test_directory/inspection.txt" >/dev/null; then
    metadata_only=1
    grep '"reference_exact":true' "$test_directory/vectors.json" >/dev/null
    grep '"reference_pts_delta":' "$test_directory/vectors.json" >/dev/null
fi
if [[ ${MVSTAB_REQUIRE_EXACT_METADATA:-0} == 1 && $metadata_only != 1 ]]; then
    echo "patched FFmpeg metadata-only path was required but not selected" >&2
    exit 1
fi
if compgen -G "$test_directory/vectors.json.mvstab-tmp-*" >/dev/null; then
    exit 1
fi

ln "$test_directory/input.mp4" "$test_directory/input-alias.mp4"
if "$mvstab_binary" dump "$test_directory/input.mp4" \
    -o "$test_directory/input-alias.mp4" 2>/dev/null; then
    exit 1
fi

if "$mvstab_binary" analyze "$test_directory/input.mp4" \
    -o "$test_directory/new-output" \
    --stats "$test_directory/./new-output" 2>/dev/null; then
    exit 1
fi
test ! -e "$test_directory/new-output"

printf 'sentinel\n' >"$test_directory/preserved.trf"
if "$mvstab_binary" analyze "$test_directory/input.mp4" \
    -o "$test_directory/preserved.trf" \
    --stats "$test_directory/missing/stats.csv" 2>/dev/null; then
    exit 1
fi
grep '^sentinel$' "$test_directory/preserved.trf" >/dev/null

printf 'sentinel\n' >"$test_directory/rollback.trf"
if "$mvstab_binary" analyze "$test_directory/input.mp4" \
    -o "$test_directory/rollback.trf" --stats "$test_directory" 2>/dev/null; then
    exit 1
fi
grep '^sentinel$' "$test_directory/rollback.trf" >/dev/null

if "$mvstab_binary" analyze "$test_directory/input.mp4" \
    -o - --stats - 2>/dev/null; then
    exit 1
fi

"$mvstab_binary" analyze "$test_directory/input.mp4" \
    --mode safe --min-confidence 0.01 \
    -o "$test_directory/motion.trf" --stats "$test_directory/motion.csv"

awk -F, 'NR == 3 {dx=$4+2; dy=$5+1; exit !(dx*dx < 0.0001 && dy*dy < 0.0001 && $15 == 1)}' \
    "$test_directory/motion.csv"
awk -F, 'NR == 12 {dx=$4+2; dy=$5+1; exit !($3 == "I" && dx*dx < 0.0001 && dy*dy < 0.0001 && $18 == 1)}' \
    "$test_directory/motion.csv"
if [[ $metadata_only == 1 ]]; then
    awk -F, 'NR == 3 {exit !($23 == 1)}' "$test_directory/motion.csv"
fi

ffmpeg -hide_banner -loglevel error -i "$test_directory/input.mp4" \
    -vf "vidstabtransform=input=$test_directory/motion.trf:relative=1:smoothing=0:maxangle=0:optzoom=0" \
    -an -c:v libx264 -preset medium -crf 12 -bf 0 -g 30 -refs 1 \
    -x264-params scenecut=0 -y "$test_directory/output.mp4"

"$mvstab_binary" analyze "$test_directory/output.mp4" \
    --mode safe --min-confidence 0.01 \
    -o "$test_directory/residual.trf" --stats "$test_directory/residual.csv"

awk -F, 'NR == 3 {dx=$4 < 0 ? -$4 : $4; dy=$5 < 0 ? -$5 : $5; exit !(dx < 0.01 && dy < 0.01)}' \
    "$test_directory/residual.csv"
