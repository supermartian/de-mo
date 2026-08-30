#ifndef MVSTAB_TIMELINE_H
#define MVSTAB_TIMELINE_H

#include <stddef.h>

#include "mvstab/estimator.h"

typedef struct {
    int64_t frame_index;
    int64_t pts;
    double pts_seconds;
    MvstabPictureType picture_type;
    int key_frame;
    FrameMotion measured;
    FrameMotion output;
    MvstabMotionEdge *edges;
    size_t edge_count;
} MvstabTimelineFrame;

void mvstab_build_timeline(
    MvstabTimelineFrame *frames,
    size_t frame_count,
    MvstabMode mode
);

#endif
