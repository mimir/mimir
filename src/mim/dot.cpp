#include <fstream>
#include <limits>
#include <ostream>
#include <sstream>

#include "mim/def.h"
#include "mim/nest.h"
#include "mim/world.h"

using namespace std::string_literals;

namespace mim {

namespace {

template<class T>
std::string escape(const T& val) {
    std::ostringstream oss;
    oss << val;
    auto str = oss.str();
    find_and_replace(str, "<", "&lt;");
    find_and_replace(str, ">", "&gt;");
    return str;
}

class Dot {
public:
    Dot(std::ostream& ostream, bool types, const Def* root = nullptr)
        : os_(ostream)
        , types_(types)
        , root_(root) {}

    void prologue() {
        std::println(os_, "{}digraph {{", tab_);
        ++tab_;
        std::println(os_, "{}ordering=out;", tab_);
        std::println(os_, "{}splines=ortho;", tab_);
        std::println(os_, "{}newrank=true;", tab_);
        std::println(os_, "{}nodesep=0.6;", tab_);
        std::println(os_, "{}ranksep=1.2;", tab_);
        std::println(os_, "{}node [shape=box,style=filled,fontname=\"monospace\"];", tab_);
    }

    void epilogue() {
        --tab_;
        std::println(os_, "{}}}", tab_);
    }

    void run(const Def* root, int max) {
        prologue();
        recurse(root, max);
        epilogue();
    }

    void recurse(const Def* def, int max) {
        if (max == 0 || !done_.emplace(def).second) return;

        std::print(os_, "{}_{}[", tab_, def->gid());

        if (def->isa_mut())
            if (def == root_)
                os_ << "style=\"filled,diagonals,bold\"";
            else
                os_ << "style=\"filled,diagonals\",penwidth=2";
        else if (def == root_)
            os_ << "style=\"filled,bold\"";

        label(def) << ',';
        color(def) << ',';
        if (def->is_closed()) os_ << "rank=min,";
        tooltip(def) << "];\n";

        if (def->is_set()) {
            for (size_t i = 0, e = def->num_ops(); i != e; ++i) {
                auto op = def->op(i);
                recurse(op, max - 1);
                if (op->isa<Lit>() || op->isa<Axm>() || def->isa<Var>() || def->isa<Nat>() || def->isa<Idx>())
                    std::println(os_, "{}_{}:{} -> _{}[color=\"#00000000\",constraint=false];", tab_, def->gid(), i,
                                 op->gid());
                else
                    std::println(os_, "{}_{}:{} -> _{};", tab_, def->gid(), i, op->gid());
            }
        }

        if (auto t = def->type(); t && types_) {
            recurse(t, max - 1);
            std::println(os_, "{}_{} -> _{}[color=\"#00000000\",constraint=false,style=dashed];", tab_, def->gid(),
                         t->gid());
        }
    }

    std::ostream& label(const Def* def) {
        auto n = def->is_set() ? def->num_ops() : size_t(0);
        if (n > 0) {
            std::print(os_, "label=<<table border=\"0\" cellspacing=\"0\" cellpadding=\"0\"><tr><td colspan=\"{}\">",
                       n);
            emit_name(def);
            os_ << "</td></tr><tr>";
            for (size_t i = 0; i < n; ++i)
                std::print(os_, "<td port=\"{}\" cellpadding=\"0\" height=\"1\" width=\"8\"></td>", i);
            os_ << "</tr></table>>";
        } else {
            os_ << "label=<";
            emit_name(def);
            os_ << ">";
        }
        return os_;
    }

    void emit_name(const Def* def) {
        if (auto lit = def->isa<Lit>())
            os_ << lit;
        else
            os_ << def->node_name();
        std::print(os_, "<br/><font point-size=\"9\">{}</font>", escape(def->unique_name()));
    }

    std::ostream& color(const Def* def) {
        float hue;
        // clang-format off
        if      (def->is_form())  hue = 0.60f; // blue   - type formation
        else if (def->is_intro()) hue = 0.35f; // green  - introduction
        else if (def->is_elim())  hue = 0.00f; // red    - elimination
        else if (def->is_meta())  hue = 0.15f; // yellow - universe/meta
        else                      hue = 0.80f; // purple - Hole
        // clang-format on
        return os_ << std::format("fillcolor=\"{} 0.5 0.75\"", hue);
    }

    std::ostream& tooltip(const Def* def) {
        static constexpr auto NL = "&#13;&#10;"; // newline

        auto loc  = escape(def->loc());
        auto type = escape(def->type());
        std::print(os_, "tooltip=\"");
        std::print(os_, "<b>expr:</b> {}{}", def, NL);
        std::print(os_, "<b>type:</b> {}{}", type, NL);
        std::print(os_, "<b>name:</b> {}{}", def->sym(), NL);
        std::print(os_, "<b>gid:</b> {}{}", def->gid(), NL);
        std::print(os_, "<b>flags:</b> 0x{:x}{}", def->flags(), NL);
        std::print(os_, "<b>mark:</b> 0x{:x}{}", def->mark(), NL);
        std::print(os_, "<b>local_muts:</b> {}{}", fe::Join(def->local_muts()), NL);
        std::print(os_, "<b>local_vars:</b> {}{}", fe::Join(def->local_vars()), NL);
        std::print(os_, "<b>free_vars:</b> {}{}", fe::Join(def->free_vars()), NL);
        if (auto mut = def->isa_mut()) std::print(os_, "<b>users:</b> {{{}}}{}", fe::Join(mut->users()), NL);
        std::print(os_, "<b>loc:</b> {}", loc);
        return os_ << std::format("\"");
    }

private:
    std::ostream& os_;
    bool types_;
    const Def* root_;
    fe::Tab tab_ = fe::Tab::spaces();
    DefSet done_;
};

} // namespace

void Def::dot(std::ostream& ostream, int max, bool types) const { Dot(ostream, types, this).run(this, max); }

void Def::dot(const char* file, int max, bool types) const {
    if (!file) {
        dot(std::cout, max, types);
    } else {
        auto of = std::ofstream(file);
        dot(of, max, types);
    }
}

void World::dot(const char* file, bool annexes, bool types) const {
    if (!file) {
        dot(std::cout, annexes, types);
    } else {
        auto of = std::ofstream(file);
        dot(of, annexes, types);
    }
}

void World::dot(std::ostream& os, bool anx, bool types) const {
    Dot dot(os, types);
    dot.prologue();
    for (auto external : externals().muts())
        dot.recurse(external, std::numeric_limits<int>::max());
    if (anx)
        for (auto annex : annexes().defs())
            dot.recurse(annex, std::numeric_limits<int>::max());
    dot.epilogue();
}

/*
 * Nest
 */

void Nest::dot(const char* file) const {
    if (!file) {
        dot(std::cout);
    } else {
        auto of = std::ofstream(file);
        dot(of);
    }
}

void Nest::dot(std::ostream& os) const {
    auto tab = fe::Tab::spaces();
    std::println(os, "{}digraph {{", tab);
    ++tab;
    std::println(os, "{}ordering=out;", tab);
    std::println(os, "{}node [shape=box,style=filled];", tab);
    root()->dot(tab, os);
    --tab;
    std::println(os, "{}}}", tab);
}

void Nest::Node::dot(fe::Tab tab, std::ostream& os) const {
    std::string s;
    for (const auto& scc : topo_) {
        s += '[';
        for (auto sep = ""s; auto n : *scc) {
            s += sep + n->name();
            sep = ", ";
        }
        s += "] ";
    }

    for (auto sibl : sibl_deps())
        std::println(os, "{}\"{}\":s -> \"{}\":s [style=dashed,constraint=false,splines=true]", tab, name(),
                     sibl->name());

    auto rec  = is_mutually_recursive() ? "rec*" : (is_directly_recursive() ? "rec" : "");
    auto html = "<b>" + name() + "</b>";
    if (*rec) html += "<br/><i>"s + rec + "</i>";
    html += "<br/><font point-size=\"8\">depth " + std::to_string(loop_depth()) + "</font>";
    std::println(os, "{}\"{}\" [label=<{}>,tooltip=\"{}\"]", tab, name(), html, s);
    for (auto child : children().nodes()) {
        std::println(os, "{}\"{}\" -> \"{}\" [splines=false]", tab, name(), child->name());
        child->dot(tab, os);
    }

    // Overlay domination between siblings and their parent
    if (idom())
        std::println(os, "{}\"{}\" -> \"{}\" [color=red,style=bold,constraint=false]", tab, idom()->name(), name());
}

} // namespace mim
