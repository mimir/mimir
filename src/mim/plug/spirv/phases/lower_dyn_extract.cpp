#include "mim/rewrite.h"
#include "mim/world.h"
namespace mim::plug::spirv {

class LowerDynExtract : public Rewriter {
public:
    const Def* rewrite_imm_Extract(const Extract* extract) override {
        if (!extract->index()->is_closed()) {
            auto& world = Rewriter::world();
            world.
        }
        return Rewriter::rewrite_imm_Extract(extract);
    }
};

} // namespace mim::plug::spirv
