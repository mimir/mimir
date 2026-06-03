"""Public plugin facades.

Generated plugin enums live in ``mim._plugins``.  This package re-exposes each
of them as ``mim.plug.<name>`` so that user code can write::

    import mim.plug.core as core
    core.bit2.and_

Most facades are plain proxies — they expose the IntEnum class as attribute
``<name>`` and forward attribute lookups to it.  ``regex`` is a real submodule
because it also carries ``RegBuilder`` and ``MimRegex``.
"""
from __future__ import annotations

import importlib
import pkgutil
import sys
import types

from .. import _plugins


def _make_facade(name: str, enum) -> types.ModuleType:
    mod = types.ModuleType(f"mim.plug.{name}")
    mod.__dict__[name] = enum
    mod.__getattr__ = lambda attr, _e=enum: getattr(_e, attr)
    return mod


__all__: list[str] = []

for _info in pkgutil.iter_modules(_plugins.__path__):
    _name = _info.name
    if _name == "regex":
        continue
    _enum = getattr(importlib.import_module(f"mim._plugins.{_name}"), _name)
    _facade = _make_facade(_name, _enum)
    sys.modules[f"mim.plug.{_name}"] = _facade
    globals()[_name] = _facade
    __all__.append(_name)

del _info, _name, _enum, _facade

from . import regex  # noqa: E402

__all__.append("regex")
