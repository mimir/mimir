"""Tests for the World.call(...) helper monkey-patched in mim/__init__.py."""

from __future__ import annotations

import pytest
import mim
import mim.plug.core as core


@pytest.fixture
def core_world(driver):
    driver.load_plugin("core")
    return driver.world()


def test(core_world):
    assert isinstance(core_world.call(core.bit2.and_), mim.Def)
    nat0 = core_world.lit_nat_0()
    assert isinstance(core_world.call(core.bit2.and_, nat0), mim.Def)
