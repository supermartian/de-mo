#include <math.h>
#include <stdio.h>
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
    return test_all_mvs_does_not_repair_rejected_frames();
}
