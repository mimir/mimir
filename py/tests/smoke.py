"""Top-level binding registration smoke tests for mim.

Catches missing init_*(m) calls in py/bindings/py.cpp and missing
re-exports in mim/__init__.py without mirroring the full public API.
"""
from __future__ import annotations

import importlib
import io

import mim
import pytest


def test_binding_symbol_present():
    assert hasattr(mim, "Driver"), "mim.Driver missing — check init_*() in py.cpp"


def test_wrapper_symbol_present():
    assert hasattr(mim, "MimCallable"), "mim.MimCallable missing — check mim/__init__.py"


def test_log_levels_exported():
    for level_name in ("Error", "Warn", "Info", "Verbose", "Debug"):
        level = getattr(mim, level_name)
        assert isinstance(level, mim.Level)


def test_mim_error_is_exception():
    assert issubclass(mim.MIM_Error, Exception)


def test_print_mim_error_writes_message():
    buffer = io.StringIO()
    mim.print_mim_error(mim.MIM_Error("boom"), file=buffer)
    assert buffer.getvalue() == "boom\n"


def test_guard_mim_errors_prints_and_suppresses():
    buffer = io.StringIO()

    with mim.guard_mim_errors(file=buffer):
        raise mim.MIM_Error("boom")

    assert buffer.getvalue() == "boom\n"


def test_guard_mim_errors_reraises_when_requested():
    buffer = io.StringIO()

    with pytest.raises(mim.MIM_Error) as exc_info:
        with mim.guard_mim_errors(file=buffer, reraise=True):
            raise mim.MIM_Error("boom")

    assert "boom" in str(exc_info.value)
    assert buffer.getvalue() == "boom\n"


def test_catch_mim_errors_prints_and_returns_default():
    buffer = io.StringIO()

    @mim.catch_mim_errors(default="fallback", file=buffer)
    def fail():
        raise mim.MIM_Error("boom")

    assert fail() == "fallback"
    assert buffer.getvalue() == "boom\n"


def test_world_call_is_monkey_patched():
    """mim/__init__.py attaches a `call` method to World."""
    assert callable(getattr(mim.World, "call", None))


def test_generated_plugin_facade_resolves():
    facade = importlib.import_module("mim.plug.core")
    generated = importlib.import_module("mim._plugins.core")

    assert facade.core is generated.core
