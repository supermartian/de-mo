#ifndef MVSTAB_WRITERS_H
#define MVSTAB_WRITERS_H

#include <stddef.h>
#include <stdio.h>

#include "mvstab/frame_motion.h"
#include "mvstab/timeline.h"

typedef enum {
    MVSTAB_DUMP_CSV,
    MVSTAB_DUMP_JSON
} MvstabDumpFormat;

typedef struct {
    FILE *file;
    MvstabDumpFormat format;
    int owns_file;
    int first_json_item;
} MvstabRawWriter;

int mvstab_raw_writer_open(
    MvstabRawWriter *writer,
    const char *path,
    MvstabDumpFormat format,
    char *error,
    size_t error_size
);
int mvstab_raw_writer_start(
    MvstabRawWriter *writer,
    FILE *file,
    MvstabDumpFormat format,
    char *error,
    size_t error_size
);
int mvstab_raw_writer_write(MvstabRawWriter *writer, const MvstabFrame *frame);
int mvstab_raw_writer_close(MvstabRawWriter *writer);

int mvstab_write_transform_file(
    const char *path,
    const MvstabTimelineFrame *frames,
    size_t frame_count,
    char *error,
    size_t error_size
);

int mvstab_write_stats_file(
    const char *path,
    MvstabDumpFormat format,
    const MvstabTimelineFrame *frames,
    size_t frame_count,
    char *error,
    size_t error_size
);

int mvstab_frame_motion_is_finite(const FrameMotion *motion);

#endif
