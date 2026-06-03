#include "mim/util/sys.h"

#include <filesystem>
#include <iostream>

#include "mim/config.h"

#include "mim/util/dbg.h"

#ifdef _WIN32
#    include <windows.h>
#    define popen  _popen
#    define pclose _pclose
#    define WEXITSTATUS
#elif defined(__APPLE__) || defined(__linux__)
#    include <dlfcn.h>
#endif

using namespace std::string_literals;

extern "C" MIM_EXPORT void mim_lib_anchor() {}

namespace mim::sys {

namespace {

bool has_plugin_dir(const fs::path& libmim_path) {
    std::error_code ignore;
    return fs::is_directory(libmim_path.parent_path() / "mim", ignore) && !ignore;
}

fs::path adjust_libmim_path(const fs::path& libmim_path) {
    if (has_plugin_dir(libmim_path)) return libmim_path;

    auto dir      = libmim_path.parent_path();
    auto lib_name = libmim_path.filename();
    while (!dir.empty()) {
        if (dir == dir.root_path()) break;

        std::error_code ignore;
        auto candidate = dir / MIM_LIBDIR / "mim";
        if (fs::is_directory(candidate, ignore) && !ignore) return candidate.parent_path() / lib_name;

        dir = dir.parent_path();
    }

    return libmim_path;
}

} // namespace

std::optional<fs::path> path_to_libmim() {
#if defined(_WIN32)
    HMODULE mod = nullptr;
    auto flags  = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (!GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&mim_lib_anchor), &mod)) return {};

    std::wstring buf;
    buf.resize(512);
    while (true) {
        DWORD len = GetModuleFileNameW(mod, buf.data(), (DWORD)buf.size());
        if (len == 0) return {};

        if (len < buf.size() - 1) {
            buf.resize(len);
            break;
        }

        buf.resize(buf.size() * 2); // buffer too small
    }

    return adjust_libmim_path(fs::weakly_canonical(fs::path(buf)));
#elif defined(__APPLE__) || defined(__linux__)
    Dl_info info;
    if (dladdr((void*)&mim_lib_anchor, &info) == 0) return {};
    return adjust_libmim_path(fs::weakly_canonical(info.dli_fname));
#else
    return {};
#endif
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
