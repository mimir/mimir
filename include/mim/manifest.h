#pragma once

#include <filesystem>
#include <string>

#include <fe/vector.h>

namespace mim {

/// Project manifest from a `mim.ini` file.
/// Holds the keys needed to compile a project: plugins, search paths, and plugin arguments.
struct Manifest {
    fe::Vector<std::string> plugins;      ///< `-p`/`--plugin`.
    fe::Vector<std::string> plugin_paths; ///< `-P`/`--plugin-path`.
    fe::Vector<std::string> import_paths; ///< `-I`/`--import-path`.
    fe::Vector<std::string> prefix_paths; ///< `-R`/`--prefix-path`.
    fe::Vector<std::string> plugin_args;  ///< `-X`/`--plugin-arg` as `<plugin>:<arg>`.
};

/// Parses the manifest at @p file; throws `std::invalid_argument` on any error.
/// Relative `*-path` entries resolve against the manifest file's directory.
Manifest load_manifest(const std::filesystem::path& file);

/// Manifest keys; each matches the long name of the corresponding CLI option without the leading `--`.
inline constexpr auto Plugin_Key      = "plugin";
inline constexpr auto Plugin_Path_Key = "plugin-path";
inline constexpr auto Import_Path_Key = "import-path";
inline constexpr auto Prefix_Path_Key = "prefix-path";
inline constexpr auto Plugin_Arg_Key  = "plugin-arg";

/// Names of the environment variables that add to the corresponding `*-path` keys of a Manifest.
inline constexpr auto Plugin_Path_Env = "MIM_PLUGIN_PATH";
inline constexpr auto Import_Path_Env = "MIM_IMPORT_PATH";
inline constexpr auto Prefix_Path_Env = "MIM_PREFIX_PATH";

/// Splits the value of environment variable @p var on the platform's path separator.
/// Returns an empty list when the variable is unset; skips empty components.
fe::Vector<std::string> paths_from_env(const char* var);

} // namespace mim
