#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mvstab/estimator.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

static void set_vector(
    MvstabVector *vector,
    int cell,
    double dx,
    double dy,
    int direction
) {
    memset(vector, 0, sizeof(*vector));
    vector->x = (cell % 8) * 100 + 50;
    vector->y = (cell / 8) * 100 + 50;
    vector->dx = dx;
    vector->dy = dy;
    vector->weight = 256.0;
    vector->width = 16;
    vector->height = 16;
    vector->motion_scale = 4;
    vector->reference_direction = direction;
}

static MvstabFrame make_frame(MvstabVector *vectors, size_t count) {
    MvstabFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.picture_type = MVSTAB_PICTURE_P;
    frame.width = 800;
    frame.height = 400;
    frame.duration_seconds = 1.0 / 30.0;
    frame.vectors = vectors;
    frame.vector_count = count;
    return frame;
}

static int test_foreground_outliers_do_not_win(void) {
    MvstabVector vectors[40];
    MvstabFrame frame;
    FrameMotion motion;
    MvstabEstimatorConfig config = mvstab_default_estimator_config();
    int index;

    for (index = 0; index < 32; ++index) {
        set_vector(&vectors[index], index, 3.0, -1.0, -1);
    }
    for (index = 32; index < 40; ++index) {
        set_vector(&vectors[index], index - 32, -20.0, 12.0, -1);
    }
    frame = make_frame(vectors, 40);
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == 0);
    CHECK(motion.valid);
    CHECK(fabs(motion.dx - 3.0) < 1e-9);
    CHECK(fabs(motion.dy + 1.0) < 1e-9);
    CHECK(motion.vector_count == 40);
    CHECK(motion.inlier_count == 32);
    CHECK(motion.confidence > 0.5);
    return 0;
}

static int test_safe_mode_rejects_b_frames(void) {
    MvstabVector vectors[16];
    MvstabFrame frame;
    FrameMotion motion;
    MvstabEstimatorConfig config = mvstab_default_estimator_config();
    int index;

    for (index = 0; index < 16; ++index) {
        set_vector(&vectors[index], index, 2.0, 1.0, -1);
    }
    frame = make_frame(vectors, 16);
    frame.picture_type = MVSTAB_PICTURE_B;
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == 0);
    CHECK(!motion.valid);
    CHECK(motion.vector_count == 0);
    return 0;
}

static int test_all_mvs_normalizes_future_direction(void) {
    MvstabVector vectors[32];
    MvstabFrame frame;
    FrameMotion motion;
    MvstabEstimatorConfig config = mvstab_default_estimator_config();
    int index;

    config.mode = MVSTAB_MODE_ALL_MVS;
    for (index = 0; index < 16; ++index) {
        set_vector(&vectors[index], index * 2, 2.0, -0.5, -1);
        set_vector(&vectors[index + 16], index * 2, -2.0, 0.5, 1);
    }
    frame = make_frame(vectors, 32);
    frame.picture_type = MVSTAB_PICTURE_B;
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == 0);
    CHECK(motion.valid);
    CHECK(fabs(motion.dx - 2.0) < 1e-9);
    CHECK(fabs(motion.dy + 0.5) < 1e-9);
    CHECK(fabs(motion.reference_agreement - 1.0) < 1e-9);
    return 0;
}

static int test_all_mvs_rejects_reference_disagreement(void) {
    MvstabVector vectors[32];
    MvstabFrame frame;
    FrameMotion motion;
    MvstabEstimatorConfig config = mvstab_default_estimator_config();
    int index;

    config.mode = MVSTAB_MODE_ALL_MVS;
    for (index = 0; index < 16; ++index) {
        set_vector(&vectors[index], index * 2, 2.0, 0.0, -1);
        set_vector(&vectors[index + 16], index * 2, -8.0, 0.0, 1);
    }
    frame = make_frame(vectors, 32);
    frame.picture_type = MVSTAB_PICTURE_B;
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == 0);
    CHECK(!motion.valid);
    CHECK(motion.reference_agreement < 0.1);
    config.min_confidence = 0.4;
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == 0);
    CHECK(!motion.valid);
    CHECK(motion.reference_agreement < 0.1);
    return 0;
}

static int test_all_mvs_needs_a_supported_reference(void) {
    MvstabVector vector;
    MvstabFrame frame;
    FrameMotion motion;
    MvstabEstimatorConfig config = mvstab_default_estimator_config();
    config.mode = MVSTAB_MODE_ALL_MVS;
    config.min_confidence = 0.0;
    set_vector(&vector, 0, 1.0, 0.0, 0);
    frame = make_frame(&vector, 1);
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == 0);
    CHECK(!motion.valid);
    return 0;
}

static void set_exact_timing(
    MvstabVector *vector,
    double reference_delta_seconds
) {
    vector->reference_exact = 1;
    vector->reference_pts_valid = 1;
    vector->reference_delta_seconds = reference_delta_seconds;
    vector->reference_direction = reference_delta_seconds > 0.0 ? 1 : -1;
}

static int test_exact_timing_unifies_distant_references(void) {
    MvstabVector vectors[32];
    MvstabEstimatorConfig config = mvstab_default_estimator_config();
    MvstabFrame frame;
    FrameMotion motion;
    for (int index = 0; index < 16; ++index) {
        set_vector(&vectors[index], index * 2, 6.0, -3.0, -1);
        set_exact_timing(&vectors[index], -0.1);
        set_vector(&vectors[index + 16], index * 2, -4.0, 2.0, 1);
        set_exact_timing(&vectors[index + 16], 2.0 / 30.0);
    }
    frame = make_frame(vectors, 32);
    frame.picture_type = MVSTAB_PICTURE_B;
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == 0);
    CHECK(motion.valid && motion.temporal_normalized);
    CHECK(fabs(motion.dx - 2.0) < 1e-9);
    CHECK(fabs(motion.dy + 1.0) < 1e-9);
    CHECK(fabs(motion.reference_agreement - 1.0) < 1e-9);
    return 0;
}

static int test_exact_edges_preserve_reference_motion(void) {
    MvstabVector vectors[32];
    MvstabEstimatorConfig config = mvstab_default_estimator_config();
    MvstabFrame frame;
    MvstabMotionEdge *edges = NULL;
    size_t edge_count = 0;
    for (int index = 0; index < 16; ++index) {
        set_vector(&vectors[index], index * 2, 6.0, -3.0, -1);
        set_exact_timing(&vectors[index], -0.1);
        vectors[index].reference_pts_delta = -3;
        set_vector(&vectors[index + 16], index * 2, -4.0, 2.0, 1);
        set_exact_timing(&vectors[index + 16], 2.0 / 30.0);
        vectors[index + 16].reference_pts_delta = 2;
    }
    frame = make_frame(vectors, 32);
    frame.pts = 100;
    frame.pts_seconds = 10.0;
    CHECK(mvstab_estimate_frame_edges(&frame, &config, &edges, &edge_count) == 0);
    CHECK(edge_count == 2);
    CHECK(edges[0].reference_pts == 97 && fabs(edges[0].motion.dx - 6.0) < 1e-9);
    CHECK(edges[1].reference_pts == 102 && fabs(edges[1].motion.dx + 4.0) < 1e-9);
    CHECK(fabs(edges[0].motion.dy + 3.0) < 1e-9);
    CHECK(fabs(edges[1].motion.dy - 2.0) < 1e-9);
    free(edges);
    return 0;
}

static int test_exact_edges_keep_reference_fields_separate(void) {
    MvstabVector vectors[32];
    MvstabEstimatorConfig config = mvstab_default_estimator_config();
    MvstabFrame frame;
    MvstabMotionEdge *edges = NULL;
    size_t edge_count = 0;
    for (int index = 0; index < 16; ++index) {
        set_vector(&vectors[index], index * 2, 2.0, 0.0, -1);
        set_exact_timing(&vectors[index], -1.0 / 30.0);
        vectors[index].reference_pts_delta = -1;
        vectors[index].reference_top_field = 1;
        set_vector(&vectors[index + 16], index * 2, 4.0, 0.0, -1);
        set_exact_timing(&vectors[index + 16], -1.0 / 30.0);
        vectors[index + 16].reference_pts_delta = -1;
        vectors[index + 16].reference_bottom_field = 1;
    }
    frame = make_frame(vectors, 32);
    frame.pts = 100;
    CHECK(mvstab_estimate_frame_edges(&frame, &config, &edges, &edge_count) == 0);
    CHECK(edge_count == 2);
    CHECK(fabs(edges[0].motion.dx + edges[1].motion.dx - 6.0) < 1e-9);
    CHECK(fabs(edges[0].motion.dx - edges[1].motion.dx) > 1.0);
    free(edges);
    return 0;
}

static int test_similarity_fit_separates_scale_and_rotation(void) {
    MvstabVector vectors[32];
    MvstabEstimatorConfig config = mvstab_default_estimator_config();
    MvstabFrame frame = make_frame(vectors, 32);
    FrameMotion motion;
    const double dx = 2.0, dy = -1.0, scale = 0.01, theta = 0.005;
    for (int index = 0; index < 32; ++index) {
        double x;
        double y;
        set_vector(&vectors[index], index, 0.0, 0.0, -1);
        x = vectors[index].x - frame.width / 2.0;
        y = vectors[index].y - frame.height / 2.0;
        vectors[index].dx = dx + scale * x - theta * y;
        vectors[index].dy = dy + theta * x + scale * y;
    }
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == 0);
    CHECK(motion.valid);
    CHECK(fabs(motion.dx - dx) < 1e-9 && fabs(motion.dy - dy) < 1e-9);
    CHECK(fabs(motion.scale - scale) < 1e-9);
    CHECK(fabs(motion.theta - theta) < 1e-9);
    return 0;
}

static int estimate_coverage(int spread, FrameMotion *motion) {
    MvstabVector vectors[16];
    MvstabEstimatorConfig config = mvstab_default_estimator_config();
    MvstabFrame frame;
    int index;

    for (index = 0; index < 16; ++index) {
        set_vector(&vectors[index], spread ? index * 2 : 0, 1.0, 0.0, -1);
    }
    frame = make_frame(vectors, 16);
    if (mvstab_estimate_frame(&frame, &config, motion) != 0) {
        return -1;
    }
    return 0;
}

static int test_spatial_coverage_affects_confidence(void) {
    FrameMotion spread;
    FrameMotion clustered;
    CHECK(estimate_coverage(1, &spread) == 0);
    CHECK(estimate_coverage(0, &clustered) == 0);
    CHECK(spread.confidence > clustered.confidence);
    CHECK(spread.valid);
    CHECK(!clustered.valid);
    return 0;
}

static int test_adjacent_cells_are_not_global_support(void) {
    MvstabVector vectors[16];
    MvstabEstimatorConfig config = mvstab_default_estimator_config();
    MvstabFrame frame;
    FrameMotion motion;
    int cells[4] = {0, 1, 8, 9};
    int index;
    for (index = 0; index < 16; ++index) {
        set_vector(&vectors[index], cells[index % 4], 1.0, 0.0, -1);
    }
    frame = make_frame(vectors, 16);
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == 0);
    CHECK(!motion.valid);
    CHECK(motion.spatial_coverage == 0.0);
    return 0;
}

static int test_one_quadrant_is_not_global_support(void) {
    MvstabVector vectors[16];
    MvstabEstimatorConfig config = mvstab_default_estimator_config();
    MvstabFrame frame;
    FrameMotion motion;
    int cells[4] = {0, 3, 8, 11};
    int index;
    for (index = 0; index < 16; ++index) {
        set_vector(&vectors[index], cells[index % 4], 1.0, 0.0, -1);
    }
    frame = make_frame(vectors, 16);
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == 0);
    CHECK(!motion.valid);
    CHECK(motion.spatial_coverage == 0.0);
    return 0;
}

static int test_invalid_configuration_is_rejected(void) {
    MvstabVector vector;
    MvstabFrame frame;
    FrameMotion motion;
    MvstabEstimatorConfig config = mvstab_default_estimator_config();
    set_vector(&vector, 0, 1.0, 0.0, -1);
    frame = make_frame(&vector, 1);
    config.residual_threshold_px = 0.0;
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == -1);
    config = mvstab_default_estimator_config();
    config.min_confidence = 1.1;
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == -1);
    config = mvstab_default_estimator_config();
    config.min_spatial_coverage = 1.1;
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == -1);
    config = mvstab_default_estimator_config();
    config.mode = (MvstabMode)99;
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == -1);
    config = mvstab_default_estimator_config();
    frame.vectors = NULL;
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == -1);
    frame.vectors = &vector;
    vector.x = NAN;
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == 0);
    CHECK(motion.vector_count == 0);
    set_vector(&vector, 0, 1.0, 0.0, -1);
    vector.weight = 0.0;
    config = mvstab_default_estimator_config();
    CHECK(mvstab_estimate_frame(&frame, &config, &motion) == 0);
    CHECK(motion.vector_count == 0);
    return 0;
}

int main(void) {
    if (test_foreground_outliers_do_not_win() != 0 ||
        test_safe_mode_rejects_b_frames() != 0 ||
        test_all_mvs_normalizes_future_direction() != 0 ||
        test_all_mvs_rejects_reference_disagreement() != 0 ||
        test_all_mvs_needs_a_supported_reference() != 0 ||
        test_exact_timing_unifies_distant_references() != 0 ||
        test_exact_edges_preserve_reference_motion() != 0 ||
        test_exact_edges_keep_reference_fields_separate() != 0 ||
        test_similarity_fit_separates_scale_and_rotation() != 0) {
        return 1;
    }
    if (test_spatial_coverage_affects_confidence() != 0 ||
        test_adjacent_cells_are_not_global_support() != 0 ||
        test_one_quadrant_is_not_global_support() != 0) {
        return 1;
    }
    return test_invalid_configuration_is_rejected();
}
