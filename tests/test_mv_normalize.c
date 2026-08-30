#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "mvstab/motion_vector.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

static int nearly_equal(double left, double right) {
    return fabs(left - right) < 1e-9;
}

static int test_reference_to_current_sign(void) {
    MvstabVector vector;
    int result = mvstab_normalize_vector(
        &vector, 90, 57, 100, 50, 16, 8, -40, 28, 4, -1, UINT64_C(3));

    CHECK(result == 0);
    CHECK(nearly_equal(vector.dx, 10.0));
    CHECK(nearly_equal(vector.dy, -7.0));
    CHECK(nearly_equal(vector.weight, 128.0));
    CHECK(vector.reference_direction == -1);
    CHECK(vector.codec_flags == UINT64_C(3));
    return 0;
}

static int test_malformed_vectors_are_rejected(void) {
    MvstabVector vector;
    CHECK(mvstab_normalize_vector(
        &vector, 0, 0, 0, 0, 16, 16, 1, 1, 0, -1, 0) == -1);
    CHECK(mvstab_normalize_vector(
        &vector, 0, 0, 0, 0, 0, 16, 1, 1, 4, -1, 0) == -1);
    CHECK(mvstab_normalize_vector(
        NULL, 0, 0, 0, 0, 16, 16, 1, 1, 4, -1, 0) == -1);
    return 0;
}

int main(void) {
    if (test_reference_to_current_sign() != 0) {
        return 1;
    }
    return test_malformed_vectors_are_rejected();
}
