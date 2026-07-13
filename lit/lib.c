#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef linux
// TODO: use platform independent time functions
#    include <sys/time.h>
#endif

// #define printf(...) do {} while (0)

void print_char(char c) { printf("%c", c); }
void println_char(char c) { printf("%c\n", c); }
void print_str(const char* s) { printf("%s", s); }
void println_str(const char* s) { printf("%s\n", s); }
void print_i32(int32_t i) { printf("%" PRId32, i); }
void println_i32(int32_t i) { printf("%" PRId32 "\n", i); }
void newline() { printf("\n"); }

void print_integer(int i) { printf("%d, ", i); }
void print_int_newline(int i) { printf("%d\n", i); }
void print_newline() { printf("\n"); }
void print_int_vector(int n, int* v) {
    for (int i = 0; i < n; i++)
        print_integer(v[i]);
    print_newline();
}
void print_int_matrix(int n, int m, int* v) {
    for (int i = 0; i < n; i++)
        print_int_vector(m, v + i * m);
}
//
void print_float(float f) { printf("%.2f, ", f); }
void print_float_newline(float f) { printf("%.2f\n", f); }
void print_float_vector(int n, float* v) {
    for (int i = 0; i < n; i++)
        print_float(v[i]);
    print_newline();
}
void print_float_matrix(int n, int m, float* v) {
    for (int i = 0; i < n; i++)
        print_float_vector(m, v + i * m);
}
// double
void print_double(double d) { printf("%.2f, ", d); }
void print_double_newline(double f) { printf("%.2f\n", f); }
void print_double_vector(int n, double* v) {
    for (int i = 0; i < n; i++)
        print_double(v[i]);
    print_newline();
}
void print_double_matrix(int n, int m, double* v) {
    for (int i = 0; i < n; i++)
        print_double_vector(m, v + i * m);
}

#ifdef linux
void* time() {
    struct timeval* tv = (struct timeval*)malloc(sizeof(*tv));
    gettimeofday(tv, NULL);
    return (void*)tv;
}

static float tdiff(struct timeval* start, struct timeval* end) {
    return (end->tv_sec - start->tv_sec) + 1e-6 * (end->tv_usec - start->tv_usec);
}

void print_time_diff(void* tv1, void* tv2) {
    printf("real\t%0.6f \n", tdiff((struct timeval*)tv1, (struct timeval*)tv2));
}
#else
void* time() { return NULL; }
void print_time_diff(void* tv1, void* tv2) {}
static float tdiff(struct timeval* start, struct timeval* end) { return 0; }
#endif
