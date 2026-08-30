#include "mvstab/motion_vector.h"
#include "mvstab/frame_motion.h"

#include <stddef.h>

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
) {
    if (vector == NULL || motion_scale == 0 || block_width <= 0 || block_height <= 0) {
        return -1;
    }

    vector->x = destination_x;
    vector->y = destination_y;
    vector->dx = -(double)motion_x / motion_scale;
    vector->dy = -(double)motion_y / motion_scale;
    vector->weight = (double)block_width * block_height;
    vector->width = block_width;
    vector->height = block_height;
    vector->source_x = source_x;
    vector->source_y = source_y;
    vector->destination_x = destination_x;
    vector->destination_y = destination_y;
    vector->motion_x = motion_x;
    vector->motion_y = motion_y;
    vector->motion_scale = motion_scale;
    vector->reference_direction = reference_direction;
    vector->codec_flags = codec_flags;
    return 0;
}

const char *mvstab_picture_type_name(MvstabPictureType picture_type) {
    switch (picture_type) {
        case MVSTAB_PICTURE_I:
            return "I";
        case MVSTAB_PICTURE_P:
            return "P";
        case MVSTAB_PICTURE_B:
            return "B";
        default:
            return "?";
    }
}
