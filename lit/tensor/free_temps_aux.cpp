// Counts the allocations `lin` makes against the ones it frees; only its result may stay allocated.
#include <cstdio>
#include <cstdlib>

#include <vector>

extern "C" {
void* __real_malloc(size_t);
void __real_free(void*);
float* lin(float*, float*, float*);

static bool counting = false;
static int mallocs = 0, frees = 0;

void* __wrap_malloc(size_t n) {
    if (counting) ++mallocs;
    return __real_malloc(n);
}

void __wrap_free(void* p) {
    if (counting && p) ++frees;
    __real_free(p);
}
}

int main() {
    std::vector<float> x(64 * 256, 1.0f), w(128 * 256, 1.0f), b(128, 1.0f);
    counting = true;
    float* y = lin(x.data(), w.data(), b.data());
    counting = false;
    __real_free(y);
    if (frees + 1 != mallocs) {
        std::printf("lin made %d allocations but freed %d; only its result may stay allocated\n", mallocs, frees);
        return 1;
    }
    return 0;
}
