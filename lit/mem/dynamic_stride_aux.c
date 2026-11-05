#include <stdint.h>
#include <stdio.h>

struct Record {
    int8_t tag;
    int32_t value;
};

extern int32_t update_matrix(uint64_t rows, uint64_t cols, int32_t* a, int32_t value);
extern int32_t read_fixed_rows(uint64_t cols, int32_t* a);
extern int32_t read_cube(uint64_t rows, uint64_t cols, uint64_t depth, int32_t* a);
extern int32_t read_record(uint64_t rows, uint64_t cols, struct Record* a);

int main(void) {
    const uint64_t sizes[] = {1, 3, 5};
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 3; ++c)
            for (unsigned d = 0; d < 3; ++d) {
                uint64_t rows = sizes[r], cols = sizes[c], depth = sizes[d];
                int32_t matrix[25], cube[125];
                struct Record records[25];
                for (uint64_t i = 0; i < rows * cols; ++i) {
                    matrix[i]  = (int32_t)(i + 100);
                    records[i] = (struct Record){(int8_t)i, (int32_t)(i + 200)};
                }
                for (uint64_t i = 0; i < rows * cols * depth; ++i)
                    cube[i] = (int32_t)(i + 300);
                uint64_t last = rows * cols - 1;
                if (rows == 3 && read_fixed_rows(cols, matrix) != (int32_t)(last + 100)) return 5;
                if (update_matrix(rows, cols, matrix, 999) != (int32_t)(last + 100)) return 1;
                for (uint64_t i = 0; i <= last; ++i)
                    if (matrix[i] != (i == last ? 999 : (int32_t)(i + 100))) return 2;
                if (read_cube(rows, cols, depth, cube) != (int32_t)(rows * cols * depth - 1 + 300)) return 3;
                if (read_record(rows, cols, records) != (int32_t)(last + 200)) return 4;
            }
    puts("dependent pointer strides: 27 shapes passed");
    return 0;
}
