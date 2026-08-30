#ifndef MVSTAB_TIMELINE_H
#define MVSTAB_TIMELINE_H

#include <stddef.h>

#include "mvstab/estimator.h"

typedef struct {
    int64_t frame_index;
    double pts_seconds;
    MvstabPictureType picture_type;
    int key_frame;
    FrameMotion measured;
    FrameMotion output;
} MvstabTimelineFrame;

void mvstab_build_timeline(
    MvstabTimelineFrame *frames,
    size_t frame_count,
    MvstabMode mode
);

#endif
