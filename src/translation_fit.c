#include "mvstab/estimator.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define AFFINE_DIMENSIONS 6

typedef struct {
    double value[AFFINE_DIMENSIONS];
} AffineModel;

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

static int compare_candidate_residual(const void *left, const void *right) {
    const MvstabCandidate *a = left;
    const MvstabCandidate *b = right;
    return (a->residual > b->residual) - (a->residual < b->residual);
}

static int compare_candidate_reference(const void *left, const void *right) {
    const MvstabCandidate *a = left;
    const MvstabCandidate *b = right;
    const MvstabVector *av = a->vector;
    const MvstabVector *bv = b->vector;
    if (av->reference_pts_delta != bv->reference_pts_delta) {
        return (av->reference_pts_delta > bv->reference_pts_delta) -
               (av->reference_pts_delta < bv->reference_pts_delta);
    }
    if (av->reference_poc_delta != bv->reference_poc_delta) {
        return (av->reference_poc_delta > bv->reference_poc_delta) -
               (av->reference_poc_delta < bv->reference_poc_delta);
    }
    if (av->reference_top_field != bv->reference_top_field) {
        return av->reference_top_field - bv->reference_top_field;
    }
    if (av->reference_bottom_field != bv->reference_bottom_field) {
        return av->reference_bottom_field - bv->reference_bottom_field;
    }
    return av->reference_long_term - bv->reference_long_term;
}

static int candidates_share_reference(
    const MvstabCandidate *left,
    const MvstabCandidate *right
) {
    const MvstabVector *a = left->vector;
    const MvstabVector *b = right->vector;
    return a->reference_pts_delta == b->reference_pts_delta &&
           a->reference_poc_delta == b->reference_poc_delta &&
           a->reference_top_field == b->reference_top_field &&
           a->reference_bottom_field == b->reference_bottom_field &&
           a->reference_long_term == b->reference_long_term;
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

static int exact_temporal_factor(
    const MvstabFrame *frame,
    const MvstabVector *vector,
    double *factor
) {
    if (!vector->reference_exact || !vector->reference_pts_valid ||
        !isfinite(vector->reference_delta_seconds) ||
        vector->reference_delta_seconds == 0.0 ||
        !isfinite(frame->duration_seconds) || frame->duration_seconds <= 0.0) {
        return 0;
    }
    *factor = frame->duration_seconds / -vector->reference_delta_seconds;
    return isfinite(*factor) && *factor != 0.0;
}

static int vector_is_usable(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    const MvstabVector *vector,
    double *factor,
    int *exact_timing
) {
    int margin = vector->width > vector->height ? vector->width : vector->height;
    *exact_timing = exact_temporal_factor(frame, vector, factor);
    if (!*exact_timing) {
        *factor = vector->reference_direction > 0 ? -1.0 : 1.0;
    }
    if (vector->motion_scale == 0 || vector->width <= 0 || vector->height <= 0 ||
        vector->reference_direction == 0 || !isfinite(vector->x) ||
        !isfinite(vector->y) || !isfinite(vector->dx) || !isfinite(vector->dy) ||
        !isfinite(vector->weight) || vector->weight <= 0.0 ||
        hypot(vector->dx * *factor, vector->dy * *factor) > config->max_mv_px) {
        return 0;
    }
    if (vector->reference_exact && vector->prediction_skip &&
        vector->motion_x == 0 && vector->motion_y == 0) {
        return 0;
    }
    if (vector->x < -margin || vector->y < -margin ||
        vector->x > frame->width + margin || vector->y > frame->height + margin) {
        return 0;
    }
    if (config->mode == MVSTAB_MODE_SAFE && !*exact_timing) {
        return frame->picture_type == MVSTAB_PICTURE_P &&
               vector->reference_direction < 0;
    }
    return 1;
}

static size_t collect_candidates(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    MvstabCandidate *candidates,
    size_t *exact_count
) {
    size_t accepted = 0;
    *exact_count = 0;
    for (size_t index = 0; index < frame->vector_count; ++index) {
        const MvstabVector *vector = &frame->vectors[index];
        double factor;
        int exact_timing;
        if (!vector_is_usable(frame, config, vector, &factor, &exact_timing)) {
            continue;
        }
        candidates[accepted].vector = vector;
        candidates[accepted].dx = vector->dx * factor;
        candidates[accepted].dy = vector->dy * factor;
        candidates[accepted].base_weight = sqrt(
            fmin(vector->weight, config->block_area_cap));
        if (vector->prediction_skip) {
            candidates[accepted].base_weight *= 0.5;
        }
        if (vector->prediction_direct) {
            candidates[accepted].base_weight *= 0.75;
        }
        candidates[accepted].weight = candidates[accepted].base_weight;
        *exact_count += exact_timing;
        ++accepted;
    }
    return accepted;
}

static int exact_vector_is_usable(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    const MvstabVector *vector
) {
    double span;
    double limit;
    int margin = vector->width > vector->height ? vector->width : vector->height;
    if (!vector->reference_exact || !vector->reference_pts_valid ||
        !isfinite(vector->reference_delta_seconds) ||
        vector->reference_delta_seconds == 0.0 ||
        !isfinite(frame->duration_seconds) || frame->duration_seconds <= 0.0) {
        return 0;
    }
    span = fabs(vector->reference_delta_seconds) / frame->duration_seconds;
    limit = config->max_mv_px * fmax(1.0, span);
    if (vector->motion_scale == 0 || vector->width <= 0 || vector->height <= 0 ||
        !isfinite(vector->x) || !isfinite(vector->y) ||
        !isfinite(vector->dx) || !isfinite(vector->dy) ||
        !isfinite(vector->weight) || vector->weight <= 0.0 ||
        hypot(vector->dx, vector->dy) > limit) {
        return 0;
    }
    if (vector->prediction_skip && vector->motion_x == 0 && vector->motion_y == 0) {
        return 0;
    }
    return vector->x >= -margin && vector->y >= -margin &&
           vector->x <= frame->width + margin && vector->y <= frame->height + margin;
}

static size_t collect_edge_candidates(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    MvstabCandidate *candidates
) {
    size_t accepted = 0;
    for (size_t index = 0; index < frame->vector_count; ++index) {
        const MvstabVector *vector = &frame->vectors[index];
        if (!exact_vector_is_usable(frame, config, vector)) {
            continue;
        }
        candidates[accepted].vector = vector;
        candidates[accepted].dx = vector->dx;
        candidates[accepted].dy = vector->dy;
        candidates[accepted].base_weight = sqrt(
            fmin(vector->weight, config->block_area_cap));
        if (vector->prediction_skip) {
            candidates[accepted].base_weight *= 0.5;
        }
        if (vector->prediction_direct) {
            candidates[accepted].base_weight *= 0.75;
        }
        candidates[accepted].weight = candidates[accepted].base_weight;
        ++accepted;
    }
    return accepted;
}

static size_t candidate_grid_cell(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    const MvstabCandidate *candidate
) {
    int column = (int)(candidate->vector->x * config->grid_columns / frame->width);
    int row = (int)(candidate->vector->y * config->grid_rows / frame->height);
    column = column < 0 ? 0 : column;
    row = row < 0 ? 0 : row;
    column = column >= config->grid_columns ? config->grid_columns - 1 : column;
    row = row >= config->grid_rows ? config->grid_rows - 1 : row;
    return (size_t)row * config->grid_columns + column;
}

static double balance_grid_weights(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    MvstabCandidate *candidates,
    size_t count
) {
    double totals[4096] = {0};
    double total = 0.0;
    for (size_t index = 0; index < count; ++index) {
        totals[candidate_grid_cell(frame, config, &candidates[index])] +=
            candidates[index].base_weight;
    }
    for (size_t index = 0; index < count; ++index) {
        double cell_total = totals[candidate_grid_cell(frame, config, &candidates[index])];
        candidates[index].base_weight /= cell_total;
        candidates[index].weight = candidates[index].base_weight;
        total += candidates[index].base_weight;
    }
    return total;
}

static void add_equation(
    double normal[AFFINE_DIMENSIONS][AFFINE_DIMENSIONS],
    double rhs[AFFINE_DIMENSIONS],
    const double row[AFFINE_DIMENSIONS],
    double target,
    double weight
) {
    for (int y = 0; y < AFFINE_DIMENSIONS; ++y) {
        rhs[y] += weight * row[y] * target;
        for (int x = 0; x < AFFINE_DIMENSIONS; ++x) {
            normal[y][x] += weight * row[y] * row[x];
        }
    }
}

static int solve_system(
    double matrix[AFFINE_DIMENSIONS][AFFINE_DIMENSIONS],
    double rhs[AFFINE_DIMENSIONS],
    double result[AFFINE_DIMENSIONS]
) {
    for (int column = 0; column < AFFINE_DIMENSIONS; ++column) {
        int pivot = column;
        for (int row = column + 1; row < AFFINE_DIMENSIONS; ++row) {
            if (fabs(matrix[row][column]) > fabs(matrix[pivot][column])) {
                pivot = row;
            }
        }
        if (fabs(matrix[pivot][column]) < 1e-12) {
            return -1;
        }
        if (pivot != column) {
            for (int x = column; x < AFFINE_DIMENSIONS; ++x) {
                double swap = matrix[column][x];
                matrix[column][x] = matrix[pivot][x];
                matrix[pivot][x] = swap;
            }
            { double swap = rhs[column]; rhs[column] = rhs[pivot]; rhs[pivot] = swap; }
        }
        for (int row = column + 1; row < AFFINE_DIMENSIONS; ++row) {
            double factor = matrix[row][column] / matrix[column][column];
            for (int x = column; x < AFFINE_DIMENSIONS; ++x) {
                matrix[row][x] -= factor * matrix[column][x];
            }
            rhs[row] -= factor * rhs[column];
        }
    }
    for (int row = AFFINE_DIMENSIONS - 1; row >= 0; --row) {
        double value = rhs[row];
        for (int x = row + 1; x < AFFINE_DIMENSIONS; ++x) {
            value -= matrix[row][x] * result[x];
        }
        result[row] = value / matrix[row][row];
    }
    return 0;
}

static int fit_affine(
    const MvstabFrame *frame,
    const MvstabCandidate *candidates,
    size_t count,
    AffineModel *model
) {
    double normal[AFFINE_DIMENSIONS][AFFINE_DIMENSIONS] = {{0}};
    double rhs[AFFINE_DIMENSIONS] = {0};
    double radius = fmax(frame->width, frame->height);
    for (size_t index = 0; index < count; ++index) {
        double u = (candidates[index].vector->x - frame->width / 2.0) / radius;
        double v = (candidates[index].vector->y - frame->height / 2.0) / radius;
        const double x_row[AFFINE_DIMENSIONS] = {1.0, 0.0, u, v, 0.0, 0.0};
        const double y_row[AFFINE_DIMENSIONS] = {0.0, 1.0, 0.0, 0.0, u, v};
        add_equation(normal, rhs, x_row, candidates[index].dx,
                     candidates[index].weight);
        add_equation(normal, rhs, y_row, candidates[index].dy,
                     candidates[index].weight);
    }
    return solve_system(normal, rhs, model->value);
}

static void compute_residuals(
    const MvstabFrame *frame,
    MvstabCandidate *candidates,
    size_t count,
    const AffineModel *model
) {
    double radius = fmax(frame->width, frame->height);
    for (size_t index = 0; index < count; ++index) {
        double u = (candidates[index].vector->x - frame->width / 2.0) / radius;
        double v = (candidates[index].vector->y - frame->height / 2.0) / radius;
        double dx = model->value[0] + model->value[2] * u + model->value[3] * v;
        double dy = model->value[1] + model->value[4] * u + model->value[5] * v;
        candidates[index].residual = hypot(candidates[index].dx - dx,
                                            candidates[index].dy - dy);
    }
}

static double robust_threshold(
    MvstabCandidate *candidates,
    size_t count,
    const MvstabEstimatorConfig *config
) {
    qsort(candidates, count, sizeof(*candidates), compare_candidate_residual);
    return fmax(config->residual_threshold_px,
                config->mad_threshold * candidates[count / 2].residual);
}

static int robust_affine_fit(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    MvstabCandidate *candidates,
    size_t count,
    AffineModel *model,
    double *threshold
) {
    for (int iteration = 0; iteration < 4; ++iteration) {
        if (fit_affine(frame, candidates, count, model) != 0) {
            return -1;
        }
        compute_residuals(frame, candidates, count, model);
        *threshold = robust_threshold(candidates, count, config);
        for (size_t index = 0; index < count; ++index) {
            double ratio = candidates[index].residual / *threshold;
            double robust = ratio < 1.0 ? (1.0 - ratio * ratio) : 0.0;
            candidates[index].weight = candidates[index].base_weight * robust * robust;
        }
    }
    if (fit_affine(frame, candidates, count, model) != 0) {
        return -1;
    }
    compute_residuals(frame, candidates, count, model);
    *threshold = robust_threshold(candidates, count, config);
    return 0;
}

static int fit_motion_cells(
    const MvstabMotionEdge *edge,
    const unsigned char *excluded,
    double model[AFFINE_DIMENSIONS]
) {
    double normal[AFFINE_DIMENSIONS][AFFINE_DIMENSIONS] = {{0}};
    double rhs[AFFINE_DIMENSIONS] = {0};
    size_t used = 0;
    for (size_t index = 0; index < edge->cell_count; ++index) {
        const MvstabMotionCell *cell = &edge->cells[index];
        const double x_row[AFFINE_DIMENSIONS] = {
            1.0, 0.0, cell->x, cell->y, 0.0, 0.0};
        const double y_row[AFFINE_DIMENSIONS] = {
            0.0, 1.0, 0.0, 0.0, cell->x, cell->y};
        if (cell->grid_index >= MVSTAB_MAX_MOTION_CELLS ||
            (excluded != NULL && excluded[cell->grid_index])) {
            continue;
        }
        add_equation(normal, rhs, x_row, cell->dx, cell->weight);
        add_equation(normal, rhs, y_row, cell->dy, cell->weight);
        ++used;
    }
    return used >= 6 ? solve_system(normal, rhs, model) : -1;
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static void update_refit_reliability(
    MvstabMotionEdge *edge,
    const unsigned char *excluded,
    const double model[AFFINE_DIMENSIONS]
) {
    double residuals[MVSTAB_MAX_MOTION_CELLS];
    double total_weight = 0.0;
    double kept_weight = 0.0;
    size_t used = 0;
    int vectors = 0;
    for (size_t index = 0; index < edge->cell_count; ++index) {
        const MvstabMotionCell *cell = &edge->cells[index];
        total_weight += cell->weight;
        if (cell->grid_index >= MVSTAB_MAX_MOTION_CELLS ||
            (excluded != NULL && excluded[cell->grid_index])) {
            continue;
        }
        double predicted_x = model[0] + model[2] * cell->x + model[3] * cell->y;
        double predicted_y = model[1] + model[4] * cell->x + model[5] * cell->y;
        residuals[used++] = hypot(cell->dx - predicted_x, cell->dy - predicted_y);
        kept_weight += cell->weight;
        vectors += cell->vector_count;
    }
    qsort(residuals, used, sizeof(*residuals), compare_double);
    double retained = total_weight > 0.0 ? kept_weight / total_weight : 0.0;
    edge->motion.confidence *= retained;
    edge->motion.inlier_weight_ratio *= retained;
    edge->motion.spatial_coverage *= (double)used / edge->cell_count;
    edge->motion.inlier_count = vectors;
    edge->motion.residual_median = residuals[used / 2];
    edge->motion.residual_p95 = residuals[(used - 1) * 95 / 100];
}

int mvstab_refit_motion_cells(
    MvstabMotionEdge *edge,
    const unsigned char *excluded
) {
    double model[AFFINE_DIMENSIONS];
    if (edge == NULL || edge->cell_count < 6 ||
        edge->cell_count > MVSTAB_MAX_MOTION_CELLS ||
        fit_motion_cells(edge, excluded, model) != 0) {
        return 0;
    }
    edge->motion.dx = model[0];
    edge->motion.dy = model[1];
    edge->motion.scale = (model[2] + model[5]) / 2.0;
    edge->motion.theta = (model[4] - model[3]) / 2.0;
    update_refit_reliability(edge, excluded, model);
    return 1;
}

static double reference_agreement(
    const MvstabFrame *frame,
    const MvstabCandidate *candidates,
    size_t count,
    const AffineModel *model,
    double threshold
) {
    double sum[2][3] = {{0}};
    double radius = fmax(frame->width, frame->height);
    for (size_t index = 0; index < count; ++index) {
        int group = candidates[index].vector->reference_direction > 0;
        double u = (candidates[index].vector->x - frame->width / 2.0) / radius;
        double v = (candidates[index].vector->y - frame->height / 2.0) / radius;
        if (!candidates[index].inlier) {
            continue;
        }
        sum[group][0] += candidates[index].base_weight;
        sum[group][1] += candidates[index].base_weight *
            (candidates[index].dx - model->value[2] * u - model->value[3] * v);
        sum[group][2] += candidates[index].base_weight *
            (candidates[index].dy - model->value[4] * u - model->value[5] * v);
    }
    if (sum[0][0] == 0.0 || sum[1][0] == 0.0) {
        return 1.0;
    }
    return exp(-hypot(sum[0][1] / sum[0][0] - sum[1][1] / sum[1][0],
                      sum[0][2] / sum[0][0] - sum[1][2] / sum[1][0]) / threshold);
}

static void populate_motion_cells(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    const MvstabCandidate *candidates,
    size_t count,
    MvstabMotionEdge *edge
) {
    double sums[MVSTAB_MAX_MOTION_CELLS][5] = {{0}};
    unsigned int vector_counts[MVSTAB_MAX_MOTION_CELLS] = {0};
    size_t grid_count = (size_t)config->grid_columns * config->grid_rows;
    edge->grid_columns = (uint16_t)config->grid_columns;
    edge->grid_rows = (uint16_t)config->grid_rows;
    if (grid_count > MVSTAB_MAX_MOTION_CELLS) {
        return;
    }
    for (size_t index = 0; index < count; ++index) {
        size_t cell = candidate_grid_cell(frame, config, &candidates[index]);
        double weight = candidates[index].base_weight;
        if (!candidates[index].inlier || weight <= 0.0) {
            continue;
        }
        sums[cell][0] += weight;
        sums[cell][1] += weight * (candidates[index].vector->x - frame->width / 2.0);
        sums[cell][2] += weight * (candidates[index].vector->y - frame->height / 2.0);
        sums[cell][3] += weight * candidates[index].dx;
        sums[cell][4] += weight * candidates[index].dy;
        ++vector_counts[cell];
    }
    for (size_t cell = 0; cell < grid_count; ++cell) {
        MvstabMotionCell *result;
        if (sums[cell][0] <= 0.0) {
            continue;
        }
        result = &edge->cells[edge->cell_count++];
        result->weight = (float)sums[cell][0];
        result->x = (float)(sums[cell][1] / sums[cell][0]);
        result->y = (float)(sums[cell][2] / sums[cell][0]);
        result->dx = (float)(sums[cell][3] / sums[cell][0]);
        result->dy = (float)(sums[cell][4] / sums[cell][0]);
        result->vector_count = vector_counts[cell] > UINT16_MAX
            ? UINT16_MAX : (uint16_t)vector_counts[cell];
        result->grid_index = (uint16_t)cell;
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

static int config_is_valid(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config
) {
    return frame != NULL && config != NULL &&
        isfinite(config->residual_threshold_px) && isfinite(config->max_mv_px) &&
        isfinite(config->block_area_cap) && isfinite(config->mad_threshold) &&
        isfinite(config->min_confidence) && isfinite(config->min_spatial_coverage) &&
        config->residual_threshold_px > 0.0 && config->max_mv_px > 0.0 &&
        config->block_area_cap > 0.0 && config->mad_threshold > 0.0 &&
        config->min_confidence >= 0.0 && config->min_confidence <= 1.0 &&
        config->min_spatial_coverage >= 0.0 && config->min_spatial_coverage <= 1.0 &&
        config->grid_columns > 0 && config->grid_rows > 0 &&
        config->grid_columns <= 64 && config->grid_rows <= 64 &&
        (size_t)config->grid_columns * config->grid_rows <=
            MVSTAB_MAX_MOTION_CELLS &&
        (config->mode == MVSTAB_MODE_SAFE || config->mode == MVSTAB_MODE_ALL_MVS) &&
        frame->width > 0 && frame->height > 0 &&
        (frame->vector_count == 0 || frame->vectors != NULL);
}

static int estimate_candidate_motion(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    MvstabCandidate *candidates,
    size_t count,
    int compare_references,
    FrameMotion *motion
) {
    AffineModel model = {{0}};
    double radius = fmax(frame->width, frame->height);
    double threshold = 0.0;
    double total_weight;
    memset(motion, 0, sizeof(*motion));
    motion->vector_count = (int)count;
    if (count < 2) {
        return 0;
    }
    total_weight = balance_grid_weights(frame, config, candidates, count);
    if (robust_affine_fit(frame, config, candidates, count,
                          &model, &threshold) != 0) {
        return 0;
    }
    motion->dx = model.value[0];
    motion->dy = model.value[1];
    motion->scale = (model.value[2] + model.value[5]) / (2.0 * radius);
    motion->theta = (model.value[4] - model.value[3]) / (2.0 * radius);
    for (size_t index = 0; index < count; ++index) {
        candidates[index].inlier = candidates[index].residual <= threshold;
        candidates[index].weight = candidates[index].base_weight;
        motion->inlier_count += candidates[index].inlier;
    }
    mvstab_compute_confidence(frame, config, candidates, count,
                              total_weight, motion);
    motion->reference_agreement = compare_references ? reference_agreement(
        frame, candidates, count, &model, config->residual_threshold_px) : 1.0;
    motion->confidence *= motion->reference_agreement;
    motion->valid = motion->confidence >= config->min_confidence &&
                    motion->spatial_coverage >= config->min_spatial_coverage &&
                    motion->reference_agreement >= exp(-4.0);
    return 0;
}

int mvstab_estimate_frame(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    FrameMotion *motion
) {
    MvstabCandidate *candidates;
    size_t count;
    size_t exact_count;

    if (motion == NULL || !config_is_valid(frame, config)) {
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
    count = collect_candidates(frame, config, candidates, &exact_count);
    estimate_candidate_motion(frame, config, candidates, count, 1, motion);
    motion->temporal_normalized = count > 0 && exact_count == count;
    free(candidates);
    return 0;
}

static int add_motion_edge(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    MvstabCandidate *candidates,
    size_t count,
    MvstabMotionEdge *edge
) {
    int64_t delta = candidates[0].vector->reference_pts_delta;
    memset(edge, 0, sizeof(*edge));
    if (estimate_candidate_motion(frame, config, candidates, count, 0,
                                  &edge->motion) != 0 || !edge->motion.valid) {
        return 0;
    }
    if (frame->pts == INT64_MIN) {
        edge->reference_pts = INT64_MIN;
    } else {
        if ((delta > 0 && frame->pts > INT64_MAX - delta) ||
            (delta < 0 && frame->pts < INT64_MIN - delta)) {
            return 0;
        }
        edge->reference_pts = frame->pts + delta;
    }
    edge->reference_pts_seconds = frame->pts_seconds +
        candidates[0].vector->reference_delta_seconds;
    populate_motion_cells(frame, config, candidates, count, edge);
    return isfinite(edge->reference_pts_seconds);
}

static size_t count_reference_groups(
    const MvstabCandidate *candidates,
    size_t count
) {
    size_t groups = 0;
    for (size_t index = 0; index < count; ++index) {
        if (index == 0 ||
            !candidates_share_reference(&candidates[index - 1],
                                        &candidates[index])) {
            ++groups;
        }
    }
    return groups;
}

int mvstab_estimate_frame_edges(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    MvstabMotionEdge **edges,
    size_t *edge_count
) {
    MvstabCandidate *candidates;
    MvstabMotionEdge *result;
    size_t count;
    size_t group_count;
    size_t start = 0;
    if (edges == NULL || edge_count == NULL || !config_is_valid(frame, config)) {
        return -1;
    }
    *edges = NULL;
    *edge_count = 0;
    if (frame->key_frame || frame->vector_count == 0) {
        return 0;
    }
    candidates = calloc(frame->vector_count, sizeof(*candidates));
    if (candidates == NULL) {
        free(candidates);
        return -1;
    }
    count = collect_edge_candidates(frame, config, candidates);
    qsort(candidates, count, sizeof(*candidates), compare_candidate_reference);
    group_count = count_reference_groups(candidates, count);
    result = calloc(group_count, sizeof(*result));
    if (group_count > 0 && result == NULL) {
        free(candidates);
        return -1;
    }
    while (start < count) {
        size_t end = start + 1;
        while (end < count &&
               candidates_share_reference(&candidates[start], &candidates[end])) {
            ++end;
        }
        *edge_count += add_motion_edge(frame, config, &candidates[start],
                                       end - start, &result[*edge_count]);
        start = end;
    }
    free(candidates);
    if (*edge_count == 0) {
        free(result);
        return 0;
    }
    *edges = result;
    return 0;
}
