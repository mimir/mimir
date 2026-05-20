"""Tests for py/bindings/world.cpp bindings (no plugins required)."""
from __future__ import annotations

import mim

def test(world):
    b  = world.type_bool()
    i8 = world.type_i8()
    i8_7 = world.lit_i8(7)
    tt = world.lit_bool(True)
    ff = world.lit_bool(False)
    assert isinstance(i8_7, mim.Def)
    assert isinstance(world.lit_nat_0(), mim.Lit)
    assert isinstance(tt, mim.Lit)
    assert isinstance(ff, mim.Lit)
    assert isinstance(world.lit_ff(), mim.Lit)
    assert isinstance(world.lit_tt(), mim.Lit)
    assert isinstance(world.top_nat(), mim.Def)
    assert isinstance(b, mim.App)
    assert isinstance(i8, mim.App)
    assert isinstance(world.type_i32(), mim.App)
    assert isinstance(world.type_idx(world.top_nat()), mim.App)
    assert isinstance(world.type_idx(256), mim.App)
    idx_t = world.type_idx(world.top_nat())
    assert isinstance(world.lit(idx_t, 0), mim.Lit)
    assert isinstance(world.cn([b, i8]), mim.Pi)
    assert isinstance(world.arr(world.top_nat(), i8), mim.Arr)
    assert isinstance(world.mut_con([b, i8]), mim.Lam)


def test_sym(world):
    s = world.sym("hello")
    assert isinstance(s, mim.Sym)
    assert s.str() == "hello"
    assert not s.empty()
    assert s.size() == len("hello")
    s = world.sym("")
    assert s.empty()
    assert s.size() == 0
