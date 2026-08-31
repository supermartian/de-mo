# A journey to a pure bitstream based video stabilization

**ENG** | [中文](readme.zh-CN.md)

Can video be stabilized without looking at its pixels? `mvstab` tests a narrow
answer: for H.264, decode prediction metadata, estimate camera motion from the
encoder's final motion vectors, and leave pixel decoding to the single render
pass that must happen anyway.

The final system is fast and genuinely bitstream-only during analysis. It also
reveals a hard limit: codec motion is useful evidence, but it is not optical
flow. Long-horizon reasoning improves the result, yet pixel-domain vid.stab
still wins on difficult motion.

[![Eight-second original, mvstab, and vid.stab comparison](assets/mvstab-showcase.gif)](mvstab-comparison.mp4)

The top is the original. Bottom-left is pure-bitstream mvstab. Bottom-right is
pixel-domain vid.stab. Select the GIF for the full 6:19 comparison, or
[open the video directly](mvstab-comparison.mp4).

## The result first

We tested one H.264 Main profile driving clip: 720×480, 29.97 fps, 11,369
frames, and 379.35 seconds. Every candidate was rendered with the same
`vidstabtransform` settings, then measured again with the same
`vidstabdetect` configuration.

| Detector | Translation median | Translation RMS | Translation p95 | Rotation RMS |
|---|---:|---:|---:|---:|
| Previous pure-MV baseline (`e1fe820`) | 4.513 px | 10.187 px | 20.124 px | 0.751° |
| Final long-horizon mvstab | **4.211 px** | **9.881 px** | **19.739 px** | **0.717°** |
| Pixel-domain vid.stab | 2.283 px | 5.651 px | 8.014 px | 0.573° |

The final pure-bitstream analyzer took 7.77 seconds and about 67 MB RSS.
`vidstabdetect` took 12.71 seconds wall time, 246.57 CPU-seconds across its
threads, and about 129 MB RSS. These times cover motion analysis, not the
shared rendering and encoding pass.

So the final trade-off is clear: mvstab analyzed this clip about 1.6× faster
with roughly half the memory, but its remaining translation RMS was 75% above
vid.stab and its p95 tail was 146% above it.

## The original idea

Video stabilization has two separate jobs:

1. estimate camera motion;
2. decode, warp, crop, and encode the frames.

The second job needs pixels. The first might not. An inter-frame codec already
stores a motion model chosen by its encoder, so perhaps the detector can read
that model directly and avoid a separate pixel-domain analysis pass.

This is not “stabilization without ever decoding pixels.” The final render
still decodes every frame. The claim is narrower and useful: **motion analysis
can operate on H.264 syntax without reconstructing luma or chroma.**

## What H.264 actually stores

H.264 divides a picture into prediction partitions. A partition can be
intra-coded, or it can predict from one or two reference pictures. An
explicitly coded inter partition uses an explicit or inferred reference index
and carries a motion-vector difference, `MVD`. Its final vector is derived as

```text
final_mv(partition, reference) = predicted_mv(neighbours, reference) + MVD
```

The predictor depends on neighboring partitions, partition geometry,
reference identity, slice boundaries, and field mode. Skip/direct partitions
derive some or all reference and motion values without an explicit index or
MVD; B-direct may also use a co-located block from another picture. Therefore,
parsing only MVD values is wrong: MVD is a coding delta, not block motion.

The useful observation exists inside the decoder after normative MV and
reference derivation:

```text
partition rectangle + final MV + exact reference picture + prediction mode
```

Quarter-pixel luma vectors, 16×16 through 4×4 partitions, list-0/list-1
references, long-term references, P-skip, B-skip, and B-direct all have to be
handled with the decoder's actual rules.

## First attempt: use FFmpeg's public vectors

Stock FFmpeg can export `AV_FRAME_DATA_MOTION_VECTORS`. That made a useful
prototype, but it loses the exact slice-local H.264 reference. Its `source`
field generally describes only earlier versus later prediction.

That is insufficient for temporal normalization. `ref_idx_l0 = 0` does not
mean “previous frame”; it means entry zero in the active reference list for
that slice. Reordering, B pictures, and long-term references break the simple
assumption.

On the test clip, the old sign-only safe estimator produced meaningful
non-zero measurements on only 7 frames. The problem was not a lack of vectors.
It was a loss of reference identity and duration.

## Extracting exact motion without reconstructing pixels

The included FFmpeg patch exports each final prediction partition while the
H.264 decoder still has its slice-local reference lists. It records exact POC
and PTS deltas, list and index, long-term and field status, and skip/direct
provenance.

The patched decoder still entropy-decodes the stream, parses macroblocks, and
derives final motion. It deliberately skips the pixel-producing stages:

- inverse quantization and inverse transform;
- motion-compensated pixel reconstruction;
- intra pixel prediction;
- deblocking.

CABAC residual symbols still have to be consumed because later entropy
contexts depend on earlier syntax. Bitstream-only means “no reconstructed
frames,” not “jump over arbitrary compressed bits.”

This path exported 20,715,192 prediction records from the test clip. Full
decode and metadata-only decode produced byte-identical motion exports for all
frames. Decoder-only time fell from 12.12 to 3.28 seconds, a 3.7× speedup.

## Reference time is part of the measurement

A vector spanning one frame and the same vector spanning four frames do not
describe the same velocity. For current time `t_i`, exact reference time
`t_r`, and nominal output interval `Δt`, mvstab normalizes a coded displacement
`m` as

```text
v = m / (t_i - t_r)
d_one_frame = v · Δt
```

The sign follows from the actual timestamp difference, so the same equation
handles past references, future references, B pictures, and variable frame
duration. With exact timing, mvstab obtained non-zero motion on 11,088 of the
11,369 frames.

## Turning block prediction into camera motion

Codec vectors are not camera tracks. A textured road may produce thousands of
small partitions while the sky produces almost none. A car may own a coherent
vector field. Zero-skip may mean a good static match or simply no observable
direction.

For every exact reference edge, mvstab balances evidence over an 8×4 image
grid and fits an affine field around the image center:

```text
dx(x,y) = tx + a·x + b·y
dy(x,y) = ty + c·x + d·y

scale = (a + d) / 2
theta = (c - b) / 2
```

`tx`, `ty`, and `theta` are the global similarity motion sent toward
stabilization. The extra affine terms absorb unequal stretch and shear so they
do not leak as strongly into translation or rotation. Four Tukey-style
iteratively reweighted least-squares rounds suppress spatial outliers:

```text
w_robust(r) = (1 - (r / τ)²)²,  r < τ
              0,                r ≥ τ
```

The threshold `τ` follows the median residual with a configured lower bound.
A model also needs broad image coverage and support on opposing halves. A
tight cluster of internally consistent vectors is not accepted as global
camera motion.

## What the other bitstream signals taught us

Motion vectors were the only direct displacement observation. The pipeline
also used a small, tested set of H.264 information: some fields are exported
to the estimator, some arrive as ordinary frame metadata, and some are decoder
context required to derive the exported values correctly.

| Information used in this experiment | Pipeline role and measured value |
|---|---|
| Exact references and PTS | Patched export and estimator input. Essential: they turned sparse, approximate directions into correctly timed P- and B-picture edges. |
| Partition geometry | Patched export and estimator input. Useful, but area must be capped and grid-balanced to prevent textured regions from dominating. |
| Skip/direct modes | Patched export and estimator input. Useful as weak evidence; mvstab discounts them and rejects exact zero-skip vectors. |
| Multiple references | Patched export and estimator input. Useful only as separate, exactly timed edges; mixing them creates a false average. |
| Keyframe status | Ordinary frame metadata. A missing observation, not an automatic cut; history can cross an I-picture if both sides agree. |
| Slice boundaries | Decoder context only. Required for correct predictor and reference derivation, but never treated as an independent motion measurement. |

We also considered signals that the current patch does **not** export to the
estimator, so they were not experimentally evaluated here:

| Signal not tested | What it might tell us |
|---|---|
| Intra-block coverage | How much of a frame lacks inter evidence; it still cannot recover a direction. |
| Raw MVD | Little by itself: final MV already combines the predictor and MVD, while raw MVD is not physical motion. |
| Residual energy, coded-block pattern, QP | Whether a prediction was costly or heavily quantized; still no missing motion direction. |
| Packet size | A coarse complexity hint that mixes motion, residuals, headers, and rate-control decisions. |

The recurring lesson was observability: metadata can say that an estimate is
weak, inherited, local, or absent. It cannot manufacture displacement when the
encoder never represented one.

## Long-horizon analysis across keyframes

Single-frame robust fitting still confused persistent foreground with camera
motion. The final version keeps a compact motion summary for each occupied
8×4 grid cell and follows residual direction through neighboring cells over a
31-frame window.

A cell is classified as persistent foreground only when it:

- moves at least 1.5 pixels per nominal frame after camera-motion removal;
- agrees with at least eight other observations;
- occupies, with its peers, no more than one quarter of supported cells;
- leaves at least six cells for the background fit.

The affected reference edges are then refit without those cells. A broad
camera impulse is retained because it is not spatially compact.

An isolated keyframe has no inter blocks, so it is treated as a missing sample.
The history crosses it when translation, rotation, scale, and timestamps are
continuous. A discontinuity starts a new segment and prevents interpolation
through a cut.

This helped, but modestly: translation RMS improved from 10.187 to 9.881
pixels, and rotation RMS from 0.751° to 0.717°. Long sequences made foreground
ownership easier to identify; they did not create information in intra or
flat regions.

## Joining references with a pose graph

An exact prediction from reference frame `r` to current frame `i` becomes a
constraint on four camera-pose coordinates: X, Y, rotation, and scale.

```text
pose[i] - pose[r] ≈ edge[r → i]
```

The sparse least-squares solve weights an edge by confidence, spatial coverage,
and inverse squared temporal span. Dense adjacent edges form the main graph.
Long edges can bridge a genuinely sparse sequence, but are admitted
conservatively because codec reference choice is an encoder decision, not a
camera model. Disconnected components are anchored separately.

mvstab emits relative transforms; FFmpeg's `vidstabtransform` performs the
actual trajectory smoothing, crop policy, and image warp. This separation
keeps the detector comparison focused on motion estimation.

## How the comparison was made

The source was [this YouTube driving clip](https://www.youtube.com/watch?v=SVA2mq9l2X8).
It was already H.264, so no transcode was needed before analysis.

Both mvstab and vid.stab produced transforms for the same source. Each was
rendered with identical smoothing and no automatic zoom. Remaining motion was
then measured with:

```text
shakiness=5, accuracy=15, stepsize=6, mincontrast=0.25
```

A no-smoothing debug pass converted the detector output into comparable global
motions. This is not absolute ground truth—vid.stab is also an estimator—but
the shared residual protocol exposes how much motion remains after each
stabilization.

![Full-clip mvstab and vid.stab motion plot](assets/mvstab-motion-plot.png)

## The final gap

Against vid.stab, final pure-bitstream mvstab has:

- 84% higher median residual translation;
- 75% higher translation RMS;
- 146% higher translation p95;
- 25% higher rotation RMS.

The long tail is the central weakness. Encoder prediction is optimized for
rate-distortion, not physical motion. It may select distant references, reuse
zero vectors, spend detail on a moving car, or provide little evidence in flat
areas. Pixel-domain tracking sees image content directly and can choose its
own features; a pure-bitstream detector cannot recover observations that the
encoder did not preserve.

The experiment therefore succeeded as an acceleration path, not as a
vid.stab-quality replacement. Pure bitstream analysis is fast, deterministic,
and useful when avoiding a detector-side pixel decode matters. For the best
stabilization quality, reconstructed image evidence still wins.

## Build and run

Install CMake, a C11 compiler, `pkg-config`, and development packages for
`libavformat`, `libavcodec`, and `libavutil`. Building the patched extractor
also requires Git, Make, and the normal FFmpeg build toolchain. The comparison
scripts require Python 3; plotting additionally requires Matplotlib.

For the stock-FFmpeg fallback, build normally. It reconstructs pixels inside
the decoder and has approximate reference timing, but needs no FFmpeg patch:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The helper builds the pinned FFmpeg revision with the checked-in H.264 patch.
This is intentionally a minimal analysis build; it contains neither
`vidstabtransform` nor libx264 or a normal MP4 muxer.

```sh
scripts/build_patched_ffmpeg.sh /tmp/mvstab-ffmpeg /tmp/mvstab-ffmpeg-install

PKG_CONFIG_PATH=/tmp/mvstab-ffmpeg-install/lib/pkgconfig \
  cmake -S . -B build-patched -DCMAKE_BUILD_TYPE=Release \
    -DMVSTAB_REQUIRE_EXACT_METADATA_TESTS=ON
cmake --build build-patched -j

LD_LIBRARY_PATH=/tmp/mvstab-ffmpeg-install/lib \
  ctest --test-dir build-patched --output-on-failure
```

Analyze with the patched libraries:

```sh
LD_LIBRARY_PATH=/tmp/mvstab-ffmpeg-install/lib \
  build-patched/mvstab analyze input.mp4 \
    --mode safe -o motion.trf --stats motion.csv
```

Render with a **separate full FFmpeg installation** that contains
`vidstabtransform`, libx264, and the target muxer. This example enables
automatic zoom for normal viewing; the benchmark below disables it:

```sh
ffmpeg -i input.mp4 \
  -vf "vidstabtransform=input=motion.trf:relative=1:smoothing=30:optzoom=1" \
  -c:v libx264 -crf 18 -c:a copy stabilized.mp4
```

`relative=1` is required. The transform contains inverse translation and the
vid.stab-calibrated angle sign. Scale is estimated as a nuisance term but is
not currently emitted as zoom.

### Reproduce the comparison

The reported experiment used the patched libraries for mvstab analysis. One
separate full FFmpeg installation was shared by vid.stab detection, residual
detection, every render, and every debug pass. Download and inspect the source:

```sh
yt-dlp -f "bv*+ba/b" --merge-output-format mp4 \
  -o 'source.%(ext)s' "https://www.youtube.com/watch?v=SVA2mq9l2X8"

ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,profile,width,height,r_frame_rate \
  -of default=noprint_wrappers=1 source.mp4
```

The tested download was already H.264. If a future download is not, convert
only its video stream before using the exact extractor:

```sh
ffmpeg -i source.mp4 -map 0:v:0 -map 0:a? \
  -c:v libx264 -preset medium -crf 18 -c:a copy input.mp4
```

Otherwise rename or link `source.mp4` as `input.mp4`. Then generate both
transforms:

```sh
LD_LIBRARY_PATH=/tmp/mvstab-ffmpeg-install/lib \
  build-patched/mvstab analyze input.mp4 \
    --mode safe -o mvstab.trf --stats mvstab.csv

ffmpeg -i input.mp4 \
  -vf "vidstabdetect=result=vidstab.trf:shakiness=5:accuracy=15:stepsize=6:mincontrast=0.25" \
  -f null -
```

Render both with identical smoothing and `optzoom=0`:

```sh
ffmpeg -i input.mp4 \
  -vf "vidstabtransform=input=mvstab.trf:relative=1:smoothing=30:optzoom=0" \
  -c:v libx264 -preset medium -crf 18 -an mvstab-output.mp4

ffmpeg -i input.mp4 \
  -vf "vidstabtransform=input=vidstab.trf:relative=1:smoothing=30:optzoom=0" \
  -c:v libx264 -preset medium -crf 18 -an vidstab-output.mp4
```

Measure each rendered output with the same detector settings:

```sh
ffmpeg -i mvstab-output.mp4 \
  -vf "vidstabdetect=result=mvstab-residual.trf:shakiness=5:accuracy=15:stepsize=6:mincontrast=0.25" \
  -f null -

ffmpeg -i vidstab-output.mp4 \
  -vf "vidstabdetect=result=vidstab-residual.trf:shakiness=5:accuracy=15:stepsize=6:mincontrast=0.25" \
  -f null -
```

For comparable global motions, run `vidstabtransform` with no smoothing and
debug output from separate empty directories:

```sh
mkdir mvstab-residual-debug
cd mvstab-residual-debug
ffmpeg -i ../mvstab-output.mp4 \
  -vf "vidstabtransform=input=../mvstab-residual.trf:relative=1:smoothing=0:optzoom=0:debug=1" \
  -f null -
cd ..

mkdir vidstab-residual-debug
cd vidstab-residual-debug
ffmpeg -i ../vidstab-output.mp4 \
  -vf "vidstabtransform=input=../vidstab-residual.trf:relative=1:smoothing=0:optzoom=0:debug=1" \
  -f null -
cd ..
```

Summarize both residual global-motion files. Translation is measured as
`sqrt(x² + y²)`, p95 uses the nearest-rank definition, and rotation is
converted from radians to degrees before RMS:

```sh
python3 tools/summarize_motion.py \
  mvstab-residual-debug/global_motions.trf \
  vidstab-residual-debug/global_motions.trf
```

To compare mvstab's raw motion with vid.stab and reproduce the checked-in plot,
create the vid.stab global-motion file, then run the comparison tools:

```sh
mkdir vidstab-global
cd vidstab-global
ffmpeg -i ../input.mp4 \
  -vf "vidstabtransform=input=../vidstab.trf:relative=1:smoothing=0:optzoom=0:debug=1" \
  -f null -
cd ..

python3 tools/compare_vidstab.py \
  --codec mvstab.csv --vidstab vidstab-global/global_motions.trf

python3 tools/plot_motion.py \
  --codec mvstab.csv --vidstab vidstab-global/global_motions.trf \
  -o assets/mvstab-motion-plot.png
```

Useful inspection commands:

```sh
LD_LIBRARY_PATH=/tmp/mvstab-ffmpeg-install/lib \
  build-patched/mvstab inspect input.mp4 --frame 217

LD_LIBRARY_PATH=/tmp/mvstab-ffmpeg-install/lib \
  build-patched/mvstab dump input.mp4 --format csv -o vectors.csv
```

## Codec support

- **Exact, pixel-free analysis:** software H.264 using the included FFmpeg
  patch.
- **Approximate fallback:** software decoders that export
  `AV_FRAME_DATA_MOTION_VECTORS`; reference timing depends on the decoder.
- **No exact patched path yet:** HEVC, VP9, AV1, hardware decoders, and codecs
  whose decoders do not export final MVs.

A container format does not standardize motion metadata. Every codec has its
own partitions, reference lists, precision, merge/skip/direct rules, and
decoder data structures. Supporting another codec requires a codec-aware
exporter at the point where final motion and exact references coexist.

## Limits and repository map

- Analysis does not reconstruct pixels, but the patched decoder still
  allocates frame buffers and parses residual syntax.
- Damaged-stream concealment is disabled in metadata-only mode.
- The output is one global similarity transform; it cannot correct parallax,
  perspective, rolling shutter, or mesh motion.
- Production scene-cut segmentation and reset of external vid.stab smoothing
  remain future work.
- Rendering always requires one normal pixel decode.

See [design.md](design.md) for the decoder audit, patch contract, algorithms,
validation, and API boundaries. The original proposal is preserved in
[mvstab_ffmpeg_codec_motion_vector_design.md](mvstab_ffmpeg_codec_motion_vector_design.md).
