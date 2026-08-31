#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mvstab/timeline.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

static void initialize_frame(
    MvstabTimelineFrame *frame,
    MvstabPictureType type,
    int key_frame
) {
    memset(frame, 0, sizeof(*frame));
    frame->picture_type = type;
    frame->key_frame = key_frame;
}

static int test_safe_mode_distributes_anchor_motion(void) {
    MvstabTimelineFrame frames[4];
    int index;
    initialize_frame(&frames[0], MVSTAB_PICTURE_I, 1);
    initialize_frame(&frames[1], MVSTAB_PICTURE_B, 0);
    initialize_frame(&frames[2], MVSTAB_PICTURE_B, 0);
    initialize_frame(&frames[3], MVSTAB_PICTURE_P, 0);
    frames[3].measured.valid = 1;
    frames[3].measured.dx = 9.0;
    frames[3].measured.dy = -3.0;
    frames[3].measured.confidence = 0.8;

    mvstab_build_timeline(frames, 4, MVSTAB_MODE_SAFE);
    CHECK(frames[0].output.valid && !frames[0].output.scene_cut);
    CHECK(frames[0].output.dx == 0.0);
    for (index = 1; index < 4; ++index) {
        CHECK(fabs(frames[index].output.dx - 3.0) < 1e-9);
        CHECK(fabs(frames[index].output.dy + 1.0) < 1e-9);
        CHECK(frames[index].output.interpolated);
    }
    return 0;
}

static int test_invalid_p_frame_advances_anchor(void) {
    MvstabTimelineFrame frames[5];
    int index;
    initialize_frame(&frames[0], MVSTAB_PICTURE_I, 1);
    initialize_frame(&frames[1], MVSTAB_PICTURE_B, 0);
    initialize_frame(&frames[2], MVSTAB_PICTURE_P, 0);
    initialize_frame(&frames[3], MVSTAB_PICTURE_B, 0);
    initialize_frame(&frames[4], MVSTAB_PICTURE_P, 0);
    frames[4].measured.valid = 1;
    frames[4].measured.dx = 4.0;

    mvstab_build_timeline(frames, 5, MVSTAB_MODE_SAFE);
    CHECK(frames[1].output.dx == 0.0);
    CHECK(frames[2].output.dx == 0.0);
    for (index = 3; index < 5; ++index) {
        CHECK(fabs(frames[index].output.dx - 2.0) < 1e-9);
    }
    return 0;
}

static int test_all_mvs_uses_measurements_without_interpolation(void) {
    MvstabTimelineFrame frames[2];
    initialize_frame(&frames[0], MVSTAB_PICTURE_I, 1);
    initialize_frame(&frames[1], MVSTAB_PICTURE_B, 0);
    frames[1].measured.valid = 1;
    frames[1].measured.dx = 1.25;
    mvstab_build_timeline(frames, 2, MVSTAB_MODE_ALL_MVS);
    CHECK(frames[0].output.dx == 0.0);
    CHECK(fabs(frames[1].output.dx - 1.25) < 1e-9);
    CHECK(!frames[1].output.interpolated);
    return 0;
}

static int test_first_frame_is_always_identity(void) {
    MvstabTimelineFrame frame;
    initialize_frame(&frame, MVSTAB_PICTURE_P, 0);
    frame.measured.valid = 1;
    frame.measured.dx = 5.0;
    mvstab_build_timeline(&frame, 1, MVSTAB_MODE_ALL_MVS);
    CHECK(frame.output.valid);
    CHECK(frame.output.dx == 0.0);
    return 0;
}

static int test_safe_mode_uses_display_timestamps(void) {
    MvstabTimelineFrame frames[3];
    initialize_frame(&frames[0], MVSTAB_PICTURE_I, 1);
    initialize_frame(&frames[1], MVSTAB_PICTURE_B, 0);
    initialize_frame(&frames[2], MVSTAB_PICTURE_P, 0);
    frames[0].pts_seconds = 0.0;
    frames[1].pts_seconds = 0.01;
    frames[2].pts_seconds = 0.04;
    frames[2].measured.valid = 1;
    frames[2].measured.dx = 4.0;
    mvstab_build_timeline(frames, 3, MVSTAB_MODE_SAFE);
    CHECK(fabs(frames[1].output.dx - 1.0) < 1e-9);
    CHECK(fabs(frames[2].output.dx - 3.0) < 1e-9);
    return 0;
}

static int test_periodic_keyframe_does_not_drop_motion(void) {
    MvstabTimelineFrame frames[4];
    initialize_frame(&frames[0], MVSTAB_PICTURE_I, 1);
    initialize_frame(&frames[1], MVSTAB_PICTURE_P, 0);
    initialize_frame(&frames[2], MVSTAB_PICTURE_I, 1);
    initialize_frame(&frames[3], MVSTAB_PICTURE_P, 0);
    frames[1].measured.valid = 1;
    frames[1].measured.dx = 2.0;
    frames[3].measured.valid = 1;
    frames[3].measured.dx = 2.0;
    mvstab_build_timeline(frames, 4, MVSTAB_MODE_SAFE);
    CHECK(fabs(frames[2].output.dx - 2.0) < 1e-9);
    CHECK(frames[2].output.interpolated);
    CHECK(!frames[2].output.scene_cut);
    return 0;
}

static int test_periodic_keyframe_fills_b_frame_tail(void) {
    MvstabTimelineFrame frames[8];
    int index;
    MvstabPictureType types[8] = {
        MVSTAB_PICTURE_I, MVSTAB_PICTURE_B, MVSTAB_PICTURE_P,
        MVSTAB_PICTURE_B, MVSTAB_PICTURE_B, MVSTAB_PICTURE_I,
        MVSTAB_PICTURE_B, MVSTAB_PICTURE_P
    };
    for (index = 0; index < 8; ++index) {
        initialize_frame(&frames[index], types[index], index == 0 || index == 5);
    }
    frames[2].measured.valid = 1;
    frames[2].measured.dx = 4.0;
    frames[7].measured.valid = 1;
    frames[7].measured.dx = 4.0;
    mvstab_build_timeline(frames, 8, MVSTAB_MODE_SAFE);
    for (index = 1; index < 8; ++index) {
        CHECK(fabs(frames[index].output.dx - 2.0) < 1e-9);
    }
    CHECK(frames[3].output.interpolated);
    CHECK(frames[4].output.interpolated);
    CHECK(frames[5].output.interpolated);
    return 0;
}

static int test_keyframe_fill_interpolates_velocity(void) {
    MvstabTimelineFrame frames[4];
    initialize_frame(&frames[0], MVSTAB_PICTURE_I, 1);
    initialize_frame(&frames[1], MVSTAB_PICTURE_P, 0);
    initialize_frame(&frames[2], MVSTAB_PICTURE_I, 1);
    initialize_frame(&frames[3], MVSTAB_PICTURE_P, 0);
    frames[0].pts_seconds = 0.0;
    frames[1].pts_seconds = 0.01;
    frames[2].pts_seconds = 0.02;
    frames[3].pts_seconds = 0.05;
    frames[1].measured.valid = 1;
    frames[1].measured.dx = 1.0;
    frames[3].measured.valid = 1;
    frames[3].measured.dx = 3.0;
    mvstab_build_timeline(frames, 4, MVSTAB_MODE_SAFE);
    CHECK(fabs(frames[2].output.dx - 1.0) < 1e-9);
    return 0;
}

static int test_keyframe_fill_stops_at_rejected_p_frame(void) {
    MvstabTimelineFrame frames[7];
    MvstabPictureType types[7] = {
        MVSTAB_PICTURE_I, MVSTAB_PICTURE_P, MVSTAB_PICTURE_P,
        MVSTAB_PICTURE_B, MVSTAB_PICTURE_I, MVSTAB_PICTURE_B,
        MVSTAB_PICTURE_P
    };
    int index;
    for (index = 0; index < 7; ++index) {
        initialize_frame(&frames[index], types[index], index == 0 || index == 4);
    }
    frames[1].measured.valid = 1;
    frames[1].measured.dx = 2.0;
    frames[6].measured.valid = 1;
    frames[6].measured.dx = 4.0;
    mvstab_build_timeline(frames, 7, MVSTAB_MODE_SAFE);
    CHECK(frames[2].output.dx == 0.0);
    CHECK(frames[3].output.dx == 0.0);
    CHECK(!frames[4].output.scene_cut);
    CHECK(frames[4].output.dx == 0.0);
    return 0;
}

static int test_all_mvs_does_not_repair_rejected_frames(void) {
    MvstabTimelineFrame frames[4];
    initialize_frame(&frames[0], MVSTAB_PICTURE_I, 1);
    initialize_frame(&frames[1], MVSTAB_PICTURE_B, 0);
    initialize_frame(&frames[2], MVSTAB_PICTURE_I, 1);
    initialize_frame(&frames[3], MVSTAB_PICTURE_B, 0);
    frames[3].measured.valid = 1;
    frames[3].measured.dx = 3.0;
    mvstab_build_timeline(frames, 4, MVSTAB_MODE_ALL_MVS);
    CHECK(frames[1].output.dx == 0.0);
    CHECK(!frames[1].output.interpolated);
    CHECK(frames[2].output.dx == 0.0);
    CHECK(!frames[2].output.interpolated);
    return 0;
}

static int test_exact_timing_uses_b_frames_and_repairs_keyframe(void) {
    MvstabTimelineFrame frames[4];
    initialize_frame(&frames[0], MVSTAB_PICTURE_B, 0);
    initialize_frame(&frames[1], MVSTAB_PICTURE_B, 0);
    initialize_frame(&frames[2], MVSTAB_PICTURE_I, 1);
    initialize_frame(&frames[3], MVSTAB_PICTURE_B, 0);
    for (int index = 0; index < 4; ++index) {
        frames[index].pts_seconds = index / 30.0;
        if (index != 2) {
            frames[index].measured.valid = 1;
            frames[index].measured.temporal_normalized = 1;
            frames[index].measured.dx = 2.0;
            frames[index].measured.theta = 0.01;
        }
    }
    mvstab_build_timeline(frames, 4, MVSTAB_MODE_SAFE);
    CHECK(frames[0].output.dx == 0.0);
    CHECK(fabs(frames[1].output.dx - 2.0) < 1e-9);
    CHECK(fabs(frames[2].output.dx - 2.0) < 1e-9);
    CHECK(fabs(frames[2].output.theta - 0.01) < 1e-9);
    CHECK(frames[2].output.interpolated);
    CHECK(fabs(frames[3].output.dx - 2.0) < 1e-9);
    return 0;
}

static int test_exact_timeline_rejects_legacy_measurement(void) {
    MvstabTimelineFrame frames[5];
    for (int index = 0; index < 5; ++index) {
        initialize_frame(&frames[index], MVSTAB_PICTURE_B, 0);
        frames[index].pts_seconds = index / 30.0;
        frames[index].measured.valid = 1;
        frames[index].measured.dx = 2.0;
        frames[index].measured.temporal_normalized = index != 2;
    }
    frames[2].measured.dx = 100.0;
    mvstab_build_timeline(frames, 5, MVSTAB_MODE_SAFE);
    CHECK(frames[2].output.interpolated);
    CHECK(fabs(frames[2].output.dx - 2.0) < 1e-9);
    return 0;
}

static int test_invalid_exact_measurement_does_not_select_precise_mode(void) {
    MvstabTimelineFrame frames[3];
    for (int index = 0; index < 3; ++index) {
        initialize_frame(&frames[index], MVSTAB_PICTURE_P, 0);
        frames[index].pts_seconds = index / 30.0;
    }
    frames[1].measured.temporal_normalized = 1;
    frames[2].measured.valid = 1;
    frames[2].measured.dx = 4.0;
    mvstab_build_timeline(frames, 3, MVSTAB_MODE_SAFE);
    CHECK(fabs(frames[2].output.dx - 4.0) < 1e-9);
    CHECK(!frames[2].output.interpolated);
    return 0;
}

static void set_edge(
    MvstabMotionEdge *edge,
    int64_t reference_pts,
    double dx
) {
    memset(edge, 0, sizeof(*edge));
    edge->reference_pts = reference_pts;
    edge->reference_pts_seconds = reference_pts / 30.0;
    edge->motion.dx = dx;
    edge->motion.confidence = 1.0;
    edge->motion.spatial_coverage = 1.0;
    edge->motion.valid = 1;
}

static int test_pose_graph_recovers_adjacent_motion(void) {
    MvstabTimelineFrame frames[5];
    MvstabMotionEdge edges[4];
    int references[4] = {0, 0, 1, 2};
    double motions[4] = {2.0, 4.0, 4.0, 4.0};
    for (int index = 0; index < 5; ++index) {
        initialize_frame(&frames[index], MVSTAB_PICTURE_B, 0);
        frames[index].pts = index;
        frames[index].pts_seconds = index / 30.0;
    }
    for (int index = 0; index < 4; ++index) {
        set_edge(&edges[index], references[index], motions[index]);
        frames[index + 1].edges = &edges[index];
        frames[index + 1].edge_count = 1;
    }
    edges[3].reference_pts = INT64_MIN;
    mvstab_build_timeline(frames, 5, MVSTAB_MODE_SAFE);
    CHECK(frames[0].output.dx == 0.0);
    for (int index = 1; index < 5; ++index) {
        CHECK(fabs(frames[index].output.dx - 2.0) < 1e-7);
        CHECK(frames[index].output.temporal_normalized);
    }
    return 0;
}

static int test_pose_graph_does_not_bridge_components(void) {
    MvstabTimelineFrame frames[4];
    MvstabMotionEdge edges[2];
    for (int index = 0; index < 4; ++index) {
        initialize_frame(&frames[index], MVSTAB_PICTURE_B, 0);
        frames[index].pts = index;
        frames[index].pts_seconds = index / 30.0;
    }
    set_edge(&edges[0], 0, 2.0);
    set_edge(&edges[1], 2, 5.0);
    frames[1].edges = &edges[0];
    frames[1].edge_count = 1;
    frames[3].edges = &edges[1];
    frames[3].edge_count = 1;
    mvstab_build_timeline(frames, 4, MVSTAB_MODE_SAFE);
    CHECK(fabs(frames[1].output.dx - 2.0) < 1e-7);
    CHECK(frames[2].output.dx == 0.0);
    CHECK(fabs(frames[3].output.dx - 5.0) < 1e-7);
    return 0;
}

static int test_pose_graph_does_not_smooth_edge_acceleration(void) {
    MvstabTimelineFrame frames[3];
    MvstabMotionEdge edges[2];
    for (int index = 0; index < 3; ++index) {
        initialize_frame(&frames[index], MVSTAB_PICTURE_P, 0);
        frames[index].pts = index;
        frames[index].pts_seconds = index / 30.0;
    }
    set_edge(&edges[0], 0, 0.0);
    set_edge(&edges[1], 1, 10.0);
    frames[1].edges = &edges[0];
    frames[1].edge_count = 1;
    frames[2].edges = &edges[1];
    frames[2].edge_count = 1;
    mvstab_build_timeline(frames, 3, MVSTAB_MODE_SAFE);
    CHECK(fabs(frames[1].output.dx) < 1e-7);
    CHECK(fabs(frames[2].output.dx - 10.0) < 1e-7);
    return 0;
}

static int test_pose_graph_weights_confident_edges(void) {
    MvstabTimelineFrame frames[2];
    MvstabMotionEdge edges[2];
    for (int index = 0; index < 2; ++index) {
        initialize_frame(&frames[index], MVSTAB_PICTURE_P, 0);
        frames[index].pts = index;
        frames[index].pts_seconds = index / 30.0;
    }
    set_edge(&edges[0], 0, 2.0);
    set_edge(&edges[1], 0, 10.0);
    edges[1].motion.confidence = 0.1;
    frames[1].edges = edges;
    frames[1].edge_count = 2;
    mvstab_build_timeline(frames, 2, MVSTAB_MODE_SAFE);
    CHECK(fabs(frames[1].output.dx - 30.0 / 11.0) < 1e-7);
    return 0;
}

static int test_pose_graph_handles_varied_edge_weights(void) {
    const int frame_count = 2501;
    MvstabTimelineFrame *frames = calloc(frame_count, sizeof(*frames));
    MvstabMotionEdge *edges = calloc(frame_count - 1, sizeof(*edges));
    int correct;
    if (frames == NULL || edges == NULL) {
        free(frames);
        free(edges);
        return 1;
    }
    for (int index = 0; index < frame_count; ++index) {
        initialize_frame(&frames[index], MVSTAB_PICTURE_P, 0);
        frames[index].pts = index;
        frames[index].pts_seconds = index / 30.0;
        if (index == 0) {
            continue;
        }
        set_edge(&edges[index - 1], 0, index);
        edges[index - 1].motion.confidence =
            1e-4 + (double)index / frame_count;
        frames[index].edges = &edges[index - 1];
        frames[index].edge_count = 1;
    }
    mvstab_build_timeline(frames, frame_count, MVSTAB_MODE_SAFE);
    correct = fabs(frames[1].output.dx - 1.0) < 1e-6 &&
              fabs(frames[frame_count / 2].output.dx - 1.0) < 1e-6 &&
              fabs(frames[frame_count - 1].output.dx - 1.0) < 1e-6;
    free(frames);
    free(edges);
    CHECK(correct);
    return 0;
}

static int test_pose_graph_preserves_disconnected_measurement(void) {
    MvstabTimelineFrame frames[3];
    MvstabMotionEdge edge;
    for (int index = 0; index < 3; ++index) {
        initialize_frame(&frames[index], MVSTAB_PICTURE_B, 0);
        frames[index].pts = index;
        frames[index].pts_seconds = index / 30.0;
    }
    set_edge(&edge, 0, 2.0);
    frames[1].edges = &edge;
    frames[1].edge_count = 1;
    frames[2].measured.valid = 1;
    frames[2].measured.temporal_normalized = 1;
    frames[2].measured.dx = 3.0;
    mvstab_build_timeline(frames, 3, MVSTAB_MODE_SAFE);
    CHECK(fabs(frames[1].output.dx - 2.0) < 1e-7);
    CHECK(fabs(frames[2].output.dx - 3.0) < 1e-7);
    return 0;
}

static int test_dense_adjacent_graph_ignores_distant_conflict(void) {
    MvstabTimelineFrame frames[5];
    MvstabMotionEdge edges[5];
    for (int index = 0; index < 5; ++index) {
        initialize_frame(&frames[index], MVSTAB_PICTURE_P, 0);
        frames[index].pts = index;
        frames[index].pts_seconds = index / 30.0;
        if (index > 0) {
            set_edge(&edges[index - 1], index - 1, 2.0);
            frames[index].edges = &edges[index - 1];
            frames[index].edge_count = 1;
        }
    }
    set_edge(&edges[4], 0, 100.0);
    frames[4].edges = &edges[3];
    frames[4].edge_count = 2;
    mvstab_build_timeline(frames, 5, MVSTAB_MODE_SAFE);
    for (int index = 1; index < 5; ++index) {
        CHECK(fabs(frames[index].output.dx - 2.0) < 1e-7);
    }
    return 0;
}

static int test_clustered_adjacent_edges_keep_distant_coverage(void) {
    MvstabTimelineFrame frames[40];
    MvstabMotionEdge edges[39];
    for (int index = 0; index < 40; ++index) {
        initialize_frame(&frames[index], MVSTAB_PICTURE_P, 0);
        frames[index].pts = index;
        frames[index].pts_seconds = index / 30.0;
        if (index == 0) {
            continue;
        }
        set_edge(&edges[index - 1], index <= 10 ? index - 1 : 0,
                 index <= 10 ? 2.0 : 2.0 * index);
        frames[index].edges = &edges[index - 1];
        frames[index].edge_count = 1;
        frames[index].measured.valid = 1;
        frames[index].measured.temporal_normalized = 1;
        frames[index].measured.dx = 2.0;
    }
    mvstab_build_timeline(frames, 40, MVSTAB_MODE_SAFE);
    for (int index = 1; index < 40; ++index) {
        CHECK(fabs(frames[index].output.dx - 2.0) < 1e-6);
    }
    return 0;
}

static int test_pose_graph_defers_to_legacy_timeline(void) {
    MvstabTimelineFrame frames[3];
    MvstabMotionEdge edge;
    initialize_frame(&frames[0], MVSTAB_PICTURE_I, 1);
    initialize_frame(&frames[1], MVSTAB_PICTURE_B, 0);
    initialize_frame(&frames[2], MVSTAB_PICTURE_P, 0);
    set_edge(&edge, 0, 20.0);
    frames[1].edges = &edge;
    frames[1].edge_count = 1;
    frames[2].measured.valid = 1;
    frames[2].measured.dx = 4.0;
    mvstab_build_timeline(frames, 3, MVSTAB_MODE_SAFE);
    CHECK(fabs(frames[1].output.dx - 2.0) < 1e-9);
    CHECK(fabs(frames[2].output.dx - 2.0) < 1e-9);
    return 0;
}

static void set_temporal_cell_edge(
    MvstabMotionEdge *edge,
    int64_t reference_pts,
    double background,
    double interval_scale
) {
    set_edge(edge, reference_pts, background + 0.5 * interval_scale);
    edge->grid_columns = 8;
    edge->grid_rows = 4;
    edge->cell_count = 32;
    for (int index = 0; index < 32; ++index) {
        MvstabMotionCell *cell = &edge->cells[index];
        cell->grid_index = (uint16_t)index;
        cell->x = (float)((index % 8) * 10 - 35);
        cell->y = (float)((index / 8) * 10 - 15);
        cell->dx = (float)(index == 10 || index == 11
            ? background + 6.0 * interval_scale : background);
        cell->dy = 0.0f;
        cell->weight = 1.0f;
        cell->vector_count = 4;
    }
}

static void add_cell_rotation(MvstabMotionEdge *edge, double theta) {
    edge->motion.theta = theta;
    for (uint16_t index = 0; index < edge->cell_count; ++index) {
        MvstabMotionCell *cell = &edge->cells[index];
        cell->dx -= (float)(theta * cell->y);
        cell->dy += (float)(theta * cell->x);
    }
}

static int test_temporal_cells_reject_foreground_across_keyframe(void) {
    MvstabTimelineFrame frames[17];
    MvstabMotionEdge edges[15];
    int edge_index = 0;
    for (int index = 0; index < 17; ++index) {
        initialize_frame(&frames[index], index == 8 ? MVSTAB_PICTURE_I
                                                    : MVSTAB_PICTURE_P,
                         index == 8);
        frames[index].pts = index;
        frames[index].pts_seconds = index / 30.0;
        if (index == 0 || index == 8) {
            continue;
        }
        set_temporal_cell_edge(&edges[edge_index], index - 1, 2.0, 1.0);
        frames[index].edges = &edges[edge_index++];
        frames[index].edge_count = 1;
    }
    mvstab_build_timeline(frames, 17, MVSTAB_MODE_SAFE);
    for (int index = 1; index < 17; ++index) {
        CHECK(fabs(frames[index].output.dx - 2.0) < 1e-6);
    }
    CHECK(edges[0].motion.confidence < 1.0);
    return 0;
}

static int test_temporal_history_stops_at_motion_cut(void) {
    MvstabTimelineFrame frames[17];
    MvstabMotionEdge edges[15];
    int edge_index = 0;
    for (int index = 0; index < 17; ++index) {
        initialize_frame(&frames[index], index == 8 ? MVSTAB_PICTURE_I
                                                    : MVSTAB_PICTURE_P,
                         index == 8);
        frames[index].pts = index;
        frames[index].pts_seconds = index / 30.0;
        if (index == 0 || index == 8) {
            continue;
        }
        set_temporal_cell_edge(&edges[edge_index], index - 1,
                               index < 8 ? 2.0 : 20.0, 1.0);
        frames[index].edges = &edges[edge_index++];
        frames[index].edge_count = 1;
    }
    mvstab_build_timeline(frames, 17, MVSTAB_MODE_SAFE);
    CHECK(fabs(frames[1].output.dx - 2.5) < 1e-6);
    CHECK(frames[8].output.dx == 0.0);
    CHECK(fabs(frames[9].output.dx - 20.5) < 1e-6);
    return 0;
}

static int test_temporal_history_stops_at_rotation_cut(void) {
    MvstabTimelineFrame frames[17];
    MvstabMotionEdge edges[15];
    int edge_index = 0;
    for (int index = 0; index < 17; ++index) {
        initialize_frame(&frames[index], index == 8 ? MVSTAB_PICTURE_I
                                                    : MVSTAB_PICTURE_P,
                         index == 8);
        frames[index].pts = index;
        frames[index].pts_seconds = index / 30.0;
        if (index == 0 || index == 8) {
            continue;
        }
        set_temporal_cell_edge(&edges[edge_index], index - 1, 2.0, 1.0);
        if (index > 8) {
            add_cell_rotation(&edges[edge_index], 0.05);
        }
        frames[index].edges = &edges[edge_index++];
        frames[index].edge_count = 1;
    }
    mvstab_build_timeline(frames, 17, MVSTAB_MODE_SAFE);
    CHECK(fabs(frames[1].output.dx - 2.5) < 1e-6);
    CHECK(frames[8].output.dx == 0.0);
    CHECK(frames[8].output.theta == 0.0);
    CHECK(fabs(frames[9].output.dx - 2.5) < 1e-6);
    CHECK(fabs(frames[9].output.theta - 0.05) < 1e-6);
    return 0;
}

static int test_temporal_history_uses_exact_time(void) {
    MvstabTimelineFrame frames[17];
    MvstabMotionEdge edges[15];
    double intervals[17] = {0};
    int edge_index = 0;
    for (int index = 0; index < 17; ++index) {
        double scale = index == 0 ? 0.0 : (index % 2 == 0 ? 2.0 : 1.0);
        initialize_frame(&frames[index], index == 8 ? MVSTAB_PICTURE_I
                                                    : MVSTAB_PICTURE_P,
                         index == 8);
        intervals[index] = scale;
        frames[index].pts = index;
        frames[index].pts_seconds = index == 0 ? 0.0 :
            frames[index - 1].pts_seconds + scale / 30.0;
        if (index == 0 || index == 8) {
            continue;
        }
        set_temporal_cell_edge(&edges[edge_index], index - 1,
                               2.0 * scale, scale);
        frames[index].edges = &edges[edge_index++];
        frames[index].edge_count = 1;
    }
    mvstab_build_timeline(frames, 17, MVSTAB_MODE_SAFE);
    for (int index = 1; index < 17; ++index) {
        CHECK(fabs(frames[index].output.dx - 2.0 * intervals[index]) < 1e-6);
    }
    return 0;
}

static int test_temporal_refit_invalidates_unsupported_edge(void) {
    MvstabTimelineFrame frames[17];
    MvstabMotionEdge edges[30];
    int edge_index = 0;
    for (int index = 0; index < 17; ++index) {
        initialize_frame(&frames[index], index == 8 ? MVSTAB_PICTURE_I
                                                    : MVSTAB_PICTURE_P,
                         index == 8);
        frames[index].pts = index;
        frames[index].pts_seconds = index / 30.0;
        if (index == 0 || index == 8) {
            continue;
        }
        set_temporal_cell_edge(&edges[edge_index], index - 1, 2.0, 1.0);
        set_temporal_cell_edge(&edges[edge_index + 1], index - 1, 2.0, 1.0);
        edges[edge_index + 1].cell_count = 6;
        frames[index].edges = &edges[edge_index];
        frames[index].edge_count = 2;
        edge_index += 2;
    }
    mvstab_build_timeline(frames, 17, MVSTAB_MODE_SAFE);
    CHECK(edges[0].motion.valid);
    CHECK(!edges[1].motion.valid);
    for (int index = 1; index < 17; ++index) {
        CHECK(fabs(frames[index].output.dx - 2.0) < 1e-6);
    }
    return 0;
}

int main(void) {
    if (test_safe_mode_distributes_anchor_motion() != 0 ||
        test_invalid_p_frame_advances_anchor() != 0) {
        return 1;
    }
    if (test_all_mvs_uses_measurements_without_interpolation() != 0) {
        return 1;
    }
    if (test_first_frame_is_always_identity() != 0) {
        return 1;
    }
    if (test_safe_mode_uses_display_timestamps() != 0) {
        return 1;
    }
    if (test_periodic_keyframe_does_not_drop_motion() != 0 ||
        test_periodic_keyframe_fills_b_frame_tail() != 0) {
        return 1;
    }
    if (test_keyframe_fill_interpolates_velocity() != 0 ||
        test_keyframe_fill_stops_at_rejected_p_frame() != 0) {
        return 1;
    }
    if (test_all_mvs_does_not_repair_rejected_frames() != 0) {
        return 1;
    }
    if (test_exact_timing_uses_b_frames_and_repairs_keyframe() != 0) {
        return 1;
    }
    if (test_exact_timeline_rejects_legacy_measurement() != 0) {
        return 1;
    }
    if (test_invalid_exact_measurement_does_not_select_precise_mode() != 0) {
        return 1;
    }
    return test_pose_graph_recovers_adjacent_motion() != 0 ||
           test_pose_graph_does_not_bridge_components() != 0 ||
           test_pose_graph_does_not_smooth_edge_acceleration() != 0 ||
           test_pose_graph_weights_confident_edges() != 0 ||
           test_pose_graph_handles_varied_edge_weights() != 0 ||
           test_pose_graph_preserves_disconnected_measurement() != 0 ||
           test_dense_adjacent_graph_ignores_distant_conflict() != 0 ||
           test_clustered_adjacent_edges_keep_distant_coverage() != 0 ||
           test_pose_graph_defers_to_legacy_timeline() != 0 ||
           test_temporal_cells_reject_foreground_across_keyframe() != 0 ||
           test_temporal_history_stops_at_motion_cut() != 0 ||
           test_temporal_history_stops_at_rotation_cut() != 0 ||
           test_temporal_history_uses_exact_time() != 0 ||
           test_temporal_refit_invalidates_unsupported_edge() != 0;
}
