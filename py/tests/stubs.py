"""Guards on the generated `_mim.pyi`.

nanobind happily binds a method whose signature mentions a type it has no caster
for: nothing fails until Python actually calls it, and `stubgen` meanwhile writes
the raw C++ spelling into the stub (`std::reverse_iterator<char const*>`, an
`absl::flat_hash_map<…>`, `mim::Dbg`). The API tests never touch such a method,
so inspect the stub itself — it is the one artifact that sees every binding.

The generator drops unbindable members on its own (see `_casters_for` in
`scripts/gen_nanobind_bindings.py`); these tests catch what slips through,
including from a hand-written `.nbextra` fragment, and on standard libraries
whose spellings differ from the one this was developed against.
"""
from __future__ import annotations

import re
from pathlib import Path

import mim
import pytest

# A type nanobind could not resolve to a Python name reaches the stub as a quoted
# annotation, e.g. `def rbegin(self) -> "std::reverse_iterator<char const*>"`.
# A quoted annotation is legitimate only as a forward reference to a Python name.
_ANNOTATION = re.compile(r'(?:->|:)\s*"([^"\n]+)"')
_PY_NAME = re.compile(r"^[A-Za-z_][\w.]*(?:\[[\w.,\[\] |]*\])?$")

# Namespaces that must never appear anywhere in the stub, quoted or not.
_CPP_NAMESPACES = ("std::", "mim::", "fe::", "absl::")

_FIX = (
    "Fix by including the matching nanobind caster header (`nanobind/stl/*.h`), "
    "exposing a Python-friendly signature in the header's .nbextra, or dropping "
    "the member there with [skip:Class]."
)


# Members that no other test touches and that have gone missing before, when
# libclang resolved a type differently on one platform than on another. A binding
# that disappears cleanly leaves no trace in the stub, so the leak checks below
# cannot see it — only naming the members does.
_REQUIRED = {
    "Sym": ("str", "view"),
    "Def": ("proj", "projs", "deps", "free_vars", "local_muts", "world", "driver", "type"),
    "World": ("set", "sym", "driver", "annex", "app", "implicit_app", "optimize", "arr", "tuple", "cn"),
    "Driver": ("sym", "world", "search_paths", "add_search_path", "load_plugins", "add_import"),
}


def test_required_members_present():
    missing = [
        f"{cls}.{member}"
        for cls, members in _REQUIRED.items()
        for member in members
        if not hasattr(getattr(mim, cls), member)
    ]
    assert not missing, (
        "bindings vanished: " + ", ".join(missing) + "\nThe generator drops a member whose "
        "signature it cannot convert; the build log's 'dropped N member(s)' note says which "
        "type it choked on. " + _FIX
    )


@pytest.fixture(scope="module")
def stub() -> str:
    path = Path(mim.__file__).parent / "_mim.pyi"
    if not path.is_file():
        pytest.fail(f"generated stub missing at {path} — check the mim_py_stubs target")
    return path.read_text()


def test_no_unresolved_annotations(stub: str):
    leaked = sorted({a for a in _ANNOTATION.findall(stub) if not _PY_NAME.match(a)})
    assert not leaked, "unbindable C++ types leaked into _mim.pyi:\n  " + "\n  ".join(leaked) + f"\n{_FIX}"


def test_no_cpp_namespaces(stub: str):
    leaked = sorted(
        {line.strip() for line in stub.splitlines() if any(ns in line for ns in _CPP_NAMESPACES)}
    )
    assert not leaked, "C++ namespaces leaked into _mim.pyi:\n  " + "\n  ".join(leaked) + f"\n{_FIX}"
