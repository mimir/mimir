#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fstream>
#include <string>

// TEMP DIAGNOSTIC: in-process crash backtrace on Windows CI (asserts/AVs otherwise die silently).
#ifdef _WIN32
#    include <csignal>

#    include <exception>

#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
// clang-format off
#    include <windows.h>
#    include <dbghelp.h>
// clang-format on
#    ifdef _DEBUG
#        include <crtdbg.h>
#    endif
// <windows.h> leaks these macros into the mim/lyra headers below; drop them.
#    undef ERROR
#    undef min
#    undef max

static void mim_backtrace(CONTEXT* ctx) {
    auto proc   = GetCurrentProcess();
    auto thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);

    STACKFRAME64 f = {};
    f.AddrPC.Mode = f.AddrFrame.Mode = f.AddrStack.Mode = AddrModeFlat;
    f.AddrPC.Offset                                     = ctx->Rip;
    f.AddrFrame.Offset                                  = ctx->Rbp;
    f.AddrStack.Offset                                  = ctx->Rsp;

    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &f, ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr)
            || f.AddrPC.Offset == 0)
            break;

        char buf[sizeof(SYMBOL_INFO) + 512] = {};
        auto sym                            = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct                   = sizeof(SYMBOL_INFO);
        sym->MaxNameLen                     = 512;
        DWORD64 disp                        = 0;
        std::fprintf(stderr, "  #%02d ", i);
        if (SymFromAddr(proc, f.AddrPC.Offset, &disp, sym))
            std::fprintf(stderr, "%s", sym->Name);
        else
            std::fprintf(stderr, "0x%llx", (unsigned long long)f.AddrPC.Offset);
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct    = sizeof(line);
        DWORD ldisp          = 0;
        if (SymGetLineFromAddr64(proc, f.AddrPC.Offset, &ldisp, &line))
            std::fprintf(stderr, "  (%s:%lu)", line.FileName, line.LineNumber);
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
}

static LONG WINAPI mim_seh_filter(EXCEPTION_POINTERS* ep) {
    std::fprintf(stderr, "\n*** unhandled exception 0x%08lx ***\n", ep->ExceptionRecord->ExceptionCode);
    mim_backtrace(ep->ContextRecord);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void mim_sigabrt(int) {
    std::fprintf(stderr, "\n*** SIGABRT (assert/abort) ***\n");
    CONTEXT ctx = {};
    RtlCaptureContext(&ctx);
    mim_backtrace(&ctx);
    ExitProcess(134);
}

static void mim_terminate() {
    std::fprintf(stderr, "\n*** std::terminate ***\n");
    CONTEXT ctx = {};
    RtlCaptureContext(&ctx);
    mim_backtrace(&ctx);
    ExitProcess(134);
}
#endif

#include <lyra/lyra.hpp>

#include "mim/config.h"
#include "mim/driver.h"
#include "mim/flags.h"
#include "mim/phase.h"
#include "mim/sexpr.h"

#include "mim/ast/parser.h"
#include "mim/phase/optimize.h"
#include "mim/util/sys.h"

using namespace mim;
using namespace std::literals;

int main(int argc, char** argv) {
    enum Backends { AST, Dot, H, PY, Md, Mim, Nest, SExpr, SlottedSExpr, ProfileTrace, Num_Backends };

    // TEMP DIAGNOSTIC: make crash/assert output visible on Windows CI (buffered/dialog-routed by default).
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::setvbuf(stdout, nullptr, _IONBF, 0); // so a teardown crash can't swallow already-produced output
#ifdef _WIN32
    ULONG stack_guarantee = 64 * 1024; // reserve stack so the filter can run on a stack overflow
    SetThreadStackGuarantee(&stack_guarantee);
    SetUnhandledExceptionFilter(mim_seh_filter);
    std::signal(SIGABRT, mim_sigabrt);
    std::set_terminate(mim_terminate);
#    ifdef _DEBUG
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // no WER dialog; let our handler run
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#    endif
#endif

    try {
        Driver driver;
        bool show_help           = false;
        bool show_version        = false;
        bool list_search_paths   = false;
        bool sexpr_include_types = false;
        DotConfig dot;
        std::string input, prefix;
        std::string clang = sys::find_cmd("clang");
        std::vector<std::string> plugins, search_paths;
#ifdef MIM_ENABLE_CHECKS
        std::vector<uint32_t> breakpoints;
        std::vector<uint32_t> watchpoints;
#endif
        std::array<std::string, Num_Backends> output;
        int verbose      = 0;
        auto inc_verbose = [&](bool) { ++verbose; };
        auto& flags      = driver.flags();

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
        auto cli = lyra::cli()
            | lyra::help(show_help)
            | lyra::opt(show_version                       )["-v"]["--version"              ]("Display version info and exit.")
            | lyra::opt(list_search_paths                  )["-l"]["--list-search-paths"    ]("List search paths in order and exit.")
            | lyra::opt(clang,        "clang"              )["-c"]["--clang"                ]("Path to clang executable (default: '" MIM_WHICH " clang').")
            | lyra::opt(plugins,      "plugin"             )["-p"]["--plugin"               ]("Dynamically load plugin.")
            | lyra::opt(search_paths, "path"               )["-P"]["--plugin-path"          ]("Path to search for plugins.")
            | lyra::opt(inc_verbose                        )["-V"]["--verbose"              ]("Verbose mode. Multiple -V options increase the verbosity. The maximum is 4.").cardinality(0, 5)
            | lyra::opt(output[AST],  "file"               )      ["--output-ast"           ]("Directly emits AST representation of input.")
            | lyra::opt(output[Dot],  "file"               )      ["--output-dot"           ]("Emits the Mim program as a MimIR graph using Graphviz' DOT language.")
            | lyra::opt(output[H  ],  "file"               )      ["--output-h"             ]("Emits a header file to be used to interface with a plugin in C++.")
            | lyra::opt(output[PY ],  "file"               )      ["--output-py"            ]("Emits a Python enum to be used to interface with a plugin in Python.")
            | lyra::opt(output[Md ],  "file"               )      ["--output-md"            ]("Emits the input formatted as Markdown.")
            | lyra::opt(output[Mim],  "file"               )["-o"]["--output-mim"           ]("Emits the Mim program again.")
            | lyra::opt(output[Nest], "file"               )      ["--output-nest"          ]("Emits program nesting tree as Dot.")
            | lyra::opt(output[SExpr],"file"               )      ["--output-sexpr"         ]("Emits the program as symbolic expression.")
            | lyra::opt(output[SlottedSExpr],"file"        )      ["--output-sexpr-slotted" ]("Emits the program as symbolic expression that follows the format required by slotted-egraphs.")
            | lyra::opt(flags.force_load                   )      ["--force-load"           ]("Load plugins even on version mismatch.")
            | lyra::opt(profile, "|summary|tree|trace"     )      ["--profile"              ]("Measure how long each phase takes and write a summary, tree or chrome://tracing compatible output to the output-profile provided destination.")
            | lyra::opt(profile_path, "file"               )      ["--output-profile"       ]("The output path (or '-' for stdout) for the profiling information.")
            | lyra::opt(flags.ascii                        )["-a"]["--ascii"                ]("Use ASCII alternatives in output instead of UTF-8.")
            | lyra::opt(flags.bootstrap                    )      ["--bootstrap"            ]("Puts mim into \"bootstrap mode\". This means a 'plugin' directive has the same effect as an 'import' and will not load a library. In addition, no standard plugins will be loaded.")
            | lyra::opt(sexpr_include_types                )      ["--sexpr-include-types"  ]("Wraps symbolic expression terms in a type annotation. Types will not be wrapped in type annotations.")
            | lyra::opt(dot.follow_types                   )      ["--dot-follow-types"     ]("Follow type dependencies in DOT output.")
            | lyra::opt(dot.all_annexes                    )      ["--dot-all-annexes"      ]("Output all annexes - even if unused - in DOT output.")
            | lyra::opt(dot.inline_consts                  )      ["--dot-inline-consts"    ]("Wire up literals, axioms, etc. with normal edges in DOT output instead of detaching them into a separate row; useful for small graphs.")
            | lyra::opt(dot.default_filter                 )      ["--dot-default-filter"   ]("Always show a lambda's filter in DOT output - even if it is the default one (ff for continuations, tt for direct-style functions).")
            | lyra::opt(dot.show_hidden                    )      ["--dot-show-hidden"      ]("Render otherwise-transparent detached edges in DOT output (Var->binder back-edges, shared literals/axioms, and type edges) in a subtle gray instead of fully transparent.")
            | lyra::opt(flags.dump_recursive               )      ["--dump-recursive"       ]("Dumps Mim program with a simple recursive algorithm that is not readable again from Mim but is less fragile and also works for broken Mim programs.")
            | lyra::opt(flags.aggressive_lam_spec          )      ["--aggr-lam-spec"        ]("Overrides LamSpec behavior to follow recursive calls.")
            | lyra::opt(flags.scalarize_threshold, "threshold")   ["--scalarize-threshold"  ]("MimIR will not scalarize tuples/packs/sigmas/arrays with a number of elements greater than or equal this threshold.")
#ifdef MIM_ENABLE_CHECKS
            | lyra::opt(breakpoints,    "gid"              )["-b"]["--break"                ]("*Triggers breakpoint when creating a node whose global id is <gid>.")
            | lyra::opt(watchpoints,    "gid"              )["-w"]["--watch"                ]("*Triggers breakpoint when setting a node whose global id is <gid>.")
            | lyra::opt(flags.reeval_breakpoints           )      ["--reeval-breakpoints"   ]("*Triggers breakpoint even upon unfying a node that has already been built.")
            | lyra::opt(flags.break_on_alpha               )      ["--break-on-alpha"       ]("*Triggers breakpoint as soon as two expressions turn out to be not alpha-equivalent.")
            | lyra::opt(flags.break_on_error               )      ["--break-on-error"       ]("*Triggers breakpoint on ELOG.")
            | lyra::opt(flags.break_on_warn                )      ["--break-on-warn"        ]("*Triggers breakpoint on WLOG.")
            | lyra::opt(flags.trace_gids                   )      ["--trace-gids"           ]("*Output gids during World::unify/insert.")
#endif
            | lyra::arg(input,          "file"             )                                  ("Input file.")
            ;
        // clang-format on

        if (auto result = cli.parse({argc, argv}); !result) throw std::invalid_argument(result.message());

        if (show_help) {
            std::cout << cli << std::endl;
#ifdef MIM_ENABLE_CHECKS
            std::cout << "*These are developer options only enabled, if 'MIM_ENABLE_CHECKS' is ON." << std::endl;
#endif
            std::cout << "Use \"-\" as <file> to output to stdout." << std::endl;
            return EXIT_SUCCESS;
        }

        if (show_version) {
            std::cout << "mim " << driver.version() << std::endl;
            std::exit(EXIT_SUCCESS);
        }

        for (auto&& path : search_paths)
            driver.add_search_path(path);

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
        driver.log().set(&std::cerr).set((Log::Level)verbose);

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
        if (input[0] == '-' || input.substr(0, 2) == "--")
            throw std::invalid_argument("error: unknown option " + input);

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
                error("couldn't read file '{}'", input);
            }
        } catch (const Error& e) { // e.loc.path doesn't exist anymore in outer scope so catch Error here
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

#ifdef _WIN32
    std::fprintf(stderr, "[mim] reached end of main; returning EXIT_SUCCESS\n"); // TEMP DIAGNOSTIC
    std::fflush(stderr);
#endif
    return EXIT_SUCCESS;
}
