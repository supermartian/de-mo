#include "mvstab/estimator.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static int compare_candidate_x(const void *left, const void *right) {
    const MvstabCandidate *a = left;
    const MvstabCandidate *b = right;
    return (a->dx > b->dx) - (a->dx < b->dx);
}

static int compare_candidate_y(const void *left, const void *right) {
    const MvstabCandidate *a = left;
    const MvstabCandidate *b = right;
    return (a->dy > b->dy) - (a->dy < b->dy);
}

double mvstab_weighted_median(
    MvstabCandidate *candidates,
    size_t count,
    int use_y
) {
    double total_weight = 0.0;
    double cumulative_weight = 0.0;
    size_t index;

    if (count == 0) {
        return 0.0;
    }
    qsort(candidates, count, sizeof(*candidates),
          use_y ? compare_candidate_y : compare_candidate_x);
    for (index = 0; index < count; ++index) {
        total_weight += candidates[index].weight;
    }
    for (index = 0; index < count; ++index) {
        cumulative_weight += candidates[index].weight;
        if (cumulative_weight >= total_weight / 2.0) {
            return use_y ? candidates[index].dy : candidates[index].dx;
        }
    }
    return 0.0;
}

static int vector_is_usable(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    const MvstabVector *vector
) {
    double magnitude = hypot(vector->dx, vector->dy);
    int coordinate_margin = vector->width > vector->height ? vector->width : vector->height;

    if (vector->motion_scale == 0 || vector->width <= 0 || vector->height <= 0) {
        return 0;
    }
    if (!isfinite(vector->x) || !isfinite(vector->y) ||
        !isfinite(magnitude) || magnitude > config->max_mv_px ||
        !isfinite(vector->weight) || vector->weight <= 0.0) {
        return 0;
    }
    if (vector->x < -coordinate_margin || vector->y < -coordinate_margin) {
        return 0;
    }
    if (vector->x > frame->width + coordinate_margin ||
        vector->y > frame->height + coordinate_margin) {
        return 0;
    }
    if (config->mode == MVSTAB_MODE_SAFE) {
        return frame->picture_type == MVSTAB_PICTURE_P && vector->reference_direction < 0;
    }
    return vector->reference_direction != 0;
}

static size_t collect_candidates(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    MvstabCandidate *candidates,
    int reference_direction
) {
    size_t accepted = 0;
    size_t index;

    for (index = 0; index < frame->vector_count; ++index) {
        const MvstabVector *vector = &frame->vectors[index];
        double direction = vector->reference_direction > 0 ? -1.0 : 1.0;
        if (!vector_is_usable(frame, config, vector) ||
            vector->reference_direction != reference_direction) {
            continue;
        }
        candidates[accepted].vector = vector;
        candidates[accepted].dx = direction * vector->dx;
        candidates[accepted].dy = direction * vector->dy;
        candidates[accepted].weight = fmin(vector->weight, config->block_area_cap);
        ++accepted;
    }
    return accepted;
}

static double total_candidate_weight(
    const MvstabCandidate *candidates,
    size_t count
) {
    double total_weight = 0.0;
    size_t index;
    for (index = 0; index < count; ++index) {
        total_weight += candidates[index].weight;
    }
    return total_weight;
}

static int compare_candidate_residual(const void *left, const void *right) {
    const MvstabCandidate *a = left;
    const MvstabCandidate *b = right;
    return (a->residual > b->residual) - (a->residual < b->residual);
}

static size_t apply_mad_filter(
    MvstabCandidate *candidates,
    size_t count,
    const MvstabEstimatorConfig *config
) {
    if (count == 0) {
        return 0;
    }
    double dx = mvstab_weighted_median(candidates, count, 0);
    double dy = mvstab_weighted_median(candidates, count, 1);
    double threshold;
    size_t source;
    size_t destination = 0;

    for (source = 0; source < count; ++source) {
        candidates[source].residual = hypot(candidates[source].dx - dx,
                                             candidates[source].dy - dy);
    }
    qsort(candidates, count, sizeof(*candidates), compare_candidate_residual);
    threshold = fmax(config->residual_threshold_px,
                     config->mad_threshold * candidates[count / 2].residual);
    for (source = 0; source < count; ++source) {
        if (candidates[source].residual <= threshold) {
            candidates[destination++] = candidates[source];
        }
    }
    return destination;
}

static void select_inliers(
    MvstabCandidate *candidates,
    size_t count,
    const MvstabEstimatorConfig *config,
    FrameMotion *motion
) {
    size_t index;

    motion->dx = mvstab_weighted_median(candidates, count, 0);
    motion->dy = mvstab_weighted_median(candidates, count, 1);
    for (index = 0; index < count; ++index) {
        candidates[index].residual = hypot(candidates[index].dx - motion->dx,
                                           candidates[index].dy - motion->dy);
        candidates[index].inlier =
            candidates[index].residual <= config->residual_threshold_px;
        motion->inlier_count += candidates[index].inlier;
    }
}

MvstabEstimatorConfig mvstab_default_estimator_config(void) {
    MvstabEstimatorConfig config = {
        .residual_threshold_px = 1.5,
        .max_mv_px = 128.0,
        .min_confidence = 0.05,
        .min_spatial_coverage = 0.125,
        .block_area_cap = 256.0,
        .mad_threshold = 4.0,
        .grid_columns = 8,
        .grid_rows = 4,
        .mode = MVSTAB_MODE_SAFE,
    };
    return config;
}

static int estimate_direction(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    MvstabCandidate *candidates,
    int reference_direction,
    FrameMotion *motion
) {
    size_t accepted_count = collect_candidates(
        frame, config, candidates, reference_direction);
    double accepted_weight = total_candidate_weight(candidates, accepted_count);
    size_t filtered_count;

    memset(motion, 0, sizeof(*motion));
    motion->vector_count = (int)accepted_count;
    filtered_count = apply_mad_filter(candidates, accepted_count, config);
    if (filtered_count == 0) {
        return 0;
    }
    select_inliers(candidates, filtered_count, config, motion);
    if (motion->inlier_count == 0) {
        return 0;
    }
    mvstab_compute_confidence(frame, config, candidates, filtered_count,
                              accepted_weight, motion);
    return 1;
}

static FrameMotion combine_reference_directions(
    const FrameMotion *past,
    int has_past,
    const FrameMotion *future,
    int has_future,
    const MvstabEstimatorConfig *config
) {
    FrameMotion combined = {0};
    double agreement;
    double total_confidence;

    if (!has_past && !has_future) {
        return combined;
    }
    if (!has_past || !has_future) {
        combined = has_past ? *past : *future;
        combined.confidence *= 0.5;
        combined.reference_agreement = 0.5;
        combined.valid = combined.confidence >= config->min_confidence &&
                         combined.spatial_coverage >= config->min_spatial_coverage;
        return combined;
    }
    agreement = hypot(past->dx - future->dx, past->dy - future->dy);
    total_confidence = past->confidence + future->confidence;
    if (total_confidence <= 0.0) {
        return combined;
    }
    combined.dx = (past->dx * past->confidence + future->dx * future->confidence) /
                  total_confidence;
    combined.dy = (past->dy * past->confidence + future->dy * future->confidence) /
                  total_confidence;
    combined.reference_agreement = exp(-agreement / config->residual_threshold_px);
    combined.confidence = sqrt(past->confidence * future->confidence) *
                          combined.reference_agreement;
    combined.inlier_weight_ratio = fmin(past->inlier_weight_ratio,
                                        future->inlier_weight_ratio);
    combined.residual_median = fmax(past->residual_median, future->residual_median);
    combined.residual_p95 = fmax(past->residual_p95, future->residual_p95);
    combined.spatial_coverage = fmin(past->spatial_coverage, future->spatial_coverage);
    combined.vector_count = past->vector_count + future->vector_count;
    combined.inlier_count = past->inlier_count + future->inlier_count;
    combined.valid = agreement <= config->residual_threshold_px &&
                     combined.confidence >= config->min_confidence &&
                     combined.spatial_coverage >= config->min_spatial_coverage;
    return combined;
}

int mvstab_estimate_frame(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    FrameMotion *motion
) {
    MvstabCandidate *candidates;
    FrameMotion past;
    FrameMotion future;
    int has_past;
    int has_future;

    if (frame == NULL || config == NULL || motion == NULL ||
        !isfinite(config->residual_threshold_px) ||
        !isfinite(config->max_mv_px) || !isfinite(config->block_area_cap) ||
        !isfinite(config->mad_threshold) || !isfinite(config->min_confidence) ||
        !isfinite(config->min_spatial_coverage) ||
        config->residual_threshold_px <= 0.0 || config->max_mv_px <= 0.0 ||
        config->block_area_cap <= 0.0 || config->mad_threshold <= 0.0 ||
        config->min_confidence < 0.0 || config->min_confidence > 1.0 ||
        config->min_spatial_coverage < 0.0 ||
        config->min_spatial_coverage > 1.0 ||
        config->grid_columns <= 0 || config->grid_rows <= 0 ||
        config->grid_columns > 64 || config->grid_rows > 64 ||
        (config->mode != MVSTAB_MODE_SAFE && config->mode != MVSTAB_MODE_ALL_MVS) ||
        frame->width <= 0 || frame->height <= 0 ||
        (frame->vector_count > 0 && frame->vectors == NULL)) {
        return -1;
    }
    memset(motion, 0, sizeof(*motion));
    if (frame->key_frame || frame->vector_count == 0) {
        return 0;
    }
    candidates = calloc(frame->vector_count, sizeof(*candidates));
    if (candidates == NULL) {
        return -1;
    }
    has_past = estimate_direction(frame, config, candidates, -1, &past);
    if (config->mode == MVSTAB_MODE_SAFE) {
        *motion = past;
        motion->valid = has_past &&
                        motion->confidence >= config->min_confidence &&
                        motion->spatial_coverage >= config->min_spatial_coverage;
    } else {
        has_future = estimate_direction(frame, config, candidates, 1, &future);
        *motion = combine_reference_directions(&past, has_past, &future,
                                                has_future, config);
    }
    free(candidates);
    return 0;
}
