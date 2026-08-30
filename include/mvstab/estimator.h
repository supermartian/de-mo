#ifndef MVSTAB_ESTIMATOR_H
#define MVSTAB_ESTIMATOR_H

#include "mvstab/frame_motion.h"

typedef enum {
    MVSTAB_MODE_SAFE,
    MVSTAB_MODE_ALL_MVS
} MvstabMode;

typedef struct {
    double residual_threshold_px;
    double max_mv_px;
    double min_confidence;
    double min_spatial_coverage;
    double block_area_cap;
    double mad_threshold;
    int grid_columns;
    int grid_rows;
    MvstabMode mode;
} MvstabEstimatorConfig;

MvstabEstimatorConfig mvstab_default_estimator_config(void);

int mvstab_estimate_frame(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    FrameMotion *motion
);

/* Allocates exact-reference edges; the caller owns and must free *edges. */
int mvstab_estimate_frame_edges(
    const MvstabFrame *frame,
    const MvstabEstimatorConfig *config,
    MvstabMotionEdge **edges,
    size_t *edge_count
);

#endif
