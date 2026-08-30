#include "writers.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int mvstab_frame_motion_is_finite(const FrameMotion *motion) {
    return isfinite(motion->dx) && isfinite(motion->dy) &&
           isfinite(motion->theta) && isfinite(motion->confidence) &&
           isfinite(motion->inlier_weight_ratio) &&
           isfinite(motion->residual_median) && isfinite(motion->residual_p95) &&
           isfinite(motion->spatial_coverage) &&
           isfinite(motion->reference_agreement);
}

int mvstab_write_transform_file(
    const char *path,
    const MvstabTimelineFrame *frames,
    size_t frame_count,
    char *error,
    size_t error_size
) {
    FILE *file;
    size_t index;

    for (index = 0; index < frame_count; ++index) {
        if (!mvstab_frame_motion_is_finite(&frames[index].output)) {
            snprintf(error, error_size, "cannot write non-finite frame transform");
            return -1;
        }
    }
    file = fopen(path, "w");
    if (file == NULL) {
        snprintf(error, error_size, "cannot open transform file '%s': %s",
                 path, strerror(errno));
        return -1;
    }
    for (index = 0; index < frame_count; ++index) {
        const FrameMotion *motion = &frames[index].output;
        if (fprintf(file, "0 %.9f %.9f 0.000000000 0.000000000 0\n",
                    -motion->dx, -motion->dy) < 0) {
            snprintf(error, error_size, "cannot write transform file '%s'", path);
            fclose(file);
            return -1;
        }
    }
    if (fclose(file) != 0) {
        snprintf(error, error_size, "cannot finish transform file '%s'", path);
        return -1;
    }
    return 0;
}
