#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern int32_t* concat_rows(uint64_t n, uint64_t m, uint64_t cols, int32_t* a, int32_t* b);
extern int32_t* concat_cols(uint64_t rows, uint64_t n, uint64_t m, int32_t* a, int32_t* b);
extern int32_t* concat_mixed(uint64_t n, int32_t* a, int32_t* b);
extern int32_t* concat_three(uint64_t n, uint64_t m, int32_t* a, int32_t* b, int32_t* c);

static int failures;
static int checked;

static void check(const char* op, uint64_t count, const int32_t* out, const int32_t* expected) {
    ++checked;
    for (uint64_t i = 0; i < count; ++i)
        if (out[i] != expected[i]) {
            fprintf(stderr, "%s: element %llu is %d, expected %d\n", op, (unsigned long long)i, out[i], expected[i]);
            ++failures;
            break;
        }
}

static int32_t* fill(uint64_t count, int32_t base) {
    int32_t* p = count ? malloc(count * sizeof(int32_t)) : NULL; // a zero-extent input is passed as NULL
    for (uint64_t i = 0; i < count; ++i)
        p[i] = base + (int32_t)i;
    return p;
}

int main(void) {
    const uint64_t sizes[] = {0, 1, 3, 5};
    for (unsigned x = 0; x < 4; ++x)
        for (unsigned y = 0; y < 4; ++y) {
            uint64_t n = sizes[x], m = sizes[y], cols = 3;

            int32_t *a = fill(n * cols, 100), *b = fill(m * cols, 500), expected[8 * 3];
            for (uint64_t i = 0; i < n * cols; ++i)
                expected[i] = a[i];
            for (uint64_t i = 0; i < m * cols; ++i)
                expected[n * cols + i] = b[i];
            int32_t* out = (n + m) * cols ? concat_rows(n, m, cols, a, b) : NULL;
            if (out) check("concat_rows", (n + m) * cols, out, expected);
            free(out);
            free(a);
            free(b);

            uint64_t rows = 2;
            a             = fill(rows * n, 100);
            b             = fill(rows * m, 500);
            for (uint64_t i = 0; i < rows; ++i) {
                for (uint64_t j = 0; j < n; ++j)
                    expected[i * (n + m) + j] = a[i * n + j];
                for (uint64_t j = 0; j < m; ++j)
                    expected[i * (n + m) + n + j] = b[i * m + j];
            }
            out = (n + m) ? concat_cols(rows, n, m, a, b) : NULL;
            if (out) check("concat_cols", rows * (n + m), out, expected);
            free(out);
            free(a);
            free(b);

            int32_t* three = fill(3, 900);
            a              = fill(n, 100);
            for (uint64_t i = 0; i < n; ++i)
                expected[i] = a[i];
            for (uint64_t i = 0; i < 3; ++i)
                expected[n + i] = three[i];
            out = concat_mixed(n, a, three);
            check("concat_mixed", n + 3, out, expected);
            free(out);
            free(a);

            a          = fill(n, 100);
            int32_t* c = fill(m, 500);
            for (uint64_t i = 0; i < n; ++i)
                expected[i] = a[i];
            for (uint64_t i = 0; i < 3; ++i)
                expected[n + i] = three[i];
            for (uint64_t i = 0; i < m; ++i)
                expected[n + 3 + i] = c[i];
            out = concat_three(n, m, a, three, c);
            check("concat_three", n + 3 + m, out, expected);
            free(out);
            free(a);
            free(c);
            free(three);
        }
    if (failures) return 1;
    printf("dynamic concat: %d shapes passed\n", checked);
    return 0;
}
