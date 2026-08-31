#include "internal.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEMPORAL_RADIUS 15
#define MIN_PERSISTENT_MATCHES 9

typedef struct {
    size_t current;
    size_t reference;
    double weight;
    double value[4];
} PoseEdge;

typedef struct {
    PoseEdge *edges;
    size_t edge_count;
    size_t frame_count;
    size_t *component;
    unsigned char *anchor;
    double *support;
} PoseGraph;

typedef struct {
    MvstabMotionEdge *edge;
    size_t reference;
    double interval_factor;
    double camera_dx;
    double camera_dy;
    double camera_theta;
    double camera_scale;
    size_t segment;
    float residual_x[MVSTAB_MAX_MOTION_CELLS];
    float residual_y[MVSTAB_MAX_MOTION_CELLS];
    uint64_t valid_cells;
} TemporalTrack;

static size_t find_component(size_t *parent, size_t node) {
    size_t root = node;
    while (parent[root] != root) {
        root = parent[root];
    }
    while (parent[node] != node) {
        size_t next = parent[node];
        parent[node] = root;
        node = next;
    }
    return root;
}

static void join_components(size_t *parent, size_t left, size_t right) {
    left = find_component(parent, left);
    right = find_component(parent, right);
    if (left != right) {
        parent[right] = left;
    }
}

static size_t find_pts_frame(
    const MvstabTimelineFrame *frames,
    size_t count,
    int64_t pts
) {
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (frames[middle].pts < pts) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low < count && frames[low].pts == pts ? low : count;
}

static size_t find_seconds_frame(
    const MvstabTimelineFrame *frames,
    size_t count,
    double seconds
) {
    size_t low = 0;
    size_t high = count;
    size_t best;
    double tolerance;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (frames[middle].pts_seconds < seconds) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    best = low == count ? count - 1 : low;
    if (best > 0 && fabs(frames[best - 1].pts_seconds - seconds) <
                    fabs(frames[best].pts_seconds - seconds)) {
        --best;
    }
    tolerance = count > 1 ? fabs(frames[1].pts_seconds - frames[0].pts_seconds) * 0.25
                          : 1e-6;
    return fabs(frames[best].pts_seconds - seconds) <= fmax(tolerance, 1e-6)
        ? best : count;
}

static size_t resolve_reference(
    const MvstabTimelineFrame *frames,
    size_t count,
    const MvstabMotionEdge *edge
) {
    size_t reference = count;
    if (edge->reference_pts != INT64_MIN) {
        reference = find_pts_frame(frames, count, edge->reference_pts);
    }
    if (reference == count && isfinite(edge->reference_pts_seconds)) {
        reference = find_seconds_frame(frames, count, edge->reference_pts_seconds);
    }
    return reference;
}

static size_t edge_capacity(
    const MvstabTimelineFrame *frames,
    size_t frame_count
) {
    size_t capacity = 0;
    for (size_t index = 0; index < frame_count; ++index) {
        if (SIZE_MAX - capacity < frames[index].edge_count) {
            return 0;
        }
        capacity += frames[index].edge_count;
    }
    return capacity;
}

static double edge_frame_span(size_t current, size_t reference) {
    return current >= reference ? (double)(current - reference)
                                : -(double)(reference - current);
}

static int edge_has_tracking_cells(const MvstabMotionEdge *edge) {
    return edge->cell_count >= 6 &&
           edge->cell_count <= MVSTAB_MAX_MOTION_CELLS &&
           edge->grid_columns > 0 && edge->grid_rows > 0 &&
           (size_t)edge->grid_columns * edge->grid_rows <=
               MVSTAB_MAX_MOTION_CELLS;
}

static MvstabMotionEdge *select_tracking_edge(
    MvstabTimelineFrame *frames,
    size_t frame_count,
    size_t current,
    size_t *reference
) {
    MvstabMotionEdge *best = NULL;
    size_t best_span = SIZE_MAX;
    for (size_t index = 0; index < frames[current].edge_count; ++index) {
        MvstabMotionEdge *edge = &frames[current].edges[index];
        size_t candidate = resolve_reference(frames, frame_count, edge);
        size_t span = candidate > current ? candidate - current : current - candidate;
        if (edge->motion.valid && edge_has_tracking_cells(edge) &&
            candidate < frame_count &&
            candidate != current && span < best_span) {
            best = edge;
            best_span = span;
            *reference = candidate;
        }
    }
    return best;
}

static void measure_track_residuals(TemporalTrack *track) {
    const FrameMotion *motion = &track->edge->motion;
    double minimum = fmax(1.5, 1.5 * motion->residual_median *
                          fabs(track->interval_factor));
    for (size_t index = 0; index < track->edge->cell_count; ++index) {
        const MvstabMotionCell *cell = &track->edge->cells[index];
        double predicted_x = motion->dx + motion->scale * cell->x -
                             motion->theta * cell->y;
        double predicted_y = motion->dy + motion->theta * cell->x +
                             motion->scale * cell->y;
        double residual_x = (cell->dx - predicted_x) * track->interval_factor;
        double residual_y = (cell->dy - predicted_y) * track->interval_factor;
        if (cell->grid_index >= MVSTAB_MAX_MOTION_CELLS ||
            hypot(residual_x, residual_y) < minimum) {
            continue;
        }
        track->residual_x[cell->grid_index] = (float)residual_x;
        track->residual_y[cell->grid_index] = (float)residual_y;
        track->valid_cells |= UINT64_C(1) << cell->grid_index;
    }
}

static double nominal_frame_interval(
    const MvstabTimelineFrame *frames,
    size_t frame_count
) {
    double samples[33];
    size_t count = 0;
    for (size_t index = 1; index < frame_count && count < 33; ++index) {
        double interval = frames[index].pts_seconds - frames[index - 1].pts_seconds;
        size_t position = count;
        if (!isfinite(interval) || interval <= 0.0) {
            continue;
        }
        while (position > 0 && samples[position - 1] > interval) {
            samples[position] = samples[position - 1];
            --position;
        }
        samples[position] = interval;
        ++count;
    }
    return count > 0 ? samples[count / 2] : 0.0;
}

static TemporalTrack *build_temporal_tracks(
    MvstabTimelineFrame *frames,
    size_t frame_count,
    double nominal_interval
) {
    TemporalTrack *tracks = calloc(frame_count, sizeof(*tracks));
    if (tracks == NULL) {
        return NULL;
    }
    for (size_t current = 0; current < frame_count; ++current) {
        tracks[current].edge = select_tracking_edge(
            frames, frame_count, current, &tracks[current].reference);
        if (tracks[current].edge == NULL) {
            continue;
        }
        double interval = frames[current].pts_seconds -
                          frames[tracks[current].reference].pts_seconds;
        if (!isfinite(interval) || interval == 0.0) {
            tracks[current].edge = NULL;
            continue;
        }
        tracks[current].interval_factor = nominal_interval / interval;
        tracks[current].camera_dx = tracks[current].edge->motion.dx *
                                    tracks[current].interval_factor;
        tracks[current].camera_dy = tracks[current].edge->motion.dy *
                                    tracks[current].interval_factor;
        tracks[current].camera_theta = tracks[current].edge->motion.theta *
                                       tracks[current].interval_factor;
        tracks[current].camera_scale = tracks[current].edge->motion.scale *
                                       tracks[current].interval_factor;
        measure_track_residuals(&tracks[current]);
    }
    return tracks;
}

static const TemporalTrack *nearby_track(
    const TemporalTrack *tracks,
    size_t frame_count,
    size_t keyframe,
    int direction
) {
    for (size_t distance = 1; distance <= TEMPORAL_RADIUS; ++distance) {
        if (direction < 0 && distance <= keyframe &&
            tracks[keyframe - distance].edge != NULL) {
            return &tracks[keyframe - distance];
        }
        if (direction > 0 && keyframe + distance < frame_count &&
            tracks[keyframe + distance].edge != NULL) {
            return &tracks[keyframe + distance];
        }
    }
    return NULL;
}

static int keyframe_breaks_history(
    const TemporalTrack *tracks,
    size_t frame_count,
    size_t keyframe
) {
    const TemporalTrack *left = nearby_track(
        tracks, frame_count, keyframe, -1);
    const TemporalTrack *right = nearby_track(
        tracks, frame_count, keyframe, 1);
    double scale;
    double theta_limit;
    double scale_limit;
    if (left == NULL || right == NULL) {
        return 1;
    }
    scale = fmax(hypot(left->camera_dx, left->camera_dy),
                 hypot(right->camera_dx, right->camera_dy));
    theta_limit = 0.01 + 0.25 * fmax(fabs(left->camera_theta),
                                     fabs(right->camera_theta));
    scale_limit = 0.02 + 0.25 * fmax(fabs(left->camera_scale),
                                     fabs(right->camera_scale));
    return hypot(left->camera_dx - right->camera_dx,
                 left->camera_dy - right->camera_dy) > 2.0 + 0.25 * scale ||
           fabs(left->camera_theta - right->camera_theta) > theta_limit ||
           fabs(left->camera_scale - right->camera_scale) > scale_limit;
}

static void assign_track_segments(
    const MvstabTimelineFrame *frames,
    size_t frame_count,
    double nominal_interval,
    TemporalTrack *tracks
) {
    size_t segment = 0;
    for (size_t index = 0; index < frame_count; ++index) {
        if (index > 0) {
            double interval = frames[index].pts_seconds - frames[index - 1].pts_seconds;
            int timestamp_break = !isfinite(interval) || interval <= 0.0 ||
                                  interval > 4.0 * nominal_interval;
            if (timestamp_break || (frames[index].key_frame &&
                                    keyframe_breaks_history(
                                        tracks, frame_count, index))) {
                ++segment;
            }
        }
        tracks[index].segment = segment;
    }
}

static int cells_are_neighbors(size_t left, size_t right, size_t columns) {
    size_t left_row = left / columns;
    size_t right_row = right / columns;
    size_t left_column = left % columns;
    size_t right_column = right % columns;
    return left_row + 1 >= right_row && right_row + 1 >= left_row &&
           left_column + 1 >= right_column && right_column + 1 >= left_column;
}

static int residual_matches(
    const TemporalTrack *left,
    size_t left_cell,
    const TemporalTrack *right
) {
    double lx = left->residual_x[left_cell];
    double ly = left->residual_y[left_cell];
    double left_length = hypot(lx, ly);
    size_t columns = left->edge->grid_columns;
    if (columns == 0 || columns != right->edge->grid_columns) {
        return 0;
    }
    for (size_t cell = 0; cell < MVSTAB_MAX_MOTION_CELLS; ++cell) {
        double rx;
        double ry;
        double distance;
        if (!(right->valid_cells & (UINT64_C(1) << cell)) ||
            !cells_are_neighbors(left_cell, cell, columns)) {
            continue;
        }
        rx = right->residual_x[cell];
        ry = right->residual_y[cell];
        distance = hypot(lx - rx, ly - ry);
        if (lx * rx + ly * ry > 0.0 &&
            distance <= 1.0 + 0.5 * fmin(left_length, hypot(rx, ry))) {
            return 1;
        }
    }
    return 0;
}

static uint64_t persistent_cell_mask(
    const TemporalTrack *tracks,
    size_t frame_count,
    size_t current
) {
    uint64_t result = 0;
    size_t begin = current > TEMPORAL_RADIUS ? current - TEMPORAL_RADIUS : 0;
    size_t end = frame_count - current > TEMPORAL_RADIUS + 1
        ? current + TEMPORAL_RADIUS + 1 : frame_count;
    for (size_t cell = 0; cell < MVSTAB_MAX_MOTION_CELLS; ++cell) {
        int matches = 1;
        if (!(tracks[current].valid_cells & (UINT64_C(1) << cell))) {
            continue;
        }
        for (size_t neighbor = begin; neighbor < end; ++neighbor) {
            if (neighbor != current && tracks[neighbor].edge != NULL &&
                tracks[neighbor].segment == tracks[current].segment) {
                matches += residual_matches(&tracks[current], cell,
                                            &tracks[neighbor]);
            }
        }
        if (matches >= MIN_PERSISTENT_MATCHES) {
            result |= UINT64_C(1) << cell;
        }
    }
    return result;
}

static int mask_is_compact(uint64_t mask, size_t occupied) {
    size_t marked = 0;
    for (size_t cell = 0; cell < MVSTAB_MAX_MOTION_CELLS; ++cell) {
        marked += (mask >> cell) & 1;
    }
    return marked > 0 && occupied >= 8 && occupied - marked >= 6 &&
           marked * 4 <= occupied;
}

static void apply_temporal_mask(MvstabTimelineFrame *frame, uint64_t mask) {
    unsigned char excluded[MVSTAB_MAX_MOTION_CELLS] = {0};
    for (size_t cell = 0; cell < MVSTAB_MAX_MOTION_CELLS; ++cell) {
        excluded[cell] = (unsigned char)((mask >> cell) & 1);
    }
    for (size_t index = 0; index < frame->edge_count; ++index) {
        if (!mvstab_refit_motion_cells(&frame->edges[index], excluded)) {
            frame->edges[index].motion.valid = 0;
        }
    }
}

static int refine_temporal_motion(
    MvstabTimelineFrame *frames,
    size_t frame_count
) {
    double nominal_interval = nominal_frame_interval(frames, frame_count);
    TemporalTrack *tracks;
    if (nominal_interval <= 0.0) {
        return 0;
    }
    tracks = build_temporal_tracks(frames, frame_count, nominal_interval);
    if (tracks == NULL) {
        return -1;
    }
    assign_track_segments(frames, frame_count, nominal_interval, tracks);
    for (size_t current = 0; current < frame_count; ++current) {
        uint64_t mask;
        if (tracks[current].edge == NULL) {
            continue;
        }
        mask = persistent_cell_mask(tracks, frame_count, current);
        if (mask_is_compact(mask, tracks[current].edge->cell_count)) {
            apply_temporal_mask(&frames[current], mask);
        }
    }
    free(tracks);
    return 0;
}

static void append_graph_edge(
    PoseGraph *graph,
    size_t current,
    size_t reference,
    const MvstabMotionEdge *source,
    size_t *parent
) {
    PoseEdge *edge = &graph->edges[graph->edge_count++];
    double span = fabs(edge_frame_span(current, reference));
    edge->current = current;
    edge->reference = reference;
    edge->weight = fmax(1e-4, source->motion.confidence *
                        fmax(source->motion.spatial_coverage, 0.125) /
                        fmax(1.0, span * span));
    edge->value[0] = source->motion.dx;
    edge->value[1] = source->motion.dy;
    edge->value[2] = source->motion.theta;
    edge->value[3] = source->motion.scale;
    graph->support[current] += edge->weight;
    graph->support[reference] += edge->weight;
    join_components(parent, current, reference);
}

static int initialize_graph(PoseGraph *graph, size_t frame_count, size_t capacity) {
    memset(graph, 0, sizeof(*graph));
    graph->frame_count = frame_count;
    graph->edges = calloc(capacity, sizeof(*graph->edges));
    graph->component = calloc(frame_count, sizeof(*graph->component));
    graph->anchor = calloc(frame_count, sizeof(*graph->anchor));
    graph->support = calloc(frame_count, sizeof(*graph->support));
    return graph->edges != NULL && graph->component != NULL &&
           graph->anchor != NULL && graph->support != NULL;
}

static int frame_has_adjacent_edge(
    const MvstabTimelineFrame *frames,
    size_t frame_count,
    size_t current
) {
    for (size_t index = 0; index < frames[current].edge_count; ++index) {
        const MvstabMotionEdge *edge = &frames[current].edges[index];
        size_t reference = resolve_reference(frames, frame_count, edge);
        if (edge->motion.valid && reference < frame_count &&
            fabs(edge_frame_span(current, reference)) == 1.0) {
            return 1;
        }
    }
    return 0;
}

static unsigned char *build_long_edge_policy(
    const MvstabTimelineFrame *frames,
    size_t frame_count
) {
    if (frame_count > SIZE_MAX / 2) {
        return NULL;
    }
    unsigned char *memory = calloc(frame_count * 2, sizeof(*memory));
    unsigned char *supported = memory;
    unsigned char *allow_long = memory == NULL ? NULL : memory + frame_count;
    size_t support_count = 0;
    if (memory == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < frame_count; ++index) {
        supported[index] = frame_has_adjacent_edge(frames, frame_count, index);
        support_count += supported[index];
    }
    if (support_count < frame_count / 4 + (frame_count % 4 != 0)) {
        memset(allow_long, 2, frame_count);
        return memory;
    }
    for (size_t start = 0; start < frame_count;) {
        size_t end = start;
        if (supported[start]) {
            ++start;
            continue;
        }
        while (end < frame_count && !supported[end]) {
            ++end;
        }
        if (end - start > TEMPORAL_RADIUS) {
            memset(allow_long + start, 1, end - start);
        }
        start = end;
    }
    return memory;
}

static int build_graph(
    const MvstabTimelineFrame *frames,
    size_t frame_count,
    PoseGraph *graph
) {
    size_t capacity = edge_capacity(frames, frame_count);
    unsigned char *edge_policy = build_long_edge_policy(frames, frame_count);
    unsigned char *allow_long = edge_policy == NULL ? NULL : edge_policy + frame_count;
    unsigned char *seen;
    memset(graph, 0, sizeof(*graph));
    if (capacity == 0 || edge_policy == NULL ||
        !initialize_graph(graph, frame_count, capacity)) {
        free(edge_policy);
        return capacity == 0 ? 0 : -1;
    }
    for (size_t index = 0; index < frame_count; ++index) {
        graph->component[index] = index;
    }
    for (size_t current = 0; current < frame_count; ++current) {
        for (size_t index = 0; index < frames[current].edge_count; ++index) {
            const MvstabMotionEdge *source = &frames[current].edges[index];
            size_t reference = resolve_reference(frames, frame_count, source);
            double span = reference < frame_count
                ? fabs(edge_frame_span(current, reference)) : 0.0;
            int trusted_long = allow_long[current] == 2 ||
                (allow_long[current] == 1 && frames[current].measured.valid);
            if (source->motion.valid && reference < frame_count &&
                reference != current && (trusted_long || span == 1.0)) {
                append_graph_edge(graph, current, reference, source, graph->component);
            }
        }
    }
    free(edge_policy);
    seen = calloc(frame_count, sizeof(*seen));
    if (seen == NULL) {
        return -1;
    }
    for (size_t index = 0; index < frame_count; ++index) {
        size_t root = find_component(graph->component, index);
        graph->component[index] = root;
        if (!seen[root]) {
            graph->anchor[index] = 1;
            seen[root] = 1;
        }
    }
    free(seen);
    return graph->edge_count > 0;
}

static void graph_product(
    const PoseGraph *graph,
    const double *input,
    double *output
) {
    memset(output, 0, graph->frame_count * sizeof(*output));
    for (size_t index = 0; index < graph->edge_count; ++index) {
        const PoseEdge *edge = &graph->edges[index];
        double difference = input[edge->current] - input[edge->reference];
        output[edge->current] += edge->weight * difference;
        output[edge->reference] -= edge->weight * difference;
    }
    for (size_t index = 0; index < graph->frame_count; ++index) {
        if (graph->anchor[index]) {
            output[index] += input[index];
        }
    }
}

static double vector_dot(const double *left, const double *right, size_t count) {
    double result = 0.0;
    for (size_t index = 0; index < count; ++index) {
        result += left[index] * right[index];
    }
    return result;
}

static void build_rhs(const PoseGraph *graph, int dimension, double *rhs) {
    memset(rhs, 0, graph->frame_count * sizeof(*rhs));
    for (size_t index = 0; index < graph->edge_count; ++index) {
        const PoseEdge *edge = &graph->edges[index];
        double value = edge->weight * edge->value[dimension];
        rhs[edge->current] += value;
        rhs[edge->reference] -= value;
    }
}

static void build_graph_diagonal(const PoseGraph *graph, double *diagonal) {
    for (size_t index = 0; index < graph->frame_count; ++index) {
        diagonal[index] = graph->anchor[index] ? 1.0 : 0.0;
    }
    for (size_t index = 0; index < graph->edge_count; ++index) {
        const PoseEdge *edge = &graph->edges[index];
        diagonal[edge->current] += edge->weight;
        diagonal[edge->reference] += edge->weight;
    }
}

static double apply_preconditioner(
    const double *residual,
    const double *diagonal,
    double *preconditioned,
    size_t count
) {
    for (size_t index = 0; index < count; ++index) {
        preconditioned[index] = residual[index] / diagonal[index];
    }
    return vector_dot(residual, preconditioned, count);
}

static int solve_dimension(const PoseGraph *graph, int dimension, double *solution) {
    size_t count = graph->frame_count;
    double *memory = calloc(count * 5, sizeof(*memory));
    double *residual;
    double *direction;
    double *product;
    double *preconditioned;
    double *diagonal;
    double residual_norm;
    double initial_norm;
    double inner_product;
    double tolerance;
    if (memory == NULL) {
        return -1;
    }
    residual = memory;
    direction = residual + count;
    product = direction + count;
    preconditioned = product + count;
    diagonal = preconditioned + count;
    build_rhs(graph, dimension, residual);
    build_graph_diagonal(graph, diagonal);
    inner_product = apply_preconditioner(
        residual, diagonal, preconditioned, count);
    memcpy(direction, preconditioned, count * sizeof(*direction));
    residual_norm = vector_dot(residual, residual, count);
    initial_norm = residual_norm;
    tolerance = fmax(1e-20, initial_norm * 1e-16);
    for (size_t iteration = 0; iteration < 2000 &&
         residual_norm > tolerance; ++iteration) {
        double denominator;
        double next_norm;
        double next_inner_product;
        graph_product(graph, direction, product);
        denominator = vector_dot(direction, product, count);
        if (!isfinite(denominator) || denominator <= 0.0 ||
            !isfinite(inner_product) || inner_product <= 0.0) {
            free(memory);
            return -1;
        }
        double alpha = inner_product / denominator;
        for (size_t index = 0; index < count; ++index) {
            solution[index] += alpha * direction[index];
            residual[index] -= alpha * product[index];
        }
        next_norm = vector_dot(residual, residual, count);
        next_inner_product = apply_preconditioner(
            residual, diagonal, preconditioned, count);
        for (size_t index = 0; index < count; ++index) {
            direction[index] = preconditioned[index] +
                (next_inner_product / inner_product) * direction[index];
        }
        residual_norm = next_norm;
        inner_product = next_inner_product;
    }
    free(memory);
    return residual_norm <= tolerance ? 0 : -1;
}

static void write_graph_output(
    MvstabTimelineFrame *frames,
    const PoseGraph *graph,
    const double *poses
) {
    frames[0].output = (FrameMotion){.valid = 1, .temporal_normalized = 1};
    for (size_t index = 1; index < graph->frame_count; ++index) {
        FrameMotion output = frames[index].measured;
        int connected = graph->component[index] == graph->component[index - 1];
        if (!connected && frames[index].measured.valid) {
            frames[index].output = frames[index].measured;
            continue;
        }
        output.dx = connected ? poses[index] - poses[index - 1] : 0.0;
        output.dy = connected ? poses[graph->frame_count + index] -
                                poses[graph->frame_count + index - 1] : 0.0;
        output.theta = connected ? poses[2 * graph->frame_count + index] -
                                   poses[2 * graph->frame_count + index - 1] : 0.0;
        output.scale = connected ? poses[3 * graph->frame_count + index] -
                                   poses[3 * graph->frame_count + index - 1] : 0.0;
        output.valid = 1;
        output.scene_cut = 0;
        output.interpolated = connected && !frames[index].measured.valid;
        output.temporal_normalized = 1;
        if (!frames[index].measured.valid) {
            output.confidence = fmin(1.0, graph->support[index]);
        }
        frames[index].output = output;
    }
}

static void free_graph(PoseGraph *graph) {
    free(graph->edges);
    free(graph->component);
    free(graph->anchor);
    free(graph->support);
}

int mvstab_build_pose_graph_timeline(
    MvstabTimelineFrame *frames,
    size_t frame_count
) {
    PoseGraph graph;
    double *poses;
    int result;
    if (refine_temporal_motion(frames, frame_count) != 0) {
        return -1;
    }
    result = build_graph(frames, frame_count, &graph);
    if (result <= 0) {
        free_graph(&graph);
        return result;
    }
    if (frame_count > SIZE_MAX / 5) {
        free_graph(&graph);
        return -1;
    }
    poses = calloc(frame_count * 4, sizeof(*poses));
    if (poses == NULL) {
        free_graph(&graph);
        return -1;
    }
    for (int dimension = 0; dimension < 4; ++dimension) {
        if (solve_dimension(&graph, dimension, poses + dimension * frame_count) != 0) {
            free(poses);
            free_graph(&graph);
            return -1;
        }
    }
    write_graph_output(frames, &graph, poses);
    free(poses);
    free_graph(&graph);
    return 1;
}
