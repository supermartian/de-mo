# mvstab

`mvstab` estimates global video translation from motion vectors exported by an
FFmpeg software decoder. It replaces the pixel-domain detection pass, then
leaves smoothing, zoom, interpolation, and rendering to FFmpeg's
`vidstabtransform` filter.

The current implementation is the translation-only V0 summarized in
[`design.md`](design.md). The original engineering specification remains in
`mvstab_ffmpeg_codec_motion_vector_design.md`. It supports:

- `inspect` for codec and motion-vector availability statistics;
- `dump` for canonical motion vectors in CSV or JSON;
- `analyze` for robust weighted-median translation, observable confidence,
  safe P-frame anchor interpolation, and old-format vid.stab transforms;
- H.264 and any other software decoder that exports
  `AV_FRAME_DATA_MOTION_VECTORS`.

## Build

Install CMake, a C11 compiler, and development packages for:

- `libavformat`
- `libavcodec`
- `libavutil`

`pkg-config` is recommended when the FFmpeg packages provide metadata. If it is
unavailable, CMake falls back to native header and library discovery.

Then build and test:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The estimator and serialization tests can be built without FFmpeg headers:

```sh
cmake -S . -B build-core -DMVSTAB_BUILD_CLI=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

## Use

Check whether a clip contains usable decoder motion vectors:

```sh
build/mvstab inspect input.mp4
build/mvstab inspect input.mp4 --frame 217
```

Dump the exported vectors. `dx` and `dy` use the canonical
reference-frame-to-current-frame image-motion convention:

```sh
build/mvstab dump input.mp4 --format csv -o vectors.csv
build/mvstab dump input.mp4 --format json -o vectors.json
```

Estimate translation:

```sh
build/mvstab analyze input.mp4 \
  --model translation \
  --mode safe \
  -o motion.trf \
  --stats motion.csv
```

Use `--stats-format json` with a `.json` stats path for JSON output.

Compare the stats with vid.stab global motion, including sign alignment, error,
temporal, and confidence-conditioned metrics:

```sh
python3 tools/compare_vidstab.py \
  --codec motion.csv \
  --vidstab global_motions.trf
python3 tools/plot_motion.py \
  --codec motion.csv \
  --vidstab global_motions.trf \
  -o comparison.png
```

The plotting command requires matplotlib; metric comparison uses only Python's
standard library. Comparison joins records by display-frame number, uses PTS
elapsed time for derivatives when available, and defines jitter-band energy as
Parseval-normalized FFT energy from 0.1 through 0.5 cycles per sample.

Render with vid.stab:

```sh
ffmpeg -i input.mp4 \
  -vf "vidstabtransform=input=motion.trf:relative=1:smoothing=30:maxangle=0:optzoom=1" \
  -c:v libx264 -crf 18 -c:a copy stabilized.mp4
```

Always specify `relative=1`. The transform writer emits the inverse of measured
image motion, which is the convention verified by the end-to-end sign
calibration test. The first frame receives an identity transform. Periodic
keyframes are filled only when motion on both sides is consistent; otherwise
they remain identity because continuity cannot be established.

## Modes and confidence

`--mode safe` accepts past-reference vectors from P-frames. A valid P-frame
anchor is divided across the display frames since the previous P-frame or
keyframe, in proportion to presentation-time intervals when timestamps are
usable. An invalid P-frame still advances the reference anchor so a later
estimate is not spread across the wrong interval.

`--mode all-mvs` is experimental. It fits past and direction-normalized future
references separately, rejects frames when those fits disagree, and lowers
confidence when only one direction is available. Stock `AVMotionVector` does
not identify the exact reference picture, so neither mode can perform exact
reference-time normalization for multi-reference streams.

Confidence combines weighted inlier ratio (including MAD-rejected evidence in
the denominator), residual, useful block count, spatial grid coverage, and
reference-direction agreement. Estimates below `--min-confidence`, or covering
less than `--min-coverage` of the spatial grid (default 0.125), become identity
transforms. Stats retain both the measured anchor and the final per-display-frame
output.

## Known limitations

- Software decoding still reconstructs frames internally.
- Only global translation is fitted; rigid rotation is not implemented.
- Stock FFmpeg cannot disambiguate multiple past or future references.
- Scene-cut detection and a way to reset external vid.stab smoothing at cuts are
  not implemented; smoothing can cross a real cut.
- Low-confidence frames use identity rather than a pixel-domain fallback.
- Rendering requires an FFmpeg build with `vidstabtransform`.
