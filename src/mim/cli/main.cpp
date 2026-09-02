#include <cstdlib>
#include <cstring>

#include <fstream>
#include <string>

#include <fe/cli.h>
#include <fe/sys.h>
#include <fe/term.h>

#include <mim/config.h>
#include <mim/driver.h>
#include <mim/flags.h>
#include <mim/phase.h>
#include <mim/sexpr.h>

#include <mim/ast/parser.h>
#include <mim/phase/optimize.h>

using namespace mim;
using namespace std::literals;

int main(int argc, char** argv) {
    enum Backends { AST, Dot, H, PY, Md, Mim, Nest, SExpr, SlottedSExpr, ProfileTrace, Num_Backends };

    fe::term::resolve_mode(); // colors in std::format-ed output depend on Auto being resolved up front

    Driver driver; // outlives the handlers below: an Error's Locs point into its SrcMap

    try {
        bool show_help           = false;
        bool show_help_md        = false;
        bool show_version        = false;
        bool list_search_paths   = false;
        bool sexpr_include_types = false;
        DotConfig dot;
        std::string input, prefix;
        std::string clang = fe::sys::find_cmd("clang");
        std::vector<std::string> plugins, search_paths, plugin_args;
#ifdef MIM_ENABLE_CHECKS
        std::vector<uint32_t> breakpoints;
        std::vector<uint32_t> watchpoints;
#endif
        std::array<std::string, Num_Backends> output;
        int verbose      = 0;
        auto inc_verbose = [&](bool) { ++verbose; };
        auto& flags      = driver.flags();

        auto loc_style = [&](const std::string& t) -> std::string {
            // clang-format off
            if (false) {}
            else if (t == "full"  ) driver.diag().loc_style = fe::Loc::Style::Full;
            else if (t == "rowcol") driver.diag().loc_style = fe::Loc::Style::RowCol;
            else if (t == "row"   ) driver.diag().loc_style = fe::Loc::Style::Row;
            else if (t == "msvc"  ) driver.diag().loc_style = fe::Loc::Style::MSVC;
            else return std::format("'{}' is not a location style", t);
            // clang-format on
            return {};
        };

        auto profile = [&](const std::string& t) {
            if (t == "tree")
                flags.profile = Flags::Profile::Tree;
            else if (t == "trace")
                flags.profile = Flags::Profile::Trace;
            else
                flags.profile = Flags::Profile::Summary;
            if (output[ProfileTrace].empty()) output[ProfileTrace] = "-";
        };
        auto profile_path = [&](const std::string& t) {
            if (t == "-" && flags.profile == mim::Flags::Profile::None)
                flags.profile = Flags::Profile::Summary;
            else
                flags.profile = Flags::Profile::Trace;
            output[ProfileTrace] = t;
        };

        // clang-format off
        auto cli = fe::Cli("mim", "MimIR is my Intermediate Representation.")
            .help(show_help)
            .opt(show_help_md             , ""          , ""  , "--help-md"             , "Displays this help as Markdown and exits.")
            .opt(show_version             , ""          , "-v", "--version"             , "Displays version info and exits.")
            .opt(list_search_paths        , ""          , "-l", "--list-search-paths"   , "Lists the search paths in order and exits.")
            .opt(clang                    , "clang"     , "-c", "--clang"               , "Path to clang executable.")
            .opt(plugins                  , "plugin"    , "-p", "--plugin"              , "Dynamically loads a plugin.")
            .opt(search_paths             , "path"      , "-P", "--plugin-path"         , "Path to search for plugins.")
            .opt(plugin_args              , "plugin:arg", "-X", "--plugin-arg"          , "Passes an argument to a plugin/phase, e.g. -X ll:o=output.ll. Repeatable.")
            .opt(flags.force_load         , ""          , ""  , "--force-load"          , "Loads plugins even on version mismatch.")
            .opt(flags.bootstrap          , ""          , ""  , "--bootstrap"           , "Bootstrap mode: a 'plugin' directive acts as an 'import' and loads no library; no standard plugins are loaded either.")
            .opt(inc_verbose              , ""          , "-V", "--verbose"             , "Raises the log level from error to warn, info, verbose, debug, trace; repeatable.").cardinality(0, 5)
            .opt(flags.ascii              , ""          , "-a", "--ascii"               , "Uses ASCII alternatives in output instead of UTF-8.")
            .grp("Output")
            .opt(output[Mim]              , "file"      , "-o", "--output-mim"          , "Emits the Mim program again.")
            .opt(output[AST]              , "file"      , ""  , "--output-ast"          , "Emits the AST of the input.")
            .opt(output[Dot]              , "file"      , ""  , "--output-dot"          , "Emits the Mim program as a MimIR graph using Graphviz' DOT language.")
            .opt(output[H]                , "file"      , ""  , "--output-h"            , "Emits a header file to be used to interface with a plugin in C++.")
            .opt(output[PY]               , "file"      , ""  , "--output-py"           , "Emits a Python enum to be used to interface with a plugin in Python.")
            .opt(output[Md]               , "file"      , ""  , "--output-md"           , "Emits the input formatted as Markdown.")
            .opt(output[Nest]             , "file"      , ""  , "--output-nest"         , "Emits the program's nesting tree using Graphviz' DOT language.")
            .opt(output[SExpr]            , "file"      , ""  , "--output-sexpr"        , "Emits the program as symbolic expression.")
            .opt(output[SlottedSExpr]     , "file"      , ""  , "--output-sexpr-slotted", "Emits the program as symbolic expression that follows the format required by slotted-egraphs.")
            .opt(sexpr_include_types      , ""          , ""  , "--sexpr-include-types" , "Wraps each term of a symbolic expression in a type annotation; types themselves stay unwrapped.")
            .opt(flags.dump_recursive     , ""          , ""  , "--dump-recursive"      , "Dumps the Mim program with a simple recursive algorithm; the result is not readable again but works for broken programs.")
            .grp("DOT Output")
            .opt(dot.follow_types         , ""          , ""  , "--dot-follow-types"    , "Follows type dependencies in DOT output.")
            .opt(dot.all_annexes          , ""          , ""  , "--dot-all-annexes"     , "Emits all annexes in DOT output - even unused ones.")
            .opt(dot.inline_consts        , ""          , ""  , "--dot-inline-consts"   , "Wires up literals, axioms, etc. with normal edges in DOT output instead of detaching them into a separate row; useful for small graphs.")
            .opt(dot.default_filter       , ""          , ""  , "--dot-default-filter"  , "Always shows a lambda's filter in DOT output - even if it is the default one (ff for continuations, tt for direct-style functions).")
            .opt(dot.show_hidden          , ""          , ""  , "--dot-show-hidden"     , "Renders otherwise-transparent detached edges in DOT output - back-edges from a Var to its binder, shared literals/axioms, and type edges - in a subtle gray.")
            .grp("Diagnostics")
            .opt(loc_style                , "style"     , ""  , "--loc-style"           , "How a diagnostic spells out a source location: full (path:row:col-row:col), rowcol (path:row:col), row (path:row), or msvc (path(row,col)).")
            .opt(driver.diag().no_snippet , ""          , ""  , "--no-snippet"          , "Does not render the offending source line and caret underneath a diagnostic.")
            .opt(driver.diag().gutter     , "width"     , ""  , "--gutter"              , "Width of a diagnostic's line-number column.")
            .opt(driver.diag().max_rows   , "num"       , ""  , "--max-rows"            , "Maximum number of rows a diagnostic's snippet renders before eliding its middle; 0 elides nothing.")
            .opt(driver.diag().max_errors , "num"       , ""  , "--max-errors"          , "Maximum number of errors to report before dropping the rest; 0 reports all of them.")
            .opt(driver.diag().werror     , ""          , ""  , "--werror"              , "Treats warnings as errors.")
            .grp("Profiling")
            .opt(profile                  , "mode"      , ""  , "--profile"             , "Measures how long each phase takes and writes the result to --output-profile; <mode> is summary, tree, or trace (chrome://tracing compatible).")
            .opt(profile_path             , "file"      , ""  , "--output-profile"      , "Where to write the profiling information.")
            .grp("Optimization")
            .opt(flags.aggressive_lam_spec, ""          , ""  , "--aggr-lam-spec"       , "Overrides LamSpec behavior to follow recursive calls.")
            .opt(flags.scalarize_threshold, "threshold" , ""  , "--scalarize-threshold" , "MimIR will not scalarize tuples/packs/sigmas/arrays with a number of elements greater than or equal this threshold.")
            .opt(flags.max_fp_iters       , "num"       , ""  , "--max-fp-iters"        , "Maximum number of fixed-point iterations before a phase errors out; guards against non-monotone analyses.")
#ifdef MIM_ENABLE_CHECKS
            .grp("Developer Options")
            .opt(breakpoints              , "gid"       , "-b", "--break"               , "Triggers a breakpoint when a node with this global id is created.")
            .opt(watchpoints              , "gid"       , "-w", "--watch"               , "Triggers a breakpoint when a node with this global id is set.")
            .opt(flags.reeval_breakpoints , ""          , ""  , "--reeval-breakpoints"  , "Triggers a breakpoint even upon unifying a node that has already been built.")
            .opt(flags.break_on_alpha     , ""          , ""  , "--break-on-alpha"      , "Triggers a breakpoint as soon as two expressions turn out not to be alpha-equivalent.")
            .opt(flags.break_on_error     , ""          , ""  , "--break-on-error"      , "Triggers a breakpoint on an error log.")
            .opt(flags.break_on_warn      , ""          , ""  , "--break-on-warn"       , "Triggers a breakpoint on a warning log.")
            .opt(flags.trace_gids         , ""          , ""  , "--trace-gids"          , "Outputs gids during World::unify/insert.")
#endif
            .arg(input, "file", "Input file.")
            .epilog(R"(Every output option accepts "-" to write to stdout.)");
        // clang-format on

        if (auto err = cli.parse(argc, argv)) throw std::invalid_argument(*err);

        if (show_help || show_help_md) {
            for (auto&& path : search_paths)
                driver.add_search_path(path);
            for (auto&& plugin : plugins) // a plugin declares its `-X` arguments in its shared library
                driver.load(plugin);

            if (!driver.known_args().empty()) cli.section("Plugin Arguments", "", {});
            for (const auto& [plugin, args] : driver.known_args()) {
                auto rows = std::vector<std::pair<std::string, std::string>>();
                for (const auto& arg : args)
                    rows.emplace_back(arg.syntax, arg.descr);
                // The Markdown gets an anchor, so that a plugin's own page can link to its table.
                auto title = show_help ? std::format("-X {}:<arg>", plugin)
                                       : std::format("-X {0}:<arg> {{#xarg_{0}}}", plugin);
                cli.section(std::move(title), "Argument", std::move(rows));
            }

            if (show_help)
                std::cout << cli;
            else
                cli.markdown(std::cout);
            return EXIT_SUCCESS;
        }

        if (show_version) {
            std::cout << "mim " << driver.version() << std::endl;
            std::exit(EXIT_SUCCESS);
        }

        for (auto&& path : search_paths)
            driver.add_search_path(path);

        for (auto&& pa : plugin_args) {
            auto pos = pa.find(':');
            if (pos == std::string::npos)
                throw std::invalid_argument("error: --plugin-arg expects <plugin>:<arg>, got '" + pa + "'");
            driver.add_arg(std::string_view(pa).substr(0, pos), pa.substr(pos + 1));
        }

        if (list_search_paths) {
            for (auto&& path : driver.search_paths() | std::views::drop(1)) // skip first empty path
                std::cout << path << std::endl;
            std::exit(EXIT_SUCCESS);
        }

        World& world = driver.world();
#ifdef MIM_ENABLE_CHECKS
        for (auto b : breakpoints)
            world.breakpoint(b);
        for (auto w : watchpoints)
            world.watchpoint(w);
#endif
        driver.log().set(&std::cerr).set((fe::Log::Level)verbose);
#ifdef MIM_ENABLE_CHECKS
        driver.log().break_on_error = flags.break_on_error;
        driver.log().break_on_warn  = flags.break_on_warn;
#endif

        // prepare output files and streams
        std::array<std::ofstream, Num_Backends> ofs;
        std::array<std::ostream*, Num_Backends> os;
        os.fill(nullptr);
        for (size_t be = 0; be != Num_Backends; ++be) {
            if (output[be].empty()) continue;
            if (output[be] == "-") {
                os[be] = &std::cout;
            } else {
                ofs[be].open(output[be]);
                os[be] = &ofs[be];
            }
        }

        if (input.empty()) throw std::invalid_argument("error: no input given");

        try {
            auto path = fs::path(input);
            world.set(path.filename().replace_extension().string());

            auto ast    = ast::AST(world);
            auto parser = ast::Parser(ast);

            if (auto mod = parser.import_main(input, plugins, os[Md])) {
                if (auto s = os[AST]) {
                    auto tab = fe::Tab::spaces();
                    mod->stream(tab, *s);
                }

                auto h  = os[H];
                auto py = os[PY];
                if (h || py) {
                    mod->bind(ast);
                    ast.error().ack();
                    auto plugin = world.sym(fs::path{path}.filename().replace_extension().string());
                    if (h) ast.bootstrap(plugin, *h);
                    if (py) ast.bootstrap_py(plugin, *py);
                    return EXIT_SUCCESS;
                }

                mod->compile(ast);
                optimize(world);

                if (auto s = os[Dot]) world.dot(*s, dot);
                if (auto s = os[Mim]) world.dump(*s);
                if (auto s = os[Nest]) mim::Nest(world).dot(*s);

                if (auto s = os[SExpr]) {
                    if (sexpr_include_types)
                        sexpr::emit_typed(world, *s);
                    else
                        sexpr::emit(world, *s);
                }
                if (auto s = os[SlottedSExpr]) {
                    if (sexpr_include_types)
                        sexpr::emit_slotted_typed(world, *s);
                    else
                        sexpr::emit_slotted(world, *s);
                }
                if (auto s = os[ProfileTrace]) {
                    switch (flags.profile) {
                        case Flags::Profile::Summary: driver.profiler().summary(*s); break;
                        case Flags::Profile::Tree: driver.profiler().tree(*s); break;
                        case Flags::Profile::Trace: driver.profiler().chrome_trace(*s); break;
                        case Flags::Profile::None: break;
                    }
                }
            } else {
                ast.error().ack(); // prefer the parser's own diagnostic, if it recorded one
                fe::throwf("could not read file `{}`", input);
            }
        } catch (const Error::Bail& e) {
            std::cerr << e;
            return EXIT_FAILURE;
        }
    } catch (const std::exception& e) {
        std::println(std::cerr, "{}", e.what());
        return EXIT_FAILURE;
    } catch (...) {
        std::println(std::cerr, "error: unknown exception");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
