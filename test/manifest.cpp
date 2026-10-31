#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <mim/manifest.h>

using namespace mim;

namespace fs = std::filesystem;

namespace {

/// Writes @p text to a manifest file in the temp dir and removes it when destroyed.
/// shortcut: fixed file name, so concurrent runs would clobber each other.
struct Manifest_File {
    explicit Manifest_File(std::string_view text)
        : path(fs::temp_directory_path() / "mim-manifest-test.toml") {
        std::ofstream(path) << text;
    }
    ~Manifest_File() { fs::remove(path); }
    fs::path path;
};

auto load(std::string_view text) { return load_manifest(Manifest_File(text).path); }

/// @returns the `what()` of the `std::invalid_argument` loading @p file raises, or "<no error>" if none does.
std::string error_of(const fs::path& file) {
    try {
        (void)load_manifest(file);
    } catch (const std::invalid_argument& e) { return e.what(); }
    return "<no error>";
}

std::string error_of(std::string_view text) { return error_of(Manifest_File{text}.path); }

bool same(const fe::Vector<std::string>& list, const std::vector<std::string>& ref) {
    return std::ranges::equal(list, ref);
}

} // namespace

TEST_CASE("Manifest") {
    SUBCASE("keys and string arrays") {
        auto manifest = load(R"(plugin = ["ll", "demo"]
import-path = []
plugin-arg = ['ll:o=x=y', "demo:greedy, aggressive"] # trailing comment
)");
        CHECK(same(manifest.plugins, {"ll", "demo"}));
        CHECK(manifest.import_paths.empty());
        CHECK(same(manifest.plugin_args, {"ll:o=x=y", "demo:greedy, aggressive"}));
    }

    SUBCASE("trailing comma") {
        auto manifest = load(R"(plugin = ["ll", "demo", ]
plugin-arg = ["ll:o=x",])");
        CHECK(same(manifest.plugins, {"ll", "demo"}));
        CHECK(same(manifest.plugin_args, {"ll:o=x"}));
    }

    SUBCASE("escapes and literal strings") {
        auto manifest = load(R"(plugin = ["a\tb\\c", 'a\tb\c'])");
        CHECK(same(manifest.plugins, {"a\tb\\c", "a\\tb\\c"}));
    }

    SUBCASE("relative paths anchor to the manifest dir") {
        Manifest_File file(R"(plugin-path = ["sub", "/abs"])");
        auto manifest = load_manifest(file.path);
        auto base     = file.path.parent_path();
        CHECK(manifest.plugin_paths[0] == (base / "sub").lexically_normal().string());
        CHECK(manifest.plugin_paths[1] == "/abs");
    }

    SUBCASE("errors") {
        auto check
            = [](std::string_view text, std::string_view what) { CHECK_MESSAGE(error_of(text).contains(what), what); };

        check("banana = [\"x\"]", "unknown manifest key `banana`");
        check("plugin = [\"a\"]\nplugin = [\"b\"]", "duplicate manifest key `plugin`");
        check("plugin = [\"ll\"]\n[demo]", "expected `<key>`");
        check("plugin = [\"abc", "unterminated string");
        check("plugin = [\"a\\x\"]", "invalid escape character `\\x`");
        check("plugin = [23]", "expected `<string>`");
        check("plugin = [\"a", "expected `]`");
        check("plugin-arg = [\"ll\"]", "<plugin>:<arg>");
        CHECK(error_of(fs::path{"does-not-exist.toml"}).contains("cannot read config file"));
    }
}
