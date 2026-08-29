#include <fe/sys.h>

#include <mim/driver.h>

#include <mim/ast/parser.h>
#include <mim/phase/optimize.h>

#include <mim/plug/mem/mem.h>

using namespace mim;
using namespace mim::plug;

int main(int, char**) {
    auto driver = Driver("hello"); // outlives the handlers below: an Error's Locs point into its SrcMap

    try {
        auto& w = driver.world();
        driver.log().set(&std::cerr).set(fe::Log::Level::Debug);
        ast::load_plugins(w, fe::View<std::string>{"core", "ll"});

        // Cn [%mem.M 0, I32, %mem.Ptr (I32, 0) Cn [%mem.M 0, I32]]
        auto mem_t  = w.call<mem::M>(0);
        auto argv_t = w.call<mem::Ptr0>(w.call<mem::Ptr0>(w.type_i32()));
        auto main   = w.mut_fun({mem_t, w.type_i32(), argv_t}, {mem_t, w.type_i32()})->set("main");

        auto [mem, argc, argv] = main->var(2, 0)->projs<3>();
        auto ret               = main->var(2, 1);
        main->app(false, ret, {mem, argc});
        main->externalize();

        // the `ll` plugin's emit phase writes `hello.ll` as part of `optimize`
        optimize(w);

        fe::sys::system("clang hello.ll -o hello -Wno-override-module");
        std::println("exit code: {}", fe::sys::system("./hello a b c"));
    } catch (const std::exception& e) {
        std::println(std::cerr, "{}", e.what());
        return EXIT_FAILURE;
    } catch (...) {
        std::println(std::cerr, "error: unknown exception");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
