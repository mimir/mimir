#include "mim/util/sys.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <vector>

#include "mim/util/print.h"

#ifdef _WIN32
#    include <windows.h>
#    define popen  _popen
#    define pclose _pclose
#    define WEXITSTATUS
#elif defined(__APPLE__)
#    include <mach-o/dyld.h>
#    include <unistd.h>
#else
#    include <dlfcn.h>
#    include <unistd.h>
#endif

using namespace std::string_literals;

namespace mim::sys {

std::optional<fs::path> path_to_curr_exe() {
    std::vector<char> path_buffer;
#ifdef __APPLE__
    uint32_t read = 0;
    _NSGetExecutablePath(nullptr, &read); // get size
    path_buffer.resize(read + 1);
    if (_NSGetExecutablePath(path_buffer.data(), &read) != 0) return {};
    return fs::path{path_buffer.data()};
#elif defined(_WIN32)
    size_t read = 0;
    do {
        // start with 256 (almost MAX_PATH) and grow exp
        path_buffer.resize(std::max(path_buffer.size(), static_cast<size_t>(128)) * 2);
        read = GetModuleFileNameA(nullptr, path_buffer.data(), static_cast<DWORD>(path_buffer.size()));
    } while (read == path_buffer.size()); // if equal, the buffer was too small.

    if (read != 0) {
        path_buffer.resize(read + 1);
        path_buffer.back() = 0;
        return fs::path{path_buffer.data()};
    }
#else  // Linux only..
    if (fs::exists("/proc/self/exe")) return fs::canonical("/proc/self/exe");
#endif // __APPLE__
    return {};
}

// see https://stackoverflow.com/a/478960
std::string exec(std::string cmd) {
    using PipeCloser = int (*)(FILE*); // spell out type explicitly to get rid of warning
    if (auto pipe = std::unique_ptr<FILE, PipeCloser>(popen(cmd.c_str(), "r"), pclose)) {
        std::array<char, 128> buffer;
        std::string result;
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
            result += buffer.data();
        return result;
    } else
        error("popen() failed!");
}

std::string find_cmd(std::string cmd) {
    auto out = exec(MIM_WHICH " "s + cmd);
    if (auto it = out.find('\n'); it != std::string::npos) out.erase(it);
    return out;
}

int system(std::string cmd) {
    std::cout << cmd << std::endl;
    int status = std::system(cmd.c_str());
    return WEXITSTATUS(status);
}

int run(std::string cmd, std::string args /* = {}*/) {
#ifdef _WIN32
    cmd += ".exe";
#else
    cmd = "./"s + cmd;
#endif
    return sys::system(cmd + " "s + args);
}

std::string escape(const std::filesystem::path& path) {
    std::string str;
    for (char c : path.string()) {
        if (isspace(c)) str += '\\';
        str += c;
    }
    return str;
}

} // namespace mim::sys
