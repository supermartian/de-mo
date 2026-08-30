#ifndef MVSTAB_MOTION_VECTOR_H
#define MVSTAB_MOTION_VECTOR_H

#include <stdint.h>

typedef struct {
    double x;
    double y;
    double dx;
    double dy;
    double weight;
    int width;
    int height;
    int source_x;
    int source_y;
    int destination_x;
    int destination_y;
    int motion_x;
    int motion_y;
    unsigned int motion_scale;
    int reference_direction;
    uint64_t codec_flags;
} MvstabVector;

int mvstab_normalize_vector(
    MvstabVector *vector,
    int source_x,
    int source_y,
    int destination_x,
    int destination_y,
    int block_width,
    int block_height,
    int motion_x,
    int motion_y,
    unsigned int motion_scale,
    int reference_direction,
    uint64_t codec_flags
);

#endif
