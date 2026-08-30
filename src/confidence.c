#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int compare_candidate_residual(const void *left, const void *right) {
    const MvstabCandidate *a = left;
    const MvstabCandidate *b = right;
    return (a->residual > b->residual) - (a->residual < b->residual);
}

static void compute_residual_percentiles(
    MvstabCandidate *candidates,
    size_t count,
    FrameMotion *motion
) {
    size_t residual_count = 0;
    size_t index;

    if (motion->inlier_count == 0) {
        return;
    }
    qsort(candidates, count, sizeof(*candidates), compare_candidate_residual);
    for (index = 0; index < count; ++index) {
        if (candidates[index].inlier) {
            ++residual_count;
        }
    }
    motion->residual_median = candidates[residual_count / 2].residual;
    motion->residual_p95 = candidates[((residual_count - 1) * 95) / 100].residual;
}

static double compute_spatial_coverage(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    const MvstabCandidate *candidates,
    size_t count
) {
    size_t cell_count = (size_t)config->grid_columns * config->grid_rows;
    unsigned char occupied[4096];
    size_t occupied_count = 0;
    size_t index;
    int min_column = config->grid_columns;
    int max_column = -1;
    int min_row = config->grid_rows;
    int max_row = -1;
    int has_left = 0;
    int has_right = 0;
    int has_top = 0;
    int has_bottom = 0;

    memset(occupied, 0, cell_count);
    for (index = 0; index < count; ++index) {
        int column;
        int row;
        size_t cell;
        if (!candidates[index].inlier) {
            continue;
        }
        column = (int)(candidates[index].vector->x * config->grid_columns / frame->width);
        row = (int)(candidates[index].vector->y * config->grid_rows / frame->height);
        column = column < 0 ? 0 : column;
        row = row < 0 ? 0 : row;
        column = column >= config->grid_columns ? config->grid_columns - 1 : column;
        row = row >= config->grid_rows ? config->grid_rows - 1 : row;
        cell = (size_t)row * config->grid_columns + column;
        occupied_count += occupied[cell] == 0;
        occupied[cell] = 1;
        min_column = column < min_column ? column : min_column;
        max_column = column > max_column ? column : max_column;
        min_row = row < min_row ? row : min_row;
        max_row = row > max_row ? row : max_row;
        has_left |= column < config->grid_columns / 2;
        has_right |= column >= config->grid_columns / 2;
        has_top |= row < config->grid_rows / 2;
        has_bottom |= row >= config->grid_rows / 2;
    }
    if (occupied_count == 0 ||
        (max_column - min_column + 1) * 2 < config->grid_columns ||
        (max_row - min_row + 1) * 2 < config->grid_rows ||
        !has_left || !has_right || !has_top || !has_bottom) {
        return 0.0;
    }
    return (double)occupied_count / cell_count;
}

static double compute_inlier_weight_ratio(
    const MvstabCandidate *candidates,
    size_t count,
    double total_candidate_weight
) {
    double inlier_weight = 0.0;
    size_t index;

    for (index = 0; index < count; ++index) {
        if (candidates[index].inlier) {
            inlier_weight += candidates[index].weight;
        }
    }
    return total_candidate_weight > 0.0 ?
           inlier_weight / total_candidate_weight : 0.0;
}

void mvstab_compute_confidence(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    MvstabCandidate *candidates,
    size_t count,
    double total_candidate_weight,
    FrameMotion *motion
) {
    double count_score;
    double residual_score;

    motion->inlier_weight_ratio = compute_inlier_weight_ratio(
        candidates, count, total_candidate_weight);
    motion->spatial_coverage = compute_spatial_coverage(frame, config, candidates, count);
    compute_residual_percentiles(candidates, count, motion);
    count_score = fmin(1.0, motion->inlier_count / 16.0);
    residual_score = exp(-motion->residual_median / config->residual_threshold_px);
    motion->confidence = motion->inlier_weight_ratio * residual_score *
                         sqrt(motion->spatial_coverage) * count_score;
    motion->reference_agreement = 1.0;
}
