#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif

#include "cli.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "ffmpeg_reader.h"
#include "inspect.h"
#include "mvstab/estimator.h"
#include "mvstab/timeline.h"
#include "writers.h"

typedef struct {
    const char *input_path;
    const char *output_path;
    MvstabDumpFormat format;
} DumpOptions;

typedef struct {
    const char *input_path;
    const char *output_path;
    const char *stats_path;
    MvstabDumpFormat stats_format;
    MvstabEstimatorConfig estimator;
} AnalyzeOptions;

typedef struct {
    MvstabTimelineFrame *frames;
    size_t frame_count;
    size_t frame_capacity;
    size_t vector_count;
    MvstabEstimatorConfig config;
} AnalyzeState;

typedef struct {
    MvstabRawWriter *writer;
    size_t vector_count;
} DumpState;

typedef struct {
    const char *path;
    char *backup_path;
    int existed;
    int published;
} AnalysisPublication;

static void print_usage(FILE *file) {
    fprintf(file,
        "Usage:\n"
        "  mvstab inspect INPUT [--frame N]\n"
        "  mvstab dump INPUT [-o FILE] [--format csv|json]\n"
        "  mvstab analyze INPUT [-o motion.trf] [--stats motion.csv]\n"
        "      [--stats-format csv|json]\n"
        "      [--mode safe|all-mvs] [--max-mv PX] [--area-cap AREA]\n"
        "      [--residual-threshold PX] [--mad-threshold K]\n"
        "      [--min-confidence VALUE] [--min-coverage VALUE]\n");
}

static char *duplicate_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

#ifdef _WIN32
static char *canonical_windows_destination(const char *path) {
    char *absolute = _fullpath(NULL, path, 0);
    char *slash;
    char *base;
    char *parent;
    char *result = NULL;
    HANDLE handle;
    DWORD length;
    size_t size;
    if (absolute == NULL) {
        return NULL;
    }
    slash = strrchr(absolute, '\\');
    base = duplicate_text(slash == NULL ? absolute : slash + 1);
    if (slash == NULL || base == NULL) {
        free(absolute);
        free(base);
        return NULL;
    }
    if (slash == absolute + 2 && absolute[1] == ':') {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    handle = CreateFileA(absolute, 0,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(absolute);
    if (handle == INVALID_HANDLE_VALUE) {
        free(base);
        return NULL;
    }
    length = GetFinalPathNameByHandleA(handle, NULL, 0, FILE_NAME_NORMALIZED);
    parent = length == 0 ? NULL : malloc((size_t)length + 1);
    if (parent != NULL &&
        GetFinalPathNameByHandleA(handle, parent, length + 1,
                                  FILE_NAME_NORMALIZED) != 0) {
        size = strlen(parent) + strlen(base) + 2;
        result = malloc(size);
        if (result != NULL) {
            snprintf(result, size, "%s\\%s", parent, base);
        }
    }
    CloseHandle(handle);
    free(parent);
    free(base);
    return result;
}
#endif

static char *canonical_destination(const char *path) {
#ifdef _WIN32
    return canonical_windows_destination(path);
#else
    char *copy = duplicate_text(path);
    char *base;
    char *parent;
    char *result;
    char *slash;
    size_t size;
    if (copy == NULL) {
        return NULL;
    }
    slash = strrchr(copy, '/');
    base = duplicate_text(slash == NULL ? copy : slash + 1);
    if (slash == NULL) {
        strcpy(copy, ".");
    } else if (slash == copy) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    parent = realpath(copy, NULL);
    free(copy);
    if (base == NULL || parent == NULL) {
        free(base);
        free(parent);
        return NULL;
    }
    size = strlen(parent) + strlen(base) + 2;
    result = malloc(size);
    if (result != NULL) {
        snprintf(result, size, "%s%s%s", parent,
                 parent[strlen(parent) - 1] == '/' ? "" : "/", base);
    }
    free(base);
    free(parent);
    return result;
#endif
}

static int canonical_paths_equal(const char *left, const char *right) {
    char *canonical_left = canonical_destination(left);
    char *canonical_right = canonical_destination(right);
    int equal = 0;
    if (canonical_left != NULL && canonical_right != NULL) {
#ifdef _WIN32
        equal = _stricmp(canonical_left, canonical_right) == 0;
#else
        equal = strcmp(canonical_left, canonical_right) == 0;
#endif
    }
    free(canonical_left);
    free(canonical_right);
    return equal;
}

static int paths_refer_to_same_file(const char *left, const char *right) {
    struct stat left_status;
    struct stat right_status;

    if (left == NULL || right == NULL || strcmp(left, "-") == 0 ||
        strcmp(right, "-") == 0) {
        return 0;
    }
    if (strcmp(left, right) == 0) {
        return 1;
    }
    if (stat(left, &left_status) != 0 || stat(right, &right_status) != 0) {
        return canonical_paths_equal(left, right);
    }
    return left_status.st_dev == right_status.st_dev &&
           left_status.st_ino == right_status.st_ino;
}

static const char *option_value(int argc, char **argv, int *index) {
    if (*index + 1 >= argc) {
        return NULL;
    }
    return argv[++*index];
}

static int parse_nonnegative_integer(const char *text, int64_t *value) {
    char *end;
    long long parsed;
    errno = 0;
    parsed = strtoll(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || parsed < 0) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int parse_positive_double(const char *text, double *value) {
    char *end;
    double parsed;
    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || *text == '\0' || *end != '\0' ||
        parsed <= 0.0 || !isfinite(parsed)) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int parse_confidence(const char *text, double *value) {
    char *end;
    double parsed;
    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || *text == '\0' || *end != '\0' ||
        parsed < 0.0 || parsed > 1.0 || !isfinite(parsed)) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int read_inspect_options(int argc, char **argv, MvstabInspectState *state) {
    int index;
    state->target_frame = -1;
    for (index = 3; index < argc; ++index) {
        const char *value;
        if (strcmp(argv[index], "--frame") != 0) {
            fprintf(stderr, "error: unknown inspect option '%s'\n", argv[index]);
            return -1;
        }
        value = option_value(argc, argv, &index);
        if (value == NULL || parse_nonnegative_integer(value, &state->target_frame) != 0) {
            fprintf(stderr, "error: --frame requires a non-negative integer\n");
            return -1;
        }
    }
    return 0;
}

static int run_inspect(int argc, char **argv) {
    MvstabInspectState state = {0};
    MvstabVideoInfo info = {0};
    char error[256];
    int result;

    if (argc < 3 || read_inspect_options(argc, argv, &state) != 0) {
        return 2;
    }
    result = mvstab_read_video(argv[2], &info, mvstab_inspect_frame,
                               &state, error, sizeof(error));
    if (result == 0) {
        mvstab_print_inspection(argv[2], &info, &state);
    } else {
        fprintf(stderr, "error: %s\n", error);
    }
    mvstab_free_inspection(&state);
    return result == 0 ? 0 : 1;
}

static int parse_dump_format(const char *value, MvstabDumpFormat *format) {
    if (strcmp(value, "csv") == 0) {
        *format = MVSTAB_DUMP_CSV;
        return 0;
    }
    if (strcmp(value, "json") == 0) {
        *format = MVSTAB_DUMP_JSON;
        return 0;
    }
    return -1;
}

static int read_dump_options(int argc, char **argv, DumpOptions *options) {
    int index;
    options->input_path = argv[2];
    options->format = MVSTAB_DUMP_CSV;
    for (index = 3; index < argc; ++index) {
        const char *value = option_value(argc, argv, &index);
        if (value == NULL) {
            fprintf(stderr, "error: option '%s' requires a value\n", argv[index]);
            return -1;
        }
        if (strcmp(argv[index - 1], "-o") == 0 ||
            strcmp(argv[index - 1], "--output") == 0) {
            options->output_path = value;
        } else if (strcmp(argv[index - 1], "--format") == 0 &&
                   parse_dump_format(value, &options->format) == 0) {
            continue;
        } else {
            fprintf(stderr, "error: invalid dump option '%s'\n", argv[index - 1]);
            return -1;
        }
    }
    return 0;
}

static int dump_callback(const MvstabFrame *frame, void *opaque) {
    DumpState *state = opaque;
    state->vector_count += frame->vector_count;
    return mvstab_raw_writer_write(state->writer, frame);
}

static FILE *open_publish_temp(
    const char *path,
    char **temp_path,
    char *error,
    size_t error_size
) {
    size_t capacity = strlen(path) + 40;
    unsigned int attempt;
    FILE *file = NULL;
    *temp_path = malloc(capacity);
    if (*temp_path == NULL) {
        snprintf(error, error_size, "cannot allocate publication path");
        return NULL;
    }
    for (attempt = 0; attempt < 10000; ++attempt) {
        snprintf(*temp_path, capacity, "%s.mvstab-tmp-%u", path, attempt);
        file = fopen(*temp_path, "wbx");
        if (file != NULL || errno != EEXIST) {
            break;
        }
    }
    if (file == NULL) {
        snprintf(error, error_size, "cannot create output beside '%s': %s",
                 path, strerror(errno));
        free(*temp_path);
        *temp_path = NULL;
    }
    return file;
}

static int copy_staged_output(
    FILE *staged,
    FILE *output,
    char *error,
    size_t error_size
) {
    unsigned char buffer[16384];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), staged)) > 0) {
        if (fwrite(buffer, 1, bytes, output) != bytes) {
            snprintf(error, error_size, "short write while publishing output");
            return -1;
        }
    }
    if (ferror(staged)) {
        snprintf(error, error_size, "cannot read staged output");
        return -1;
    }
    return 0;
}

static int replace_output_file(const char *temp_path, const char *path) {
#ifdef _WIN32
    if (!MoveFileExA(temp_path, path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        errno = EIO;
        return -1;
    }
    return 0;
#else
    return rename(temp_path, path);
#endif
}

static int publish_staged_output(
    FILE *staged,
    const char *path,
    char *error,
    size_t error_size
) {
    FILE *output;
    char *temp_path = NULL;
    int failed;

    if (fseek(staged, 0, SEEK_SET) != 0) {
        snprintf(error, error_size, "cannot rewind staged output: %s", strerror(errno));
        return -1;
    }
    output = open_publish_temp(path, &temp_path, error, error_size);
    if (output == NULL) {
        return -1;
    }
    failed = copy_staged_output(staged, output, error, error_size) != 0;
    if (!failed && fflush(output) != 0) {
        snprintf(error, error_size, "cannot flush output '%s': %s", path, strerror(errno));
        failed = 1;
    }
    if (fclose(output) != 0 && !failed) {
        snprintf(error, error_size, "cannot close output '%s': %s", path, strerror(errno));
        failed = 1;
    }
    if (!failed && replace_output_file(temp_path, path) != 0) {
        snprintf(error, error_size, "cannot replace output '%s': %s",
                 path, strerror(errno));
        failed = 1;
    }
    if (failed) {
        remove(temp_path);
    }
    free(temp_path);
    return failed ? -1 : 0;
}

static int open_dump_writer(
    const DumpOptions *options,
    MvstabRawWriter *writer,
    FILE **staged,
    char *error,
    size_t error_size
) {
    int stage_output = options->output_path != NULL &&
                       strcmp(options->output_path, "-") != 0;
    if (!stage_output) {
        return mvstab_raw_writer_open(writer, options->output_path,
                                      options->format, error, error_size);
    }
    *staged = tmpfile();
    if (*staged == NULL) {
        snprintf(error, error_size, "cannot create staged output: %s", strerror(errno));
        return -1;
    }
    if (mvstab_raw_writer_start(writer, *staged, options->format,
                                error, error_size) != 0) {
        fclose(*staged);
        *staged = NULL;
        return -1;
    }
    return 0;
}

static int run_dump(int argc, char **argv) {
    DumpOptions options = {0};
    MvstabRawWriter writer;
    DumpState state = {.writer = &writer};
    MvstabVideoInfo info;
    FILE *staged = NULL;
    char error[256];
    int result;

    if (argc < 3 || read_dump_options(argc, argv, &options) != 0) {
        return 2;
    }
    if (paths_refer_to_same_file(options.input_path, options.output_path)) {
        fprintf(stderr, "error: dump output must not overwrite the input file\n");
        return 2;
    }
    if (open_dump_writer(&options, &writer, &staged, error, sizeof(error)) != 0) {
        fprintf(stderr, "error: %s\n", error);
        return 1;
    }
    result = mvstab_read_video(options.input_path, &info, dump_callback,
                               &state, error, sizeof(error));
    if (mvstab_raw_writer_close(&writer) != 0 && result == 0) {
        snprintf(error, sizeof(error), "cannot finish motion-vector output");
        result = -1;
    }
    if (result == 0 && state.vector_count == 0) {
        snprintf(error, sizeof(error),
                 "decoder produced no AV_FRAME_DATA_MOTION_VECTORS for codec %s (%s)",
                 info.codec_name, info.decoder_name);
        result = -1;
    }
    if (result == 0 && staged != NULL) {
        result = publish_staged_output(staged, options.output_path,
                                       error, sizeof(error));
    }
    if (result != 0) {
        fprintf(stderr, "error: %s\n", error);
    }
    if (staged != NULL) {
        fclose(staged);
    }
    return result == 0 ? 0 : 1;
}

static int parse_mode(const char *value, MvstabMode *mode) {
    if (strcmp(value, "safe") == 0) {
        *mode = MVSTAB_MODE_SAFE;
        return 0;
    }
    if (strcmp(value, "all-mvs") == 0) {
        *mode = MVSTAB_MODE_ALL_MVS;
        return 0;
    }
    return -1;
}

static int set_analyze_option(
    AnalyzeOptions *options,
    const char *name,
    const char *value
) {
    if (strcmp(name, "-o") == 0 || strcmp(name, "--output") == 0) {
        options->output_path = value;
        return 0;
    }
    if (strcmp(name, "--stats") == 0) {
        options->stats_path = value;
        return 0;
    }
    if (strcmp(name, "--stats-format") == 0) {
        return parse_dump_format(value, &options->stats_format);
    }
    if (strcmp(name, "--mode") == 0) {
        return parse_mode(value, &options->estimator.mode);
    }
    if (strcmp(name, "--model") == 0) {
        return strcmp(value, "translation") == 0 ? 0 : -1;
    }
    if (strcmp(name, "--max-mv") == 0) {
        return parse_positive_double(value, &options->estimator.max_mv_px);
    }
    if (strcmp(name, "--area-cap") == 0) {
        return parse_positive_double(value, &options->estimator.block_area_cap);
    }
    if (strcmp(name, "--residual-threshold") == 0 ||
        strcmp(name, "--ransac-threshold") == 0) {
        return parse_positive_double(value, &options->estimator.residual_threshold_px);
    }
    if (strcmp(name, "--mad-threshold") == 0) {
        return parse_positive_double(value, &options->estimator.mad_threshold);
    }
    if (strcmp(name, "--min-confidence") == 0) {
        return parse_confidence(value, &options->estimator.min_confidence);
    }
    if (strcmp(name, "--min-coverage") == 0) {
        return parse_confidence(value, &options->estimator.min_spatial_coverage);
    }
    return -1;
}

static int read_analyze_options(int argc, char **argv, AnalyzeOptions *options) {
    int index;
    options->input_path = argv[2];
    options->output_path = "motion.trf";
    options->stats_format = MVSTAB_DUMP_CSV;
    options->estimator = mvstab_default_estimator_config();
    for (index = 3; index < argc; ++index) {
        const char *name = argv[index];
        const char *value = option_value(argc, argv, &index);
        if (value == NULL || set_analyze_option(options, name, value) != 0) {
            fprintf(stderr, "error: invalid value for analyze option '%s'\n", name);
            return -1;
        }
    }
    return 0;
}

static MvstabTimelineFrame *append_timeline_frame(AnalyzeState *state) {
    MvstabTimelineFrame *resized;
    size_t capacity;

    if (state->frame_count < state->frame_capacity) {
        return &state->frames[state->frame_count++];
    }
    capacity = state->frame_capacity == 0 ? 256 : state->frame_capacity * 2;
    resized = realloc(state->frames, capacity * sizeof(*resized));
    if (resized == NULL) {
        return NULL;
    }
    state->frames = resized;
    state->frame_capacity = capacity;
    return &state->frames[state->frame_count++];
}

static int analyze_callback(const MvstabFrame *frame, void *opaque) {
    AnalyzeState *state = opaque;
    MvstabTimelineFrame *timeline_frame = append_timeline_frame(state);
    if (timeline_frame == NULL) {
        return -1;
    }
    memset(timeline_frame, 0, sizeof(*timeline_frame));
    timeline_frame->frame_index = frame->display_index;
    timeline_frame->pts = frame->pts;
    timeline_frame->pts_seconds = frame->pts_seconds;
    timeline_frame->picture_type = frame->picture_type;
    timeline_frame->key_frame = frame->key_frame;
    state->vector_count += frame->vector_count;
    if (mvstab_estimate_frame(frame, &state->config,
                              &timeline_frame->measured) != 0) {
        return -1;
    }
    return mvstab_estimate_frame_edges(frame, &state->config,
                                       &timeline_frame->edges,
                                       &timeline_frame->edge_count);
}

static void free_analysis_frames(AnalyzeState *state) {
    for (size_t index = 0; index < state->frame_count; ++index) {
        free(state->frames[index].edges);
    }
    free(state->frames);
}

static int reserve_analysis_temp(
    const char *path,
    char **temp_path,
    char *error,
    size_t error_size
) {
    FILE *file = open_publish_temp(path, temp_path, error, error_size);
    if (file == NULL) {
        return -1;
    }
    if (fclose(file) != 0) {
        snprintf(error, error_size, "cannot close staged output '%s': %s",
                 *temp_path, strerror(errno));
        remove(*temp_path);
        free(*temp_path);
        *temp_path = NULL;
        return -1;
    }
    return 0;
}

static int publish_analysis_temp(
    char **temp_path,
    const char *path,
    char *error,
    size_t error_size
) {
    if (replace_output_file(*temp_path, path) != 0) {
        snprintf(error, error_size, "cannot replace output '%s': %s",
                 path, strerror(errno));
        return -1;
    }
    free(*temp_path);
    *temp_path = NULL;
    return 0;
}

static int mode_is_directory(int mode) {
#ifdef _WIN32
    return (mode & _S_IFMT) == _S_IFDIR;
#else
    return S_ISDIR(mode);
#endif
}

static int prepare_analysis_publication(
    AnalysisPublication *publication,
    char *error,
    size_t error_size
) {
    struct stat status;
    FILE *placeholder;
    if (stat(publication->path, &status) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        snprintf(error, error_size, "cannot inspect output '%s': %s",
                 publication->path, strerror(errno));
        return -1;
    }
    if (mode_is_directory(status.st_mode)) {
        snprintf(error, error_size, "output '%s' is a directory", publication->path);
        return -1;
    }
    placeholder = open_publish_temp(publication->path, &publication->backup_path,
                                    error, error_size);
    if (placeholder == NULL) {
        return -1;
    }
    fclose(placeholder);
    remove(publication->backup_path);
    if (replace_output_file(publication->path, publication->backup_path) != 0) {
        snprintf(error, error_size, "cannot preserve output '%s': %s",
                 publication->path, strerror(errno));
        free(publication->backup_path);
        publication->backup_path = NULL;
        return -1;
    }
    publication->existed = 1;
    return 0;
}

static void rollback_analysis_publication(AnalysisPublication *publication) {
    if (publication->published) {
        remove(publication->path);
    }
    if (publication->existed && publication->backup_path != NULL) {
        replace_output_file(publication->backup_path, publication->path);
    }
    free(publication->backup_path);
    publication->backup_path = NULL;
}

static void finish_analysis_publication(AnalysisPublication *publication) {
    if (publication->backup_path != NULL) {
        remove(publication->backup_path);
    }
    free(publication->backup_path);
    publication->backup_path = NULL;
}

static int write_analysis(
    const AnalyzeOptions *options,
    AnalyzeState *state,
    char *error,
    size_t error_size
) {
    char *transform_temp = NULL;
    char *stats_temp = NULL;
    AnalysisPublication transform = {.path = options->output_path};
    AnalysisPublication stats = {.path = options->stats_path};
    int result = -1;

    mvstab_build_timeline(state->frames, state->frame_count, options->estimator.mode);
    if (reserve_analysis_temp(options->output_path, &transform_temp,
                              error, error_size) != 0 ||
        mvstab_write_transform_file(transform_temp, state->frames,
                                    state->frame_count, error, error_size) != 0) {
        goto cleanup;
    }
    if (options->stats_path != NULL &&
        (reserve_analysis_temp(options->stats_path, &stats_temp,
                               error, error_size) != 0 ||
         mvstab_write_stats_file(stats_temp, options->stats_format, state->frames,
                                 state->frame_count, error, error_size) != 0)) {
        goto cleanup;
    }
    if (prepare_analysis_publication(&transform, error, error_size) != 0 ||
        (stats.path != NULL &&
         prepare_analysis_publication(&stats, error, error_size) != 0)) {
        goto rollback;
    }
    if (publish_analysis_temp(&transform_temp, transform.path,
                              error, error_size) != 0) {
        goto rollback;
    }
    transform.published = 1;
    if (stats_temp != NULL &&
        publish_analysis_temp(&stats_temp, stats.path,
                              error, error_size) != 0) {
        goto rollback;
    }
    stats.published = stats.path != NULL;
    finish_analysis_publication(&transform);
    finish_analysis_publication(&stats);
    result = 0;
    goto cleanup;
rollback:
    rollback_analysis_publication(&stats);
    rollback_analysis_publication(&transform);
cleanup:
    if (transform_temp != NULL) {
        remove(transform_temp);
    }
    if (stats_temp != NULL) {
        remove(stats_temp);
    }
    free(transform_temp);
    free(stats_temp);
    return result;
}

static int analysis_paths_are_safe(const AnalyzeOptions *options) {
    if (strcmp(options->output_path, "-") == 0 ||
        (options->stats_path != NULL && strcmp(options->stats_path, "-") == 0)) {
        fprintf(stderr, "error: analyze outputs must be filesystem paths\n");
        return 0;
    }
    if (paths_refer_to_same_file(options->input_path, options->output_path) ||
        paths_refer_to_same_file(options->input_path, options->stats_path)) {
        fprintf(stderr, "error: analysis output must not overwrite the input file\n");
        return 0;
    }
    if (paths_refer_to_same_file(options->output_path, options->stats_path)) {
        fprintf(stderr, "error: transform and stats outputs must be different files\n");
        return 0;
    }
    return 1;
}

static int run_analyze(int argc, char **argv) {
    AnalyzeOptions options = {0};
    AnalyzeState state = {0};
    MvstabVideoInfo info;
    char error[256];
    int result;

    if (argc < 3 || read_analyze_options(argc, argv, &options) != 0 ||
        !analysis_paths_are_safe(&options)) {
        return 2;
    }
    state.config = options.estimator;
    result = mvstab_read_video(options.input_path, &info, analyze_callback,
                               &state, error, sizeof(error));
    if (result == 0 && state.vector_count == 0) {
        snprintf(error, sizeof(error),
                 "decoder produced no AV_FRAME_DATA_MOTION_VECTORS for codec %s (%s)",
                 info.codec_name, info.decoder_name);
        result = -1;
    }
    if (result == 0 && state.frame_count == 0) {
        snprintf(error, sizeof(error), "input contains no decoded video frames");
        result = -1;
    }
    if (result == 0) {
        result = write_analysis(&options, &state, error, sizeof(error));
    }
    if (result != 0) {
        fprintf(stderr, "error: %s\n", error);
    } else {
        printf("Wrote %zu frame transforms to %s\n", state.frame_count, options.output_path);
        if (options.stats_path != NULL) {
            printf("Wrote estimator statistics to %s\n", options.stats_path);
        }
    }
    free_analysis_frames(&state);
    return result == 0 ? 0 : 1;
}

int mvstab_run_cli(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }
    if (strcmp(argv[1], "inspect") == 0) {
        return run_inspect(argc, argv);
    }
    if (strcmp(argv[1], "dump") == 0) {
        return run_dump(argc, argv);
    }
    if (strcmp(argv[1], "analyze") == 0) {
        return run_analyze(argc, argv);
    }
    fprintf(stderr, "error: unknown command '%s'\n", argv[1]);
    print_usage(stderr);
    return 2;
}
