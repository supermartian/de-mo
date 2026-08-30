#include "writers.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int mvstab_write_csv_start(MvstabRawWriter *writer);
int mvstab_write_csv_frame(MvstabRawWriter *writer, const MvstabFrame *frame);
int mvstab_write_stats_csv_file(
    const char *path,
    const MvstabTimelineFrame *frames,
    size_t frame_count,
    char *error,
    size_t error_size
);

static void format_json_double(double value, char *output, size_t output_size) {
    if (isfinite(value)) {
        snprintf(output, output_size, "%.9f", value);
    } else {
        snprintf(output, output_size, "null");
    }
}

static int write_json_vector(
    MvstabRawWriter *writer,
    const MvstabFrame *frame,
    const MvstabVector *vector
) {
    char pts[32];
    char dx[32];
    char dy[32];
    int written;
    format_json_double(frame->pts_seconds, pts, sizeof(pts));
    format_json_double(vector->dx, dx, sizeof(dx));
    format_json_double(vector->dy, dy, sizeof(dy));
    written = fprintf(writer->file,
        "%s{\"frame_index\":%" PRId64 ",\"pts_seconds\":%s,"
        "\"pict_type\":\"%s\",\"source\":%d,\"dst_x\":%d,\"dst_y\":%d,"
        "\"src_x\":%d,\"src_y\":%d,\"block_w\":%d,\"block_h\":%d,"
        "\"motion_x\":%d,\"motion_y\":%d,\"motion_scale\":%u,"
        "\"mv_ref_to_cur_x\":%s,\"mv_ref_to_cur_y\":%s,"
        "\"flags\":\"0x%" PRIx64 "\"}",
        writer->first_json_item ? "" : ",\n",
        frame->display_index, pts,
        mvstab_picture_type_name(frame->picture_type), vector->reference_direction,
        vector->destination_x, vector->destination_y,
        vector->source_x, vector->source_y, vector->width, vector->height,
        vector->motion_x, vector->motion_y, vector->motion_scale,
        dx, dy, vector->codec_flags);
    writer->first_json_item = 0;
    return written < 0 ? -1 : 0;
}

static int write_stats_json_row(
    FILE *file,
    const MvstabTimelineFrame *frame,
    int first
) {
    const FrameMotion *motion = &frame->output;
    char pts[32];
    format_json_double(frame->pts_seconds, pts, sizeof(pts));
    return fprintf(file,
        "%s{\"frame_index\":%" PRId64 ",\"pts_seconds\":%s,\"pict_type\":\"%s\","
        "\"dx\":%.9f,\"dy\":%.9f,\"theta\":%.9f,\"confidence\":%.6f,"
        "\"inlier_weight_ratio\":%.6f,\"residual_median\":%.9f,"
        "\"residual_p95\":%.9f,\"spatial_coverage\":%.6f,\"vector_count\":%d,"
        "\"inlier_count\":%d,\"valid\":%s,\"key_frame\":%s,\"scene_cut\":%s,"
        "\"interpolated\":%s,\"measured_dx\":%.9f,\"measured_dy\":%.9f,"
        "\"measured_valid\":%s,\"reference_agreement\":%.6f}",
        first ? "" : ",\n", frame->frame_index, pts,
        mvstab_picture_type_name(frame->picture_type),
        motion->dx, motion->dy, motion->theta, motion->confidence,
        motion->inlier_weight_ratio, motion->residual_median, motion->residual_p95,
        motion->spatial_coverage, motion->vector_count, motion->inlier_count,
        motion->valid ? "true" : "false", frame->key_frame ? "true" : "false",
        motion->scene_cut ? "true" : "false",
        motion->interpolated ? "true" : "false",
        frame->measured.dx, frame->measured.dy,
        frame->measured.valid ? "true" : "false",
        motion->reference_agreement) < 0 ? -1 : 0;
}

static int write_stats_json_file(
    const char *path,
    const MvstabTimelineFrame *frames,
    size_t frame_count,
    char *error,
    size_t error_size
) {
    FILE *file = fopen(path, "w");
    size_t index;
    int failed = file == NULL;

    for (index = 0; !failed && index < frame_count; ++index) {
        if ((index == 0 && fprintf(file, "[\n") < 0) ||
            write_stats_json_row(file, &frames[index], index == 0) != 0) {
            failed = 1;
        }
    }
    if (!failed && frame_count == 0 && fprintf(file, "[") < 0) {
        failed = 1;
    }
    if (!failed && fprintf(file, "\n]\n") < 0) {
        failed = 1;
    }
    if (file != NULL && fclose(file) != 0) {
        failed = 1;
    }
    if (failed) {
        snprintf(error, error_size, "cannot write stats file '%s': %s",
                 path, file == NULL ? strerror(errno) : "output error");
        return -1;
    }
    return 0;
}

int mvstab_write_stats_file(
    const char *path,
    MvstabDumpFormat format,
    const MvstabTimelineFrame *frames,
    size_t frame_count,
    char *error,
    size_t error_size
) {
    size_t index;
    if (path == NULL) {
        return 0;
    }
    for (index = 0; index < frame_count; ++index) {
        if (!mvstab_frame_motion_is_finite(&frames[index].output) ||
            !mvstab_frame_motion_is_finite(&frames[index].measured)) {
            snprintf(error, error_size, "cannot write non-finite frame statistics");
            return -1;
        }
    }
    if (format == MVSTAB_DUMP_JSON) {
        return write_stats_json_file(path, frames, frame_count, error, error_size);
    }
    return mvstab_write_stats_csv_file(path, frames, frame_count, error, error_size);
}

static int write_json_frame(MvstabRawWriter *writer, const MvstabFrame *frame) {
    size_t index;
    for (index = 0; index < frame->vector_count; ++index) {
        if (write_json_vector(writer, frame, &frame->vectors[index]) != 0) {
            return -1;
        }
    }
    return 0;
}

int mvstab_raw_writer_start(
    MvstabRawWriter *writer,
    FILE *file,
    MvstabDumpFormat format,
    char *error,
    size_t error_size
) {
    int start_result;
    if (writer == NULL || file == NULL) {
        snprintf(error, error_size, "invalid motion-vector output stream");
        return -1;
    }
    memset(writer, 0, sizeof(*writer));
    writer->file = file;
    writer->format = format;
    writer->first_json_item = 1;
    if (format == MVSTAB_DUMP_JSON) {
        start_result = fprintf(writer->file, "[\n") < 0 ? -1 : 0;
    } else {
        start_result = mvstab_write_csv_start(writer);
    }
    if (start_result != 0) {
        snprintf(error, error_size, "cannot start motion-vector output");
    }
    return start_result;
}

int mvstab_raw_writer_open(
    MvstabRawWriter *writer,
    const char *path,
    MvstabDumpFormat format,
    char *error,
    size_t error_size
) {
    FILE *file;
    int owns_file = path != NULL && strcmp(path, "-") != 0;
    int result;

    if (path == NULL || strcmp(path, "-") == 0) {
        file = stdout;
    } else {
        file = fopen(path, "w");
    }
    if (file == NULL) {
        snprintf(error, error_size, "cannot open output '%s': %s", path, strerror(errno));
        return -1;
    }
    result = mvstab_raw_writer_start(writer, file, format, error, error_size);
    if (result != 0 && owns_file) {
        fclose(file);
    } else {
        writer->owns_file = owns_file;
    }
    return result;
}

int mvstab_raw_writer_write(MvstabRawWriter *writer, const MvstabFrame *frame) {
    if (writer->format == MVSTAB_DUMP_JSON) {
        return write_json_frame(writer, frame);
    }
    return mvstab_write_csv_frame(writer, frame);
}

int mvstab_raw_writer_close(MvstabRawWriter *writer) {
    int result = 0;
    if (writer->file == NULL) {
        return 0;
    }
    if (writer->format == MVSTAB_DUMP_JSON && fprintf(writer->file, "\n]\n") < 0) {
        result = -1;
    }
    if (writer->owns_file && fclose(writer->file) != 0) {
        result = -1;
    } else if (!writer->owns_file && fflush(writer->file) != 0) {
        result = -1;
    }
    writer->file = NULL;
    return result;
}
