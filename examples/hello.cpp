#include <mim/driver.h>

#include <mim/ast/parser.h>
#include <mim/pass/optimize.h>
#include <mim/util/sys.h>

#include <mim/plug/mem/mem.h>

using namespace mim;
using namespace mim::plug;

int main(int, char**) {
    try {
        auto driver = Driver("hello");
        auto& w     = driver.world();
        driver.log().set(&std::cerr).set(Log::Level::Debug);
        ast::load_plugins(w, View<std::string>{"core", "ll"});

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

        sys::system("clang hello.ll -o hello -Wno-override-module");
        std::println("exit code: {}", sys::system("./hello a b c"));
    } catch (const std::exception& e) {
        std::println(std::cerr, "{}", e.what());
        return EXIT_FAILURE;
    } catch (...) {
        std::println(std::cerr, "error: unknown exception");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
