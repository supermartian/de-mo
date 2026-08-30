#include "inspect.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define MVSTAB_MAX_HISTOGRAM_MAGNITUDE 65536.0

static void append_magnitude(MvstabInspectState *state, double magnitude) {
    double clamped = fmin(fmax(magnitude, 0.0), MVSTAB_MAX_HISTOGRAM_MAGNITUDE);
    double scale = (MVSTAB_MAGNITUDE_BIN_COUNT - 1) /
                   log1p(MVSTAB_MAX_HISTOGRAM_MAGNITUDE);
    size_t bin = (size_t)(log1p(clamped) * scale);
    ++state->magnitude_bins[bin];
    ++state->magnitude_count;
}

static void count_picture(MvstabInspectState *state, MvstabPictureType picture_type) {
    if (picture_type == MVSTAB_PICTURE_I) {
        ++state->picture_counts[0];
    } else if (picture_type == MVSTAB_PICTURE_P) {
        ++state->picture_counts[1];
    } else if (picture_type == MVSTAB_PICTURE_B) {
        ++state->picture_counts[2];
    }
}

static void count_block(MvstabInspectState *state, const MvstabVector *vector) {
    if (vector->width == 4 && vector->height == 4) {
        ++state->block_counts[0];
    } else if (vector->width == 8 && vector->height == 8) {
        ++state->block_counts[1];
    } else if (vector->width == 16 && vector->height == 16) {
        ++state->block_counts[2];
    } else {
        ++state->block_counts[3];
    }
}

static void print_target_frame(const MvstabFrame *frame) {
    if (frame->vector_count == 0) {
        printf("Frame %" PRId64 ": type=%s pts=%.6f vectors=0\n",
               frame->display_index, mvstab_picture_type_name(frame->picture_type),
               frame->pts_seconds);
        return;
    }
    printf("Frame %" PRId64 ": type=%s pts=%.6f vectors=%zu first_mv=(%.3f, %.3f)\n",
           frame->display_index, mvstab_picture_type_name(frame->picture_type),
           frame->pts_seconds, frame->vector_count,
           frame->vectors[0].dx, frame->vectors[0].dy);
}

int mvstab_inspect_frame(const MvstabFrame *frame, void *opaque) {
    MvstabInspectState *state = opaque;
    size_t index;

    ++state->frame_count;
    count_picture(state, frame->picture_type);
    state->frames_with_vectors += frame->vector_count > 0;
    state->vector_count += (int64_t)frame->vector_count;
    if (frame->display_index == state->target_frame) {
        print_target_frame(frame);
    }
    for (index = 0; index < frame->vector_count; ++index) {
        const MvstabVector *vector = &frame->vectors[index];
        state->past_vectors += vector->reference_direction < 0;
        state->future_vectors += vector->reference_direction > 0;
        state->exact_vectors += vector->reference_exact;
        state->timed_vectors += vector->reference_pts_valid;
        state->list1_vectors += vector->reference_list == 1;
        state->long_term_vectors += vector->reference_long_term;
        state->direct_vectors += vector->prediction_direct;
        state->skip_vectors += vector->prediction_skip;
        count_block(state, vector);
        append_magnitude(state, hypot(vector->dx, vector->dy));
    }
    return 0;
}

static double percentile(const MvstabInspectState *state, size_t percent) {
    uint64_t target;
    uint64_t cumulative = 0;
    size_t bin;
    if (state->magnitude_count == 0) {
        return 0.0;
    }
    target = ((state->magnitude_count - 1) * percent) / 100;
    for (bin = 0; bin < MVSTAB_MAGNITUDE_BIN_COUNT; ++bin) {
        cumulative += state->magnitude_bins[bin];
        if (cumulative > target) {
            double fraction = (double)bin / (MVSTAB_MAGNITUDE_BIN_COUNT - 1);
            return expm1(fraction * log1p(MVSTAB_MAX_HISTOGRAM_MAGNITUDE));
        }
    }
    return MVSTAB_MAX_HISTOGRAM_MAGNITUDE;
}

static double block_percentage(const MvstabInspectState *state, int category) {
    if (state->vector_count == 0) {
        return 0.0;
    }
    return 100.0 * state->block_counts[category] / state->vector_count;
}

void mvstab_print_inspection(
    const char *path,
    const MvstabVideoInfo *info,
    MvstabInspectState *state
) {
    printf("Input:       %s\nCodec:       %s %s\nDecoder:     %s\n",
           path, info->codec_name, info->profile_name, info->decoder_name);
    printf("Resolution:  %dx%d\nFrame rate:  %.3f fps\nDuration:    %.3f s\n\n",
           info->width, info->height, info->frame_rate, info->duration_seconds);
    printf("Decode path: %s\n\n", info->metadata_only_decode ?
           "H.264 syntax + motion metadata only" : "full software frame decode");
    printf("Frames:\n  I: %" PRId64 "\n  P: %" PRId64 "\n  B: %" PRId64 "\n\n",
           state->picture_counts[0], state->picture_counts[1], state->picture_counts[2]);
    printf("Motion vectors:\n  frames with MV side data: %" PRId64 " / %" PRId64 "\n",
           state->frames_with_vectors, state->frame_count);
    printf("  total vectors:            %" PRId64 "\n", state->vector_count);
    printf("  past-reference vectors:   %" PRId64 "\n", state->past_vectors);
    printf("  future-reference vectors: %" PRId64 "\n\n", state->future_vectors);
    printf("  exact-reference vectors:  %" PRId64 "\n", state->exact_vectors);
    printf("  timestamped references:   %" PRId64 "\n", state->timed_vectors);
    printf("  list-1 vectors:            %" PRId64 "\n", state->list1_vectors);
    printf("  long-term references:      %" PRId64 "\n", state->long_term_vectors);
    printf("  direct / skip vectors:     %" PRId64 " / %" PRId64 "\n\n",
           state->direct_vectors, state->skip_vectors);
    printf("Block sizes:\n  4x4:   %5.1f%%\n  8x8:   %5.1f%%\n",
           block_percentage(state, 0), block_percentage(state, 1));
    printf("  16x16: %5.1f%%\n  other: %5.1f%%\n\n",
           block_percentage(state, 2), block_percentage(state, 3));
    printf("MV magnitude:\n  p50: %.3f px\n  p90: %.3f px\n  p99: %.3f px\n",
           percentile(state, 50), percentile(state, 90), percentile(state, 99));
}

void mvstab_free_inspection(MvstabInspectState *state) {
    memset(state->magnitude_bins, 0, sizeof(state->magnitude_bins));
    state->magnitude_count = 0;
}
