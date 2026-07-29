#include "mim/util/dl.h"

#include "mim/util/dbg.h"

#ifdef _WIN32
#    include <windows.h>
#else
#    include <dlfcn.h>
#endif

namespace mim::dl {

void* open(const char* file) {
#ifdef _WIN32
    if (HMODULE handle = LoadLibraryA(file)) {
        return static_cast<void*>(handle);
    } else {
        fe::throwf("could not load plugin '{}' due to error '{}'\n"
                   "see https://docs.microsoft.com/en-us/windows/win32/debug/system-error-codes\n",
                   file, GetLastError());
    }
#else
    if (void* handle = dlopen(file, RTLD_NOW))
        return handle;
    else if (auto err = dlerror())
        fe::throwf("could not load plugin '{}' due to error '{}'\n", file, err);
    else
        fe::throwf("could not load plugin '{}'\n", file);
#endif
}

void* get(void* handle, const char* symbol) {
#ifdef _WIN32
    if (auto addr = GetProcAddress(static_cast<HMODULE>(handle), symbol)) {
        return reinterpret_cast<void*>(addr);
    } else {
        fe::throwf("could not find symbol '{}' in plugin due to error '{}'\n"
                   "see (https://docs.microsoft.com/en-us/windows/win32/debug/system-error-codes)\n",
                   symbol, GetLastError());
    }
#else
    dlerror(); // clear error state
    void* addr = dlsym(handle, symbol);
    if (auto err = dlerror())
        fe::throwf("could not find symbol '{}' in plugin due to error '{}' \n", symbol, err);
    else
        return addr;
#endif
}

void close(void* handle) {
#ifdef _WIN32
    if (!FreeLibrary(static_cast<HMODULE>(handle))) fe::throwf("FreeLibrary() failed\n");
#else
    if (auto err = dlclose(handle)) fe::throwf("dlclose() failed with error code '{}'\n", err);
#endif
}

} // namespace mim::dl
