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
        bool show_plugin_args    = false;
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
        auto cli = fe::cli::Cli("mim", "MimIR is my Intermediate Representation.")
            | fe::cli::help(show_help)
            | fe::cli::opt(show_help_md                          )      ["--help-md"              ]("Displays this help as Markdown and exits.")
            | fe::cli::opt(show_plugin_args                      )      ["--plugin-args-md"       ]("Displays the -X arguments the loaded plugins understand as Markdown and exits.")
            | fe::cli::opt(show_version                          )["-v"]["--version"              ]("Displays version info and exits.")
            | fe::cli::opt(list_search_paths                     )["-l"]["--list-search-paths"    ]("Lists the search paths in order and exits.")
            | fe::cli::opt(clang,             "clang"            )["-c"]["--clang"                ]("Path to clang executable.")
            | fe::cli::opt(plugins,           "plugin"           )["-p"]["--plugin"               ]("Dynamically loads a plugin.")
            | fe::cli::opt(search_paths,      "path"             )["-P"]["--plugin-path"          ]("Path to search for plugins.")
            | fe::cli::opt(plugin_args,       "plugin:arg"       )["-X"]["--plugin-arg"           ]("Passes an argument to a plugin/phase, e.g. -X ll:--target=sm_80. Repeatable.")
            | fe::cli::opt(flags.force_load                      )      ["--force-load"           ]("Loads plugins even on version mismatch.")
            | fe::cli::opt(flags.bootstrap                       )      ["--bootstrap"            ]("Bootstrap mode: a 'plugin' directive acts as an 'import' and loads no library; no standard plugins are loaded either.")
            | fe::cli::opt(inc_verbose                           )["-V"]["--verbose"              ]("Raises the log level from error to warn, info, verbose, debug, trace; repeatable.").cardinality(0, 5)
            | fe::cli::opt(flags.ascii                           )["-a"]["--ascii"                ]("Uses ASCII alternatives in output instead of UTF-8.")
            | fe::cli::group("Output")
            | fe::cli::opt(output[Mim],       "file"             )["-o"]["--output-mim"           ]("Emits the Mim program again.")
            | fe::cli::opt(output[AST],       "file"             )      ["--output-ast"           ]("Emits the AST of the input.")
            | fe::cli::opt(output[Dot],       "file"             )      ["--output-dot"           ]("Emits the Mim program as a MimIR graph using Graphviz' DOT language.")
            | fe::cli::opt(output[H  ],       "file"             )      ["--output-h"             ]("Emits a header file to be used to interface with a plugin in C++.")
            | fe::cli::opt(output[PY ],       "file"             )      ["--output-py"            ]("Emits a Python enum to be used to interface with a plugin in Python.")
            | fe::cli::opt(output[Md ],       "file"             )      ["--output-md"            ]("Emits the input formatted as Markdown.")
            | fe::cli::opt(output[Nest],      "file"             )      ["--output-nest"          ]("Emits the program's nesting tree using Graphviz' DOT language.")
            | fe::cli::opt(output[SExpr],     "file"             )      ["--output-sexpr"         ]("Emits the program as symbolic expression.")
            | fe::cli::opt(output[SlottedSExpr], "file"          )      ["--output-sexpr-slotted" ]("Emits the program as symbolic expression that follows the format required by slotted-egraphs.")
            | fe::cli::opt(sexpr_include_types                   )      ["--sexpr-include-types"  ]("Wraps each term of a symbolic expression in a type annotation; types themselves stay unwrapped.")
            | fe::cli::opt(flags.dump_recursive                  )      ["--dump-recursive"       ]("Dumps the Mim program with a simple recursive algorithm; the result is not readable again but works for broken programs.")
            | fe::cli::group("DOT Output")
            | fe::cli::opt(dot.follow_types                      )      ["--dot-follow-types"     ]("Follows type dependencies in DOT output.")
            | fe::cli::opt(dot.all_annexes                       )      ["--dot-all-annexes"      ]("Emits all annexes in DOT output - even unused ones.")
            | fe::cli::opt(dot.inline_consts                     )      ["--dot-inline-consts"    ]("Wires up literals, axioms, etc. with normal edges in DOT output instead of detaching them into a separate row; useful for small graphs.")
            | fe::cli::opt(dot.default_filter                    )      ["--dot-default-filter"   ]("Always shows a lambda's filter in DOT output - even if it is the default one (ff for continuations, tt for direct-style functions).")
            | fe::cli::opt(dot.show_hidden                       )      ["--dot-show-hidden"      ]("Renders otherwise-transparent detached edges in DOT output - back-edges from a Var to its binder, shared literals/axioms, and type edges - in a subtle gray.")
            | fe::cli::group("Diagnostics")
            | fe::cli::opt(loc_style,         "style"            )      ["--loc-style"            ]("How a diagnostic spells out a source location: full (path:row:col-row:col), rowcol (path:row:col), row (path:row), or msvc (path(row,col)).")
            | fe::cli::opt(driver.diag().no_snippet              )      ["--no-snippet"           ]("Does not render the offending source line and caret underneath a diagnostic.")
            | fe::cli::opt(driver.diag().gutter,    "width"      )      ["--gutter"               ]("Width of a diagnostic's line-number column.")
            | fe::cli::opt(driver.diag().max_rows,  "num"        )      ["--max-rows"             ]("Maximum number of rows a diagnostic's snippet renders before eliding its middle; 0 elides nothing.")
            | fe::cli::opt(driver.diag().max_errors, "num"       )      ["--max-errors"           ]("Maximum number of errors to report before dropping the rest; 0 reports all of them.")
            | fe::cli::opt(driver.diag().werror                  )      ["--werror"               ]("Treats warnings as errors.")
            | fe::cli::group("Profiling")
            | fe::cli::opt(profile,           "mode"             )      ["--profile"              ]("Measures how long each phase takes and writes the result to --output-profile; <mode> is summary, tree, or trace (chrome://tracing compatible).")
            | fe::cli::opt(profile_path,      "file"             )      ["--output-profile"       ]("Where to write the profiling information.")
            | fe::cli::group("Optimization")
            | fe::cli::opt(flags.aggressive_lam_spec             )      ["--aggr-lam-spec"        ]("Overrides LamSpec behavior to follow recursive calls.")
            | fe::cli::opt(flags.scalarize_threshold, "threshold")      ["--scalarize-threshold"  ]("MimIR will not scalarize tuples/packs/sigmas/arrays with a number of elements greater than or equal this threshold.")
            | fe::cli::opt(flags.max_fp_iters, "num"             )      ["--max-fp-iters"         ]("Maximum number of fixed-point iterations before a phase errors out; guards against non-monotone analyses.")
#ifdef MIM_ENABLE_CHECKS
            | fe::cli::group("Developer Options")
            | fe::cli::opt(breakpoints,       "gid"              )["-b"]["--break"                ]("Triggers a breakpoint when a node with this global id is created.")
            | fe::cli::opt(watchpoints,       "gid"              )["-w"]["--watch"                ]("Triggers a breakpoint when a node with this global id is set.")
            | fe::cli::opt(flags.reeval_breakpoints              )      ["--reeval-breakpoints"   ]("Triggers a breakpoint even upon unifying a node that has already been built.")
            | fe::cli::opt(flags.break_on_alpha                  )      ["--break-on-alpha"       ]("Triggers a breakpoint as soon as two expressions turn out not to be alpha-equivalent.")
            | fe::cli::opt(flags.break_on_error                  )      ["--break-on-error"       ]("Triggers a breakpoint on an error log.")
            | fe::cli::opt(flags.break_on_warn                   )      ["--break-on-warn"        ]("Triggers a breakpoint on a warning log.")
            | fe::cli::opt(flags.trace_gids                      )      ["--trace-gids"           ]("Outputs gids during World::unify/insert.")
#endif
            | fe::cli::arg(input,             "file"             )                                 ("Input file.")
            ;
        // clang-format on
        cli.epilog(R"(Every output option accepts "-" to write to stdout.)");

        if (auto res = cli.parse(argc, argv); !res) throw std::invalid_argument(res.message());

        if (show_help) {
            std::cout << cli;
            return EXIT_SUCCESS;
        }

        if (show_help_md) {
            cli.md(std::cout);
            return EXIT_SUCCESS;
        }

        if (show_plugin_args) {
            for (auto&& path : search_paths)
                driver.add_search_path(path);
            for (auto&& plugin : plugins)
                driver.load(driver.sym(plugin));

            for (const auto& [plugin, args] : driver.known_args()) {
                std::println(std::cout, "\n### {0} {{#xarg_{0}}}\n\n| Argument | Effect |\n| --- | --- |", plugin);
                for (const auto& arg : args)
                    std::println(std::cout, "| `{}` | {} |", arg.syntax, arg.descr);
            }
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
            driver.add_arg(driver.sym(pa.substr(0, pos)), pa.substr(pos + 1));
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
