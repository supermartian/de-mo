#ifndef MVSTAB_INSPECT_H
#define MVSTAB_INSPECT_H

#include <stddef.h>
#include <stdint.h>

#include "ffmpeg_reader.h"

#define MVSTAB_MAGNITUDE_BIN_COUNT 4096

typedef struct {
    int64_t target_frame;
    int64_t frame_count;
    int64_t picture_counts[3];
    int64_t frames_with_vectors;
    int64_t vector_count;
    int64_t past_vectors;
    int64_t future_vectors;
    int64_t block_counts[4];
    uint64_t magnitude_bins[MVSTAB_MAGNITUDE_BIN_COUNT];
    uint64_t magnitude_count;
} MvstabInspectState;

int mvstab_inspect_frame(const MvstabFrame *frame, void *opaque);
void mvstab_print_inspection(
    const char *path,
    const MvstabVideoInfo *info,
    MvstabInspectState *state
);
void mvstab_free_inspection(MvstabInspectState *state);

#endif
