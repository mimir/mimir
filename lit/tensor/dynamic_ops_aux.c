#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int32_t* splat_matrix(uint64_t rows, uint64_t cols, int32_t value);
extern int32_t* generate_matrix(uint64_t rows, uint64_t cols, int32_t seed);
extern int32_t* broadcast_rows(uint64_t rows, uint64_t cols, int32_t* a);
extern int32_t* broadcast_singleton(uint64_t rows, uint64_t cols, int32_t* a);
extern int32_t* flip_matrix(uint64_t rows, uint64_t cols, int32_t* a);
extern int32_t* flatten_matrix(uint64_t rows, uint64_t cols, int32_t* a);
extern int32_t* transpose_matrix(uint64_t rows, uint64_t cols, int32_t* a);
extern int32_t* reduce_columns(uint64_t rows, uint64_t cols, int32_t* a);
extern int32_t* set_last(uint64_t rows, uint64_t cols, int32_t* a);
extern int32_t recursive_read(uint64_t rows, uint64_t cols, uint64_t count, int32_t acc, int32_t* a);
extern int32_t* slice_columns(uint64_t rows, uint64_t cols, int32_t* a);
extern int32_t* repeat_matrix(uint64_t rows, uint64_t cols, int32_t* a);
extern int32_t* concat_columns(uint64_t rows, int32_t* a, int32_t* b);
extern int32_t* gather_columns(uint64_t rows, uint64_t cols, int32_t* a, uint64_t* index);
extern int32_t* scatter_columns(uint64_t rows, uint64_t cols, int32_t* a, uint64_t* index, int32_t* updates);
extern int32_t* gather_rows(uint64_t rows, uint64_t cols, int32_t* a, uint64_t* index);
extern int32_t* scatter_rows(uint64_t rows, uint64_t cols, int32_t* a, uint64_t* index, int32_t* updates);
extern int32_t*
scatter_some_rows(uint64_t rows, uint64_t cols, uint64_t count, int32_t* a, uint64_t* index, int32_t* updates);
extern int32_t* replicate_matrix(uint64_t rows, uint64_t cols, int32_t* a);

static int failures;

static void check(const char* op, uint64_t rows, uint64_t cols, uint64_t count, int32_t* out, const int32_t* expected) {
    for (uint64_t i = 0; i < count; ++i)
        if (out[i] != expected[i]) {
            fprintf(stderr, "%s (%llu,%llu) at %llu: got %d, expected %d\n", op, (unsigned long long)rows,
                    (unsigned long long)cols, (unsigned long long)i, out[i], expected[i]);
            ++failures;
            break;
        }
    free(out);
}

int main(void) {
    const uint64_t sizes[] = {0, 1, 3, 7};
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c) {
            uint64_t rows = sizes[r], cols = sizes[c], count = rows * cols;
            int32_t a[49], updates[49], expected[294];
            uint64_t index[49];
            for (uint64_t i = 0; i < count; ++i) {
                a[i]       = (int32_t)(i * 3 + 11);
                updates[i] = (int32_t)(i + 101);
            }
            int32_t* input = count ? a : NULL;

            for (uint64_t i = 0; i < count; ++i)
                expected[i] = 19;
            check("splat", rows, cols, count, splat_matrix(rows, cols, 19), expected);
            for (uint64_t i = 0; i < rows; ++i)
                for (uint64_t j = 0; j < cols; ++j)
                    expected[i * cols + j] = 13 + (i == j ? 5 : 2);
            check("generate", rows, cols, count, generate_matrix(rows, cols, 13), expected);
            for (uint64_t i = 0; i < rows; ++i)
                for (uint64_t j = 0; j < cols; ++j)
                    expected[i * cols + j] = a[j];
            check("broadcast", rows, cols, count, broadcast_rows(rows, cols, cols ? a : NULL), expected);
            if ((rows == 1 || rows == 3) && (cols == 1 || cols == 3)) {
                for (uint64_t i = 0; i < 3; ++i)
                    for (uint64_t j = 0; j < 3; ++j)
                        expected[3 * i + j] = a[(rows == 1 ? 0 : i) * cols + (cols == 1 ? 0 : j)];
                check("broadcast_singleton", rows, cols, 9, broadcast_singleton(rows, cols, a), expected);
            }
            for (uint64_t i = 0; i < count; ++i)
                expected[i] = a[count - 1 - i];
            check("flip", rows, cols, count, flip_matrix(rows, cols, input), expected);
            check("reshape", rows, cols, count, flatten_matrix(rows, cols, input), a);

            for (uint64_t i = 0; i < rows; ++i)
                for (uint64_t j = 0; j < cols; ++j)
                    expected[j * rows + i] = a[i * cols + j];
            check("transpose", rows, cols, count, transpose_matrix(rows, cols, input), expected);
            for (uint64_t i = 0; i < rows; ++i) {
                expected[i] = 0;
                for (uint64_t j = 0; j < cols; ++j)
                    expected[i] += a[i * cols + j];
            }
            check("reduce", rows, cols, rows, reduce_columns(rows, cols, input), expected);
            if (count) {
                memcpy(expected, a, count * sizeof(*a));
                expected[count - 1] = 37;
                check("set", rows, cols, count, set_last(rows, cols, a), expected);
                const uint64_t depths[] = {0, 1, 4, 17};
                for (unsigned d = 0; d < 4; ++d)
                    if (recursive_read(rows, cols, depths[d], 0, a) != a[0] + (int32_t)depths[d]) return 3;
            }

            int32_t wide[98];
            for (uint64_t i = 0; i < 2 * count; ++i)
                wide[i] = (int32_t)(i + 41);
            for (uint64_t i = 0; i < count; ++i)
                expected[i] = wide[2 * i];
            check("slice", rows, cols, count, slice_columns(rows, cols, count ? wide : NULL), expected);
            for (uint64_t i = 0; i < 2 * rows; ++i)
                for (uint64_t j = 0; j < 3 * cols; ++j)
                    expected[i * (3 * cols) + j] = a[(i % rows) * cols + j % cols];
            check("repeat", rows, cols, 6 * count, repeat_matrix(rows, cols, input), expected);

            for (uint64_t i = 0; i < rows; ++i)
                for (uint64_t j = 0; j < cols; ++j) {
                    index[i * cols + j]    = cols - 1 - j;
                    expected[i * cols + j] = a[i * cols + cols - 1 - j];
                }
            check("gather", rows, cols, count, gather_columns(rows, cols, input, count ? index : NULL), expected);
            for (uint64_t i = 0; i < rows; ++i)
                for (uint64_t j = 0; j < cols; ++j) {
                    index[i * cols + j]    = 0;
                    expected[i * cols + j] = a[i * cols];
                }
            check("gather_duplicate", rows, cols, count, gather_columns(rows, cols, input, count ? index : NULL),
                  expected);
            memcpy(expected, a, count * sizeof(*a));
            for (uint64_t i = 0; i < rows; ++i)
                for (uint64_t j = 0; j < cols; ++j) {
                    index[i * cols + j] = 0;
                    expected[i * cols]  = updates[i * cols + j];
                }
            check("scatter", rows, cols, count,
                  scatter_columns(rows, cols, input, count ? index : NULL, count ? updates : NULL), expected);

            for (uint64_t i = 0; i < rows; ++i)
                for (uint64_t j = 0; j < cols; ++j) {
                    index[i * cols + j]    = rows - 1 - i;
                    expected[i * cols + j] = a[(rows - 1 - i) * cols + j];
                }
            check("gather_rows", rows, cols, count, gather_rows(rows, cols, input, count ? index : NULL), expected);
            for (uint64_t i = 0; i < rows; ++i)
                for (uint64_t j = 0; j < cols; ++j) {
                    index[i * cols + j]    = 0;
                    expected[i * cols + j] = a[j];
                }
            check("gather_rows_duplicate", rows, cols, count, gather_rows(rows, cols, input, count ? index : NULL),
                  expected);
            memcpy(expected, a, count * sizeof(*a));
            for (uint64_t i = 0; i < rows; ++i)
                for (uint64_t j = 0; j < cols; ++j) {
                    index[i * cols + j] = 0;
                    expected[j]         = updates[i * cols + j];
                }
            check("scatter_rows", rows, cols, count,
                  scatter_rows(rows, cols, input, count ? index : NULL, count ? updates : NULL), expected);
            check("scatter_empty_updates", rows, cols, count, scatter_some_rows(rows, cols, 0, input, NULL, NULL), a);

            if (rows && cols) {
                for (uint64_t i = 0; i < rows + 2; ++i)
                    for (uint64_t j = 0; j < cols + 2; ++j) {
                        uint64_t si                  = i == 0 ? 0 : i > rows ? rows - 1 : i - 1;
                        uint64_t sj                  = j == 0 ? 0 : j > cols ? cols - 1 : j - 1;
                        expected[i * (cols + 2) + j] = a[si * cols + sj];
                    }
                check("replicate", rows, cols, (rows + 2) * (cols + 2), replicate_matrix(rows, cols, a), expected);
            }
            for (uint64_t i = 0; i < count; ++i)
                if (a[i] != (int32_t)(i * 3 + 11)) return 2;
        }
    for (unsigned r = 0; r < 4; ++r) {
        uint64_t rows = sizes[r];
        int32_t a[14], b[21], expected[35];
        for (uint64_t i = 0; i < 2 * rows; ++i)
            a[i] = (int32_t)(i + 17);
        for (uint64_t i = 0; i < 3 * rows; ++i)
            b[i] = (int32_t)(i + 53);
        for (uint64_t i = 0; i < rows; ++i)
            for (uint64_t j = 0; j < 5; ++j)
                expected[5 * i + j] = j < 2 ? a[2 * i + j] : b[3 * i + j - 2];
        check("concat", rows, 5, 5 * rows, concat_columns(rows, rows ? a : NULL, rows ? b : NULL), expected);
        for (uint64_t i = 0; i < 2 * rows; ++i)
            if (a[i] != (int32_t)(i + 17)) return 2;
        for (uint64_t i = 0; i < 3 * rows; ++i)
            if (b[i] != (int32_t)(i + 53)) return 2;
    }
    if (failures) return 1;
    puts("dynamic operators: 16 shapes passed");
    return 0;
}
