"""Tests for py/bindings/driver.cpp bindings."""

from __future__ import annotations

import pytest
import mim


def test_driver(driver):
    assert isinstance(mim.Driver(), mim.Driver)
    assert isinstance(driver.world(), mim.World)
    assert isinstance(driver.world(), mim.World)  # repeat call
    chained = driver.log().set_stdout().set(mim.Level.Debug)
    assert chained is not None
    assert driver.sym("foo").str() == "foo"


def test_add_search_path_accepts_pathlib(driver, tmp_path):
    driver.add_search_path(tmp_path)


def test_load_plugins_core_succeeds(driver):
    driver.load_plugin("core")


def test_load_plugins_unknown_raises(driver, tmp_path):
    driver.add_search_path(tmp_path)
    with pytest.raises(Exception):
        driver.load_plugins(["this_plugin_does_not_exist_xyzzy"])
