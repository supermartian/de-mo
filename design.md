# mvstab design: H.264 syntax-derived stabilization metadata

## Objective

`mvstab` estimates camera motion from information already decoded from a
compressed H.264 stream. It does not inspect reconstructed pixels during the
detection pass. FFmpeg still demuxes the container and performs the entropy and
prediction-syntax decode; a small FFmpeg patch stops before inverse transform,
motion compensation, pixel reconstruction, and deblocking.

The output remains a relative transform stream for `vidstabtransform`, which
performs trajectory smoothing, cropping, interpolation, rendering, encoding,
and muxing. Pixel processing is therefore deferred to the one rendering pass
where it is unavoidable.

```text
MP4/MOV H.264 packets
        |
        v
FFmpeg H.264 syntax decode
  - NAL/RBSP and slice headers
  - CAVLC/CABAC syntax
  - reference-list construction and modification
  - ref_idx, MVD, skip/direct MV derivation
        |
        v
exact prediction partitions + final MVs + exact references
        |
        v
group vectors by exact reference picture
        |
        v
robust spatially balanced affine fit per reference edge
        |
        v
31-frame block-grid persistence analysis
  - compact foreground rejection
  - keyframes as missing observations
        |
        v
confidence-weighted pose graph
  - exact reference constraints
  - independently anchored reference components
        |
        v
relative old-format vid.stab transforms
        |
        v
FFmpeg vidstabtransform + final encode
```

The earlier `mvstab_ffmpeg_codec_motion_vector_design.md` is the V0 proposal.
This document supersedes its assumptions about stock `AVMotionVector`,
translation-only fitting, and mandatory pixel reconstruction.

## H.264 from first principles

The normative source is Recommendation ITU-T H.264 (06/2026), especially
clauses 7.3.4, 7.4.5, 8.2.4, 8.4.1, and 8.4.2. H.264 does not contain optical
flow and it does not store a ready-to-use camera trajectory.

### Coded hierarchy

An H.264 byte stream is divided into NAL units. Slice NAL units contain an RBSP
whose slice header establishes picture order, field mode, active reference
counts, reference-list modifications, weighting, and entropy-coding state.
The slice data then codes macroblocks.

For inter prediction, the relevant macroblock syntax is:

- `mb_type`: intra/inter, partition shape, prediction-list use, skip/direct;
- `sub_mb_type`: the subdivision and list use of each 8×8 region;
- `ref_idx_l0` and `ref_idx_l1`: indices into the active reference lists;
- `mvd_l0` and `mvd_l1`: signed differences from a derived motion-vector
  predictor, not final motion vectors;
- skip/direct syntax: modes in which some or all reference indices and vectors
  are derived without an explicit MVD.

Macroblock prediction partitions can be 16×16, 16×8, 8×16, or 8×8. An 8×8
sub-macroblock can be divided further into 8×4, 4×8, or 4×4 partitions. Luma
motion vectors use quarter-sample units. Chroma motion follows the chroma
format's subsampling and interpolation rules.

### Final motion derivation

For an explicitly coded inter partition, the conceptual operation is:

```text
final_mv(list, partition) = predicted_mv(neighbours, reference) + mvd
```

The predictor depends on neighboring partitions, their reference identities,
partition geometry, slice boundaries, field/MBAFF state, and special median or
directional rules. P-skip, B-skip, and B-direct add spatial and temporal
derivation paths. Temporal direct prediction consults a co-located block in a
reference picture and scales its motion using picture-order relationships.

Consequently, reading only `mvd_l0`/`mvd_l1` from the bitstream is incorrect.
A useful extractor must entropy-decode every macroblock and run the normative
MV/reference derivation. It does not need to inverse-transform residuals,
motion-compensate pixels, or deblock the picture.

CABAC residual symbols still have to be consumed because later CABAC contexts
depend on earlier syntax. “Metadata only” means skipping pixel reconstruction,
not seeking past arbitrary residual bits.

### Reference identity

`ref_idx_l0 = 0` does not mean “the previous frame.” It means entry zero in the
active list for that slice. The decoder first constructs default lists from
short- and long-term reference pictures, then applies any slice-level list
modification. The same picture can occupy different indices, and list 0/list 1
do not reliably mean past/future after reordering.

The extractor therefore resolves a reference while the slice-local list is
available and records:

- list and list index;
- exact reference POC minus current POC;
- reference PTS minus current PTS;
- short-term versus long-term status;
- top/bottom-field status;
- direct, skip, and interlaced provenance.

POC describes codec presentation order but is not a duration. PTS in the
frame timestamp time base is used for temporal normalization when available.

## What stock FFmpeg exposes

Software decoders can attach `AV_FRAME_DATA_MOTION_VECTORS`. Its public
`AVMotionVector` supplies block size, source/destination positions, a vector,
and its scale. In stock FFmpeg:

- `source` specifies only an earlier/later direction;
- `flags` carries no H.264 reference identity;
- exact list/index, POC, PTS, long-term status, and skip/direct provenance are
  absent;
- the generic H.264 export path cannot preserve the slice-local reference list;
- the public record is insufficient to normalize vectors that span different
  reference durations.

FFmpeg's H.264 decoder already derives the required information internally:

- `H264SliceContext.mv_cache` and `ref_cache` contain the derived current MB;
- `H264SliceContext.ref_list` contains the exact slice-local references;
- `H264Picture.motion_val` retains final MVs on the 4×4 luma grid;
- `H264Picture.ref_index` retains list indices at 8×8 granularity;
- `mb_type`, `sub_mb_type`, POC, long-term state, and frame timestamps are
  available during decode.

The information loss occurs at the public export boundary, not in H.264
decoding itself.

## FFmpeg patch

The repository contains one patch based on FFmpeg commit
`b32f8d1c2377079302d23f82d555d13deda68c57`:

`patches/ffmpeg/0001-avcodec-h264-export-exact-motion-metadata-without-pix.patch`

### Exact exporter

At H.264 motion write-back time, the patch resolves each 8×8 reference through
the current slice's list. It stores exact POC/PTS deltas and reference flags in
the decoded picture. It also persists the true prediction-partition layout,
including 8×4, 4×8, and 4×4 subpartitions.

At frame output, the H.264-specific exporter emits one initialized
`AVMotionVector` per prediction partition and reference list. `source` is the
exact POC delta when `AV_MV_FLAG_REFERENCE_EXACT` is set. Low `flags` bits hold
the list/index and coding flags; the upper 48 bits hold a signed PTS delta when
`AV_MV_FLAG_PTS_DELTA_VALID` is set. Zero initialization is required because
`AVMotionVector` contains ABI padding. For MBAFF field-coded 16×8 and 8×16
partitions, vertical motion is converted from field to frame coordinates in
the same way as FFmpeg's generic exporter.

The packed-flags representation deliberately preserves the existing public
struct size. It is a project patch, not a claim that this is the ideal upstream
FFmpeg API. A future upstream design should use a versioned side-data record
rather than spend most remaining flag bits on a timestamp.

### Metadata-only decoder option

The private H.264 decoder option is:

```text
motion_metadata_only=1
```

It still parses H.264 syntax and derives references/MVs, but skips the four
high-level macroblock reconstruction calls and the loop filter. Error
concealment is disabled because it can synthesize motion not present in the
bitstream. Missing-field pixel duplication is also skipped.

Decoded `AVFrame` objects and reference-picture bookkeeping still exist, and
pixel buffers are currently allocated for decoder/thread compatibility. Their
contents are undefined and must never be consumed in metadata-only mode. The
mode supports the software H.264 decoder only; hardware decoders are outside
scope.

## Project metadata contract

`MvstabVector` is independent of FFmpeg types. In addition to the canonical
vector and partition geometry it carries:

- `reference_exact`, `reference_list`, and `reference_index`;
- `reference_poc_delta` and optional `reference_pts_delta`;
- `reference_delta_seconds` in the stream time base;
- long-term and field flags;
- direct, skip, and interlaced flags;
- the unmodified codec flag word for diagnostics.

Stock FFmpeg remains supported. The reader tries `motion_metadata_only`; if the
option is missing it performs a normal software decode and consumes legacy MV
side data. `inspect` states which path was selected.

## Coordinate and temporal conventions

FFmpeg defines:

```text
src = dst + motion / motion_scale
```

The project stores image feature motion from the reference picture to the
current picture:

```text
raw_dx = -motion_x / motion_scale
raw_dy = -motion_y / motion_scale
```

For exact metadata, let `Δt_ref = reference_pts - current_pts` and let `Δt_out`
be the current display-frame duration. The vector is normalized to one output
interval with:

```text
factor = Δt_out / -Δt_ref
dx = raw_dx * factor
dy = raw_dy * factor
```

This single equation handles past and future references and references several
frames away. If exact timing is absent, safe mode retains the V0 behavior of
using past-reference P pictures as anchors; all-MV mode uses direction-only
sign normalization and remains approximate.

## Camera-motion estimator

Codec MVs are encoder prediction decisions. They can follow moving objects,
use zero skip in textureless regions, quantize to quarter-pixel units, or select
a reference for rate-distortion reasons. They are evidence, not ground truth.

### Evidence beyond final motion vectors

The compressed stream contains reliability evidence in addition to final MVs:
macroblock and sub-macroblock type, intra/inter/skip/direct coverage,
partition geometry, list and reference identity, explicit MVD presence,
coded-block patterns, residual coefficients, QP, slice boundaries, and packet
size. These signals can identify when a vector field is inherited, weakly
observed, or likely owned by a local object. The checked-in patch currently
exports the reference, partition, skip, and direct subset needed by the
estimator.

Reliability evidence cannot manufacture a missing displacement. A mostly
intra-coded frame or a zero-skip field can say “the MV estimate is
unobservable,” but it contains no camera-motion direction to recover. The
pure-MV estimator reports or interpolates such gaps conservatively rather than
inventing observations.

The estimator fits a 2D affine field around the image center:

```text
dx(x,y) = tx + a*x + b*y
dy(x,y) = ty + c*x + d*y
scale   = (a + d) / 2
theta   = (c - b) / 2
```

`tx`, `ty`, and `theta` describe the projected global similarity motion.
Independent affine terms absorb expansion, unequal axis stretch, and shear so
those effects contaminate translation and rotation less. Scale is reported but
is not currently sent to vid.stab.

Each reference-edge fit uses:

1. malformed, unsupported, and excessive-vector rejection;
2. exact grouping by reference timestamp where possible;
3. reduced weight for skip/direct predictions and rejection of exact zero-skip
   evidence;
4. capped partition-area weights;
5. grid balancing so a textured road or moving foreground cannot own the fit;
6. four Tukey-style iteratively reweighted least-squares rounds;
7. residual inliers and spatial coverage;
8. confidence gating before the edge enters the trajectory solve.

At least half-image support in both axes and support on opposing image halves
is required. Exact-timed B and P pictures can both contribute constraints.

### Block-grid sequence model

After the robust edge fit, inlier vectors are summarized in an 8×4 image grid.
Each occupied cell retains its weighted center, displacement, support, and
exact reference edge. The raw per-block array can then be released; the fixed
summary bounds memory independently of partition count.

For each display frame, the nearest exact-reference edge supplies a local
motion-rate residual after subtracting the fitted camera field. Exact reference
timestamps normalize that residual to one nominal output interval. The
analyzer searches the same and neighboring grid cells over 15 frames on either
side.
A residual is classified as persistent foreground only when it is at least
1.5 pixels per frame, agrees with at least eight other observations, leaves at
least six background cells, and occupies no more than one quarter of the
supported grid. All exact edges for that frame are refit without the marked
cells.

This is a foreground classifier, not detector-side trajectory smoothing. A
genuine camera impulse affects the field broadly and is retained. An isolated
I-picture contributes no block observation, so histories can span periodic GOP
boundaries. At a keyframe, incompatible camera rates on its two sides start a
new history segment; discontinuous PTS does the same. Pose-graph components
remain separately anchored, and the existing motion-continuity gate refuses
keyframe interpolation at a detected break.

### Exact-reference pose graph

For an exact edge from reference picture `r` to current picture `i`, the raw
similarity estimate is kept at its coded temporal span instead of first being
divided into a one-frame estimate:

```text
pose[i] - pose[r] ~= edge[r -> i]
```

The four pose coordinates are translation X/Y, rotation, and fractional scale
under a small-motion approximation. A sparse least-squares solve weights each
edge by fit confidence, spatial coverage, and inverse squared temporal span.
When adjacent exact edges support at least one quarter of the frames, those
local constraints enter the graph. A run longer than the temporal window can
admit longer edges when its combined frame estimate is also valid; globally
sparse inputs keep all valid graph edges. Longer references have greater codec
model uncertainty. The detector does not apply a second temporal smoothing
objective: `vidstabtransform` owns that policy during rendering. A diagonal
preconditioner keeps the graph solve stable. Reference-connected components
are solved and anchored separately, so the optimizer cannot pull one GOP or
scene through an unrelated component. Adjacent pose differences become the
relative output rows.

If exact edge metadata is absent, the previous timestamp-normalized timeline is
used unchanged. Its gaps of at most three frames are interpolated only when
reliable models on both sides have continuous velocity. A continuity-gated
repair fills isolated periodic keyframes after a graph solve.

## vid.stab output convention

The old transform row is:

```text
0 x y alpha zoom 0
```

Rows are relative and require `vidstabtransform:relative=1`. Empirical
end-to-end calibration establishes the conversion:

```text
x = -image_dx
y = -image_dy
alpha = image_theta
zoom = 0
```

The apparent angle-sign asymmetry comes from vid.stab's image-coordinate
rotation convention. A synthetic rotating-video test verifies that this sign
reduces residual rotation.

## Validation

The normal suite covers vector normalization, exact past/future duration
normalization, affine-to-similarity recovery, outliers, reference disagreement,
spatial coverage, long-horizon foreground rejection across a keyframe, precise
and legacy timelines, writers, comparison tools, and an encoded H.264
end-to-end stabilization.

Patch validation additionally covers:

- complete semantic and byte-for-byte MV equality between full and
  metadata-only decode for all 11,369 frames of the driving clip;
- Main-profile CABAC with B pictures and multiple references;
- Baseline-profile CAVLC without B pictures;
- interlaced H.264;
- a forced-MBAFF fixture whose field-coded 16×8/8×16 exports match stock
  FFmpeg's vertical-coordinate conversion;
- exact reference and timestamp fields in the CLI end-to-end test;
- a synthetic rotating stream used to catch motion-grid stride and angle-sign
  errors.

On the 720×480, 379.35-second driving clip, the patched decoder processed all
11,369 frames in 3.28 seconds versus 12.12 seconds for full H.264 decode in the
same single-process probe (3.7× faster). Full temporal `mvstab analyze`,
including compact cell histories and CSV output, took 7.77 seconds and about
67 MB RSS. The earlier
pixel-domain `vidstabdetect` run took 12.71 seconds wall time, 246.57 CPU
seconds, and about 129 MB RSS on the same host.

The estimators are not expected to agree frame-for-frame: vid.stab measures
selected pixel patches while metadata-only mvstab observes encoder prediction.
Under one fair residual protocol (`shakiness=5`, `accuracy=15`, `stepsize=6`,
and `mincontrast=0.25`), the previous metadata-only baseline reaches 4.513
pixels median residual translation and 10.187 RMS. Long-horizon mvstab reaches
4.211 pixels median, 9.881 RMS, and 19.739 p95. Pixel-domain vid.stab reaches
2.283 pixels median, 5.651 RMS, and 8.014 p95. Long-horizon rotation improves
from the previous pure-MV baseline's 0.751 degrees RMS to 0.717 degrees;
vid.stab reaches 0.573 degrees RMS.

## Known boundaries and next work

- Metadata-only mode still allocates pixel buffers; eliminating them requires a
  deeper decoder/thread-frame abstraction.
- Damaged streams receive no error-concealed synthetic motion in metadata-only
  mode. Callers must inspect decode-error flags and decide whether to fall back.
- Encoder motion can be unreliable in flat areas, during flashes/cuts, and for
  independently moving foreground objects.
- The emitted global similarity transform does not correct perspective,
  rolling shutter, mesh motion, or parallax.
- `MvstabMotionEdge` gained fixed cell summaries in the 0.x API; applications
  embedding the public struct must rebuild and no 0.x binary ABI is promised.
- Scene-cut segmentation and resetting external vid.stab smoothing remain
  necessary for production-quality long-form rendering.
- The exact public flag layout is H.264-specific and patch-specific; an
  upstream API should be codec-neutral and versioned.
- HEVC/VP9/AV1 can use the generic fallback today only when their decoder
  exports MVs; they do not receive exact timing or metadata-only decoding from
  this patch.
