#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "writers.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

static int read_first_line(const char *path, char *line, size_t line_size) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    if (fgets(line, (int)line_size, file) == NULL) {
        fclose(file);
        return -1;
    }
    return fclose(file);
}

static int read_file(const char *path, char *contents, size_t contents_size) {
    FILE *file = fopen(path, "r");
    size_t bytes;
    if (file == NULL || contents_size == 0) {
        return -1;
    }
    bytes = fread(contents, 1, contents_size - 1, file);
    contents[bytes] = '\0';
    return fclose(file);
}

static int test_transform_format(void) {
    const char *path = "test-motion.trf";
    MvstabTimelineFrame frame;
    char error[128];
    char line[128];

    memset(&frame, 0, sizeof(frame));
    frame.output.dx = 2.5;
    frame.output.dy = -1.25;
    CHECK(mvstab_write_transform_file(path, &frame, 1, error, sizeof(error)) == 0);
    CHECK(read_first_line(path, line, sizeof(line)) == 0);
    CHECK(strcmp(line, "0 -2.500000000 1.250000000 0.000000000 0.000000000 0\n") == 0);
    CHECK(remove(path) == 0);
    return 0;
}

static int test_json_dump_handles_missing_pts(void) {
    const char *path = "test-mvs.json";
    MvstabRawWriter writer;
    MvstabVector vector;
    MvstabFrame frame;
    char error[128];
    char contents[1024];

    memset(&vector, 0, sizeof(vector));
    memset(&frame, 0, sizeof(frame));
    frame.pts_seconds = NAN;
    frame.picture_type = MVSTAB_PICTURE_P;
    frame.vectors = &vector;
    frame.vector_count = 1;
    CHECK(mvstab_raw_writer_open(&writer, path, MVSTAB_DUMP_JSON,
                                 error, sizeof(error)) == 0);
    CHECK(mvstab_raw_writer_write(&writer, &frame) == 0);
    CHECK(mvstab_raw_writer_close(&writer) == 0);
    CHECK(read_file(path, contents, sizeof(contents)) == 0);
    CHECK(strstr(contents, "\"pts_seconds\":null") != NULL);
    CHECK(remove(path) == 0);
    return 0;
}

static int test_stats_json_contains_output_and_measurement(void) {
    const char *path = "test-stats.json";
    MvstabTimelineFrame frame;
    char error[128];
    char contents[2048];
    memset(&frame, 0, sizeof(frame));
    frame.pts_seconds = NAN;
    frame.output.valid = 1;
    frame.output.dx = 2.0;
    frame.measured.dx = 3.0;
    CHECK(mvstab_write_stats_file(path, MVSTAB_DUMP_JSON, &frame, 1,
                                  error, sizeof(error)) == 0);
    CHECK(read_file(path, contents, sizeof(contents)) == 0);
    CHECK(strstr(contents, "\"pts_seconds\":null") != NULL);
    CHECK(strstr(contents, "\"dx\":2.000000000") != NULL);
    CHECK(strstr(contents, "\"measured_dx\":3.000000000") != NULL);
    CHECK(remove(path) == 0);
    return 0;
}

static int test_non_finite_transforms_are_rejected(void) {
    const char *path = "test-invalid.trf";
    MvstabTimelineFrame frame;
    char error[128];
    memset(&frame, 0, sizeof(frame));
    frame.output.dx = NAN;
    CHECK(mvstab_write_transform_file(path, &frame, 1,
                                      error, sizeof(error)) == -1);
    CHECK(mvstab_write_stats_file(path, MVSTAB_DUMP_JSON, &frame, 1,
                                  error, sizeof(error)) == -1);
    return 0;
}

int main(void) {
    if (test_transform_format() != 0) {
        return 1;
    }
    if (test_json_dump_handles_missing_pts() != 0) {
        return 1;
    }
    if (test_stats_json_contains_output_and_measurement() != 0) {
        return 1;
    }
    return test_non_finite_transforms_are_rejected();
}
