"""Regex plugin smoke + end-to-end via RegBuilder.jit()."""

from __future__ import annotations

import shutil
import pytest
import mim
import mim.plug.regex as regex


def test_regex_plugin_loads(regex_world):
    assert regex_world.annex(regex.lit) is not None


@pytest.mark.slow
@pytest.mark.needs_clang
def test_regbuilder_jit_match(tmp_path, monkeypatch):
    """End-to-end: build a tiny regex, JIT, run the matcher.

    Uses the public mim.plug.regex API.
    Runs in tmp_path so the generated .ll/.so don't pollute the repo.
    """
    if shutil.which("clang") is None:
        pytest.skip("clang not on PATH")

    monkeypatch.chdir(tmp_path)

    driver = mim.Driver()
    b = regex.RegBuilder(driver, "regex_test", mim.Level.Error)
    pattern = b.lit("a") + b.lit("b") + b.lit("c")
    library = pattern.jit()

    match_func = library["match_func"]
    assert match_func(b"abc") is True
    assert match_func(b"xyz") is False
