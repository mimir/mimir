#include "pbqp/json.h"

#include <cassert>
#include <charconv>
#include <cstdint>

#include <limits>
#include <optional>
#include <print>
#include <string_view>
#include <utility>

#include <fe/driver.h>
#include <fe/lexer.h>
#include <fe/parser.h>

namespace pbqp::json {

namespace utf8 = fe::utf8;

// clang-format off
#define PBQP_JSON_KEY(m) \
    m(K_true,  "true")   \
    m(K_false, "false")  \
    m(K_null,  "null")   \

#define PBQP_JSON_VAL(m)         \
    m(V_str,       "<string>")   \
    m(V_int,       "<integer>")  \
    m(V_num,       "<number>")   \

#define PBQP_JSON_TOK(m)             \
    m(EoF,         "<end of file>")  \
    m(D_brace_l,   "{")              \
    m(D_brace_r,   "}")              \
    m(D_bracket_l, "[")              \
    m(D_bracket_r, "]")              \
    m(T_colon,     ":")              \
    m(T_comma,     ",") // clang-format on

namespace {

class Tok {
public:
    enum class Tag {
        Nil,
#define CODE(t, _) t,
        PBQP_JSON_KEY(CODE) PBQP_JSON_VAL(CODE) PBQP_JSON_TOK(CODE)
#undef CODE
    };

    Tok() {}
    Tok(fe::Loc loc, Tag tag)
        : loc_(loc)
        , tag_(tag) {}
    Tok(fe::Loc loc, Tag tag, fe::Sym sym)
        : loc_(loc)
        , tag_(tag)
        , sym_(sym) {}
    Tok(fe::Loc loc, std::int64_t i64)
        : loc_(loc)
        , tag_(Tag::V_int)
        , i64_(i64) {}

    fe::Loc loc() const { return loc_; }
    Tag tag() const { return tag_; }
    bool isa(Tag tag) const { return tag == tag_; }
    explicit operator bool() const { return tag_ != Tag::Nil; }

    fe::Sym sym() const {
        assert(isa(Tag::V_str) || isa(Tag::V_num));
        return sym_;
    }
    std::int64_t i64() const {
        assert(isa(Tag::V_int));
        return i64_;
    }

    static std::string_view tag2str(Tag tag) {
        switch (tag) {
#define CODE(t, str) \
    case Tag::t: return str;
            PBQP_JSON_KEY(CODE)
            PBQP_JSON_VAL(CODE)
            PBQP_JSON_TOK(CODE)
#undef CODE
            default: std::unreachable();
        }
    }

    friend std::ostream& operator<<(std::ostream& o, Tok tok) {
        if (tok.isa(Tag::V_str)) return o << '"' << tok.sym() << '"';
        if (tok.isa(Tag::V_num)) return o << tok.sym();
        if (tok.isa(Tag::V_int)) return o << tok.i64();
        return o << tag2str(tok.tag());
    }

private:
    fe::Loc loc_;
    Tag tag_ = Tag::Nil;
    union {
        fe::Sym sym_;
        std::int64_t i64_;
    };
};

} // namespace

} // namespace pbqp::json

#ifndef DOXYGEN
// clang-format off
template<> struct std::formatter<pbqp::json::Tok> : fe::ostream_formatter {};
// clang-format on
#endif

namespace pbqp::json {

namespace {

using Tag = Tok::Tag;

/// std::from_chars leaves @p res alone on overflow, so saturate instead.
std::int64_t to_i64(std::string_view sv) {
    std::int64_t res = 0;
    if (std::from_chars(sv.data(), sv.data() + sv.size(), res).ec == std::errc::result_out_of_range)
        return sv.starts_with('-') ? std::numeric_limits<std::int64_t>::min()
                                   : std::numeric_limits<std::int64_t>::max();
    return res;
}

class Lexer : public fe::Lexer<1, Lexer> {
public:
    Lexer(fe::Driver& driver, const fe::Src& src)
        : fe::Lexer<1, Lexer>(src)
        , driver_(driver) {}

    fe::Driver& driver() { return driver_; }

    Tok lex() {
        while (true) {
            start();

            if (accept(utf8::EoF)) return {loc_, Tag::EoF};
            if (accept(utf8::isspace)) continue;
            if (recover_utf8()) continue;
            if (accept('{')) return {loc_, Tag::D_brace_l};
            if (accept('}')) return {loc_, Tag::D_brace_r};
            if (accept('[')) return {loc_, Tag::D_bracket_l};
            if (accept(']')) return {loc_, Tag::D_bracket_r};
            if (accept(':')) return {loc_, Tag::T_colon};
            if (accept(',')) return {loc_, Tag::T_comma};
            if (accept('"')) return lex_string();
            if (ahead() == '-' || utf8::isdigit(ahead())) return lex_number();

            if (accept(utf8::isalpha)) {
                accept_while(utf8::isalpha);
#define CODE(t, str) \
    if (view() == str) return {loc_, Tag::t};
                PBQP_JSON_KEY(CODE)
#undef CODE
                error().e(loc_, "unknown literal `{}`", view());
                continue;
            }

            recover_char();
        }
    }

private:
    bool accept_digits() { return !accept_while(utf8::isdigit).empty(); }

    Tok lex_number() {
        accept('-');
        bool ok      = accept_digits();
        bool integer = true;
        if (accept('.')) {
            integer = false;
            ok      = accept_digits() && ok;
        }
        if (accept('e') || accept('E')) {
            integer = false;
            if (!accept('+')) accept('-');
            ok = accept_digits() && ok;
        }
        if (!ok) error().e(loc_, "malformed number `{}`", view());
        if (integer && ok) return {loc_, to_i64(view())};
        return {loc_, Tag::V_num, driver_.sym(view())};
    }

    /// Escapes stay verbatim; none of the keys or cost spellings needs one.
    Tok lex_string() {
        while (true) {
            accept_while_none_of(u8'"', u8'\\');
            if (accept('"')) return {loc_, Tag::V_str, driver_.sym(view().substr(1, view().size() - 2))};
            if (!accept('\\') || ahead() == utf8::EoF) break;
            next();
        }
        error().e(loc_, "unterminated string");
        return {loc_, Tag::V_str, driver_.sym(view().substr(1))};
    }

    fe::Driver& driver_;
};

struct Field {
    fe::Loc loc;
    size_t value = 0;
};

struct EdgeData {
    fe::Loc loc;
    Field src;
    Field dst;
    std::optional<Field> rows;
    std::optional<Field> cols;
    fe::Loc costs_loc;
    Costs costs;
};

class Parser : public fe::Parser<Tok, Tag, 1, Parser> {
public:
    Parser(fe::Driver& driver, const fe::Src& src)
        : lexer_(driver, src) {
        init();
    }

    fe::Driver& driver() { return lexer_.driver(); }
    Lexer& lexer() { return lexer_; }

    void parse_instance(Instance& instance) {
        parse_object("PBQP instance", [&](fe::Sym key) {
            // clang-format off
            if      (key == "nodes"      ) parse_array("`nodes`", [this] { parse_node(); });
            else if (key == "edges"      ) parse_array("`edges`", [this] { parse_edge(); });
            else if (key == "rn_priority") parse_rn_priority();
            else parse_value("PBQP instance");
            // clang-format on
        });
        expect(Tag::EoF, "PBQP instance");
        if (error().ok()) build(instance);
    }

private:
    void parse_list(Tag l, Tag r, fe::Cite ctx, auto&& parse_element) {
        auto tok_l = expect(l, ctx);
        if (!tok_l) return;
        auto _ = anchor(tok_l, r);
        if (accept(r)) return;
        do {
            parse_element();
        } while (accept(Tag::T_comma));
        if (!expect(r, ctx)) {
            recover([](Tag t) { return t != Tag::EoF; }, ctx);
            accept(r);
        }
    }

    void parse_object(fe::Cite ctx, auto&& parse_member) {
        parse_list(Tag::D_brace_l, Tag::D_brace_r, ctx, [&] {
            auto key = expect(Tag::V_str, ctx);
            expect(Tag::T_colon, ctx);
            parse_member(key ? key.sym() : fe::Sym());
        });
    }

    void parse_array(fe::Cite ctx, auto&& parse_element) {
        parse_list(Tag::D_bracket_l, Tag::D_bracket_r, ctx, parse_element);
    }

    void parse_value(fe::Cite ctx) {
        switch (ahead().tag()) {
            case Tag::D_brace_l: parse_object(ctx, [&](fe::Sym) { parse_value(ctx); }); return;
            case Tag::D_bracket_l: parse_array(ctx, [&] { parse_value(ctx); }); return;
#define CODE(t, _) case Tag::t:
                PBQP_JSON_KEY(CODE)
                PBQP_JSON_VAL(CODE)
#undef CODE
                lex();
                return;
            default: syntax_err("value", ctx);
        }
    }

    Field parse_index(fe::Cite ctx) {
        auto tok = expect(Tag::V_int, ctx);
        if (!tok) return {};
        if (tok.i64() < 0) error().e(tok.loc(), "expected a non-negative integer, got `{}`", tok);
        return {tok.loc(), static_cast<size_t>(tok.i64())};
    }

    Cost to_cost(Tok tok, std::int64_t value) {
        if (value < std::numeric_limits<Cost>::min() || kInfinity <= value) {
            error().e(tok.loc(), "cost `{}` is out of range", tok);
            return 0;
        }
        return static_cast<Cost>(value);
    }

    Cost parse_cost(fe::Cite ctx) {
        switch (ahead().tag()) {
            case Tag::V_int: {
                auto tok = lex();
                return to_cost(tok, tok.i64());
            }
            case Tag::V_str: {
                auto tok = lex();
                if (tok.sym() == "inf") return kInfinity;
                error().e(tok.loc(), "expected a cost, got `{}`", tok);
                return 0;
            }
            case Tag::V_num:
#define CODE(t, _) case Tag::t:
                PBQP_JSON_KEY(CODE)
#undef CODE
                syntax_err("cost", ctx, lex());
                return 0;
            default: syntax_err("cost", ctx); return 0;
        }
    }

    fe::Loc parse_costs(Costs& costs, fe::Cite ctx) {
        auto track = tracker();
        costs.clear();
        parse_array(ctx, [&] { costs.push_back(parse_cost(ctx)); });
        return track;
    }

    void parse_rn_priority() {
        auto track = tracker();
        rn_priority_.clear();
        parse_array("`rn_priority`", [this] { rn_priority_.push_back(parse_index("`rn_priority`").value); });
        rn_priority_loc_ = track;
    }

    void parse_node() {
        auto track = tracker();
        Costs costs;
        fe::Loc costs_loc;
        parse_object("node", [&](fe::Sym key) {
            if (key == "costs")
                costs_loc = parse_costs(costs, "node `costs`");
            else
                parse_value("node");
        });
        if (!costs_loc) return void(error().e(track, "node without `costs`"));
        if (costs.empty()) error().e(costs_loc, "node {} has no options", nodes_.size());
        nodes_.push_back(std::move(costs));
    }

    void parse_edge() {
        auto track = tracker();
        EdgeData edge;
        std::optional<Field> src, dst;
        parse_object("edge", [&](fe::Sym key) {
            // clang-format off
            if      (key == "src"  ) src = parse_index("edge `src`");
            else if (key == "dst"  ) dst = parse_index("edge `dst`");
            else if (key == "rows" ) edge.rows      = parse_index("edge `rows`");
            else if (key == "cols" ) edge.cols      = parse_index("edge `cols`");
            else if (key == "costs") edge.costs_loc = parse_costs(edge.costs, "edge `costs`");
            else parse_value("edge");
            // clang-format on
        });
        edge.loc = track;
        if (!src) return void(error().e(edge.loc, "edge without `src`"));
        if (!dst) return void(error().e(edge.loc, "edge without `dst`"));
        if (!edge.costs_loc) return void(error().e(edge.loc, "edge without `costs`"));
        edge.src = *src;
        edge.dst = *dst;
        edges_.push_back(std::move(edge));
    }

    void build(Instance& instance) {
        Graph& graph = instance.graph;
        for (const auto& costs : nodes_)
            graph.add(costs);

        const auto known = [&](const Field& field) {
            if (field.value < graph.num_nodes()) return true;
            error().e(field.loc, "unknown node {}", field.value);
            return false;
        };

        for (const auto& edge : edges_) {
            if (!known(edge.src) || !known(edge.dst)) continue;
            const NodeId src = edge.src.value;
            const NodeId dst = edge.dst.value;
            if (src == dst) {
                error().e(edge.loc, "self-edge on node {}", src);
                continue;
            }
            if (graph.edge(src, dst) != Graph::npos) {
                error().e(edge.loc, "duplicate edge between nodes {} and {}", src, dst);
                continue;
            }
            const size_t rows = graph.num_options(src);
            const size_t cols = graph.num_options(dst);
            if (edge.rows && edge.rows->value != rows)
                error().e(edge.rows->loc, "`rows` is {}, but node {} has {} options", edge.rows->value, src, rows);
            else if (edge.cols && edge.cols->value != cols)
                error().e(edge.cols->loc, "`cols` is {}, but node {} has {} options", edge.cols->value, dst, cols);
            else if (edge.costs.size() != rows * cols)
                error().e(edge.costs_loc, "expected {} × {} costs, got {}", rows, cols, edge.costs.size());
            else
                graph.link(src, dst, edge.costs);
        }

        if (rn_priority_loc_ && rn_priority_.size() != graph.num_nodes())
            error().e(rn_priority_loc_, "expected {} priorities, got {}", graph.num_nodes(), rn_priority_.size());
        instance.rn_priority = std::move(rn_priority_);
    }

    Lexer lexer_;
    fe::Vector<Costs> nodes_;
    fe::Vector<EdgeData> edges_;
    fe::Vector<size_t> rn_priority_;
    fe::Loc rn_priority_loc_;
};

void write_costs(std::ostream& out, fe::View<Cost> costs) {
    out << '[';
    for (size_t i = 0; i < costs.size(); ++i) {
        if (i != 0) out << ", ";
        if (costs[i] == kInfinity)
            out << R"("inf")";
        else
            std::print(out, "{}", costs[i]);
    }
    out << ']';
}

} // namespace

} // namespace pbqp::json

namespace pbqp {

void read_json(std::istream& in, Instance& instance, const std::filesystem::path& path) {
    fe::Driver driver;
    const fe::Src* src = driver.src().add(path, fe::SrcMap::slurp(in)).first;
    json::Parser(driver, *src).parse_instance(instance);
    driver.error().ack();
}

void write_json(std::ostream& out, const Graph& graph, fe::View<size_t> rn_priority) {
    assert((rn_priority.empty() || rn_priority.size() == graph.num_nodes())
           && "write_json: rn_priority must have one entry per node");

    std::println(out, "{{");

    std::print(out, R"(  "nodes": [)");
    const char* separator = "\n";
    for (const auto n : graph.nodes()) {
        std::print(out, R"({}    {{"costs": )", separator);
        json::write_costs(out, graph.node_costs(n));
        out << '}';
        separator = ",\n";
    }
    std::print(out, "\n  ],\n");

    std::print(out, R"(  "edges": [)");
    separator = "\n";
    for (const auto e : graph.edges()) {
        const auto [src, dst] = graph.edge(e);
        std::print(out, R"({}    {{"src": {}, "dst": {}, "rows": {}, "cols": {}, "costs": )", separator, src, dst,
                   graph.num_options(src), graph.num_options(dst));
        json::write_costs(out, graph.edge_costs(e));
        out << '}';
        separator = ",\n";
    }
    std::print(out, "\n  ]");

    if (!rn_priority.empty()) {
        std::print(out, ",\n  \"rn_priority\": [");
        for (size_t i = 0; i < rn_priority.size(); ++i)
            std::print(out, "{}{}", i == 0 ? "" : ", ", rn_priority[i]);
        out << ']';
    }
    std::println(out, "\n}}");
}

} // namespace pbqp
