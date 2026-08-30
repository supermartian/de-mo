#include "writers.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int write_csv_header(FILE *file) {
    return fprintf(file,
        "frame_index,pts_seconds,pict_type,source,dst_x,dst_y,src_x,src_y,"
        "block_w,block_h,motion_x,motion_y,motion_scale,"
        "mv_ref_to_cur_x,mv_ref_to_cur_y,reference_poc_delta,reference_index,"
        "reference_list,reference_exact,reference_pts_delta,reference_delta_seconds,"
        "long_term,top_field,bottom_field,direct,skip,interlaced,flags\n") < 0 ? -1 : 0;
}

static int write_csv_vector(
    FILE *file,
    const MvstabFrame *frame,
    const MvstabVector *vector
) {
    int written = fprintf(file,
        "%" PRId64 ",%.9f,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%.9f,%.9f,"
        "%" PRId32 ",%d,%d,%d,%" PRId64 ",%.9f,%d,%d,%d,%d,%d,%d,0x%" PRIx64 "\n",
        frame->display_index, frame->pts_seconds,
        mvstab_picture_type_name(frame->picture_type), vector->reference_direction,
        vector->destination_x, vector->destination_y,
        vector->source_x, vector->source_y, vector->width, vector->height,
        vector->motion_x, vector->motion_y, vector->motion_scale,
        vector->dx, vector->dy, vector->reference_poc_delta,
        vector->reference_index, vector->reference_list, vector->reference_exact,
        vector->reference_pts_delta, vector->reference_delta_seconds,
        vector->reference_long_term, vector->reference_top_field,
        vector->reference_bottom_field, vector->prediction_direct,
        vector->prediction_skip, vector->prediction_interlaced,
        vector->codec_flags);
    return written < 0 ? -1 : 0;
}

static int write_stats_row(FILE *file, const MvstabTimelineFrame *frame) {
    const FrameMotion *motion = &frame->output;
    int written = fprintf(file, "%" PRId64 ",%.9f,%s,%.9f,%.9f,%.9f,%.9f,%.6f,%.6f,"
            "%.9f,%.9f,%.6f,%d,%d,%d,%d,%d,%d,%.9f,%.9f,%d,%.6f,%d\n",
            frame->frame_index, frame->pts_seconds,
            mvstab_picture_type_name(frame->picture_type),
            motion->dx, motion->dy, motion->theta, motion->scale,
            motion->confidence,
            motion->inlier_weight_ratio, motion->residual_median,
            motion->residual_p95, motion->spatial_coverage,
            motion->vector_count, motion->inlier_count, motion->valid,
            frame->key_frame, motion->scene_cut, motion->interpolated,
            frame->measured.dx, frame->measured.dy, frame->measured.valid,
            motion->reference_agreement, motion->temporal_normalized);
    return written < 0 ? -1 : 0;
}

int mvstab_write_csv_frame(MvstabRawWriter *writer, const MvstabFrame *frame) {
    size_t index;
    for (index = 0; index < frame->vector_count; ++index) {
        if (write_csv_vector(writer->file, frame, &frame->vectors[index]) != 0) {
            return -1;
        }
    }
    return 0;
}

int mvstab_write_csv_start(MvstabRawWriter *writer) {
    return write_csv_header(writer->file);
}

int mvstab_write_stats_csv_file(
    const char *path,
    const MvstabTimelineFrame *frames,
    size_t frame_count,
    char *error,
    size_t error_size
) {
    FILE *file;
    size_t index;

    if (path == NULL) {
        return 0;
    }
    file = fopen(path, "w");
    if (file == NULL) {
        snprintf(error, error_size, "cannot open stats file '%s': %s", path, strerror(errno));
        return -1;
    }
    if (fprintf(file, "frame_index,pts_seconds,pict_type,dx,dy,theta,scale,confidence,"
                      "inlier_weight_ratio,residual_median,residual_p95,spatial_coverage,"
                      "vector_count,inlier_count,valid,key_frame,scene_cut,interpolated,"
                      "measured_dx,measured_dy,measured_valid,reference_agreement,"
                      "temporal_normalized\n") < 0) {
        snprintf(error, error_size, "cannot write stats file '%s'", path);
        fclose(file);
        return -1;
    }
    for (index = 0; index < frame_count; ++index) {
        if (write_stats_row(file, &frames[index]) != 0) {
            snprintf(error, error_size, "cannot write stats file '%s'", path);
            fclose(file);
            return -1;
        }
    }
    if (fclose(file) != 0) {
        snprintf(error, error_size, "cannot finish stats file '%s'", path);
        return -1;
    }
    return 0;
}
