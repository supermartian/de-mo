#ifndef MVSTAB_FFMPEG_READER_H
#define MVSTAB_FFMPEG_READER_H

#include <stddef.h>

#include "mvstab/frame_motion.h"

typedef struct {
    char codec_name[64];
    char profile_name[64];
    char decoder_name[64];
    int width;
    int height;
    double frame_rate;
    double duration_seconds;
    int metadata_only_decode;
} MvstabVideoInfo;

typedef int (*MvstabFrameCallback)(const MvstabFrame *frame, void *opaque);

int mvstab_read_video(
    const char *path,
    MvstabVideoInfo *info,
    MvstabFrameCallback callback,
    void *opaque,
    char *error,
    size_t error_size
);

#endif
