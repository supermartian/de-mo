#include "mvstab/timeline.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "internal.h"

static FrameMotion identity_motion(int scene_cut) {
    FrameMotion motion;
    memset(&motion, 0, sizeof(motion));
    motion.valid = 1;
    motion.scene_cut = scene_cut;
    return motion;
}

static int timestamps_are_usable(
    const MvstabTimelineFrame *frames,
    size_t previous_anchor,
    size_t anchor
) {
    size_t index;
    double duration = frames[anchor].pts_seconds - frames[previous_anchor].pts_seconds;
    if (!isfinite(duration) || duration <= 0.0) {
        return 0;
    }
    for (index = previous_anchor + 1; index <= anchor; ++index) {
        double frame_duration = frames[index].pts_seconds - frames[index - 1].pts_seconds;
        if (!isfinite(frame_duration) || frame_duration <= 0.0) {
            return 0;
        }
    }
    return 1;
}

static void distribute_anchor(
    MvstabTimelineFrame *frames,
    size_t previous_anchor,
    size_t anchor
) {
    FrameMotion divided = frames[anchor].measured;
    size_t interval = anchor - previous_anchor;
    size_t index;
    double duration = frames[anchor].pts_seconds - frames[previous_anchor].pts_seconds;
    int use_timestamps = timestamps_are_usable(frames, previous_anchor, anchor);

    divided.interpolated = interval > 1;
    for (index = previous_anchor + 1; index <= anchor; ++index) {
        double frame_duration = frames[index].pts_seconds - frames[index - 1].pts_seconds;
        double fraction = 1.0 / interval;
        if (use_timestamps && isfinite(frame_duration) && frame_duration > 0.0) {
            fraction = frame_duration / duration;
        }
        divided.dx = frames[anchor].measured.dx * fraction;
        divided.dy = frames[anchor].measured.dy * fraction;
        divided.theta = frames[anchor].measured.theta * fraction;
        divided.scale = frames[anchor].measured.scale * fraction;
        frames[index].output = divided;
    }
}

static void build_safe_timeline(MvstabTimelineFrame *frames, size_t frame_count) {
    size_t previous_anchor = 0;
    size_t index;

    for (index = 0; index < frame_count; ++index) {
        frames[index].output = identity_motion(0);
        if (frames[index].key_frame) {
            previous_anchor = index;
            continue;
        }
        if (frames[index].picture_type != MVSTAB_PICTURE_P) {
            continue;
        }
        if (frames[index].measured.valid && index > previous_anchor) {
            distribute_anchor(frames, previous_anchor, index);
        }
        previous_anchor = index;
    }
}

static int frame_has_motion(const MvstabTimelineFrame *frame) {
    return frame->measured.valid || frame->output.interpolated;
}

static int interval_duration(
    const MvstabTimelineFrame *frames,
    size_t index,
    double *duration
);

static int motions_are_continuous(
    const MvstabTimelineFrame *frames,
    size_t left,
    size_t right
) {
    double left_dt = 0.0;
    double right_dt = 0.0;
    double left_dx = frames[left].output.dx;
    double left_dy = frames[left].output.dy;
    double right_dx = frames[right].output.dx;
    double right_dy = frames[right].output.dy;
    double scale;
    double theta_limit;
    double zoom_limit;

    if (interval_duration(frames, left, &left_dt) &&
        interval_duration(frames, right, &right_dt)) {
        left_dx /= left_dt;
        left_dy /= left_dt;
        right_dx /= right_dt;
        right_dy /= right_dt;
    }
    scale = fmax(hypot(left_dx, left_dy), hypot(right_dx, right_dy));
    theta_limit = 0.01 + 0.25 * fmax(fabs(frames[left].output.theta),
                                     fabs(frames[right].output.theta));
    zoom_limit = 0.02 + 0.25 * fmax(fabs(frames[left].output.scale),
                                    fabs(frames[right].output.scale));
    return hypot(left_dx - right_dx, left_dy - right_dy) <= 2.0 + 0.25 * scale &&
           fabs(frames[left].output.theta - frames[right].output.theta) <=
               theta_limit &&
           fabs(frames[left].output.scale - frames[right].output.scale) <= zoom_limit;
}

static int interval_duration(
    const MvstabTimelineFrame *frames,
    size_t index,
    double *duration
) {
    if (index == 0) {
        return 0;
    }
    *duration = frames[index].pts_seconds - frames[index - 1].pts_seconds;
    return isfinite(*duration) && *duration > 0.0;
}

static FrameMotion interpolate_gap_motion(
    const MvstabTimelineFrame *frames,
    size_t target,
    size_t left,
    int has_left,
    size_t right,
    int has_right
) {
    const FrameMotion *a = has_left ? &frames[left].output : &frames[right].output;
    const FrameMotion *b = has_right ? &frames[right].output : a;
    FrameMotion motion = *a;
    double target_dt = 0.0;
    double left_dt = 0.0;
    double right_dt = 0.0;
    double alpha = 0.5;
    int timed = interval_duration(frames, target, &target_dt) &&
                (!has_left || interval_duration(frames, left, &left_dt)) &&
                (!has_right || interval_duration(frames, right, &right_dt));

    if (has_left && has_right &&
        frames[right].pts_seconds > frames[left].pts_seconds) {
        alpha = (frames[target].pts_seconds - frames[left].pts_seconds) /
                (frames[right].pts_seconds - frames[left].pts_seconds);
        alpha = fmax(0.0, fmin(1.0, alpha));
    }
    if (timed) {
        double avx = a->dx / (has_left ? left_dt : right_dt);
        double avy = a->dy / (has_left ? left_dt : right_dt);
        double bvx = b->dx / (has_right ? right_dt : left_dt);
        double bvy = b->dy / (has_right ? right_dt : left_dt);
        motion.dx = (avx + alpha * (bvx - avx)) * target_dt;
        motion.dy = (avy + alpha * (bvy - avy)) * target_dt;
    } else {
        motion.dx = a->dx + alpha * (b->dx - a->dx);
        motion.dy = a->dy + alpha * (b->dy - a->dy);
    }
    motion.theta = a->theta + alpha * (b->theta - a->theta);
    motion.scale = a->scale + alpha * (b->scale - a->scale);
    motion.confidence = fmin(a->confidence, b->confidence);
    motion.valid = 1;
    motion.scene_cut = 0;
    motion.interpolated = 1;
    return motion;
}

static void fill_keyframe_gap(
    MvstabTimelineFrame *frames,
    size_t frame_count,
    size_t keyframe
) {
    size_t start = keyframe;
    size_t right = keyframe + 1;
    size_t index;
    int has_left;
    int has_right;

    while (start > 1 &&
           frames[start - 1].picture_type == MVSTAB_PICTURE_B &&
           !frame_has_motion(&frames[start - 1])) {
        --start;
    }
    while (right < frame_count &&
           frames[right].picture_type == MVSTAB_PICTURE_B &&
           !frame_has_motion(&frames[right])) {
        ++right;
    }
    has_left = start > 0 && frame_has_motion(&frames[start - 1]);
    has_right = right < frame_count && frame_has_motion(&frames[right]);
    if (!has_left || !has_right ||
        !motions_are_continuous(frames, start - 1, right)) {
        frames[keyframe].output = identity_motion(0);
        return;
    }
    for (index = start; index <= keyframe; ++index) {
        frames[index].output = interpolate_gap_motion(
            frames, index, start - 1, has_left, right, has_right);
    }
}

static void fill_keyframe_motion(
    MvstabTimelineFrame *frames,
    size_t frame_count
) {
    size_t index;
    for (index = 1; index < frame_count; ++index) {
        if (frames[index].key_frame) {
            fill_keyframe_gap(frames, frame_count, index);
        }
    }
}

static int has_temporally_normalized_motion(
    const MvstabTimelineFrame *frames,
    size_t frame_count
) {
    for (size_t index = 0; index < frame_count; ++index) {
        if (frames[index].measured.valid &&
            frames[index].measured.temporal_normalized) {
            return 1;
        }
    }
    return 0;
}

static int precise_measurement_is_valid(const MvstabTimelineFrame *frame) {
    return frame->measured.valid && frame->measured.temporal_normalized;
}

static int has_legacy_valid_motion(
    const MvstabTimelineFrame *frames,
    size_t frame_count
) {
    for (size_t index = 0; index < frame_count; ++index) {
        if (frames[index].measured.valid &&
            !frames[index].measured.temporal_normalized) {
            return 1;
        }
    }
    return 0;
}

static void build_precise_timeline(
    MvstabTimelineFrame *frames,
    size_t frame_count
) {
    for (size_t index = 0; index < frame_count; ++index) {
        if (index == 0 || !precise_measurement_is_valid(&frames[index])) {
            frames[index].output = identity_motion(0);
        } else {
            frames[index].output = frames[index].measured;
        }
    }
    for (size_t start = 1; start + 1 < frame_count;) {
        size_t right = start;
        if (precise_measurement_is_valid(&frames[start])) {
            ++start;
            continue;
        }
        while (right < frame_count &&
               !precise_measurement_is_valid(&frames[right])) {
            ++right;
        }
        if (right < frame_count && right - start <= 3 &&
            precise_measurement_is_valid(&frames[start - 1]) &&
            motions_are_continuous(frames, start - 1, right)) {
            for (size_t index = start; index < right; ++index) {
                frames[index].output = interpolate_gap_motion(
                    frames, index, start - 1, 1, right, 1);
            }
        }
        start = right;
    }
}

int mvstab_build_timeline(
    MvstabTimelineFrame *frames,
    size_t frame_count,
    MvstabMode mode
) {
    int graph_result;
    size_t index;

    if (frames == NULL || frame_count == 0) {
        return 0;
    }
    if (!has_legacy_valid_motion(frames, frame_count)) {
        graph_result = mvstab_build_pose_graph_timeline(frames, frame_count);
        if (graph_result < 0) {
            return -1;
        }
        if (graph_result > 0) {
            fill_keyframe_motion(frames, frame_count);
            return 0;
        }
    }
    if (has_temporally_normalized_motion(frames, frame_count)) {
        build_precise_timeline(frames, frame_count);
    } else if (mode == MVSTAB_MODE_SAFE) {
        build_safe_timeline(frames, frame_count);
    } else {
        for (index = 0; index < frame_count; ++index) {
            if (index == 0 || frames[index].key_frame || !frames[index].measured.valid) {
                frames[index].output = identity_motion(0);
            } else {
                frames[index].output = frames[index].measured;
            }
        }
    }
    if (mode == MVSTAB_MODE_SAFE &&
        !has_temporally_normalized_motion(frames, frame_count)) {
        fill_keyframe_motion(frames, frame_count);
    }
    return 0;
}
