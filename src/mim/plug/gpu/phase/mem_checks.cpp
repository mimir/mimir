#include "mim/plug/gpu/phase/mem_checks.h"

#include <mim/axm.h>

#include <mim/plug/mem/mem.h>

namespace mim::plug::gpu::phase {

namespace {

class MemFinder {
public:
    MemFinder(const Def* to_search)
        : to_search({to_search}) {}
    MemFinder(DefVec&& to_search)
        : to_search(std::move(to_search)) {}

    using IsaMem = Axm::IsA<mem::M, mim::App>;

    IsaMem next_mem();

private:
    DefVec to_search;
};

MemFinder::IsaMem MemFinder::next_mem() {
    while (!to_search.empty()) {
        auto cur = to_search.back();
        to_search.pop_back();
        if (cur->isa<Sigma>())
            for (auto op : cur->ops())
                to_search.push_back(op);
        else if (auto mem = Axm::isa<mem::M>(cur))
            return mem;
    }
    return IsaMem();
}

} // namespace

const Def* MemChecks::rewrite_imm_App(const App* app) {
    if (auto launch = Axm::isa<gpu::launch>(app)) {
        auto kernel        = launch->decurry()->decurry()->arg();
        auto kernel_args   = launch->decurry()->arg();
        auto kernel_args_t = kernel_args->type();

        MemFinder mem_finder(kernel_args_t);
        if (mem_finder.next_mem()) {
            error("You may not pass any %mem.M across device boundaries: passing {} : {} from host to kernel '{}'",
                  kernel_args, kernel_args_t, kernel);
        }
    }
    return Super::rewrite_imm_App(app);
}

void MemChecks::rewrite_external(Def* def) {
    auto lam = def->isa<Lam>();
    if (lam && lam->sym().str() == "main") {
        MemFinder intype_mem_finder(lam->type()->dom());
        while (auto mem = intype_mem_finder.next_mem()) {
            auto addr_space = mem->arg();
            if (Lit::as(addr_space) != 0)
                error("The main function may not take %mem.M n with a non-zero n as an argument");
        }

        MemFinder outtype_mem_finder(lam->type()->ret_dom());
        while (auto mem = outtype_mem_finder.next_mem()) {
            auto addr_space = mem->arg();
            if (Lit::as(addr_space) != 0) error("The main function may not return any %mem.M n with a non-zero n");
        }
    }
    Super::rewrite_external(def);
}

} // namespace mim::plug::gpu::phase
