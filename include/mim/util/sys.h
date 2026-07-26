#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#    define MIM_WHICH "where"
#else
#    define MIM_WHICH "which"
#endif

namespace mim {

namespace fs = std::filesystem;

namespace sys {

/// Yields `std::nullopt` if an error occurred.
/// The returned path is adjusted so `<parent>/mim` resolves to the default in-tree plugin directory when possible.
std::optional<fs::path> path_to_libmim();

/// Executes command @p cmd.
/// @returns the output as string.
std::string exec(std::string cmd);

std::string find_cmd(std::string);

/// Thrown by sys::require_cmd when a command cannot be located on the system.
class CmdNotFound : public std::logic_error {
public:
    CmdNotFound(const std::string& s)
        : std::logic_error(s) {}
};

/// Locates @p name on the system or throws CmdNotFound.
std::string require_cmd(std::string_view name);

/// Wraps `std::system` and makes the return value usable.
int system(std::string);

/// Runs @p cmd via sys::system and throws if it exits with a non-zero status.
void require_run(const std::string& cmd);

/// Wraps sys::system and puts `.exe` at the back (Windows) and `./` at the front (otherwise) of @p cmd.
int run(std::string cmd, std::string args = {});

/// Returns the @p path as `std::string` and escapes all whitespaces with backslash.
std::string escape(const std::filesystem::path& path);

} // namespace sys
} // namespace mim
