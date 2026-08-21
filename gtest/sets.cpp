#include <deque>
#include <map>
#include <random>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include <mim/util/sets.h>

using namespace mim;

namespace {

/// Minimal stand-in for a Def: Sets only needs gid() for ordering/hashing and a writable tid_.
struct Elem {
    explicit Elem(u32 gid)
        : gid_(gid) {}

    u32 gid() const { return gid_; }
    u32 tid() const { return tid_; }
    const char* sym() const { return "elem"; } ///< only needed so Set::stream instantiates

    u32 gid_;
    u32 tid_ = 0;
};

using S                   = Sets<Elem>;
static constexpr size_t N = 16; ///< Sets' Data/Node switch-over point.

/// Stable storage, interned by gid: Sets keys on pointers but *orders* by gid, so one gid must map to
/// exactly one Elem - otherwise the sortedness/uniqueness invariant is violated by construction.
struct Pool {
    Elem* operator()(u32 gid) {
        auto [i, ins] = gid2elem.emplace(gid, nullptr);
        if (ins) i->second = &elems.emplace_back(gid);
        return i->second;
    }

    std::deque<Elem> elems;
    std::map<u32, Elem*> gid2elem;
};

std::set<u32> gids(S::Set s) {
    auto res = std::set<u32>();
    for (auto e : s)
        res.emplace(e->gid());
    return res;
}

S::Set make(S& sets, Pool& pool, std::set<u32> want) {
    auto v = Vector<Elem*>();
    for (auto g : want)
        v.emplace_back(pool(g));
    return sets.create(std::move(v));
}

} // namespace

TEST(Sets, flavours) {
    auto sets = S();
    auto pool = Pool();

    EXPECT_TRUE(make(sets, pool, {}).empty());
    EXPECT_EQ(make(sets, pool, {}).size(), 0);

    for (size_t n : {size_t(1), size_t(2), N - 1, N, N + 1, 2 * N, 3 * N}) {
        auto want = std::set<u32>();
        for (u32 i = 0; i != n; ++i)
            want.emplace(i * 7 + 1); // arbitrary, distinct
        auto s = make(sets, pool, want);
        EXPECT_EQ(s.size(), n) << "n = " << n;
        EXPECT_EQ(gids(s), want) << "n = " << n;
        for (auto g : want)
            EXPECT_TRUE(s.contains(pool(g))) << "n = " << n << ", gid = " << g;
    }
}

TEST(Sets, create_dedups_and_is_order_independent) {
    auto sets = S();
    auto pool = Pool();

    // Equal contents must yield the *same* Set - Data is hash-consed, trie paths are canonical.
    for (size_t n : {size_t(2), N, N + 1, 2 * N}) {
        auto fwd = Vector<Elem*>();
        auto rev = Vector<Elem*>();
        auto es  = std::vector<Elem*>();
        for (u32 i = 0; i != n; ++i)
            es.emplace_back(pool(i * 3 + 5));

        for (auto e : es)
            fwd.emplace_back(e);
        for (auto i = es.rbegin(), ie = es.rend(); i != ie; ++i)
            rev.emplace_back(*i);
        rev.emplace_back(es.front()); // and a duplicate for good measure

        auto s1 = sets.create(std::move(fwd));
        auto s2 = sets.create(std::move(rev));
        EXPECT_EQ(s1, s2) << "n = " << n;
        EXPECT_EQ(s1.size(), n) << "n = " << n;
    }
}

TEST(Sets, insert) {
    auto sets = S();
    auto pool = Pool();

    auto want = std::set<u32>();
    auto s    = S::Set();
    for (u32 i = 0; i != 3 * N; ++i) {
        auto g = (i * 11 + 3) % 97;
        auto e = pool(g);
        s      = sets.insert(s, e);
        want.emplace(g);
        EXPECT_EQ(gids(s), want) << "i = " << i;

        auto again = sets.insert(s, e); // idempotent
        EXPECT_EQ(again, s) << "i = " << i;
    }
}

TEST(Sets, erase) {
    auto sets = S();
    auto pool = Pool();

    for (size_t n : {size_t(1), size_t(2), N, N + 1, 2 * N}) {
        auto want = std::set<u32>();
        auto es   = std::vector<Elem*>();
        for (u32 i = 0; i != n; ++i)
            want.emplace(i);
        auto s = make(sets, pool, want);
        for (auto g : want)
            es.emplace_back(pool(g));

        // erasing something that is not in there is a no-op
        EXPECT_EQ(sets.erase(s, pool(1000 + n)), s) << "n = " << n;

        for (auto e : es) {
            s = sets.erase(s, e);
            want.erase(e->gid());
            EXPECT_EQ(gids(s), want) << "n = " << n;
        }
        EXPECT_TRUE(s.empty()) << "n = " << n;
    }
}

/// The 4x4 flavour cross product for merge - this is what the linear array union has to get right.
TEST(Sets, merge_flavour_matrix) {
    auto sets  = S();
    auto pool  = Pool();
    auto sizes = {size_t(0), size_t(1), size_t(2), N - 1, N, N + 1, 2 * N};

    for (auto n1 : sizes) {
        for (auto n2 : sizes) {
            for (u32 offset : {u32(0), u32(1), u32(1000)}) { // disjoint, overlapping, and interleaved
                auto w1 = std::set<u32>();
                auto w2 = std::set<u32>();
                for (u32 i = 0; i != n1; ++i)
                    w1.emplace(2 * i);
                for (u32 i = 0; i != n2; ++i)
                    w2.emplace(2 * i + offset);

                auto s1 = make(sets, pool, w1);
                auto s2 = make(sets, pool, w2);

                auto want = w1;
                want.insert(w2.begin(), w2.end());

                auto m = sets.merge(s1, s2);
                EXPECT_EQ(gids(m), want) << n1 << " u " << n2 << " @" << offset;
                EXPECT_EQ(m.size(), want.size()) << n1 << " u " << n2 << " @" << offset;
                EXPECT_EQ(sets.merge(s2, s1), m) << "merge must be commutative"; // canonical => same Set
                EXPECT_EQ(sets.merge(m, s1), m) << "absorption";
            }
        }
    }
}

TEST(Sets, has_intersection) {
    auto sets  = S();
    auto pool  = Pool();
    auto sizes = {size_t(1), size_t(2), N, N + 1, 2 * N};

    for (auto n1 : sizes) {
        for (auto n2 : sizes) {
            for (u32 offset : {u32(0), u32(1), u32(10000)}) {
                auto w1 = std::set<u32>();
                auto w2 = std::set<u32>();
                for (u32 i = 0; i != n1; ++i)
                    w1.emplace(2 * i);
                for (u32 i = 0; i != n2; ++i)
                    w2.emplace(2 * i + offset);

                auto want = false;
                for (auto g : w1)
                    want |= w2.contains(g);

                auto s1 = make(sets, pool, w1);
                auto s2 = make(sets, pool, w2);
                EXPECT_EQ(s1.has_intersection(s2), want) << n1 << " ^ " << n2 << " @" << offset;
                EXPECT_EQ(s2.has_intersection(s1), want) << n1 << " ^ " << n2 << " @" << offset;
            }
        }
    }
}

/// Differential test: drive Sets and std::set through the same random operations.
TEST(Sets, random_vs_std_set) {
    auto sets = S();
    auto pool = Pool();
    auto rng  = std::mt19937(42);

    auto elems = std::vector<Elem*>();
    for (u32 g = 0; g != 128; ++g)
        elems.emplace_back(pool(g));

    auto ref = std::vector<std::set<u32>>{{}};
    auto got = std::vector<S::Set>{{}};

    for (int step = 0; step != 20000; ++step) {
        auto i  = rng() % got.size();
        auto op = rng() % 4;

        if (op == 0) { // insert
            auto e = elems[rng() % elems.size()];
            got.emplace_back(sets.insert(got[i], e));
            auto r = ref[i];
            r.emplace(e->gid());
            ref.emplace_back(std::move(r));
        } else if (op == 1) { // erase
            auto e = elems[rng() % elems.size()];
            got.emplace_back(sets.erase(got[i], e));
            auto r = ref[i];
            r.erase(e->gid());
            ref.emplace_back(std::move(r));
        } else if (op == 2) { // merge
            auto j = rng() % got.size();
            got.emplace_back(sets.merge(got[i], got[j]));
            auto r = ref[i];
            r.insert(ref[j].begin(), ref[j].end());
            ref.emplace_back(std::move(r));
        } else { // check membership / intersection against the reference
            auto j = rng() % got.size();
            auto e = elems[rng() % elems.size()];
            ASSERT_EQ(got[i].contains(e), ref[i].contains(e->gid())) << "step " << step;

            auto want = false;
            for (auto g : ref[i])
                want |= ref[j].contains(g);
            ASSERT_EQ(got[i].has_intersection(got[j]), want) << "step " << step;
            continue;
        }

        ASSERT_EQ(gids(got.back()), ref.back()) << "step " << step << ", op " << op;
        ASSERT_EQ(got.back().size(), ref.back().size()) << "step " << step << ", op " << op;

        if (got.size() > 64) { // keep the working set bounded
            got.erase(got.begin() + 1, got.begin() + 32);
            ref.erase(ref.begin() + 1, ref.begin() + 32);
        }
    }
}
