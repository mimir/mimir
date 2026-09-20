#include <fstream>
#include <ostream>
#include <print>

#include "mim/def.h"
#include "mim/driver.h"
#include "mim/lam.h"
#include "mim/nest.h"
#include "mim/world.h"

using namespace std::string_literals;

namespace mim {

namespace {

/// Formats and escapes for a Graphviz HTML-like label (`label=<...>`).
template<class... Args>
std::string html(std::format_string<Args...> fmt, Args&&... args) {
    std::string res;
    for (auto c : std::format(fmt, std::forward<Args>(args)...)) {
        switch (c) {
            case '&': res += "&amp;"; break;
            case '<': res += "&lt;"; break;
            case '>': res += "&gt;"; break;
            default: res += c;
        }
    }
    return res;
}

/// Escapes @p s for a double-quoted Graphviz string such as `tooltip`.
/// Graphviz turns `\n` back into a newline, so a tooltip may span several lines.
std::string quote(std::string_view s) {
    std::string res;
    for (auto c : s) {
        switch (c) {
            case '\\': res += "\\\\"; break;
            case '"': res += "\\\""; break;
            case '\n': res += "\\n"; break;
            default: res += c;
        }
    }
    return res;
}

// Edges and arrowheads are black by default, which vanishes on a dark backdrop.
constexpr auto DARK_FG = "#c9d1d9";

class Dot {
public:
    Dot(std::ostream& ostream, DotConfig cfg, const Def* root = nullptr)
        : os_(ostream)
        , cfg_(cfg)
        , root_(root) {}

    void prologue() {
        std::println(os_, "{}digraph {{", tab_);
        ++tab_;
        // The embedder paints the backdrop; only what is drawn on it has to change.
        if (cfg_.dark) {
            std::println(os_, "{}bgcolor=\"transparent\";", tab_);
            std::println(os_, "{}edge [color=\"{}\"];", tab_, DARK_FG);
        }
        std::println(os_, "{}ordering=out;", tab_);
        std::println(os_, "{}splines=ortho;", tab_);
        std::println(os_, "{}newrank=true;", tab_);
        std::println(os_, "{}margin=0;", tab_);
        // inline mode is meant for small graphs, so we tighten the spacing.
        std::println(os_, "{}nodesep={};", tab_, cfg_.inline_consts ? "0.25" : "0.6");
        std::println(os_, "{}ranksep={};", tab_, cfg_.inline_consts ? "0.4" : "1.2");
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

    /// Emits a single node with id @p nid for @p def.
    /// The same @p def may be emitted under several ids when inline_ duplicates shared leaves.
    void emit_node(std::string_view nid, const Def* def) {
        std::print(os_, "{}{}[", tab_, nid);

        if (def->isa_mut())
            if (def == root_)
                std::print(os_, "style=\"filled,diagonals,bold\",");
            else
                std::print(os_, "style=\"filled,diagonals\",penwidth=2,");
        else if (def == root_)
            std::print(os_, "style=\"filled,bold\",");

        label(def);
        color(def);
        // Pin closed defs to the top row.
        // In inline mode only mutables are pinned, so shared leaves flow next to their users instead of piling up in a
        // detached row.
        if (def->is_closed() && (!cfg_.inline_consts || def->isa_mut())) std::print(os_, "rank=min,");
        tooltip(def);
        std::println(os_, "];");
    }

    void recurse(const Def* def, int max) {
        if (max == 0 || !done_.emplace(def).second) return;

        emit_node(std::format("_{}", def->gid()), def);

        if (def->is_set()) {
            for (size_t i = 0, e = def->num_ops(); i != e; ++i) {
                auto op = def->op(i);
                // By default hide a Lam::filter() that still carries its kind's default: continuations default to ff,
                // direct-style functions to tt.
                if (!cfg_.default_filter && i == 0)
                    if (auto lam = def->isa<Lam>();
                        lam && lam->filter() == (Lam::isa_cn(lam) ? lam->world().lit_ff() : lam->world().lit_tt()))
                        continue;

                // Literals and axioms are heavily shared, so by default we detach their edges (invisible and
                // non-constraining) to keep the layout readable. With inline_ we instead duplicate such a leaf per use:
                // each reference gets its own local node, which avoids a star of long shared edges - handy for small
                // graphs. A Var points back at its binder, so its edge is always detached to avoid long back-edges.
                if (cfg_.inline_consts && (op->isa<Lit>() || op->isa<Axm>())) {
                    auto dup = std::format("_{}_{}", def->gid(), i);
                    emit_node(dup, op);
                    std::println(os_, "{}_{}:{} -> {};", tab_, def->gid(), i, dup);
                    type_edge(dup, op, max - 1);
                } else {
                    recurse(op, max - 1);
                    bool detach = def->isa<Var>()
                               || (!cfg_.inline_consts
                                   && (op->isa<Lit>() || op->isa<Axm>() || def->isa<Nat>() || def->isa<Idx>()));
                    if (detach) {
                        // Detached edges are transparent by default to keep the layout readable (xdot still
                        // highlights them on hover). With show_hidden we render them statically in a subtle gray.
                        auto edge_color = cfg_.show_hidden ? "gray" : "#00000000";
                        std::println(os_, "{}_{}:{} -> _{}[color=\"{}\",constraint=false];", tab_, def->gid(), i,
                                     op->gid(), edge_color);
                    } else
                        std::println(os_, "{}_{}:{} -> _{};", tab_, def->gid(), i, op->gid());
                }
            }
        }

        type_edge(std::format("_{}", def->gid()), def, max - 1);
    }

    /// Recurses into @p def's Def::type() and wires a type edge from node @p nid to it if DotConfig::follow_types.
    /// Shared by the normal and the inline_consts duplication path so both honor follow_types uniformly.
    void type_edge(std::string_view nid, const Def* def, int max) {
        if (auto t = def->type(); t && cfg_.follow_types) {
            recurse(t, max);
            auto edge_color = cfg_.show_hidden ? "gray" : "#00000000";
            std::println(os_, "{}{} -> _{}[color=\"{}\",constraint=false,style=dashed];", tab_, nid, t->gid(),
                         edge_color);
        }
    }

    /// One port per op, so an edge docks under the very op it stands for.
    void label(const Def* def) {
        auto n = def->is_set() ? def->num_ops() : size_t(0);
        if (n > 0) {
            std::print(os_, "label=<<table border=\"0\" cellspacing=\"0\" cellpadding=\"0\"><tr><td colspan=\"{}\">",
                       n);
            emit_name(def);
            std::print(os_, "</td></tr><tr>");
            for (size_t i = 0; i < n; ++i)
                std::print(os_, "<td port=\"{}\" cellpadding=\"0\" height=\"1\" width=\"8\"></td>", i);
            std::print(os_, "</tr></table>>,");
        } else {
            std::print(os_, "label=<");
            emit_name(def);
            std::print(os_, ">,");
        }
    }

    void emit_name(const Def* def) {
        auto lit  = def->isa<Lit>();
        auto name = lit ? html("{}", lit) : html("{}", def->node_name());
        std::print(os_, "{}<br/><font point-size=\"9\">{}</font>", name, html("{}", def->unique_name()));
    }

    void color(const Def* def) {
        float hue;
        // clang-format off
        if      (def->is_form())  hue = 0.60f; // blue   - type formation
        else if (def->is_intro()) hue = 0.35f; // green  - introduction
        else if (def->is_elim())  hue = 0.00f; // red    - elimination
        else if (def->is_meta())  hue = 0.15f; // yellow - universe/meta
        else                      hue = 0.80f; // purple - Hole
        // clang-format on
        std::print(os_, "fillcolor=\"{} 0.5 0.75\",", hue);
    }

    /// A tooltip is plain text - markup would show up verbatim in xdot and in the browser alike.
    void tooltip(const Def* def) {
        std::string s;
        auto add = [&, sep = ""](std::string_view key, const auto& val) mutable {
            s += std::format("{}{}: {}", sep, key, val);
            sep = "\n";
        };

        add("expr", def);
        add("type", def->type());
        add("name", def->sym());
        add("gid", def->gid());
        add("flags", std::format("0x{:x}", def->flags()));
        add("mark", std::format("0x{:x}", def->mark()));
        add("local_muts", fe::Join(def->local_muts()));
        add("local_vars", fe::Join(def->local_vars()));
        add("free_vars", fe::Join(def->free_vars()));
        if (auto mut = def->isa_mut()) add("users", std::format("{{{}}}", fe::Join(mut->users())));
        add("loc", def->loc());

        std::print(os_, "tooltip=\"{}\",", quote(s));
    }

private:
    std::ostream& os_;
    DotConfig cfg_;
    const Def* root_;
    fe::Tab tab_ = fe::Tab::spaces();
    DefSet done_;
};

} // namespace

void Def::dot(std::ostream& ostream, DotConfig cfg) const { Dot(ostream, cfg, this).run(this, cfg.max); }

void Def::dot(const char* file, DotConfig cfg) const {
    if (!file) {
        dot(std::cout, cfg);
    } else {
        auto of = std::ofstream(file);
        dot(of, cfg);
    }
}

void World::dot(const char* file, DotConfig cfg) const {
    if (!file) {
        dot(std::cout, cfg);
    } else {
        auto of = std::ofstream(file);
        dot(of, cfg);
    }
}

void World::dot(std::ostream& os, DotConfig cfg) const {
    Dot dot(os, cfg);
    dot.prologue();
    for (auto external : externals().muts())
        dot.recurse(external, cfg.max);
    if (cfg.all_annexes)
        for (auto annex : annexes().defs())
            dot.recurse(annex, cfg.max);
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

    auto rec   = is_mutually_recursive() ? "rec*" : (is_directly_recursive() ? "rec" : "");
    auto label = std::format("<b>{}</b>", html("{}", name()));
    if (*rec) label += std::format("<br/><i>{}</i>", rec);
    label += std::format("<br/><font point-size=\"8\">depth {}</font>", loop_depth());
    std::println(os, "{}\"{}\" [label=<{}>,tooltip=\"{}\"]", tab, name(), label, quote(s));
    for (auto child : children().nodes()) {
        std::println(os, "{}\"{}\" -> \"{}\" [splines=false]", tab, name(), child->name());
        child->dot(tab, os);
    }

    // Overlay domination between siblings and their parent
    if (idom())
        std::println(os, "{}\"{}\" -> \"{}\" [color=red,style=bold,constraint=false]", tab, idom()->name(), name());
}

} // namespace mim
