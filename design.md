# mvstab design

## Purpose

`mvstab` replaces the pixel-domain motion-detection stage of a video
stabilization pipeline with global motion estimated from codec motion vectors.
It deliberately reuses FFmpeg for decoding and `vidstabtransform` for
trajectory smoothing, cropping, interpolation, rendering, encoding, and muxing.

This document describes the implemented translation-only V0. The longer
`mvstab_ffmpeg_codec_motion_vector_design.md` records the original proposal and
future direction.

## Scope

V0 provides:

- an FFmpeg-backed command-line interface;
- raw codec motion-vector inspection and export;
- robust global 2D translation estimation;
- confidence and diagnostic statistics;
- safe P-frame timeline reconstruction;
- an experimental past/future-reference mode;
- old-format relative transforms accepted by `vidstabtransform`;
- comparison and plotting tools.

The core estimator uses plain C11 data structures and does not expose FFmpeg
types. Rotation, exact multi-reference timing, pixel fallback, custom
resampling, and production scene-cut segmentation are outside V0.

## Architecture

```text
container input
    |
    v
FFmpeg demux + software decode + AV_FRAME_DATA_MOTION_VECTORS
    |
    v
canonical MvstabFrame / MvstabVector
    |
    +--> inspect or raw CSV/JSON dump
    |
    v
robust translation estimator + confidence
    |
    v
display-timeline reconstruction
    |
    +--> estimator statistics CSV/JSON
    |
    v
relative vid.stab transform file
    |
    v
FFmpeg vidstabtransform + encode
```

The implementation is divided into four layers:

- `src/ffmpeg_reader.c` owns demuxing, decoder configuration, frame delivery,
  timestamp conversion, and FFmpeg-to-core adaptation.
- `src/mv_normalize.c`, `src/translation_fit.c`, `src/confidence.c`, and
  `src/timeline.c` form the FFmpeg-independent estimator core.
- `src/csv_writer.c`, `src/json_writer.c`, and `src/trf_writer.c` serialize
  diagnostics and transforms.
- `src/cli.c` parses commands, validates paths, stages outputs, and coordinates
  the pipeline.

## Coordinate and sign convention

Every `MvstabVector` uses image motion from the reference picture to the
current picture:

```text
dx = -motion_x / motion_scale
dy = -motion_y / motion_scale
```

The vector location is the destination block position. Block area supplies the
initial weight, capped by configuration so large partitions cannot dominate.
Past references have direction `-1`; future references have direction `+1`.

The internal convention represents measured image motion. The transform writer
negates `dx` and `dy` because stabilization applies the inverse motion. Output
is incremental and must be rendered with `vidstabtransform:relative=1`. The
first display frame always receives identity.

## Decoder boundary

The reader selects the best video stream, requests the decoder option
`flags2=+export_mvs`, and consumes `AV_FRAME_DATA_MOTION_VECTORS` from decoded
frames. Frames are emitted in decoder presentation order with a monotonically
assigned display index and `best_effort_timestamp` converted through the
stream time base.

Malformed vectors are rejected during normalization or estimation. `inspect`
reports a zero-vector input successfully; `dump` and `analyze` fail explicitly
rather than silently producing empty data or an identity transform file.

## Translation estimator

The estimator processes each reference direction independently:

1. Reject vectors with invalid scale, block dimensions, coordinates, weights,
   non-finite values, unsupported direction, or excessive magnitude.
2. Cap block-area weights.
3. Compute weighted medians for horizontal and vertical displacement.
4. Apply a median-absolute-deviation prefilter.
5. Refit the weighted-median translation and select residual inliers.
6. Compute confidence and spatial support.

Confidence combines:

- weighted inlier ratio, including rejected evidence in the denominator;
- median residual relative to the residual threshold;
- useful inlier count;
- occupied grid coverage;
- support across opposing horizontal and vertical image halves;
- past/future-reference agreement when applicable.

An estimate is valid only when it passes both `min_confidence` and
`min_spatial_coverage`. Rejected estimates become identity on the output
timeline; they are not resurrected by later interpolation.

## Reference modes

### Safe mode

Safe mode accepts past-reference vectors from P-frames. A valid P-frame motion
is treated as an anchor displacement since the previous P-frame or keyframe.
It is distributed over intervening display intervals, using PTS duration ratios
when all timestamps are finite and increasing, or equal fractions otherwise.

An invalid P-frame still advances the anchor. This prevents a later valid
estimate from being spread across an interval whose reference relationship is
unknown.

Periodic keyframes contain no usable inter-frame vectors. Motion is repaired
only when reliable motion exists on both sides, agrees within a conservative
velocity threshold, and the unresolved preceding frames are B-frames. Repair
never crosses a rejected P-frame.

### All-MV mode

All-MV mode is experimental. Past and sign-normalized future references are fit
separately. Contradictory direction models invalidate the frame regardless of
the configured confidence threshold. A single supported direction receives a
confidence penalty.

Stock `AVMotionVector` identifies reference direction but not exact reference
picture or PTS. Therefore this mode cannot normalize different reference
distances exactly and does not repair missing keyframe or rejected-frame
motion.

## Output contracts

`dump` writes one raw record per accepted codec vector in CSV or JSON. Raw JSON
uses `null` for unavailable non-finite metadata rather than invalid JSON number
tokens.

`analyze` writes:

- an old-format vid.stab transform row per display frame;
- optional CSV or JSON statistics containing the measured anchor, emitted
  per-frame motion, confidence components, validity, and interpolation state.

Transform and statistics outputs are staged beside their destinations. Before
publication, path aliases, hard links, input collisions, directories, and
duplicate destinations are rejected. Existing outputs are preserved as
same-directory backups; if either atomic replacement fails, both destinations
enter a best-effort rollback that restores those backups. A restoration failure
cannot be made fully transactional across two independent filesystem paths.

## Comparison tools

`tools/compare_vidstab.py` joins codec and reference records by display-frame
number and selects the sign with the lower RMSE. It reports correlation,
absolute-error percentiles, RMSE, PTS-normalized derivative and acceleration
error, confidence-conditioned metrics, and Parseval-normalized FFT energy in
the 0.1-to-0.5 cycles-per-sample jitter band.

Non-finite motion and confidence fields are rejected. A `nan` PTS emitted for a
missing timestamp is treated as unavailable, causing frame-number timing to be
used instead.

`tools/plot_motion.py` plots the full codec motion, confidence, and residual
series using the selected sign. An optional reference overlay uses only common
frame indices. Matplotlib is optional and used only by this plotting tool.

## Portability and dependencies

The estimator and writers build without FFmpeg by setting
`MVSTAB_BUILD_CLI=OFF`. The CLI needs libavformat, libavcodec, and libavutil.
CMake first probes pkg-config metadata and falls back to native header/library
discovery when pkg-config or the FFmpeg `.pc` files are unavailable.

The code targets C11. GCC/Clang builds use strict warnings; MSVC builds define
the required CRT compatibility setting. Windows destination comparison resolves
the final identity of an existing parent directory so junction aliases are
handled before files are created.

## Validation strategy

The test suite contains:

- motion-vector sign and normalization tests;
- robust translation, reference disagreement, outlier, and spatial-dispersion
  tests;
- safe/all-MV timeline, B-frame tail, keyframe, rejected-anchor, and VFR tests;
- transform, CSV, and JSON writer tests;
- comparison-tool alignment, timing, finite-input, and FFT-energy tests;
- an end-to-end H.264 test that generates known image motion, analyzes it,
  applies `vidstabtransform`, and verifies near-zero residual motion.

The end-to-end test also exercises input/output hard-link protection,
nonexistent path aliases, atomic dump replacement, two-output rollback, and
preservation of existing destinations on failure.

## Known boundaries

- Software decoding still reconstructs pixels internally even though motion
  detection consumes codec metadata.
- Only translation is estimated.
- Exact multi-reference temporal normalization requires reference identity and
  timing metadata not present in stock `AVMotionVector`.
- A single old-format vid.stab transform cannot reset the external smoother at
  a scene boundary; production cut handling requires segmented rendering or a
  richer transform interface.
- Low-confidence frames become identity instead of using pixel-domain fallback.
- Rendering requires an FFmpeg build containing `vidstabtransform`.
