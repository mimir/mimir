#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern int32_t* set_first(uint64_t n, int32_t* a);
extern int32_t read_at(uint64_t n, uint64_t i, int32_t* a);
extern int32_t* pad_vector(uint64_t n, int32_t* a);
extern int32_t* pad_matrix(uint64_t rows, uint64_t cols, int32_t* a);

static void check(const char* op, uint64_t size, uint64_t i, int32_t got, int32_t expected) {
    if (got != expected) {
        fprintf(stderr, "%s size %llu index %llu: got %d, expected %d\n", op, (unsigned long long)size,
                (unsigned long long)i, got, expected);
        exit(1);
    }
}

int main(void) {
    const uint64_t sizes[] = {1, 3, 7};
    for (unsigned s = 0; s < 3; ++s) {
        uint64_t n = sizes[s];
        int32_t a[14];
        for (uint64_t i = 0; i < 2 * n; ++i)
            a[i] = (int32_t)(i + 11);
        int32_t* out = set_first(n, a);
        for (uint64_t i = 0; i < 2 * n; ++i) {
            check("set", n, i, out[i], i == 0 ? 7 : (int32_t)(i + 11));
            check("source", n, i, a[i], (int32_t)(i + 11));
        }
        free(out);
        for (uint64_t i = 0; i < n; ++i)
            check("read_at", n, i, read_at(n, i, a), a[i]);
    }

    const uint64_t pad_sizes[] = {0, 1, 3};
    for (unsigned r = 0; r < 3; ++r) {
        uint64_t rows    = pad_sizes[r];
        int32_t vector[] = {21, 22, 23};
        int32_t* out     = pad_vector(rows, rows ? vector : NULL);
        for (uint64_t i = 0; i < rows + 2; ++i)
            check("pad_vector", rows, i, out[i], i > 0 && i <= rows ? vector[i - 1] : 7);
        free(out);
        for (unsigned c = 0; c < 3; ++c) {
            uint64_t cols = pad_sizes[c];
            int32_t a[9];
            for (uint64_t i = 0; i < rows * cols; ++i)
                a[i] = (int32_t)(i + 31);
            out = pad_matrix(rows, cols, rows * cols ? a : NULL);
            for (uint64_t i = 0; i < rows + 2; ++i)
                for (uint64_t j = 0; j < cols + 2; ++j) {
                    int32_t expected = i > 0 && i <= rows && j > 0 && j <= cols ? a[(i - 1) * cols + j - 1] : 7;
                    check("pad_matrix", rows * cols, i * (cols + 2) + j, out[i * (cols + 2) + j], expected);
                }
            free(out);
        }
    }
    puts("dynamic boundaries: PASS");
    return 0;
}
