#include "internal.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

static void append_graph_edge(
    PoseGraph *graph,
    size_t current,
    size_t reference,
    const MvstabMotionEdge *source,
    size_t *parent
) {
    PoseEdge *edge = &graph->edges[graph->edge_count++];
    edge->current = current;
    edge->reference = reference;
    edge->weight = fmax(1e-4, source->motion.confidence *
                        fmax(source->motion.spatial_coverage, 0.125));
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

static int build_graph(
    const MvstabTimelineFrame *frames,
    size_t frame_count,
    PoseGraph *graph
) {
    size_t capacity = edge_capacity(frames, frame_count);
    unsigned char *seen;
    memset(graph, 0, sizeof(*graph));
    if (capacity == 0 || !initialize_graph(graph, frame_count, capacity)) {
        return capacity == 0 ? 0 : -1;
    }
    for (size_t index = 0; index < frame_count; ++index) {
        graph->component[index] = index;
    }
    for (size_t current = 0; current < frame_count; ++current) {
        for (size_t index = 0; index < frames[current].edge_count; ++index) {
            const MvstabMotionEdge *source = &frames[current].edges[index];
            size_t reference = resolve_reference(frames, frame_count, source);
            if (source->motion.valid && reference < frame_count && reference != current) {
                append_graph_edge(graph, current, reference, source, graph->component);
            }
        }
    }
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

static int solve_dimension(const PoseGraph *graph, int dimension, double *solution) {
    size_t count = graph->frame_count;
    double *memory = calloc(count * 4, sizeof(*memory));
    double *residual;
    double *direction;
    double *product;
    double residual_norm;
    double initial_norm;
    double tolerance;
    if (memory == NULL) {
        return -1;
    }
    residual = memory + count;
    direction = residual + count;
    product = direction + count;
    build_rhs(graph, dimension, residual);
    memcpy(direction, residual, count * sizeof(*direction));
    residual_norm = vector_dot(residual, residual, count);
    initial_norm = residual_norm;
    tolerance = fmax(1e-20, initial_norm * 1e-16);
    for (size_t iteration = 0; iteration < 2000 &&
         residual_norm > tolerance; ++iteration) {
        double denominator;
        double next_norm;
        graph_product(graph, direction, product);
        denominator = vector_dot(direction, product, count);
        if (!isfinite(denominator) || denominator <= 0.0) {
            free(memory);
            return -1;
        }
        double alpha = residual_norm / denominator;
        for (size_t index = 0; index < count; ++index) {
            solution[index] += alpha * direction[index];
            residual[index] -= alpha * product[index];
        }
        next_norm = vector_dot(residual, residual, count);
        for (size_t index = 0; index < count; ++index) {
            direction[index] = residual[index] +
                (next_norm / residual_norm) * direction[index];
        }
        residual_norm = next_norm;
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
    int result = build_graph(frames, frame_count, &graph);
    if (result <= 0) {
        free_graph(&graph);
        return result;
    }
    if (frame_count > SIZE_MAX / 4) {
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
