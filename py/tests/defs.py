"""Tests for py/bindings/def.cpp bindings."""
from __future__ import annotations

import mim
import pytest


def test_world(world):
    m1 = world.mut_con([world.type_bool()])
    m2 = world.mut_con([world.type_bool(), world.type_i8()])
    m3 = world.mut_con([world.type_bool(), world.type_i8(), world.type_i32()])
    lit = world.lit_i8(0)
    lit.dump()  # don't raise
    assert isinstance(lit.world(), mim.World)
    assert isinstance(m1.set("named"), mim.Def)
    assert m3.var().num_projs() == 3
    parts = m3.var().projs(3)
    assert isinstance(parts, list)
    assert len(parts) == 3
    for p in parts:
        assert isinstance(p, mim.Def)
    assert isinstance(m2.var().proj(0), mim.Def)
    assert isinstance(m2.var().proj(2, 0), mim.Def)
    assert isinstance(m2.var()[0], mim.Def)
    assert isinstance(m2.var()[2, 0], mim.Def)

def test_driver(driver):
    d = driver.world().lit_i8(0).driver()
    assert d is not None

@pytest.mark.skip(reason="calling .var() on a projection currently segfaults; "
                    "convert to xfail-strict once the binding raises MIM_Error "
                    "instead of crashing.")
def test_var_on_projection_raises(world):
    m = world.mut_con([world.type_bool(), world.type_i8()])
    p = m.var().proj(0)
    with pytest.raises(mim.MIM_Error):
        p.var()
