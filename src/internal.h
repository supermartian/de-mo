#ifndef MVSTAB_INTERNAL_H
#define MVSTAB_INTERNAL_H

#include <stddef.h>

#include "mvstab/timeline.h"

typedef struct {
    const MvstabVector *vector;
    double dx;
    double dy;
    double weight;
    double base_weight;
    double residual;
    int inlier;
} MvstabCandidate;

double mvstab_weighted_median(
    MvstabCandidate *candidates,
    size_t count,
    int use_y
);

void mvstab_compute_confidence(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    MvstabCandidate *candidates,
    size_t count,
    double total_candidate_weight,
    FrameMotion *motion
);

int mvstab_build_pose_graph_timeline(
    MvstabTimelineFrame *frames,
    size_t frame_count
);

#endif
