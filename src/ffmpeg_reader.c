#include "ffmpeg_reader.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/motion_vector.h>
#include <libavutil/opt.h>

#include "ffmpeg_motion_metadata.h"
#include "mvstab/motion_vector.h"

typedef struct {
    AVFormatContext *format;
    AVCodecContext *decoder;
    AVPacket *packet;
    AVFrame *frame;
    AVStream *stream;
    int stream_index;
    int64_t frame_index;
    double previous_pts_seconds;
    double nominal_duration_seconds;
    int metadata_only_decode;
    MvstabFrameCallback callback;
    void *opaque;
    char *error;
    size_t error_size;
} Reader;

static void set_av_error(Reader *reader, const char *operation, int code) {
    char detail[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code, detail, sizeof(detail));
    snprintf(reader->error, reader->error_size, "%s: %s", operation, detail);
}

static void close_reader(Reader *reader) {
    av_frame_free(&reader->frame);
    av_packet_free(&reader->packet);
    avcodec_free_context(&reader->decoder);
    avformat_close_input(&reader->format);
}

static int open_input(Reader *reader, const char *path) {
    const AVCodec *codec;
    AVDictionary *options = NULL;
    int result;

    result = avformat_open_input(&reader->format, path, NULL, NULL);
    if (result < 0) {
        set_av_error(reader, "cannot open input", result);
        return -1;
    }
    result = avformat_find_stream_info(reader->format, NULL);
    if (result < 0) {
        set_av_error(reader, "cannot read stream information", result);
        return -1;
    }
    result = av_find_best_stream(reader->format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (result < 0) {
        set_av_error(reader, "cannot find a video stream", result);
        return -1;
    }
    reader->stream_index = result;
    reader->stream = reader->format->streams[result];
    codec = avcodec_find_decoder(reader->stream->codecpar->codec_id);
    if (codec == NULL) {
        snprintf(reader->error, reader->error_size, "cannot find a software decoder");
        return -1;
    }
    reader->decoder = avcodec_alloc_context3(codec);
    if (reader->decoder == NULL) {
        snprintf(reader->error, reader->error_size, "cannot allocate decoder context");
        return -1;
    }
    result = avcodec_parameters_to_context(reader->decoder, reader->stream->codecpar);
    if (result < 0) {
        set_av_error(reader, "cannot copy codec parameters", result);
        return -1;
    }
    reader->decoder->pkt_timebase = reader->stream->time_base;
    result = av_dict_set(&options, "flags2", "+export_mvs", 0);
    if (result < 0) {
        set_av_error(reader, "cannot request exported motion vectors", result);
        av_dict_free(&options);
        return -1;
    }
#if LIBAVCODEC_VERSION_MAJOR >= 59
    reader->decoder->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;
#endif
    if (reader->stream->codecpar->codec_id == AV_CODEC_ID_H264 &&
        av_opt_set(reader->decoder->priv_data, "motion_metadata_only", "1", 0) >= 0) {
        reader->metadata_only_decode = 1;
    }
    result = avcodec_open2(reader->decoder, codec, &options);
    av_dict_free(&options);
    if (result < 0) {
        set_av_error(reader, "cannot open software decoder", result);
        return -1;
    }
    return 0;
}

static int64_t motion_pts_delta(uint64_t flags) {
    uint64_t raw = (flags >> MVSTAB_AV_MV_PTS_DELTA_SHIFT) &
                   MVSTAB_AV_MV_PTS_DELTA_MASK;
    if (raw & (UINT64_C(1) << 47)) {
        raw |= ~MVSTAB_AV_MV_PTS_DELTA_MASK;
    }
    return (int64_t)raw;
}

static void copy_reference_metadata(
    MvstabVector *output,
    const AVMotionVector *input,
    double time_base
) {
    uint64_t flags = input->flags;
    output->reference_exact = !!(flags & MVSTAB_AV_MV_REFERENCE_EXACT);
    output->reference_poc_delta = input->source;
    if (!output->reference_exact) {
        return;
    }
    output->reference_index = flags & MVSTAB_AV_MV_REFERENCE_INDEX_MASK;
    output->reference_list = !!(flags & MVSTAB_AV_MV_REFERENCE_LIST1);
    output->reference_long_term = !!(flags & MVSTAB_AV_MV_LONG_REFERENCE);
    output->reference_top_field = !!(flags & MVSTAB_AV_MV_REFERENCE_TOP_FIELD);
    output->reference_bottom_field = !!(flags & MVSTAB_AV_MV_REFERENCE_BOTTOM_FIELD);
    output->prediction_direct = !!(flags & MVSTAB_AV_MV_DIRECT);
    output->prediction_skip = !!(flags & MVSTAB_AV_MV_SKIP);
    output->prediction_interlaced = !!(flags & MVSTAB_AV_MV_INTERLACED);
    output->reference_pts_valid = !!(flags & MVSTAB_AV_MV_PTS_DELTA_VALID);
    if (output->reference_pts_valid) {
        output->reference_pts_delta = motion_pts_delta(flags);
        output->reference_delta_seconds = output->reference_pts_delta * time_base;
        output->reference_direction =
            (output->reference_pts_delta > 0) - (output->reference_pts_delta < 0);
    }
}

static MvstabPictureType convert_picture_type(enum AVPictureType picture_type) {
    if (picture_type == AV_PICTURE_TYPE_I) {
        return MVSTAB_PICTURE_I;
    }
    if (picture_type == AV_PICTURE_TYPE_P) {
        return MVSTAB_PICTURE_P;
    }
    if (picture_type == AV_PICTURE_TYPE_B) {
        return MVSTAB_PICTURE_B;
    }
    return MVSTAB_PICTURE_UNKNOWN;
}

static size_t copy_motion_vectors(
    const Reader *reader,
    const AVFrame *frame,
    MvstabVector **output
) {
    AVFrameSideData *side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
    const AVMotionVector *source;
    MvstabVector *vectors;
    size_t source_count;
    size_t accepted = 0;
    size_t index;

    *output = NULL;
    if (side_data == NULL || (size_t)side_data->size < sizeof(*source)) {
        return 0;
    }
    source = (const AVMotionVector *)side_data->data;
    source_count = side_data->size / sizeof(*source);
    vectors = calloc(source_count, sizeof(*vectors));
    if (vectors == NULL) {
        return SIZE_MAX;
    }
    for (index = 0; index < source_count; ++index) {
        int direction = (source[index].source > 0) - (source[index].source < 0);
        int result = mvstab_normalize_vector(
            &vectors[accepted], source[index].src_x, source[index].src_y,
            source[index].dst_x, source[index].dst_y,
            source[index].w, source[index].h,
            source[index].motion_x, source[index].motion_y,
            source[index].motion_scale, direction, source[index].flags);
        if (result == 0) {
            copy_reference_metadata(&vectors[accepted], &source[index],
                                    av_q2d(reader->stream->time_base));
        }
        accepted += result == 0;
    }
    *output = vectors;
    return accepted;
}

static int emit_frame(Reader *reader) {
    MvstabVector *vectors;
    size_t vector_count = copy_motion_vectors(reader, reader->frame, &vectors);
    int64_t pts = reader->frame->best_effort_timestamp;
    MvstabFrame frame;
    int result;

    if (vector_count == SIZE_MAX) {
        snprintf(reader->error, reader->error_size, "cannot allocate motion vectors");
        return -1;
    }
    frame.decode_index = -1;
    frame.display_index = reader->frame_index++;
    frame.pts = pts;
    frame.pts_seconds = pts == AV_NOPTS_VALUE ? NAN : pts * av_q2d(reader->stream->time_base);
    frame.duration_seconds = reader->nominal_duration_seconds;
    if (isfinite(frame.pts_seconds) && isfinite(reader->previous_pts_seconds) &&
        frame.pts_seconds > reader->previous_pts_seconds) {
        frame.duration_seconds = frame.pts_seconds - reader->previous_pts_seconds;
    }
    frame.picture_type = convert_picture_type(reader->frame->pict_type);
#if LIBAVUTIL_VERSION_MAJOR >= 58
    frame.key_frame = !!(reader->frame->flags & AV_FRAME_FLAG_KEY);
#else
    frame.key_frame = reader->frame->key_frame;
#endif
    frame.width = reader->frame->width;
    frame.height = reader->frame->height;
    frame.vectors = vectors;
    frame.vector_count = vector_count;
    result = reader->callback(&frame, reader->opaque);
    reader->previous_pts_seconds = frame.pts_seconds;
    free(vectors);
    if (result != 0 && reader->error[0] == '\0') {
        snprintf(reader->error, reader->error_size, "frame callback failed at frame %lld",
                 (long long)frame.display_index);
    }
    return result;
}

static int receive_frames(Reader *reader) {
    int result;

    while ((result = avcodec_receive_frame(reader->decoder, reader->frame)) >= 0) {
        if (emit_frame(reader) != 0) {
            return -1;
        }
        av_frame_unref(reader->frame);
    }
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return 0;
    }
    set_av_error(reader, "cannot decode frame", result);
    return -1;
}

static int decode_video(Reader *reader) {
    int result;

    while ((result = av_read_frame(reader->format, reader->packet)) >= 0) {
        if (reader->packet->stream_index == reader->stream_index) {
            result = avcodec_send_packet(reader->decoder, reader->packet);
            if (result < 0) {
                set_av_error(reader, "cannot send video packet", result);
                av_packet_unref(reader->packet);
                return -1;
            }
            if (receive_frames(reader) != 0) {
                av_packet_unref(reader->packet);
                return -1;
            }
        }
        av_packet_unref(reader->packet);
    }
    if (result != AVERROR_EOF) {
        set_av_error(reader, "cannot read input packet", result);
        return -1;
    }
    result = avcodec_send_packet(reader->decoder, NULL);
    if (result < 0 && result != AVERROR_EOF) {
        set_av_error(reader, "cannot flush decoder", result);
        return -1;
    }
    return receive_frames(reader);
}

static void populate_video_info(const Reader *reader, MvstabVideoInfo *info) {
    AVRational rate = av_guess_frame_rate(reader->format, reader->stream, NULL);
    const char *profile = avcodec_profile_name(reader->decoder->codec_id,
                                               reader->decoder->profile);

    snprintf(info->codec_name, sizeof(info->codec_name), "%s",
             avcodec_get_name(reader->decoder->codec_id));
    snprintf(info->decoder_name, sizeof(info->decoder_name), "%s",
             reader->decoder->codec->name);
    snprintf(info->profile_name, sizeof(info->profile_name), "%s",
             profile == NULL ? "unknown" : profile);
    info->width = reader->decoder->width;
    info->height = reader->decoder->height;
    info->frame_rate = rate.den == 0 ? 0.0 : av_q2d(rate);
    info->metadata_only_decode = reader->metadata_only_decode;
    if (reader->stream->duration != AV_NOPTS_VALUE) {
        info->duration_seconds = reader->stream->duration * av_q2d(reader->stream->time_base);
    } else if (reader->format->duration != AV_NOPTS_VALUE) {
        info->duration_seconds = reader->format->duration / (double)AV_TIME_BASE;
    } else {
        info->duration_seconds = NAN;
    }
}

int mvstab_read_video(
    const char *path,
    MvstabVideoInfo *info,
    MvstabFrameCallback callback,
    void *opaque,
    char *error,
    size_t error_size
) {
    Reader reader = {.callback = callback, .opaque = opaque,
                     .previous_pts_seconds = NAN,
                     .error = error, .error_size = error_size};
    int result = -1;

    if (error != NULL && error_size > 0) {
        error[0] = '\0';
    }
    if (path == NULL || info == NULL || callback == NULL || error == NULL || error_size == 0) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "invalid video reader arguments");
        }
        return -1;
    }
    if (open_input(&reader, path) != 0) {
        close_reader(&reader);
        return -1;
    }
    reader.packet = av_packet_alloc();
    reader.frame = av_frame_alloc();
    if (reader.packet == NULL || reader.frame == NULL) {
        snprintf(error, error_size, "cannot allocate decode buffers");
    } else {
        populate_video_info(&reader, info);
        reader.nominal_duration_seconds = info->frame_rate > 0.0 ?
                                          1.0 / info->frame_rate : NAN;
        result = decode_video(&reader);
    }
    close_reader(&reader);
    return result;
}
