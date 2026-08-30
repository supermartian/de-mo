# Design Proposal: Codec Motion-Vector–Driven Video Stabilization Using FFmpeg

**Project working name:** `mvstab`  
**Status:** Engineering design / implementation plan  
**Primary goal:** Replace pixel-domain motion detection with motion vectors extracted from the compressed video stream, while reusing FFmpeg and vid.stab for as much of the remaining pipeline as possible.  
**Initial target:** H.264 in MP4/MOV, software decode, translation-only stabilization, Linux/macOS/Windows CLI.  
**Long-term target:** A small motion-estimation core suitable for a high-performance browser implementation.

---

## 1. Executive Summary

Traditional video stabilization performs motion estimation from decoded pixels, then smooths the estimated camera trajectory and transforms each frame. For already-compressed video, this duplicates work that the encoder performed during inter-frame prediction.

H.264, HEVC, VP9, AV1, and similar codecs already contain block-level motion information. FFmpeg can expose motion vectors from supported decoders through `AV_FRAME_DATA_MOTION_VECTORS`. The proposed project uses that information as the primary input for global camera-motion estimation.

The first implementation deliberately avoids reimplementing the rest of the stabilization stack.

```text
Input video
    |
    v
FFmpeg/libavformat
    |
    v
FFmpeg/libavcodec
  + export_mvs
    |
    v
AVMotionVector[]
    |
    v
mvstab
  - normalize vectors
  - reject bad vectors
  - estimate global translation
  - later: estimate rotation
  - compute confidence
    |
    v
vid.stab-compatible transform stream
    |
    v
FFmpeg + vidstabtransform
    |
    v
Stabilized output
```

This architecture gives us an extremely small experimental surface:

1. FFmpeg demuxes the container.
2. FFmpeg decodes enough codec syntax to expose block motion vectors.
3. `mvstab` converts those vectors into per-frame global motion.
4. Existing `vidstabtransform` performs trajectory smoothing, cropping/zooming, interpolation, and frame transformation.
5. FFmpeg performs the final encode and mux.

The central technical question is therefore isolated:

> **Can codec motion vectors estimate camera motion accurately enough to replace pixel-domain `vidstabdetect`?**

The project should answer that question before implementing custom rendering, WebCodecs integration, GPU transforms, or a standalone bitstream parser.

---

## 2. Goals

### 2.1 Primary Goals

The first useful version should:

- Read H.264 video through FFmpeg/libavformat.
- Request exported decoder motion vectors through FFmpeg/libavcodec.
- Associate exported vectors with decoded frames and timestamps.
- Convert codec vector conventions into a canonical image-motion convention.
- Estimate one robust global 2D translation per usable frame.
- Attach a confidence score to every estimate.
- Export:
  - raw motion-vector CSV/JSON for debugging,
  - per-frame global-motion CSV/JSON,
  - a transform file accepted by `vidstabtransform`.
- Use existing FFmpeg + vid.stab to produce stabilized video.
- Provide tools to compare the codec-MV estimator against ordinary `vidstabdetect`.
- Keep the core estimator independent from FFmpeg-specific data structures.

### 2.2 Secondary Goals

After translation-only works:

- Add global rotation estimation.
- Add scene-cut handling.
- Improve B-frame handling.
- Improve multi-reference handling.
- Support HEVC.
- Add temporal confidence propagation.
- Add automatic fallback to pixel-based motion estimation on low-confidence frames.
- Add an FFmpeg patch that exports exact reference-frame metadata.
- Make the estimator portable to WebAssembly.

### 2.3 Non-Goals for V0

The first version should **not** attempt to implement:

- mesh warping,
- perspective stabilization,
- affine shear,
- rolling-shutter correction,
- optical-flow warping,
- custom frame resampling,
- custom H.264 parsing,
- custom H.264 entropy decoding,
- custom video encoding,
- a GUI,
- WebAssembly,
- hardware-decoder motion-vector extraction.

Keeping these out of V0 is intentional.

---

## 3. Why Reuse FFmpeg

FFmpeg already provides almost every component we need except the actual global-camera-motion estimator.

### FFmpeg can already provide

- MP4/MOV/MKV/etc. demuxing.
- Codec discovery.
- H.264/H.265/etc. decoding.
- Frame reordering.
- presentation timestamps.
- frame type information.
- exported block motion vectors for codecs that support the feature.
- visualization through `codecview`.
- final decoding, filtering, encoding, and muxing.
- vid.stab integration through `vidstabtransform`.

The official FFmpeg `extract_mvs.c` example already demonstrates the basic API:

```c
av_dict_set(&opts, "flags2", "+export_mvs", 0);
avcodec_open2(dec_ctx, dec, &opts);
```

For each decoded frame:

```c
AVFrameSideData *sd =
    av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
```

The payload contains an array of:

```c
AVMotionVector
```

This means V0 does not need any FFmpeg patch at all.

---

## 4. Important Constraint: FFmpeg Exported Motion Vectors Are Not Complete Reference Metadata

`AVMotionVector` currently provides fields including:

```c
int32_t  source;
uint8_t  w;
uint8_t  h;

int16_t  src_x;
int16_t  src_y;

int16_t  dst_x;
int16_t  dst_y;

uint64_t flags;

int32_t  motion_x;
int32_t  motion_y;

uint16_t motion_scale;
```

The documented meaning of `source` is only:

- negative: reference comes from the past,
- positive: reference comes from the future.

This is insufficient to distinguish, for example:

```text
current frame: 100

vector A -> reference frame 99
vector B -> reference frame 97
```

Both may appear simply as a negative `source`.

For basic P-frame experiments this limitation is manageable.

For robust B-frame and multi-reference processing it is not.

Therefore the roadmap explicitly separates:

- **V0:** stock FFmpeg, no internal patch.
- **V1:** tiny FFmpeg patch exposing exact reference metadata.

---

## 5. Proposed Repository Layout

```text
mvstab/
├── CMakeLists.txt
├── README.md
├── LICENSE
│
├── include/
│   ├── mvstab/
│   │   ├── frame_motion.h
│   │   ├── motion_vector.h
│   │   ├── estimator.h
│   │   └── confidence.h
│
├── src/
│   ├── main.c
│   ├── cli.c
│   ├── ffmpeg_reader.c
│   ├── mv_normalize.c
│   ├── motion_filter.c
│   ├── translation_fit.c
│   ├── rigid_fit.c
│   ├── confidence.c
│   ├── trf_writer.c
│   ├── csv_writer.c
│   └── json_writer.c
│
├── tools/
│   ├── compare_vidstab.py
│   ├── plot_motion.py
│   ├── generate_test_clip.py
│   └── inspect_trf.py
│
└── tests/
    ├── test_mv_sign.c
    ├── test_translation.c
    ├── test_rotation.c
    ├── test_scene_cut.c
    └── samples/
```

The core estimator should operate on project-owned data types rather than directly on `AVMotionVector`.

That separation matters for eventual WebAssembly use.

---

## 6. CLI Design

The proposed executable:

```bash
mvstab
```

### 6.1 Inspect Codec and Motion-Vector Availability

```bash
mvstab inspect input.mp4
```

Example:

```text
Input:       input.mp4
Codec:       H.264 High
Resolution:  3840x2160
Frame rate:  59.940 fps
Duration:    122.01 s

Frames:
  I: 31
  P: 2411
  B: 4870

Motion vectors:
  frames with MV side data: 7250 / 7312
  total vectors:            12,531,442
  past-reference vectors:    8,715,211
  future-reference vectors:  3,816,231

Block sizes:
  4x4:    12.1%
  8x8:    31.4%
  16x16:  42.0%
  other:  14.5%

MV magnitude:
  p50:  1.25 px
  p90:  8.75 px
  p99: 38.50 px
```

This command should be implemented very early because it tells us whether a clip is useful for experimentation.

---

### 6.2 Dump Raw Exported Motion Vectors

```bash
mvstab dump input.mp4 --format csv -o mvs.csv
```

Suggested CSV:

```text
frame_index,pts_seconds,pict_type,source,
dst_x,dst_y,src_x,src_y,
block_w,block_h,
motion_x,motion_y,motion_scale,
mv_ref_to_cur_x,mv_ref_to_cur_y,
flags
```

Example row:

```text
217,7.2072,B,-1,
640,352,636,353,
16,16,
-16,4,4,
4.0,-1.0,
0x0
```

JSON output should also be supported for tooling.

---

### 6.3 Analyze Global Motion

```bash
mvstab analyze input.mp4 \
    --model translation \
    --mode safe \
    --output motion.trf \
    --stats motion.csv
```

Later:

```bash
mvstab analyze input.mp4 \
    --model rigid \
    --mode all-mvs \
    --output motion.trf
```

---

### 6.4 Produce a Stabilized Video

The project should initially leave rendering to FFmpeg:

```bash
ffmpeg -i input.mp4 \
  -vf "vidstabtransform=input=motion.trf:relative=1:smoothing=30:maxangle=0:optzoom=1" \
  -c:v libx264 -crf 18 \
  -c:a copy \
  stabilized.mp4
```

For the translation-only model, explicitly use:

```text
maxangle=0
```

to guarantee that the rendering stage cannot introduce rotation.

For a "no dynamic breathing" mode:

```text
optzoom=1
```

uses static zoom rather than adaptive per-frame zoom.

For debugging geometry without zoom:

```text
optzoom=0
```

---

## 7. Canonical Motion-Vector Convention

This must be established explicitly because codec MV sign conventions are easy to misuse.

FFmpeg documents:

```text
src_x = dst_x + motion_x / motion_scale
src_y = dst_y + motion_y / motion_scale
```

Therefore:

```text
codec_mv_x = motion_x / motion_scale
codec_mv_y = motion_y / motion_scale
```

describes the displacement from the **current/destination block location toward the reference/source location**.

For stabilization we want a canonical vector describing where image content moved:

```text
reference frame -> current frame
```

Thus define:

```text
image_motion_x = dst_x - src_x
image_motion_y = dst_y - src_y
```

equivalently:

```text
image_motion_x = -motion_x / motion_scale
image_motion_y = -motion_y / motion_scale
```

The project-owned representation should therefore be:

```c
typedef struct {
    double x;
    double y;

    double dx;   // reference -> current image displacement
    double dy;

    double weight;

    int width;
    int height;

    int reference_direction; // -1 past, +1 future, 0 unknown

    uint64_t codec_flags;
} MvstabVector;
```

### 7.1 Sign Calibration Test

Do not trust this convention only by inspection.

Generate synthetic videos with known motion:

1. A fixed image translated +10 px in X every frame.
2. A fixed image translated -7 px in Y every frame.
3. Known diagonal translation.
4. Known rotation.

Encode them with H.264.

Then ensure:

```text
estimated image motion == injected image motion
```

before any stabilization-transform sign is implemented.

---

## 8. Camera Motion vs. Image Motion

The estimator observes image motion.

Example:

```text
camera moves right
        |
        v
background image moves left
```

The final stabilizing frame transform is the inverse of the unwanted image trajectory.

However, `vidstabtransform` has its own interpretation of supplied relative transforms.

Therefore **do not hard-code the final sign based purely on intuition**.

Implement an automated calibration:

```text
synthetic known translation
        |
        v
codec MV estimate
        |
        v
write candidate .trf
        |
        v
vidstabtransform
        |
        v
measure residual motion
```

Test both transform signs once and freeze the proven convention in code and unit tests.

This prevents subtle sign inversions from surviving into production.

---

## 9. V0 Pipeline

### 9.1 Demux

Use:

```c
avformat_open_input()
avformat_find_stream_info()
av_find_best_stream()
```

### 9.2 Decoder Setup

Copy stream codec parameters into `AVCodecContext`.

Before `avcodec_open2()`:

```c
AVDictionary *opts = NULL;
av_dict_set(&opts, "flags2", "+export_mvs", 0);
```

V0 should use software decoding.

Do not rely on NVDEC, VAAPI, VideoToolbox, DXVA, or other hardware decoders because decoder-internal motion vectors are generally not exposed through a portable API.

### 9.3 Decode

Use normal FFmpeg send/receive:

```c
avcodec_send_packet()
avcodec_receive_frame()
```

This also means:

> V0 is not yet a "motion-vector-only parser."

The software decoder may still reconstruct complete frames internally.

That is acceptable for the first experiment.

The objective of V0 is to eliminate **duplicate motion estimation**, not necessarily all pixel reconstruction.

---

## 10. Frame Metadata

For every decoded frame, record at least:

```c
typedef struct {
    int64_t decode_index;
    int64_t display_index;

    int64_t pts;
    double pts_seconds;

    enum AVPictureType pict_type;

    int key_frame;

    int width;
    int height;

    MvstabVector *vectors;
    size_t vector_count;
} MvstabFrame;
```

Use presentation timestamps rather than assuming constant frame rate.

Prefer the best available presentation timestamp exposed by FFmpeg.

The estimator should operate in display-time order.

---

## 11. V0 Operating Modes

Because exact reference-frame identity is not available from stock `AVMotionVector`, provide two explicit modes.

---

### 11.1 `--mode safe`

Goal:

> Establish whether codec MVs contain a high-quality global camera-motion signal without being derailed by ambiguous B-frame references.

Initial behavior:

- Ignore I-frames.
- Prefer P-frames.
- Use past-reference vectors.
- Ignore ambiguous future-reference vectors.
- Treat accepted P-frames as motion anchors.
- Interpolate motion for unmeasured display frames when required.

For a GOP such as:

```text
I  B  B  P  B  B  P
|        |        |
anchor   anchor   anchor
```

the system initially estimates the P-frame anchor motion and fills intermediate display frames conservatively.

This is not the final algorithm.

It is an experimental baseline.

---

### 11.2 `--mode all-mvs`

Experimental behavior:

- Accept past-reference and future-reference vectors.
- Fit them separately.
- Compare model consistency.
- Derive a confidence score.

Example:

```text
past-reference global estimate:   (+3.1, -0.8)
future-reference global estimate: (+3.0, -0.7)
```

This is likely a high-confidence frame.

If:

```text
past estimate:   (+3, -1)
future estimate: (-8, +6)
```

confidence should be low.

Without exact reference POC/PTS, this mode must not pretend it knows the exact temporal baseline of every vector.

---

## 12. Motion-Vector Filtering

Codec MVs are rate-distortion decisions, not ground-truth optical flow.

Filtering is essential.

### 12.1 Reject Invalid Vectors

Reject vectors when:

- `motion_scale == 0`,
- block dimensions are zero,
- coordinates are nonsensical,
- magnitude exceeds a configurable physical bound,
- metadata is malformed,
- vector reference direction is unsupported by the selected mode.

---

### 12.2 Block-Area Weighting

A natural first weight is:

```text
weight = block_width * block_height
```

However, large blocks should not dominate arbitrarily.

Use:

```text
weight = min(block_width * block_height, area_cap)
```

Example:

```text
area_cap = 256
```

Make the cap configurable.

---

### 12.3 Border Reweighting

Codec motion near frame boundaries may be less reliable.

Optionally reduce weights close to edges:

```text
distance-to-edge < threshold
    -> lower weight
```

Do not enable aggressive edge rejection by default until measured.

---

### 12.4 Magnitude Outlier Rejection

Before robust fitting, estimate the median vector and median absolute deviation.

Reject extreme outliers:

```text
|v - median(v)| > k * MAD
```

This is inexpensive and improves RANSAC behavior.

---

### 12.5 Spatial Coherence

For each vector, compare its motion with neighboring blocks.

A vector whose direction and magnitude disagree strongly with nearby blocks is likely:

- an independently moving subject,
- a bad codec match,
- an occlusion,
- or a multi-reference artifact.

Spatial coherence can become an additional weight rather than a binary filter.

---

## 13. Translation-Only Estimator

The first real model is:

```text
u_i = T_x
v_i = T_y
```

where every accepted block predicts the same global image translation.

---

### 13.1 Baseline: Weighted Median

Compute:

```text
T_x = weighted_median(u_i)
T_y = weighted_median(v_i)
```

This is extremely useful as V0.1 because it is:

- small,
- deterministic,
- resistant to independently moving foreground objects,
- easy to debug.

It should exist even after more advanced estimators are implemented.

---

### 13.2 Robust Consensus Fit

For better robustness, use consensus fitting.

Candidate model:

```text
T = (T_x, T_y)
```

Residual for vector `i`:

```text
r_i = sqrt(
    (u_i - T_x)^2 +
    (v_i - T_y)^2
)
```

An inlier satisfies:

```text
r_i < threshold_px
```

Suggested initial threshold:

```text
1.5 px
```

but scale it with resolution and codec behavior.

A candidate score should use covered image area, not only block count.

For example:

```text
score =
    sum(inlier_weight) /
    sum(all_weight)
```

After selecting the best consensus, refine using weighted robust averaging over inliers.

---

## 14. Confidence Metric

Every per-frame motion result should carry:

```c
typedef struct {
    double dx;
    double dy;
    double theta;

    double confidence;

    double inlier_weight_ratio;
    double residual_median;
    double residual_p95;

    int vector_count;
    int inlier_count;

    int valid;
} FrameMotion;
```

Suggested confidence components:

```text
C_area      = weighted inlier area ratio
C_residual  = function(median residual)
C_count     = function(number of useful blocks)
C_spatial   = spatial coverage score
C_temporal  = consistency with neighboring frames
C_refs      = agreement between reference directions
```

Combine conservatively:

```text
confidence =
    C_area *
    C_residual *
    C_spatial *
    C_refs
```

or use a weighted geometric mean.

Confidence must remain observable in logs and CSV rather than being hidden inside the estimator.

---

## 15. Spatial Coverage Matters

A failure mode:

```text
1000 motion vectors
all concentrated on one moving person
```

would have a high vector count but poor camera-motion evidence.

Divide the frame into a coarse grid:

```text
8 x 4
```

or:

```text
8 x 8
```

Track how many cells contain inlier support.

A good global-motion estimate should cover a significant fraction of the image.

Example:

```text
coverage = occupied_inlier_cells / usable_cells
```

This should contribute to confidence.

---

## 16. Scene-Cut Detection

At a scene cut:

- motion vectors may disappear,
- vectors may become meaningless,
- residuals may explode,
- reference structure resets.

Detect cuts using signals already available:

- keyframe state,
- vector count collapse,
- inlier ratio collapse,
- large residual spike,
- unusually large global motion,
- abrupt PTS/GOP change.

On a cut:

```text
motion = identity
trajectory state = reset
```

Never smooth across a detected scene boundary.

---

## 17. Translation + Rotation Model

Only add this after translation-only is validated.

For a small rotation around frame center:

```text
u_i ~= T_x - theta * y_i
v_i ~= T_y + theta * x_i
```

where coordinates are centered:

```text
x_i = block_center_x - frame_width / 2
y_i = block_center_y - frame_height / 2
```

Fit:

```text
T_x
T_y
theta
```

using robust least squares or RANSAC.

This preserves the project's "no warp" guarantee:

```text
allowed:
  translation
  rotation

not allowed:
  nonuniform scale
  shear
  homography
  mesh deformation
```

Later, optional uniform zoom may be supported separately.

---

## 18. Handling Variable Frame Rate

Do not assume:

```text
frame n == frame n-1 + fixed dt
```

Record real presentation timestamps.

Motion should conceptually be associated with:

```text
reference timestamp -> current timestamp
```

Once exact reference timestamps are available in V1, normalize:

```text
velocity_x = displacement_x / delta_t
velocity_y = displacement_y / delta_t
```

Then derive motion for the actual display interval.

V0 cannot do this perfectly for ambiguous multi-reference vectors, which is one reason to treat V0 as a quality experiment rather than the final temporal model.

---

## 19. The B-Frame Problem

Consider display order:

```text
100  101  102
```

A B-frame at 101 may contain blocks referring to:

```text
100
102
```

or potentially other reference frames depending on codec structure.

Stock exported MV metadata tells us whether the source is broadly past or future but not the exact reference picture.

Therefore:

```text
MV / frame_interval
```

is not generally valid unless the actual reference distance is known.

This is the major reason V1 should expose reference information.

---

## 20. V1: Minimal FFmpeg Patch

If V0 shows codec vectors are useful, avoid writing a custom H.264 parser.

Instead patch FFmpeg narrowly so exported vectors carry:

- reference list: L0 or L1,
- reference index,
- reference POC or another stable picture identifier,
- ideally reference PTS or enough information to recover it.

### 20.1 Preferred Upstream-Quality Design

The clean long-term solution would be an FFmpeg API extension or new side-data structure.

Example conceptually:

```c
typedef struct AVMotionVectorEx {
    AVMotionVector mv;

    int8_t  ref_list;
    int8_t  ref_idx;

    int32_t ref_poc;
    int64_t ref_pts;
} AVMotionVectorEx;
```

That requires ABI/API discussion and is outside V0.

---

### 20.2 Experimental Fork Shortcut

For an internal experimental fork, the existing `flags` field could temporarily encode reference metadata.

Example private convention:

```text
bits 0..7     ref_idx
bit  8        list (0=L0, 1=L1)
bits 16..47   signed POC/reference delta
```

This is **not** proposed as an upstream ABI.

It is merely a fast way to validate the concept without changing the struct layout.

The internal fork must mark this clearly.

---

## 21. V1 Temporal Normalization

Once exact reference timing is known:

For every block:

```text
current PTS = t_cur
reference PTS = t_ref
```

with image displacement:

```text
d = reference -> current
```

compute:

```text
v = d / (t_cur - t_ref)
```

This lets vectors from different reference distances contribute to a common instantaneous-motion estimate.

Example:

```text
MV A:
  ref = n-1
  dt = 16.7 ms
  displacement = 1.2 px

MV B:
  ref = n-4
  dt = 66.7 ms
  displacement = 4.7 px
```

Their normalized velocities may agree closely.

This also makes B-frame forward/backward references far easier to combine.

---

## 22. Future: Motion-Vector-Only Codec Parsing

Only after V1 should we consider avoiding full frame reconstruction.

The theoretical pipeline:

```text
container demux
    |
    v
NAL/OBU parsing
    |
    v
slice/tile parsing
    |
    v
entropy decoding
    |
    v
partition / prediction syntax
    |
    v
reference selection
    |
    v
motion-vector reconstruction
```

and stop before:

```text
inverse transform
pixel reconstruction
deblocking
SAO
color conversion
```

However, this is substantially more complex than merely reading motion-vector syntax because modern codecs use:

- motion-vector predictors,
- neighboring-block dependencies,
- temporal prediction,
- merge modes,
- reference lists,
- partition-tree state.

Therefore it should **not** be part of the initial implementation.

First prove that the signal is worth extracting.

---

## 23. vid.stab Transform Interoperability

Modern `vidstabdetect` writes a current `.trf` format containing lists of local motions.

However, `vidstabtransform` also accepts the older per-frame global-transform text format.

That format is ideal for this project.

Columns:

```text
time x y alpha zoom extra
```

where:

- `time` is ignored by the reader,
- `x`, `y` are translation,
- `alpha` is rotation in radians,
- `zoom` is percentage,
- `extra` should be `0`.

Example translation-only file:

```text
0 0.000000  0.000000 0.000000 0.000000 0
0 2.310000 -0.810000 0.000000 0.000000 0
0 1.820000 -1.020000 0.000000 0.000000 0
0 3.140000 -1.710000 0.000000 0.000000 0
```

Whether values are interpreted as relative or absolute is controlled by `vidstabtransform`.

Always specify:

```text
relative=1
```

for our per-frame incremental motion format.

Do not rely on version-specific defaults.

---

## 24. First-Frame Convention

The first display frame has no preceding image-motion estimate.

Emit:

```text
0 0 0 0 0 0
```

For scene-cut frames, also emit identity and reset temporal state.

---

## 25. Trajectory Smoothing

V0 should not implement its own production-quality smoother.

Reuse:

```text
vidstabtransform
```

Example:

```bash
ffmpeg -i input.mp4 \
  -vf "vidstabtransform=input=motion.trf:relative=1:smoothing=30:optzoom=1" \
  -c:v libx264 -crf 18 \
  -c:a copy \
  out.mp4
```

This leaves `mvstab` responsible only for measurement, not stabilization policy.

Eventually we may want our own trajectory smoother for:

- causal/streaming mode,
- low-latency operation,
- special pan preservation,
- Web implementation,
- confidence-aware filtering.

But not yet.

---

## 26. Debug Visualization

FFmpeg can visualize decoder motion vectors:

```bash
ffplay \
  -flags2 +export_mvs \
  input.mp4 \
  -vf "codecview=mv=pf+bf+bb"
```

Use this during debugging to inspect whether:

- background MVs are coherent,
- people dominate the field,
- B-frame vectors behave as expected,
- the encoder has produced meaningful inter prediction.

Add an `mvstab inspect --frame N` mode that prints the corresponding estimator statistics.

---

## 27. Comparison Against Ordinary vid.stab

The project needs a reproducible baseline.

### 27.1 Generate Standard vid.stab Analysis

```bash
ffmpeg -i input.mp4 \
  -vf "vidstabdetect=shakiness=5:accuracy=15:result=vidstab.trf" \
  -f null -
```

### 27.2 Extract vid.stab Global Motions

Run the second pass with debugging enabled:

```bash
ffmpeg -i input.mp4 \
  -vf "vidstabtransform=input=vidstab.trf:debug=1" \
  -f null -
```

This creates:

```text
global_motions.trf
```

### 27.3 Generate Codec-MV Motion

```bash
mvstab analyze input.mp4 \
  --model translation \
  --output codec.trf \
  --stats codec.csv
```

### 27.4 Compare

`tools/compare_vidstab.py` should compute:

```text
Pearson correlation:
  dx
  dy

Error after convention/sign alignment:
  median absolute error
  RMSE
  p90
  p95
  p99

Temporal metrics:
  derivative error
  acceleration error
  jitter-band energy

Confidence-conditioned metrics:
  error for confidence > 0.5
  error for confidence > 0.8
  error for confidence > 0.95
```

Also plot:

```text
frame -> dx
frame -> dy
frame -> confidence
frame -> residual
```

Correlation alone is insufficient; bias and scale errors matter.

---

## 28. More Important Validation: Residual Motion After Stabilization

Comparing against vid.stab is useful but not ground truth.

The stronger test is:

```text
input
  |
  v
our estimator
  |
  v
stabilize
  |
  v
measure residual background motion
```

For synthetic clips the true residual should approach zero.

For real clips, compare:

- original,
- vidstabdetect-based stabilization,
- codec-MV-based stabilization.

Measure residual motion using an independent estimator, preferably one not used by either pipeline.

This prevents "agreement with vid.stab" from becoming the only quality definition.

---

## 29. Synthetic Test Corpus

Generate clips with known transforms.

### Case A: Translation

```text
dx = +3 px/frame
dy = -1 px/frame
```

### Case B: Sinusoidal Hand Shake

```text
dx(t) = 7 sin(2*pi*3*t)
dy(t) = 4 sin(2*pi*4.1*t)
```

### Case C: Smooth Pan + Shake

```text
dx(t) =
    smooth_pan(t)
  + high_frequency_shake(t)
```

This is important because stabilization should remove shake without destroying intentional pan.

### Case D: Foreground Motion

Static background plus a large moving person.

### Case E: Low Texture

Large blank wall.

### Case F: Water / foliage

Highly non-rigid background.

### Case G: Scene Cut

Two unrelated scenes.

### Case H: Different GOP Structures

Encode the same source with:

- no B-frames,
- 2 B-frames,
- 4 B-frames,
- different reference counts,
- different presets.

### Case I: Bitrate / Quality

Encode at:

- high quality,
- medium quality,
- aggressive compression.

This determines how encoder decisions affect stabilization quality.

---

## 30. Codec Dependence

Motion vectors are an encoder artifact.

The same visual source encoded by two encoders may produce different block MVs.

Test at least:

- x264,
- common camera-generated H.264,
- hardware-encoded H.264 when available.

Important variables:

- GOP length,
- B-frame count,
- reference count,
- macroblock/partition decisions,
- bitrate,
- encoder preset,
- scene-cut settings.

This is not necessarily a weakness; it simply means confidence estimation is mandatory.

---

## 31. Handling I-Frames

I-frames have no ordinary inter-frame MVs.

V0 options:

1. emit identity for the frame,
2. interpolate from neighboring reliable frame-motion estimates,
3. eventually use a pixel fallback.

For continuous stabilization, option 2 is preferable when timestamps and neighboring estimates permit it.

At actual scene boundaries, never interpolate across the cut.

---

## 32. Low-Confidence Fallback

Long-term architecture:

```text
codec MVs
   |
   v
global estimate
   |
   v
confidence
   |
   +---- high ---> use estimate
   |
   +---- low ----> pixel fallback
```

Pixel fallback can operate on:

```text
640-960 px wide grayscale image
```

rather than full-resolution 4K frames.

Possible fallback algorithms:

- FAST/GFTT + Lucas-Kanade,
- ORB matching,
- phase correlation,
- block matching.

This hybrid approach is likely stronger than either method alone.

---

## 33. Performance Expectations

V0 still performs FFmpeg software decoding.

Therefore V0 should **not** be sold as a complete decode-cost elimination.

The performance win being tested is:

```text
remove duplicated pixel-domain motion search
```

instead of:

```text
remove all video decoding
```

Expected computational structure:

```text
V0:
  demux
  software decode
  export already-decoded codec MVs
  cheap global fit
  second decode for rendering
  encode

traditional vid.stab:
  decode
  pixel-domain motion search
  second decode
  frame transform
  encode
```

If the estimator succeeds, later versions can attack decode overhead separately.

---

## 34. Potential Optimization: Analysis-Only Decode Path

After V0 validation, investigate whether FFmpeg's decoder can expose sufficient motion syntax while skipping some reconstruction work.

Possibilities include codec-specific modifications that avoid unnecessary:

- output pixel copies,
- color conversion,
- post-processing.

Do not assume that a generic FFmpeg "skip everything but MV" flag exists.

Measure first.

---

## 35. Why H.264 First

H.264 is the best first target because:

- enormous real-world sample availability,
- mature FFmpeg decoder,
- common camera/phone source format,
- easy synthetic generation with x264,
- mature tooling,
- straightforward comparison experiments.

HEVC should be the second target, not the first.

The CLI should fail clearly when a decoder does not provide MV side data.

Example:

```text
error: decoder produced no AV_FRAME_DATA_MOTION_VECTORS
codec: hevc
decoder: hevc
suggestion: try a supported software decoder or use --pixel-fallback
```

---

## 36. Build Plan

Linux example:

```bash
cc -O3 \
  src/*.c \
  $(pkg-config --cflags --libs \
      libavformat \
      libavcodec \
      libavutil) \
  -lm \
  -o mvstab
```

Prefer CMake for portability:

```bash
cmake -S . -B build
cmake --build build -j
```

Dependencies:

```text
libavformat
libavcodec
libavutil
```

The analyzer itself should not require libavfilter.

vid.stab is only required by the external FFmpeg binary used for rendering.

---

## 37. Recommended Internal Interfaces

### 37.1 Reader

```c
typedef int (*MvstabFrameCallback)(
    const MvstabFrame *frame,
    void *opaque
);

int mvstab_read_video(
    const char *path,
    MvstabFrameCallback cb,
    void *opaque
);
```

### 37.2 Estimator

```c
typedef enum {
    MVSTAB_MODEL_TRANSLATION,
    MVSTAB_MODEL_RIGID
} MvstabModel;

typedef struct {
    MvstabModel model;

    double ransac_threshold_px;

    double max_mv_px;
    double min_confidence;

    double block_area_cap;

    int safe_mode;
} MvstabEstimatorConfig;

int mvstab_estimate_frame(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *cfg,
    FrameMotion *out
);
```

### 37.3 Writer

```c
int mvstab_write_vidstab_global_trf(
    FILE *fp,
    const FrameMotion *motion
);
```

---

## 38. Suggested CLI Options

```text
mvstab analyze INPUT

Input:
  --video-stream N

Model:
  --model translation|rigid

Reference handling:
  --mode safe|all-mvs

Filtering:
  --max-mv PX
  --area-cap AREA
  --edge-margin PX
  --mad-threshold K

Robust fit:
  --ransac-threshold PX
  --ransac-iterations N

Confidence:
  --min-confidence X

Output:
  -o, --output FILE
  --stats FILE
  --dump-mvs FILE

Debug:
  --verbose
  --frame N
```

Avoid dozens of knobs in the first release.

Expose only values needed for experiments.

---

## 39. Example End-to-End Workflow

### Step 1: Check Input

```bash
mvstab inspect clip.mp4
```

### Step 2: Inspect Raw MVs

```bash
mvstab dump clip.mp4 -o clip-mvs.csv
```

Optional visualization:

```bash
ffplay \
  -flags2 +export_mvs \
  clip.mp4 \
  -vf "codecview=mv=pf+bf+bb"
```

### Step 3: Estimate Global Translation

```bash
mvstab analyze clip.mp4 \
  --model translation \
  --mode safe \
  -o clip-codec.trf \
  --stats clip-motion.csv
```

### Step 4: Stabilize With Existing vid.stab Transform Stage

```bash
ffmpeg -i clip.mp4 \
  -vf "vidstabtransform=input=clip-codec.trf:relative=1:smoothing=30:maxangle=0:optzoom=1" \
  -c:v libx264 -crf 18 -preset slow \
  -c:a copy \
  clip-stabilized.mp4
```

### Step 5: Produce Standard vid.stab Baseline

```bash
ffmpeg -i clip.mp4 \
  -vf "vidstabdetect=shakiness=5:accuracy=15:result=clip-vidstab.trf" \
  -f null -
```

```bash
ffmpeg -i clip.mp4 \
  -vf "vidstabtransform=input=clip-vidstab.trf:relative=1:smoothing=30:debug=1" \
  -f null -
```

### Step 6: Compare

```bash
python tools/compare_vidstab.py \
  --codec clip-motion.csv \
  --vidstab global_motions.trf
```

---

## 40. V0 Milestones

### V0.1 — FFmpeg MV Extractor

Deliver:

```text
mvstab inspect
mvstab dump
```

Acceptance criteria:

- H.264 MP4 opens successfully.
- decoded frames are in display order.
- PTS is recorded.
- MV side data is exported.
- raw vector sign is validated using synthetic translation.

---

### V0.2 — Weighted-Median Translation

Deliver:

```text
mvstab analyze --model translation
```

with CSV output.

Acceptance criteria:

- known synthetic translations recovered within a small error,
- moving foreground object does not completely dominate estimate,
- confidence drops on bad frames.

---

### V0.3 — vid.stab-Compatible Output

Deliver:

```text
motion.trf
```

accepted directly by:

```text
vidstabtransform
```

Acceptance criteria:

- known synthetic shaky video stabilizes in the expected direction,
- transform sign conventions are unit-tested,
- `relative=1` behavior is verified.

---

### V0.4 — Comparison Harness

Deliver:

```text
compare_vidstab.py
plot_motion.py
```

Acceptance criteria:

- plots codec-MV and vid.stab estimates on same timeline,
- prints correlation and error metrics,
- plots confidence.

---

### V0.5 — Robust Translation

Add:

- robust consensus fit,
- spatial coverage,
- MAD filtering,
- scene cuts,
- confidence.

Acceptance criteria:

- large independently moving foreground object does not routinely hijack global motion,
- scene boundaries reset correctly,
- low-texture failures are marked low confidence.

---

## 41. V1 Milestones

### V1.1 — FFmpeg Reference Metadata Patch

Expose:

```text
reference list
reference index
reference POC / stable picture identity
```

Acceptance criteria:

- every exported MV can be associated with its true reference picture where codec syntax provides one.

### V1.2 — Temporal Normalization

Normalize vectors across reference distances.

Acceptance criteria:

- estimates remain consistent across GOP structures with different reference distances.

### V1.3 — Full B-Frame Use

Use past and future references.

Acceptance criteria:

- B-frame estimates agree with synthetic ground truth,
- quality improves over P-anchor interpolation.

---

## 42. V2 Milestones

### V2.1 — Rigid Model

Add:

```text
translation + rotation
```

No warp.

### V2.2 — Pixel Fallback

Only on low-confidence frames.

### V2.3 — HEVC

Validate FFmpeg MV export and metadata path.

### V2.4 — Custom Trajectory Smoother

Remove dependency on vid.stab for motion smoothing if needed.

---

## 43. Long-Term Browser Architecture

Once `MV -> global motion` is proven, the browser design becomes:

```text
Compressed video
      |
      +--------------------------+
      |                          |
      v                          v
bitstream/MV path          WebCodecs decoder
      |                          |
      v                          v
global camera motion         VideoFrame
      |                          |
      +------------+-------------+
                   |
                   v
           trajectory smoother
                   |
                   v
             WebGPU transform
                   |
                   v
           WebCodecs encoder
```

The key benefit is that the browser does not need to run a complete `ffmpeg.wasm` software codec pipeline.

The reusable portable component from this command-line project is:

```text
codec motion metadata
        |
        v
vector filtering
        |
        v
global model fitting
        |
        v
confidence
```

That portion should remain plain C with minimal dependencies.

---

## 44. Risks

### Risk 1: Codec MVs Do Not Represent Camera Motion Well Enough

Mitigation:

- robust consensus,
- spatial coverage scoring,
- confidence,
- pixel fallback.

### Risk 2: Large Moving Foreground Dominates

Mitigation:

- spatial sampling,
- block-area caps,
- robust fit,
- neighborhood coherence.

### Risk 3: B-Frame/Multi-Reference Ambiguity

Mitigation:

- safe mode in V0,
- explicit FFmpeg reference-metadata patch in V1.

### Risk 4: Encoder Dependence

Mitigation:

- test multiple encoders/settings,
- confidence-aware behavior,
- hybrid pixel fallback.

### Risk 5: FFmpeg Decoder Does Not Export MVs for Some Codec

Mitigation:

- detect immediately,
- provide explicit unsupported message,
- later add codec-specific support.

### Risk 6: Exported MV Sign/Coordinate Conventions Are Misinterpreted

Mitigation:

- synthetic unit tests,
- transform-sign calibration,
- never infer by visual guess alone.

---

## 45. Key Engineering Principle

Do **not** initially build a new stabilization system.

Build a replacement for exactly one component:

```text
traditional:

pixels
  |
  v
vidstabdetect
  |
  v
motion
  |
  v
vidstabtransform


proposed:

compressed codec MVs
  |
  v
mvstab
  |
  v
motion
  |
  v
vidstabtransform
```

This keeps the experiment measurable, fast to implement, and easy to compare.

If codec-derived motion is bad, we learn that quickly.

If it is good, we have a very small core suitable for later native optimization and browser deployment.

---

## 46. Recommended First Implementation

The first code commit should be based closely on FFmpeg's official motion-vector extraction example.

Implement only:

```text
1. open file
2. find video stream
3. open software decoder with +export_mvs
4. receive decoded frame
5. read AV_FRAME_DATA_MOTION_VECTORS
6. convert each AVMotionVector to canonical reference->current image motion
7. dump CSV
```

Then generate three synthetic H.264 clips:

```text
+X translation
+Y translation
diagonal translation
```

Before adding RANSAC, `.trf`, smoothing, or B-frame logic, verify the vector convention numerically.

The second commit should implement weighted-median global translation.

The third commit should write vid.stab-compatible global transforms and produce the first stabilized video.

That sequence gives us useful evidence after only a small amount of code.

---

## 47. Decision Gates

### Gate A — After V0.2

Question:

> Do codec MVs correlate strongly with known/sensible global motion?

If no, stop or move directly to hybrid estimation.

### Gate B — After V0.3

Question:

> Does codec-MV stabilization produce visually credible output?

If no, inspect whether failure comes from spatial fitting or reference timing.

### Gate C — After V0.5

Question:

> Is quality competitive enough with `vidstabdetect` to justify better reference metadata?

If yes, build V1 FFmpeg patch.

### Gate D — After V1.3

Question:

> Is the codec-MV estimator good enough that pixel fallback is exceptional rather than routine?

If yes, proceed to browser architecture.

---

## 48. References

1. FFmpeg, `AVMotionVector` structure documentation:  
   https://ffmpeg.org/doxygen/trunk/structAVMotionVector.html

2. FFmpeg official motion-vector extraction example, `extract_mvs.c`:  
   https://www.ffmpeg.org/doxygen/trunk/extract_mvs_8c-example.html

3. FFmpeg codec documentation, motion-vector side-data export:  
   https://www.ffmpeg.org/ffmpeg-codecs.html

4. FFmpeg `AV_FRAME_DATA_MOTION_VECTORS` documentation/source:  
   https://svn.ffmpeg.org/doxygen/trunk/frame_8h_source.html

5. vid.stab transform-file format:  
   https://github.com/georgmartius/vid.stab/blob/master/docs/trf-format.md

6. vid.stab project and FFmpeg usage:  
   https://github.com/georgmartius/vid.stab

7. FFmpeg `vidstabtransform` implementation/options:  
   https://ffmpeg.org/doxygen/trunk/vf__vidstabtransform_8c_source.html

---

## 49. Short Version

The implementation strategy in one diagram:

```text
                         V0
                         ==

H.264 MP4/MOV
     |
     v
libavformat
     |
     v
libavcodec software decoder
  flags2=+export_mvs
     |
     v
AVMotionVector[]
     |
     v
normalize sign / coordinates
     |
     v
reject bad vectors
     |
     v
weighted median / robust translation fit
     |
     v
confidence
     |
     v
old-format vid.stab global .trf
     |
     v
FFmpeg vidstabtransform
     |
     v
stable video


                         V1
                         ==

same pipeline
     |
     +--> tiny FFmpeg patch:
          exact reference list/index/POC
                    |
                    v
          proper B-frame + multi-ref
          temporal normalization


                         V2+
                         ===

translation + rotation
hybrid pixel fallback
HEVC
custom smoother
WebAssembly estimator
WebCodecs + WebGPU browser frontend
```

The first objective is not maximum performance.

The first objective is to prove:

> **The motion information already present in compressed video is accurate enough to replace pixel-domain camera-motion estimation for no-warp stabilization.**
