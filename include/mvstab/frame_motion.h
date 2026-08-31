#ifndef MVSTAB_FRAME_MOTION_H
#define MVSTAB_FRAME_MOTION_H

#include <stddef.h>
#include <stdint.h>

#include "mvstab/motion_vector.h"

typedef enum {
    MVSTAB_PICTURE_UNKNOWN,
    MVSTAB_PICTURE_I,
    MVSTAB_PICTURE_P,
    MVSTAB_PICTURE_B
} MvstabPictureType;

typedef struct {
    int64_t decode_index;
    int64_t display_index;
    int64_t pts;
    double pts_seconds;
    double duration_seconds;
    MvstabPictureType picture_type;
    int key_frame;
    int width;
    int height;
    MvstabVector *vectors;
    size_t vector_count;
} MvstabFrame;

typedef struct {
    double dx;
    double dy;
    double theta;
    double scale;
    double confidence;
    double inlier_weight_ratio;
    double residual_median;
    double residual_p95;
    double spatial_coverage;
    double reference_agreement;
    int vector_count;
    int inlier_count;
    int valid;
    int scene_cut;
    int interpolated;
    int temporal_normalized;
} FrameMotion;

#define MVSTAB_MAX_MOTION_CELLS 64

/* Compact inlier summary; x/y are centered image coordinates in pixels. */
typedef struct {
    float x;
    float y;
    float dx;
    float dy;
    float weight;
    uint16_t vector_count;
    uint16_t grid_index;
} MvstabMotionCell;

typedef struct {
    int64_t reference_pts;
    double reference_pts_seconds;
    FrameMotion motion;
    MvstabMotionCell cells[MVSTAB_MAX_MOTION_CELLS];
    size_t cell_count;
    uint16_t grid_columns;
    uint16_t grid_rows;
} MvstabMotionEdge;

const char *mvstab_picture_type_name(MvstabPictureType picture_type);

#endif
