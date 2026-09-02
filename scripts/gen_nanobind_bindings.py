#!/usr/bin/env python3
"""
Generate nanobind Python bindings for C++ headers using libclang (from venv).

Usage:
    ./gen_nanobind_bindings.py path/to/header.h                    # stdout
    ./gen_nanobind_bindings.py path/to/header.h -o bind.cpp        # to file
    ./gen_nanobind_bindings.py --dir include/mim --recursive       # batch
    ./gen_nanobind_bindings.py --dir include/mim --recursive
        --namespace mim -I /custom/include

Requires the `libclang` wheel (bundles libclang + its Python bindings); it is
installed into the build venv by CMake, which also invokes this script.
"""

from __future__ import annotations

import argparse
import difflib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path

# ---------------------------------------------------------------------------
#  Bootstrap: import the venv's bundled libclang
# ---------------------------------------------------------------------------
# Normally this runs under the build venv's own Python (invoked by CMake), so
# the `libclang` wheel — which self-locates its native library — is already
# importable.  When run manually with a system Python, add the repo `.venv`'s
# site-packages so the same wheel is found.

_SCRIPT_DIR = Path(__file__).resolve().parent  # scripts/
_REPO_ROOT = _SCRIPT_DIR.parent                # repo root

if sys.prefix == sys.base_prefix:  # not already inside a venv
    _site = _REPO_ROOT / ".venv" / "lib" / f"python{sys.version_info.major}.{sys.version_info.minor}" / "site-packages"
    if _site.is_dir():
        sys.path.insert(0, str(_site))

try:
    import clang.cindex  # pyright: ignore[reportMissingImports]
except Exception as e:  # ImportError, or libclang failing to load
    print(
        f"ERROR: libclang not available: {e}\n"
        "  Run under the build venv, or `pip install libclang` into it.",
        file=sys.stderr,
    )
    sys.exit(1)

clang = clang.cindex
CursorKind = clang.CursorKind
TypeKind = clang.TypeKind


# ---------------------------------------------------------------------------
#  Type helpers
# ---------------------------------------------------------------------------

def _type_spelling(t) -> str:
    """*t*'s spelling without the elaborated-type keywords libclang prefixes."""
    return t.spelling.replace("class ", "").replace("struct ", "").replace("enum ", "")


# ---------------------------------------------------------------------------
#  Bindability: the types nanobind can actually convert
# ---------------------------------------------------------------------------
# Binding a method whose signature mentions a type nanobind has no caster for
# still *compiles* — the failure only surfaces when Python calls it, and
# `stubgen` meanwhile spells the raw C++ type into `_mim.pyi`
# (`std::reverse_iterator<char const*>`, `absl::flat_hash_map<…>`, `mim::Dbg`).
# Such a binding is dead weight that leaks implementation types into the public
# stub, so every parameter, return and field type is checked here first and the
# member is dropped unless nanobind can convert it.

# Implementation-detail namespaces (libstdc++ `__cxx11`, libc++ `__1` and `__fs`,
# abseil's `lts_<date>`) differ per toolchain and must never reach a name we match
# on — nor an emitted one, where they would also pin the output to one library.
_IMPL_NS = re.compile(r"^(?:__[A-Za-z0-9_]+|lts_\d+)$")
_INLINE_NS = re.compile(r"\b(?:__[A-Za-z0-9_]+|lts_\d+)::")


def _canon_spelling(t) -> str:
    """Canonical spelling of *t*, free of typedefs and inline namespaces.

    Dropping the inline namespace keeps the spelling both matchable and valid
    C++: `std::__cxx11::basic_string<char>` and `std::basic_string<char>` name
    the same type.
    """
    return _INLINE_NS.sub("", _type_spelling(t.get_canonical()))


def _qualified_name(decl) -> str:
    """The scope-qualified name of *decl*, without template arguments.

    Built from the declaration itself rather than from a printed type spelling,
    because that spelling is not portable: libclang prints libc++'s `std::string`
    as the sugared typedef, and libstdc++'s as `std::basic_string<char>`, so a
    table keyed on the printed form matches on one platform and misses on the
    other. The declaration always answers `std::basic_string`, and skipping the
    implementation-detail namespaces keeps `std::filesystem::path` (libc++ nests it
    in `__fs`) and `fe::XTrie::Set` recognisable everywhere.
    """
    parts = []
    cursor = decl
    while cursor is not None and cursor.kind != CursorKind.TRANSLATION_UNIT:
        if not cursor.spelling:
            break  # an anonymous scope: no name to match on
        if not (cursor.kind == CursorKind.NAMESPACE and _IMPL_NS.match(cursor.spelling)):
            parts.append(cursor.spelling)
        cursor = cursor.semantic_parent
    return "::".join(reversed(parts))


_LEADING_CV = re.compile(r"^(?:const|volatile)\s+")
_TRAILING_CONST = re.compile(r"\s*\bconst$")


def _strip_cv(spelling: str) -> str:
    """*spelling* without its leading cv-qualifiers (`const std::list<…>` → `std::list<…>`)."""
    return _LEADING_CV.sub("", spelling)


def _kinds(*names) -> frozenset:
    """The named `TypeKind`s known to this libclang; older ones lack e.g. `CHAR8`."""
    return frozenset(k for k in (getattr(TypeKind, n, None) for n in names) if k is not None)


_CHAR_KINDS = _kinds("CHAR_S", "CHAR_U", "SCHAR", "UCHAR", "CHAR8")

# Cursor kinds that introduce a scope worth descending into, and the two that
# declare a class.
_RECORD_KINDS = (CursorKind.CLASS_DECL, CursorKind.STRUCT_DECL)
_SCOPE_KINDS = (CursorKind.NAMESPACE, CursorKind.ENUM_DECL, *_RECORD_KINDS)

# libclang does not always resolve a type to something concrete, and its view of
# the standard library is less complete than the compiler's. "Unresolved" must
# never be read as "unbindable": that would drop the member on whichever toolchain
# came up short, and nothing else would say so. Such a member is emitted as before
# — its written spelling is valid C++ by construction — leaving the verdict to the
# C++ compiler and to `py/tests/stubs.py`, which fails if the type really has no
# caster.
_OPAQUE_KINDS = _kinds("INVALID", "UNEXPOSED")

# Types nanobind converts out of the box, without any caster header.
_BUILTIN_KINDS = _CHAR_KINDS | _kinds(
    "VOID", "BOOL", "CHAR16", "CHAR32", "WCHAR", "SHORT", "USHORT", "INT", "UINT", "LONG", "ULONG",
    "LONGLONG", "ULONGLONG", "INT128", "UINT128", "FLOAT", "DOUBLE", "LONGDOUBLE", "NULLPTR",
)

# Standard containers nanobind casts, mapped to the caster header they need and
# to how many leading template arguments carry a Python-visible type. The
# remaining ones are allocators, deleters and traits that nanobind ignores, so
# they must not be checked for bindability. `-1` means every argument counts.
_STL_CASTERS = {
    "std::basic_string":      ("string.h", 0),
    "std::basic_string_view": ("string_view.h", 0),
    "std::filesystem::path":  ("filesystem.h", 0),
    "std::vector":            ("vector.h", 1),
    "std::list":              ("list.h", 1),
    "std::set":               ("set.h", 1),
    "std::unordered_set":     ("unordered_set.h", 1),
    "std::optional":          ("optional.h", 1),
    "std::unique_ptr":        ("unique_ptr.h", 1),
    "std::shared_ptr":        ("shared_ptr.h", 1),
    "std::pair":              ("pair.h", 2),
    "std::map":               ("map.h", 2),
    "std::unordered_map":     ("unordered_map.h", 2),
    "std::complex":           ("complex.h", 1),
    "std::tuple":             ("tuple.h", -1),
    "std::variant":           ("variant.h", -1),
}

# `std::function` is not in the table above: its single template argument is a
# signature rather than a value type, so it needs its own traversal.
_FUNCTION_CASTER = "function.h"

# `fe`'s own containers: no caster, but they are ranges of elements nanobind
# *can* bind, so signatures mentioning one are rewritten to a `std::vector` of
# the element type (see `_range_value_type`).
_MIM_RANGES = ("fe::Span", "fe::Vector")
_MIM_SET = "fe::XTrie::Set"  # `Vars`/`Muts`: a forward range of `D*`

# How many entries a note spells out before summarising the rest.
_MAX_REPORTED = 20

_BOUND_HEADERS: frozenset = frozenset()


@lru_cache(maxsize=None)
def _realpath(path: str) -> str:
    """`os.path.realpath`, memoised: the same handful of headers is resolved again
    for every single type that is checked."""
    return os.path.realpath(path)


def set_bound_headers(paths) -> None:
    """Record the manifest of headers whose classes and enums the *module* binds.

    Only those types may appear in a generated signature: a `const Def*` becomes
    a bound Python object, whereas a `mim::Dbg` — declared in a header nobody
    binds — has no Python counterpart at all.

    This is the whole build's manifest, not just the header being generated: each
    invocation parses one header but must recognise the types registered by its
    siblings. It is therefore only as accurate as that manifest — a header listed
    here whose `init_*` never makes it into the module entry point would still be
    treated as bound.
    """
    global _BOUND_HEADERS
    _BOUND_HEADERS = frozenset(_realpath(p) for p in paths)


def _decl_in_bound_header(decl) -> bool:
    try:
        file = decl.location.file
    except Exception:
        return False
    return file is not None and _realpath(file.name) in _BOUND_HEADERS


_BOUND_TYPE_NAMES: frozenset = frozenset()


def collect_bound_types(tu) -> None:
    """Record every class and enum a bound header declares in *tu*, by qualified name.

    Must run before extracting from *tu*, and answers only for that one TU.

    The file of the declaration libclang hands back is not a reliable test on its
    own: for an incomplete type it is whichever forward declaration came first, so
    the include order decides the verdict. `Driver` is forward-declared in both
    `plugin.h` and `world.h`, libclang reports the `plugin.h` one, and `World::driver()`
    was dropped as "unbound" even though `driver.h` is in the manifest. Matching by
    name instead makes it independent of which redeclaration is reported — qualified,
    so that `fe::Driver` cannot pass for `mim::Driver`.
    """
    global _BOUND_TYPE_NAMES
    names: set = set()

    def walk(cursor):
        for child in cursor.get_children():
            # A namespace or class cursor is one *reopening*, so it lies in a
            # single file: if that file is not bound, neither is anything it
            # contains — which prunes all of `std::` and `absl::` right here.
            if child.kind not in _SCOPE_KINDS or not _decl_in_bound_header(child):
                continue
            if child.kind != CursorKind.NAMESPACE and child.spelling:
                names.add(_qualified_name(child))
            if child.kind != CursorKind.ENUM_DECL:
                walk(child)  # a class may nest enums, which bind at module scope

    walk(tu.cursor)
    _BOUND_TYPE_NAMES = frozenset(names)


def _is_bound_decl(decl) -> bool:
    """True if *decl* is one of the types this build registers with nanobind."""
    return _qualified_name(decl) in _BOUND_TYPE_NAMES or _decl_in_bound_header(decl)


def _is_bound_class(decl) -> bool:
    """True if *decl* names a class this build registers as a Python type.

    Only a top-level class of a bound header is registered, so a nested class
    (`World::State`, `XTrie<D, K>::Set`) or a template specialization never is.
    """
    if decl is None or not decl.spelling or "<" in decl.displayname:
        return False
    parent = decl.semantic_parent
    if parent is not None and parent.kind not in (CursorKind.NAMESPACE, CursorKind.TRANSLATION_UNIT):
        return False
    return _is_bound_decl(decl)


def _casters_for_signature(proto, depth: int) -> set | None:
    """Casters needed by a `std::function`'s signature, or None if unbindable."""
    canon = proto.get_canonical()
    if canon.kind != TypeKind.FUNCTIONPROTO:
        return None
    need: set = set()
    for t in [canon.get_result(), *canon.argument_types()]:
        if (sub := _casters_for(t, depth + 1)) is None:
            return None
        need |= sub
    return need


def _casters_for(t, depth: int = 0) -> set | None:
    """The nanobind caster headers needed to bind *t*, or None if nanobind can't.

    An empty set means *t* needs no caster beyond `nanobind.h`.
    """
    if depth > 6:  # a signature this deeply nested is not worth binding
        return None

    canon = t.get_canonical()
    kind = canon.kind

    if kind in _OPAQUE_KINDS:
        return set()  # cannot judge, so keep the member — see _OPAQUE_KINDS

    if kind in (TypeKind.POINTER, TypeKind.LVALUEREFERENCE, TypeKind.RVALUEREFERENCE):
        pointee = canon.get_pointee().get_canonical()
        if pointee.kind in _CHAR_KINDS:
            return set()  # `const char*` is just a Python string
        if pointee.kind == TypeKind.FUNCTIONPROTO:
            return None  # raw function pointer: no caster
        if kind == TypeKind.POINTER:
            # A pointer is only Python-visible if it points at a *bound* class: a
            # caster converts a value or a reference (`const std::string&`), never
            # a pointer to one (`const std::filesystem::path*`).
            if pointee.kind != TypeKind.RECORD:
                return None
            return set() if _is_bound_class(pointee.get_declaration()) else None
        return _casters_for(pointee, depth + 1)

    if kind in _BUILTIN_KINDS:
        return set()

    if kind == TypeKind.ENUM:
        # Nested enums are bound at module scope, so only the declaring header
        # matters here — not whether the enum sits inside a class.
        return set() if _is_bound_decl(canon.get_declaration()) else None

    if kind == TypeKind.RECORD:
        decl = canon.get_declaration()
        name = _qualified_name(decl)
        if name == "std::function":
            sub = _casters_for_signature(canon.get_template_argument_type(0), depth)
            return None if sub is None else sub | {_FUNCTION_CASTER}
        if (entry := _STL_CASTERS.get(name)) is not None:
            header, n_value_args = entry
            need = {header}
            # -1 template arguments means "not a template" (e.g. `fs::path`).
            total = canon.get_num_template_arguments()
            for i in range(total if n_value_args < 0 else min(n_value_args, total)):
                arg = canon.get_template_argument_type(i)
                if arg.kind == TypeKind.INVALID:
                    continue  # a non-type argument (e.g. an extent): nothing to bind
                if (sub := _casters_for(arg, depth + 1)) is None:
                    return None
                need |= sub
            return need
        if canon.get_num_template_arguments() >= 0:
            return None  # some other template: nanobind has no caster for it
        return set() if _is_bound_class(decl) else None

    return None


def _range_value_type(t) -> tuple[str, set] | None:
    """`(element spelling, casters)` if *t* is a MimIR range, else None.

    The element spelling is the range's `value_type`, i.e. suitable as
    `std::vector<...>`: a `Span<const T* const>` yields `const T*`, and a
    `XTrie<D, K>::Set` — whose iterator hands out `D*` — yields `D*`.
    """
    canon = t.get_canonical()
    if canon.kind in (TypeKind.LVALUEREFERENCE, TypeKind.RVALUEREFERENCE):
        canon = canon.get_pointee().get_canonical()  # `const DefVec&` is a range, too
    if canon.kind != TypeKind.RECORD:
        return None
    decl = canon.get_declaration()
    name = _qualified_name(decl)

    # `XTrie` hands out pointers to its element type, the other two store the
    # element type itself.
    elem, elem_is_pointee = None, False
    if name in _MIM_RANGES:
        elem = canon.get_template_argument_type(0)
    elif name == _MIM_SET:
        # `Set` is nested in the `XTrie<D, K, N>` specialization, which is where the
        # element type D lives.
        parent = decl.semantic_parent
        if parent is not None:
            elem, elem_is_pointee = parent.type.get_template_argument_type(0), True

    if elem is None or elem.kind == TypeKind.INVALID:
        return None
    if (casters := _casters_for(elem)) is None:
        return None

    value = _TRAILING_CONST.sub("", _canon_spelling(elem))  # `T* const` → `T*`
    if elem_is_pointee:
        value += "*"
    elif elem.get_canonical().kind != TypeKind.POINTER:
        # `std::vector` requires a non-const value_type. Only a top-level const
        # is dropped — for a pointer element the const belongs to the pointee
        # (`Span<const Def* const>` → `const Def*`) and must stay.
        value = _strip_cv(value)
    return value, casters | {"vector.h"}


# ---------------------------------------------------------------------------
#  Extraction helpers
# ---------------------------------------------------------------------------

def _is_public(cursor) -> bool:
    try:
        return cursor.access_specifier == clang.AccessSpecifier.PUBLIC
    except Exception:
        return True


def _is_deleted(cursor) -> bool:
    """Whether *cursor* declares a `= delete`d function.

    libclang exposes this directly; neither the extent nor the doc comment ever
    contains the declaration's source text, so they cannot be searched for it.
    `availability` is the fallback for libclang builds predating
    `is_deleted_method()` — a deleted function is reported as NOT_AVAILABLE.
    """
    try:
        return bool(cursor.is_deleted_method())
    except Exception:
        pass
    try:
        return cursor.availability == clang.AvailabilityKind.NOT_AVAILABLE
    except Exception:
        return False


def _nested_decl(t):
    """If *t* (through ptr/ref/cv) names a type nested in a class, return
    ``(parent_class_name, declaration_cursor)``; otherwise ``None``.
    """
    try:
        canon = t.get_canonical()
        if canon.kind in (TypeKind.POINTER, TypeKind.LVALUEREFERENCE, TypeKind.RVALUEREFERENCE):
            decl = canon.get_pointee().get_declaration()
        else:
            decl = t.get_declaration()
        if decl is None or not decl.spelling:
            return None
        parent = decl.semantic_parent
        if parent is not None and parent.kind in _RECORD_KINDS:
            return parent.spelling, decl
    except Exception:
        pass
    return None


def _resolve_param_type(p) -> tuple[str, set] | None:
    """``(spelling to emit, casters needed)``, or ``None`` to skip the whole method.

    - a type nanobind cannot convert makes the whole method unbindable → skip
      (this also covers function pointers, which have no caster);
    - a MimIR range (`Defs`, `DefVec`) is taken as the corresponding
      `std::vector`, which converts implicitly at the call site, so Python can
      pass a plain list;
    - a param declared through a nested type is spelled unqualified by libclang
      (`Lam::Filter` → `Filter`), which would not resolve at namespace scope. A
      nested *enum* is repaired by qualifying it (``Level`` → ``Log::Level``);
      anything else has to be skipped.
    """
    if rng := _range_value_type(p.type):
        value, casters = rng
        return f"const std::vector<{value}>&", casters
    if (casters := _casters_for(p.type)) is None:
        return None
    spelling = _type_spelling(p.type)
    if nd := _nested_decl(p.type):
        parent_name, decl = nd
        if decl.kind != CursorKind.ENUM_DECL:
            return None
        spelling = re.sub(
            rf"\b{re.escape(decl.spelling)}\b", f"{parent_name}::{decl.spelling}", spelling, count=1
        )
    return spelling, casters


_UNDEDUCED = re.compile(r"\b(auto|decltype)\b")
# A template argument that is a lone uppercase letter is a template parameter
# (e.g. `Vector<R>`), never a concrete type in this codebase.
_TEMPLATE_PARAM_ARG = re.compile(r"<\s*[A-Z]\s*[,>]")


def _is_unresolved_return(ret: str) -> bool:
    """True if *ret* is not a concrete, bindable type.

    Catches undeduced `auto`/`decltype(auto)` and dependent template parameters
    such as `Vector<R>` (a single uppercase-letter template argument), which
    arise from the MIM_PROJ mixin methods and cannot be bound as written.
    """
    return bool(_UNDEDUCED.search(ret) or _TEMPLATE_PARAM_ARG.search(ret))


def _is_copy_or_move_ctor(cursor, class_name: str) -> bool:
    """True for the copy/move constructors of *class_name*.

    These are never meaningful to expose to Python and are frequently deleted
    (e.g. `Driver`, `World`), which would make the `nb::init<>` binding ill-formed.
    """
    try:
        if cursor.is_copy_constructor() or cursor.is_move_constructor():
            return True
    except Exception:
        pass
    params = [p for p in cursor.get_children() if p.kind == CursorKind.PARM_DECL]
    if len(params) != 1:
        return False
    base = params[0].type.spelling
    for tok in ("const", "&&", "&", "class ", "struct "):
        base = base.replace(tok, "")
    return base.strip().split("::")[-1] == class_name


def _header_stem(header_path: str) -> str:
    """`include/mim/util/log.h` → `log`: names the init function and .nbextra file."""
    return os.path.splitext(os.path.basename(header_path))[0]


def _samefile(a, b):
    try:
        return os.path.samefile(a, b)
    except OSError:
        return os.path.abspath(a) == os.path.abspath(b)


def _param_default(p) -> str | None:
    """The source text of a parameter's default argument (after `=`), or None."""
    try:
        toks = [t.spelling for t in p.get_tokens()]
        if "=" in toks:
            return " ".join(toks[toks.index("=") + 1:]).strip() or None
    except Exception:
        pass
    return None


# ---------------------------------------------------------------------------
#  Extraction: collect classes, methods, enums from a header
# ---------------------------------------------------------------------------

# Word boundaries in a CamelCase identifier: after a lowercase letter, and before
# the last capital of a run (`isaLit` → `isa_lit`, `MIMError` → `mim_error`).
_CAMEL_BOUNDARIES = (re.compile(r"(?<=[a-z])(?=[A-Z])"), re.compile(r"(?<=[A-Z])(?=[A-Z][a-z])"))


def _camel_to_snake(name: str) -> str:
    for boundary in _CAMEL_BOUNDARIES:
        name = boundary.sub("_", name)
    return name.lower()


@dataclass
class Param:
    """One parameter of a member to bind."""

    spelling: str          # the C++ type to emit in the generated lambda
    name: str = ""         # empty for an unnamed parameter
    default: str | None = None  # source text of its default argument, if any


@dataclass
class MethodInfo:
    """One member to bind: a method, constructor, static method or data field."""

    name: str
    return_type: str
    params: list[Param] = field(default_factory=list)
    class_name: str = ""
    is_const: bool = False
    is_static: bool = False
    is_constructor: bool = False
    is_field: bool = False
    # nanobind caster headers this signature needs (see `_casters_for`).
    casters: set = field(default_factory=set)
    # Element type if the return is a MimIR range copied into a std::vector.
    range_elem: str | None = None

    @property
    def py_name(self) -> str:
        return _camel_to_snake(self.name)


@dataclass
class ClassInfo:
    """One class to register, as parsed from the header."""

    bases: list[str] = field(default_factory=list)
    methods: list[MethodInfo] = field(default_factory=list)
    # A class deriving from std::exception is bound via nb::exception<> instead.
    is_exception: bool = False
    # An abstract class cannot be constructed from Python.
    is_abstract: bool = False


def _extract_params(cursor) -> tuple[list[Param], set] | None:
    """``(parameters, casters needed)`` for a function cursor, or ``None`` to skip.

    ``None`` signals that a parameter type is unbindable (see `_resolve_param_type`),
    so the whole method/constructor must be dropped.
    """
    params, casters = [], set()
    for p in cursor.get_children():
        if p.kind == CursorKind.PARM_DECL:
            if (resolved := _resolve_param_type(p)) is None:
                return None
            spelling, needed = resolved
            params.append(Param(spelling, p.spelling, _param_default(p)))
            casters |= needed
    return params, casters


def _no_caster_reason(role: str, t) -> str:
    """Why nanobind cannot convert *t*, including how libclang resolved it.

    The canonical kind and spelling are part of the message on purpose: when a
    member is bound on one platform but not on another, the difference is always in
    what libclang made of the type, and that is otherwise invisible — macOS reports
    libc++'s `std::string` as `RECORD 'std::string'` where Linux says
    `RECORD 'std::basic_string<char>'`.
    """
    canon = t.get_canonical()
    return f"no caster for {role} type {_type_spelling(t)!r} ({canon.kind.name} {canon.spelling!r})"


def _why_unbindable(cursor) -> str:
    """Which type in *cursor*'s signature cannot be bound, and why.

    Checks the signature in the same order as `_extract_method`/`_resolve_param_type`
    decide it, so the reason reported is the one that actually caused the drop.
    """
    params = [p.type for p in cursor.get_children() if p.kind == CursorKind.PARM_DECL]
    for role, t in [("return", cursor.result_type), *(("parameter", p) for p in params)]:
        if _range_value_type(t) is None and _casters_for(t) is None:
            return _no_caster_reason(role, t)
        # Only a parameter has to be *spelled* in the generated lambda.
        if role == "parameter" and (nd := _nested_decl(t)) and nd[1].kind != CursorKind.ENUM_DECL:
            return f"parameter type {_type_spelling(t)!r} is nested, so libclang spells it unqualified"
    return "reason unclear — please report this"


def _signature(cursor, class_name: str) -> str:
    """`World::set(std::string_view): no caster for …` — one dropped-member note."""
    return f"{class_name}::{cursor.displayname}: {_why_unbindable(cursor)}"


def _extract_method(cursor, class_name: str, drops: list[str]) -> MethodInfo | None:
    """The binding for one member, or None if it must not or cannot be bound.

    A member dropped because nanobind cannot convert a type in its signature is
    appended to *drops*; the deliberate omissions (deleted, non-public, operators,
    destructors, copy/move constructors) are not — they are never Python API.
    """
    if (
        _is_deleted(cursor)
        or cursor.kind == CursorKind.DESTRUCTOR
        or cursor.spelling.startswith("operator")
        or not _is_public(cursor)
    ):
        return None

    if cursor.kind == CursorKind.CONSTRUCTOR:
        if _is_copy_or_move_ctor(cursor, class_name):
            return None
        if (extracted := _extract_params(cursor)) is None:
            drops.append(_signature(cursor, class_name))
            return None
        params, casters = extracted
        return MethodInfo(
            name=cursor.spelling,
            return_type="",
            params=params,
            class_name=class_name,
            is_constructor=True,
            casters=casters,
        )

    ret = _type_spelling(cursor.result_type)
    # Skip methods whose return type libclang could not resolve to a concrete
    # type (the MIM_PROJ mixin methods): either a bare `auto`/`decltype` or a
    # dependent template parameter such as `Vector<R>`. The bindability check
    # below rejects those too, but only via their canonical type — this catches
    # them by their written spelling, which is what the emitted code uses to pick
    # a return-value policy.
    if _is_unresolved_return(ret):
        drops.append(f"{class_name}::{cursor.displayname}: libclang did not resolve the return type {ret!r}")
        return None
    # A MimIR range return is copied into a std::vector so it reaches Python as
    # a list; anything else must be a type nanobind can convert.
    range_elem = None
    if rng := _range_value_type(cursor.result_type):
        range_elem, ret_casters = rng
    elif (ret_casters := _casters_for(cursor.result_type)) is None:
        drops.append(_signature(cursor, class_name))
        return None
    if (extracted := _extract_params(cursor)) is None:
        drops.append(_signature(cursor, class_name))
        return None
    params, casters = extracted

    return MethodInfo(
        name=cursor.spelling,
        return_type=ret,
        params=params,
        class_name=class_name,
        is_const=cursor.is_const_method(),
        is_static=cursor.is_static_method(),
        casters=casters | ret_casters,
        range_elem=range_elem,
    )


def _print_capped(lines: list[str], prefix: str) -> None:
    """Print *lines* to stderr, each behind *prefix*, summarising past `_MAX_REPORTED`."""
    for line in lines[:_MAX_REPORTED]:
        print(f"{prefix}{line}", file=sys.stderr)
    if len(lines) > _MAX_REPORTED:
        print(f"{prefix}... and {len(lines) - _MAX_REPORTED} more", file=sys.stderr)


def _report_diagnostics(tu, header_path: str) -> int:
    """Print libclang's *fatal* diagnostics for *tu* to stderr; return their count.

    A fatal (e.g. `'vector' file not found`) aborts the parse and leaves a
    partial, garbage AST that the generator then turns into uncompilable
    bindings — the classic symptom being output that only breaks on one
    toolchain (e.g. a Windows/MSVC tree whose STL/SDK headers libclang cannot
    locate).  Non-fatal errors are deliberately ignored: libclang lags the
    bleeding-edge standard libraries it parses, so a clean, fully-compilable run
    still emits plenty of benign `error`-level noise.
    """
    fatals = [d for d in tu.diagnostics if d.severity >= clang.Diagnostic.Fatal]
    # The first few point at the root cause.
    _print_capped([f"{header_path}: {d.spelling} [{d.location}]" for d in fatals], "  libclang: ")
    return len(fatals)


def _is_bindable_record(cursor) -> bool:
    """True if *cursor* is a class definition worth registering as a Python type."""
    if not cursor.is_definition():
        return False  # a forward/opaque declaration has nothing to bind
    name = cursor.spelling
    if not name or name.startswith("__") or name.startswith("("):
        return False
    # Skip template specializations (e.g. `template<> struct fe::is_bit_enum<mim::Dep>`
    # or `formatter<...>`): they are not real, bindable classes.
    return "<" not in cursor.displayname


def _extract_class(cursor, enums: list, drops: list[str]) -> ClassInfo:
    """Collect the members to bind for the class at *cursor*.

    Nested enums are appended to *enums*, since they are bound at module scope
    rather than inside the class; members left out as unbindable land in *drops*.
    """
    info = ClassInfo(is_abstract=cursor.is_abstract_record())
    for child in cursor.get_children():
        kind = child.kind
        if kind == CursorKind.CXX_BASE_SPECIFIER:
            base = _type_spelling(child.type).strip()
            # A class deriving from std::exception is bound as a Python
            # exception, not as an ordinary class.
            if "exception" in base:
                info.is_exception = True
            # Only keep bases that are themselves bindable mim classes. Foreign
            # (`fe::SymPool`, `std::true_type`) and template mixin bases
            # (`fe::RuntimeCast<Def>`) are not registered nanobind types, so
            # binding against them would not compile.
            if "::" not in base and "<" not in base:
                info.bases.append(base)
        elif kind in (CursorKind.CXX_METHOD, CursorKind.CONSTRUCTOR):
            if mi := _extract_method(child, cursor.spelling, drops):
                info.methods.append(mi)
        elif kind == CursorKind.FIELD_DECL and _is_public(child):
            if (casters := _casters_for(child.type)) is None:
                drops.append(
                    f"{cursor.spelling}::{child.spelling}: {_no_caster_reason('field', child.type)}"
                )
                continue  # nanobind cannot convert the field's type
            info.methods.append(MethodInfo(
                name=child.spelling, return_type=_type_spelling(child.type), params=[],
                class_name=cursor.spelling, is_field=True,
                is_const=child.type.is_const_qualified(), casters=casters,
            ))
        elif kind == CursorKind.ENUM_DECL and _is_public(child):
            enums.append(child)
    return info


def extract_from_header(tu, header_path: str) -> tuple[dict[str, ClassInfo], list, list[str]]:
    """The classes and enums *header_path* itself declares, in declaration order.

    Declarations pulled in from other headers are ignored — each one is bound by
    the unit generated for the header that owns it. The third result lists the
    members left out because nanobind cannot convert a type in their signature.
    """
    classes: dict[str, ClassInfo] = {}
    enums: list = []
    drops: list[str] = []

    def walk(cursor):
        loc = cursor.location
        if not loc.file or not _samefile(str(loc.file), header_path):
            return
        if cursor.kind == CursorKind.NAMESPACE:
            for child in cursor.get_children():
                walk(child)
        elif cursor.kind == CursorKind.ENUM_DECL:
            enums.append(cursor)
        elif cursor.kind in _RECORD_KINDS and _is_bindable_record(cursor):
            classes[cursor.spelling] = _extract_class(cursor, enums, drops)

    for child in tu.cursor.get_children():
        walk(child)

    return classes, enums, drops


# ---------------------------------------------------------------------------
#  Code generation
# ---------------------------------------------------------------------------

# Casters every generated unit includes: they cost nothing extra to include and
# the hand-written .nbextra fragments rely on them being there.
_BASE_CASTERS = frozenset({"string.h", "string_view.h", "vector.h"})


def _includes_block(casters: set) -> str:
    """The nanobind includes for a unit: the core header plus the casters it uses.

    A caster header is what teaches nanobind to convert a type; without it the
    binding compiles but neither works at runtime nor renders in `_mim.pyi`.
    """
    lines = ["#include <nanobind/nanobind.h>"]
    lines += [f"#include <nanobind/stl/{c}>" for c in sorted(_BASE_CASTERS | casters)]
    # Every unit needs the nanobind::detail::type_hook<mim::Def> specialisation in scope: mim::Def is not
    # polymorphic, so without it nanobind hands Python the base `Def` instead of the concrete node class.
    lines += ["", '#include "nb_type_hook.h"']
    return "\n".join(lines) + "\n"


def _returns_lvalue_ref(ret_type: str) -> bool:
    norm = ret_type.replace(" ", "")
    return norm.endswith("&") and not norm.endswith("&&")


def _policy_suffix(ret_type: str, static: bool) -> str:
    """`, nb::rv_policy::…` for a return that must not be copied into Python.

    A reference or pointer to a bound C++ object has to reach Python as a
    non-owning handle; `reference_internal` ties its lifetime to `self`, which a
    static method does not have. Value returns are moved by nanobind's default
    `automatic` policy and need no annotation.
    """
    if not (_returns_lvalue_ref(ret_type) or "*" in ret_type):
        return ""
    return f", nb::rv_policy::{'reference' if static else 'reference_internal'}"


def _gen_constructor_binding(mi: MethodInfo, indent: str = "    ") -> str:
    # nanobind has no defaulted `nb::init`, so a defaulted parameter becomes one
    # overload per callable arity.
    required = sum(1 for p in mi.params if p.default is None)
    specs = [", ".join(p.spelling for p in mi.params[:n]) for n in range(required, len(mi.params) + 1)]
    return "\n".join(f"{indent}.def(nb::init<{spec}>())" for spec in specs)


def _gen_field_binding(mi: MethodInfo, indent: str = "    ") -> str:
    # Public data members are exposed as writable properties, unless the field
    # is const (or a reference we can't rebind), in which case it is read-only.
    accessor = "def_ro" if (mi.is_const or "&" in mi.return_type) else "def_rw"
    return f'{indent}.{accessor}("{mi.py_name}", &{mi.class_name}::{mi.name})'


# Default values simple enough to reproduce verbatim in a nb::arg() default.
_SAFE_DEFAULT = re.compile(r"^(true|false|nullptr|-?\d+[uUlL]*|0[xX][0-9a-fA-F]+[uUlL]*)$")


def _param_names(mi: MethodInfo) -> list[str]:
    """The parameter names to emit; an unnamed parameter gets a synthetic `a<i>`."""
    return [p.name or f"a{i}" for i, p in enumerate(mi.params)]


def _arg_spec(mi: MethodInfo) -> str:
    """`, nb::arg(...)` clause carrying parameter default values, or "".

    Only emitted when every defaulted parameter has a literal we can reproduce;
    otherwise the parameters simply stay required (C++ guarantees defaults are
    trailing, so we never emit a default ahead of a non-default one).
    """
    given = [p.default for p in mi.params if p.default is not None]
    if not given or not all(_SAFE_DEFAULT.match(d) for d in given):
        return ""
    parts = [
        f'nb::arg("{name}")' + (f" = {p.default}" if p.default is not None else "")
        for name, p in zip(_param_names(mi), mi.params)
    ]
    return ", " + ", ".join(parts)


def _lambda_params(mi: MethodInfo) -> tuple[str, str]:
    """The `(declaration, call)` strings for a method's parameters."""
    names = _param_names(mi)
    decls = [f"{p.spelling} {name}" for p, name in zip(mi.params, names)]
    return ", ".join(decls), ", ".join(names)


def _range_copy_lambda(params: str, call: str, elem: str) -> str:
    """A lambda that copies a MimIR range return into a `std::vector`.

    `Defs`, `DefVec`, `Vars` and `Muts` have no nanobind caster, but a vector of
    their element type converts to a Python list.
    """
    return f'[]({params}) {{ auto _v = {call}; return std::vector<{elem}>(_v.begin(), _v.end()); }}'


def _gen_method_lambda(mi: MethodInfo, indent: str = "    ") -> str:
    """Bind a (static) method through a call-site lambda.

    A lambda rather than a pointer-to-member: taking `&Class::method` is
    ambiguous whenever the method has template or const/non-const overloads, so
    `overload_cast` fails, whereas a lambda lets ordinary C++ overload resolution
    pick the right one.
    """
    decls, args = _lambda_params(mi)
    if mi.is_static:
        params, call = decls, f"{mi.class_name}::{mi.name}({args})"
    else:
        self_param = f"{'const ' if mi.is_const else ''}{mi.class_name}& self"
        params = f"{self_param}, {decls}" if decls else self_param
        call = f"self.{mi.name}({args})"

    if mi.range_elem:
        body = _range_copy_lambda(params, call, mi.range_elem)
        # A range is copied by value, but its elements are the bound objects the
        # reference policy is about.
        policy = f", nb::rv_policy::{'reference' if mi.is_static else 'reference_internal'}"
    else:
        # An lvalue-reference return is bound through its address so it reaches
        # Python as a non-owning pointer instead of being copied by value (which
        # fails for non-copyable or forward-declared types such as World/Driver).
        ret = "&" + call if _returns_lvalue_ref(mi.return_type) else call
        body = f"[]({params}) {{ return {ret}; }}"
        policy = _policy_suffix(mi.return_type, static=mi.is_static)

    definer = "def_static" if mi.is_static else "def"
    return f'{indent}.{definer}("{mi.py_name}", {body}{policy}{_arg_spec(mi)})'


def _generate_method_binding(mi: MethodInfo, indent: str = "    ") -> str:
    if mi.is_constructor:
        return _gen_constructor_binding(mi, indent)
    if mi.is_field:
        return _gen_field_binding(mi, indent)
    return _gen_method_lambda(mi, indent)


def _enum_qualified_name(cursor) -> str:
    """Enum name qualified by any enclosing classes (e.g. `Log::Level`).

    The enclosing namespace is dropped because the binding is emitted inside it.
    """
    parts = [cursor.spelling]
    p = cursor.semantic_parent
    while p is not None and p.kind in _RECORD_KINDS:
        parts.append(p.spelling)
        p = p.semantic_parent
    return "::".join(reversed(parts))


def _gen_enum_binding(cursor, indent: str = "    ") -> str:
    name = _enum_qualified_name(cursor)
    lines = [f'{indent}nb::enum_<{name}>(m, "{name.split("::")[-1]}")']
    for child in cursor.get_children():
        if child.kind == CursorKind.ENUM_CONSTANT_DECL:
            lines.append(f'{indent}    .value("{child.spelling}", {name}::{child.spelling})')
    # Export enumerators into the enclosing scope to match hand-written bindings.
    lines.append(f'{indent}    .export_values();')
    return "\n".join(lines)


def _base_spec(bases: list[str]) -> str:
    if not bases:
        return ""
    return f", {bases[0].split('::')[-1]}"


@dataclass
class Extra:
    """The hand-written fragments a companion .nbextra file injects into a unit.

    Section headers are bracketed tags on their own line:
        [include]           — extra #include lines appended after the standard nanobind includes
        [class:ClassName]   — method-chain fragment appended to the named class binding
        [skip:ClassName]    — whitespace-separated method names to drop from that class
                              (C++ name or snake_case py_name); pair with [class:...] to
                              substitute a hand-written binding for something the generator
                              can't express
        [standalone]        — raw code injected inside init_* after all class/enum blocks
    """

    stem: str = ""
    includes: str = ""
    standalone: str = ""
    classes: dict[str, str] = field(default_factory=dict)
    skips: dict[str, set] = field(default_factory=dict)

    def add_section(self, tag: str, body: str) -> None:
        """Record one parsed section; an unrecognised tag warns and is dropped."""
        if tag == "include":
            self.includes = body
        elif tag == "standalone":
            self.standalone = body
        elif tag.startswith("class:"):
            self.classes[tag.removeprefix("class:")] = body
        elif tag.startswith("skip:"):
            self.skips.setdefault(tag.removeprefix("skip:"), set()).update(body.split())
        else:
            print(f"warning: {self.stem}.nbextra: unrecognised section [{tag}] — skipped", file=sys.stderr)


def _parse_extra_file(content: str, stem: str) -> Extra:
    """Parse a structured .nbextra file (see `Extra`).

    Content before the first section header is ignored.
    """
    extra = Extra(stem=stem)
    tag: str | None = None
    body: list[str] = []

    def flush() -> None:
        if tag is not None and (text := "\n".join(body).strip()):
            extra.add_section(tag, text)
        body.clear()

    for line in content.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]") and len(stripped) > 2:
            flush()
            tag = stripped[1:-1].strip()
        else:
            body.append(line)
    flush()
    return extra


def _load_extra(header_path: str, extra_dir: str | None) -> Extra:
    """Load and parse the companion .nbextra file for *header_path*, if any."""
    stem = _header_stem(header_path)
    if not extra_dir:
        return Extra(stem=stem)
    try:
        with open(os.path.join(extra_dir, stem + ".nbextra")) as f:
            return _parse_extra_file(f.read(), stem)
    except OSError:
        return Extra(stem=stem)


class ExtraSectionError(Exception):
    """A .nbextra section targets a class that will never receive it."""


def _check_extra_sections(extra: Extra, classes: dict[str, ClassInfo], header_path: str) -> None:
    """Reject `[class:X]`/`[skip:X]` sections that cannot be applied.

    Both are matched against the C++ class name verbatim, so a typo or a stale
    name left behind by a rename yields a module that still compiles but is
    missing every binding the section declared.
    """
    header = os.path.basename(header_path)
    problems = []

    for kind, names in (("class", extra.classes), ("skip", extra.skips)):
        for name in names:
            if name in classes:
                continue
            hint = difflib.get_close_matches(name, classes, n=1, cutoff=0.6)
            # Case-only mismatches score below the cutoff for short names,
            # so check for them explicitly — they are the common typo.
            if not hint:
                hint = [c for c in classes if c.lower() == name.lower()]
            suggestion = f"; did you mean [{kind}:{hint[0]}]?" if hint else ""
            problems.append(
                f"  [{kind}:{name}] matches no class parsed from {header}{suggestion}"
            )

    # Exception classes are registered via nb::exception<>, which takes no
    # `.def(...)` chain, so a [class:X] body aimed at one is dropped as well.
    for name in extra.classes:
        if name in classes and classes[name].is_exception:
            problems.append(
                f"  [class:{name}] targets an exception class registered via "
                f"nb::exception<{name}>, which accepts no .def(...) chain"
            )

    if problems:
        known = ", ".join(sorted(classes)) or "(none)"
        raise ExtraSectionError(
            f"{extra.stem}.nbextra: unusable section(s):\n"
            + "\n".join(problems)
            + f"\nClasses available from {header}: {known}"
        )


def _bindable_methods(class_name: str, info: ClassInfo, extra: Extra) -> list[MethodInfo]:
    """*info*'s members minus the ones this class must not or cannot expose."""
    methods = info.methods

    # Abstract classes cannot be constructed from Python (placement-new of an
    # abstract type is ill-formed), so drop their constructor bindings.
    if info.is_abstract:
        methods = [m for m in methods if not m.is_constructor]

    # Explicit [skip:Class] denylist: drop members the generator can't express
    # (matched by C++ name or snake_case py_name); a companion [class:Class]
    # section can substitute a hand-written binding.
    if skip := extra.skips.get(class_name):
        methods = [m for m in methods if m.name not in skip and m.py_name not in skip]

    # nanobind rejects a Python name carrying both static and instance overloads
    # (e.g. Def::zonk() const vs. static Def::zonk(Defs)). Keep the instance
    # overloads and drop the colliding static ones.
    instance = {m.py_name for m in methods if not (m.is_static or m.is_constructor or m.is_field)}
    return [m for m in methods if not (m.is_static and m.py_name in instance)]


def _gen_class_binding(class_name: str, info: ClassInfo, methods: list[MethodInfo], extra: Extra) -> list[str]:
    """The registration block for one class, as lines."""
    # A class deriving from std::exception is registered as a Python exception
    # rather than an ordinary class.
    if info.is_exception:
        return [f'    nb::exception<{class_name}>(m, "{class_name}");']

    # Def and everything deriving from it is never_destruct — the World owns all
    # Def lifetimes.
    never_destruct = class_name == "Def" or "Def" in info.bases
    head = (
        f'    nb::class_<{class_name}{_base_spec(info.bases)}>'
        f'(m, "{class_name}"{", nb::never_destruct()" if never_destruct else ""})'
    )

    # The `.def(...)` chain: generated bindings first, hand-written ones last.
    chain = [_generate_method_binding(mi) for mi in methods]
    if class_extra := extra.classes.get(class_name):
        chain.append(class_extra.strip())
    if not chain:
        return [head + ";"]

    lines = [f"{head} {chain[0].strip()}", *chain[1:]]
    if not lines[-1].rstrip().endswith(";"):  # a hand-written fragment may close itself
        lines[-1] += ";"
    return lines


def generate_bindings(
    header_path: str, classes: dict[str, ClassInfo], enums: list, ns: str = "", extra_dir: str | None = None
) -> str:
    """The complete nanobind translation unit registering *classes* and *enums*."""
    extra = _load_extra(header_path, extra_dir)
    # An unmatched [class:X]/[skip:X] is almost always a typo or a stale name
    # after a C++ rename. Silently dropping the section produces a module that
    # compiles cleanly but is missing every binding the section declared, so fail
    # loudly here — before emitting anything.
    _check_extra_sections(extra, classes, header_path)

    # An exception class is registered without a `.def(...)` chain, so none of its
    # members — and none of their casters — reach the output.
    bindable = {
        name: [] if info.is_exception else _bindable_methods(name, info, extra)
        for name, info in classes.items()
    }

    lines = []
    if extra.includes:
        lines += [extra.includes, ""]
    # The two runtime hub types are cross-referenced by most accessors (e.g.
    # `Def::world()`, `World::driver()`); include their full definitions so
    # nanobind can cast references/pointers to them (their headers are on the
    # PUBLIC include path of libmim).
    lines += [
        f'#include "{os.path.relpath(header_path, os.getcwd())}"',
        "#include <mim/driver.h>",
        "#include <mim/world.h>",
        "",
        "namespace nb = nanobind;",
        "",
    ]
    if ns:
        lines += [f"namespace {ns} {{", ""]
    lines += [f"void init_{_header_stem(header_path)}(nb::module_& m) {{", "    // clang-format off"]

    for cursor in enums:
        lines += ["", _gen_enum_binding(cursor)]
    for class_name, info in classes.items():
        lines += ["", *_gen_class_binding(class_name, info, bindable[class_name], extra)]
    if extra.standalone:
        lines += ["", extra.standalone]

    lines += ["", "    // clang-format on", "}"]
    if ns:
        lines += ["", f"}} // namespace {ns}"]
    lines.append("")

    # Only the bindings that survived the filtering need a caster, so the include
    # block is assembled last.
    casters = {c for methods in bindable.values() for mi in methods for c in mi.casters}
    return "\n".join([_includes_block(casters), *lines])


def generate_module(units: list[tuple[str, str]], module_name: str = "_mim") -> str:
    """Emit the ``NB_MODULE`` entry point that composes every init function.

    *units* is an ordered list of ``(namespace, init_name)`` pairs; the calls are
    emitted in that order (foundational types first so signatures resolve to
    Python class names).
    """
    lines = ["#include <nanobind/nanobind.h>", "", "namespace nb = nanobind;", ""]

    # Group the forward declarations by namespace; a dict keeps first-seen order.
    by_ns: dict[str, list[str]] = {}
    for ns, init in units:
        by_ns.setdefault(ns, []).append(init)
    for ns, inits in by_ns.items():
        decls = " ".join(f"void {i}(nb::module_&);" for i in inits)
        lines.append(f"namespace {ns} {{ {decls} }}" if ns else decls)

    lines.append("")
    lines.append(f"NB_MODULE({module_name}, m) {{")
    for ns, init in units:
        lines.append(f"    {ns}::{init}(m);" if ns else f"    {init}(m);")
    lines.append("}")
    lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
#  CMake integration: derive the real per-target compile flags from build/compile_commands.json
# ---------------------------------------------------------------------------

def _load_compile_commands(build_dir: Path) -> list | None:
    """Load `<build_dir>/compile_commands.json`, or return None if unavailable."""
    cc = Path(build_dir) / "compile_commands.json"
    if not cc.is_file():
        return None
    try:
        with open(cc) as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"warning: could not read {cc}: {e}", file=sys.stderr)
        return None


@lru_cache(maxsize=None)
def _anchor_base(anchor: str) -> Path:
    return (_REPO_ROOT / anchor).resolve()


def _rel_under(path, anchor: str) -> Path | None:
    """Path relative to `<repo>/<anchor>`, or None if it does not live there."""
    try:
        return Path(path).resolve().relative_to(_anchor_base(anchor))
    except ValueError:
        return None


def _match_cc_entry(header_path: str, entries: list) -> dict | None:
    """Pick the compile-command entry whose target best matches *header_path*.

    Headers under `include/<sub>` are mapped to sources under `src/<sub>`; the
    entry sharing the longest directory prefix (plus a bonus for a matching
    file stem) wins. This routes a plugin header to its plugin module's flags
    rather than to libmim's, picking up any target-specific defines.
    """
    if not entries:
        return None
    hrel = _rel_under(header_path, "include")
    best, best_score = entries[0], -1
    for e in entries:
        srel = _rel_under(e.get("file", ""), "src")
        score = 0
        if hrel is not None and srel is not None:
            for x, y in zip(hrel.parts[:-1], srel.parts[:-1]):
                if x == y:
                    score += 1
                else:
                    break
            if hrel.stem == srel.stem:
                score += 1
        if score > best_score:
            best_score, best = score, e
    return best


def _norm_std(value: str) -> str:
    """Normalize a `-std`/`/std:` value to one libclang accepts."""
    v = value.strip().lower()
    # MSVC's open-ended 'latest' has no libclang equivalent; pin it to the
    # standard we otherwise parse with.
    return "c++23" if v in ("c++latest", "c++2b") else v


def _strip_prefix(s: str, prefixes: tuple) -> str | None:
    """The remainder of *s* after the first matching (non-empty) prefix, else None."""
    for p in prefixes:
        if s.startswith(p) and len(s) > len(p):
            return s[len(p):]
    return None


# GNU value-carrying flags (separate form: flag then value) and their glued
# prefixes. These are passed through *verbatim* — the exact form libclang parses
# natively; rewriting them (e.g. re-gluing `-isystem <dir>`) regressed libc++
# discovery on macOS.
_GNU_VALUE_FLAGS = ("-I", "-isystem", "-iquote", "-D", "-U", "-include")
_GNU_GLUED_PREFIXES = ("-I", "-isystem", "-iquote", "-D", "-U", "-std=", "-include")


def _flags_from_cc_entry(entry: dict) -> list:
    """Extract the parse-relevant flags from a compile-command entry.

    Only include directories, preprocessor defines and the C++ standard affect
    how libclang parses a header; codegen, warning and other toolchain flags are
    irrelevant and dropped.  GNU spellings (`-I`, `-isystem`, `-D`, `-std=`) are
    forwarded verbatim; MSVC / clang-cl spellings (`/I`, `-external:I`, `-imsvc`,
    `/D`, `-std:`, `/std:`) are translated to their GNU equivalents so an MSVC
    `compile_commands.json` still yields usable defines and standard flags.
    """
    if "arguments" in entry:
        raw = list(entry["arguments"])
    elif os.name == "nt":
        # POSIX shlex treats '\' as an escape and would destroy the backslash
        # paths in a Windows `command` string; split without POSIX rules and
        # drop the surviving surrounding quotes.
        raw = [t.strip('"') for t in shlex.split(entry.get("command", ""), posix=False)]
    else:
        raw = shlex.split(entry.get("command", ""))

    out: list = []
    i, n = 0, len(raw)
    while i < n:
        a = raw[i]
        nxt = raw[i + 1] if i + 1 < n else None
        step = 1

        # --- GNU spellings: forward verbatim (native libclang form) ---
        if a in _GNU_VALUE_FLAGS and nxt is not None:
            out += [a, nxt]; step = 2
        elif a.startswith(_GNU_GLUED_PREFIXES):
            out.append(a)
        # --- MSVC / clang-cl spellings: translate to GNU ---
        elif m := re.match(r"[-/]std:(.+)", a):                           # /std:, -std:
            out.append(f"-std={_norm_std(m.group(1))}")
        elif a == "/I" and nxt is not None:                               # user include (separate)
            out += ["-I", nxt]; step = 2
        elif a.startswith("/I") and len(a) > 2:                           # user include (glued)
            out.append(f"-I{a[2:]}")
        elif a in ("-external:I", "/external:I", "-imsvc") and nxt is not None:
            out += ["-isystem", nxt]; step = 2                            # system include (separate)
        elif (rest := _strip_prefix(a, ("-external:I", "/external:I", "-imsvc"))) is not None:
            out += ["-isystem", rest]                                     # system include (glued)
        elif a in ("/D", "/U") and nxt is not None:                       # define/undef (separate)
            out.append(f"-{a[1]}{nxt}"); step = 2
        elif a.startswith(("/D", "/U")) and len(a) > 2:                   # define/undef (glued)
            out.append(f"-{a[1]}{a[2:]}")

        i += step
    return out


def _resource_include_from(resource_dir: str) -> str | None:
    """`-isystem` flag for a clang resource dir, if it holds the builtin headers."""
    inc = Path(resource_dir.strip()) / "include"
    return f"-isystem{inc}" if (inc / "stddef.h").is_file() else None


def _clang_exes(env_var: str, names: tuple) -> list:
    """Resolved clang executables to try: an `$env_var` override first (if it
    names a clang), then *names* found on `PATH`; de-duplicated, order preserved.
    """
    override = os.environ.get(env_var, "")
    ordered = ([override] if "clang" in os.path.basename(override) else []) + list(names)
    seen, out = set(), []
    for name in ordered:
        path = shutil.which(name)
        if path and path not in seen:
            seen.add(path)
            out.append(path)
    return out


def _parse_search_dirs(stderr: str) -> list:
    """Pull the include directories out of `clang -v` diagnostic output."""
    dirs, capture = [], False
    for line in stderr.splitlines():
        s = line.strip()
        if "search starts here:" in s:
            capture = True
        elif s.startswith("End of search list"):
            break
        elif capture and s and not s.startswith("#"):
            # macOS annotates framework dirs; keep only the path.
            dirs.append(s.replace(" (framework directory)", ""))
    return dirs


def _compiler_system_includes() -> list:
    """Ask a real C++ compiler for its default system include dirs, as `-isystem` flags.

    The pip `libclang` wheel ships no standard library and compile_commands.json
    never lists the toolchain's implicit include dirs, so libclang cannot find
    `<memory>`, `<cstdint>`, … on a toolchain whose stdlib is outside its own
    default search — notably macOS/libc++.  Querying the compiler (`clang++ -v`)
    yields exactly the libc++/libstdc++ and SDK paths it uses, which is portable
    and always correct.  Returns an empty list if no compiler is reachable.
    """
    for path in _clang_exes("CXX", ("clang++", "clang")):
        try:
            proc = subprocess.run(
                [path, "-E", "-x", "c++", "-v", os.devnull],
                capture_output=True, text=True, timeout=30,
            )
        except (OSError, subprocess.SubprocessError):
            continue
        if dirs := _parse_search_dirs(proc.stderr):
            return [f"-isystem{d}" for d in dirs]
    return []


def _clang_resource_include() -> str | None:
    """Locate libclang's builtin headers (stddef.h et al.) as an `-isystem` flag.

    compile_commands.json never records the resource dir, and the pip `libclang`
    wheel ships only the shared library — not its builtin headers — so libclang
    cannot find `stddef.h` on its own.  Ask a real clang where they live
    (`clang -print-resource-dir`), which is correct on Linux, macOS and Windows
    alike; fall back to scanning the usual install locations if none is on PATH.
    """
    # Primary, portable: ask a clang executable for its resource dir.
    for path in _clang_exes("CC", ("clang", "clang++")):
        try:
            proc = subprocess.run(
                [path, "-print-resource-dir"], capture_output=True, text=True, timeout=10
            )
        except (OSError, subprocess.SubprocessError):
            continue
        if proc.returncode == 0 and (flag := _resource_include_from(proc.stdout)):
            return flag

    # Fallback: scan the usual per-platform `.../lib/clang/<ver>` roots.
    roots = [
        Path("/usr/lib/clang"), Path("/usr/lib64/clang"),           # Linux
        Path("/Library/Developer/CommandLineTools/usr/lib/clang"),  # macOS CommandLineTools
        Path("/opt/homebrew/opt/llvm/lib/clang"),                   # macOS Homebrew (arm64)
        Path("/usr/local/opt/llvm/lib/clang"),                      # macOS Homebrew (x86_64)
        Path("C:/Program Files/LLVM/lib/clang"),                    # Windows LLVM installer
    ]
    prefix = os.environ.get("LLVM_PREFIX") or os.environ.get("LLVM_PATH")
    if prefix:
        roots.insert(0, Path(prefix) / "lib" / "clang")
    for base in roots:
        if not base.is_dir():
            continue
        for ver_dir in sorted(base.iterdir(), reverse=True):
            if flag := _resource_include_from(str(ver_dir)):
                return flag
    return None


# ---------------------------------------------------------------------------
#  CLI
# ---------------------------------------------------------------------------

def parse_args(argv=None):
    p = argparse.ArgumentParser(description="Generate nanobind bindings using libclang")
    p.add_argument("headers", nargs="*", help="C++ header files to wrap")
    p.add_argument("--dir", help="Directory of headers (recursive with --recursive)")
    p.add_argument("--recursive", action="store_true")
    p.add_argument("-o", "--output", default=None, help="Output file (default: stdout)")
    p.add_argument("--namespace", default="", help="C++ namespace for the init function (e.g. mim)")
    p.add_argument("--extra-args", default="-std=c++23", help="Extra Clang arguments (default: -std=c++23)")
    p.add_argument(
        "--extra-dir",
        default=None,
        help="Directory containing .nbextra patch files. Each file is named <header_stem>.nbextra "
        "and may contain [include], [class:ClassName], and [standalone] sections.",
    )
    p.add_argument("-I", action="append", dest="includes", default=[], help="Include paths")
    p.add_argument(
        "--bound-header",
        action="append",
        dest="bound_headers",
        default=[],
        metavar="HEADER",
        help="A header whose classes/enums the module registers (repeatable). Only these types "
        "may appear in a generated signature; a member mentioning any other is dropped, since "
        "nanobind cannot convert it and stubgen would leak the raw C++ spelling into _mim.pyi. "
        "Pass every header of the build's manifest, not just the one being generated. "
        "Defaults to the headers given on the command line.",
    )
    p.add_argument(
        "--build-dir",
        default=str(_REPO_ROOT / "build"),
        help="CMake build directory to source compile flags from (default: <repo>/build). "
        "Point at a Release vs Debug tree to flip build-type guards such as NDEBUG.",
    )
    p.add_argument(
        "--no-cmake",
        action="store_true",
        help="Do not read compile_commands.json; fall back to built-in include detection.",
    )
    p.add_argument(
        "--emit-module",
        default=None,
        metavar="MODNAME",
        help="Emit the NB_MODULE(<MODNAME>, m) entry point instead of parsing headers. "
        "Provide the composed init functions via repeated --unit in call order.",
    )
    p.add_argument(
        "--unit",
        action="append",
        default=[],
        metavar="NS#INIT",
        help="For --emit-module: a namespace and init function separated by '#', "
        "e.g. 'mim#init_def', 'fe#init_sym', or 'mim::ast#init_ast' (empty NS allowed).",
    )
    return p.parse_args(argv)


def _find_headers(directory: str, recursive: bool) -> list[str]:
    p = Path(directory)
    pattern = "**/*.h" if recursive else "*.h"
    return sorted(str(f) for f in p.glob(pattern))


def _header_has_decls(path: str) -> bool:
    try:
        with open(path) as f:
            return bool(re.search(r"\b(class|struct|enum)\s+\w+", f.read(8000)))
    except OSError:
        return False


def _write_or_print(code: str, output: str | None) -> None:
    if output:
        # CMake's file(MAKE_DIRECTORY) runs at configure time only, so the
        # output directory may be gone on a later build; recreate it here.
        os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
        with open(output, "w") as f:
            f.write(code)
        print(f"Written to {output}")
    else:
        print(code)


def _split_flags(value: str) -> list:
    """Split a command-line string the way the platform's shell would.

    shlex, not `str.split()`: a quoted define such as `-DNAME="a b"` is one
    argument. On Windows, POSIX rules would treat the `\\` of a path as an escape,
    so split without them and drop the surviving quotes instead.
    """
    if os.name == "nt":
        return [t.strip('"') for t in shlex.split(value, posix=False)]
    return shlex.split(value)


def _toolchain_includes() -> list:
    """The system include flags libclang needs to find the standard library.

    The pip `libclang` wheel ships none. On Windows libclang auto-detects the MSVC
    toolchain, so only its own builtin headers are missing; elsewhere ask the
    compiler for its full list and fall back to those builtins.
    """
    if os.name != "nt" and (includes := _compiler_system_includes()):
        return includes
    return [flag] if (flag := _clang_resource_include()) else []


def _clang_args(args, cc_entries: list | None, header_path: str) -> list:
    """The full libclang command line for parsing *header_path*."""
    # `-ferror-limit=0` disables clang's early bail-out: libclang lags the
    # standard libraries it parses and emits benign errors deep in the STL
    # (harmless — the mim declarations still resolve).  Under the default cap
    # (~20) a stdlib-heavy toolchain such as macOS/libc++ trips "too many errors
    # emitted, stopping now" and yields a truncated, unusable AST.
    out = ["-x", "c++-header", "-ferror-limit=0"]
    if args.extra_args:
        out += _split_flags(args.extra_args)
    out += [f"-I{inc}" for inc in args.includes]
    out += _toolchain_includes()

    # The header roots libmim, fe and abseil live under, plus the build tree's
    # generated headers (`mim/config.h`).  These are passed as clean path tokens
    # on every platform and are the single source of truth for *locating*
    # headers — compile_commands.json is used only for build-type defines on top.
    # We deliberately do not trust its own `-I` paths: in the `command`-string
    # form CMake emits on Windows they are backslash paths that shlex mangles,
    # which would leave libclang unable to find these headers.
    build_dir = Path(args.build_dir)
    out += [
        f"-I{_REPO_ROOT / 'include'}",
        f"-I{_REPO_ROOT / 'submodules' / 'fe' / 'include'}",
        f"-I{_REPO_ROOT / 'submodules' / 'abseil-cpp'}",
        f"-I{build_dir / 'include'}",
        f"-I{build_dir}",
    ]
    # Adds build-type defines (NDEBUG, ABSL_*, …) on top of the include roots.
    if cc_entries and (entry := _match_cc_entry(header_path, cc_entries)):
        out += _flags_from_cc_entry(entry)
    return out


def _report_drops(header_path: str, drops: list[str]) -> None:
    """Note the members left out of *header_path*'s unit as unbindable.

    Reported on every run, because which members survive depends on what libclang
    resolves — so the same source can yield a different Python API on another
    toolchain. That divergence is invisible from the outside (a method is simply
    absent), which is exactly how a libc++ build once lost every `std::string`
    member while libstdc++ kept them.
    """
    if not drops:
        return
    print(
        f"note: {os.path.basename(header_path)}: dropped {len(drops)} member(s) whose "
        "signature nanobind cannot convert:",
        file=sys.stderr,
    )
    _print_capped(drops, "    ")


def _bindings_for_header(index, args, cc_entries: list | None, header_path: str) -> str | None:
    """Generate one unit, or None if *header_path* has nothing to bind."""
    if not _header_has_decls(header_path):
        return None

    tu = index.parse(header_path, _clang_args(args, cc_entries, header_path))
    if not tu:
        print(f"ERROR: failed to parse {header_path}", file=sys.stderr)
        return None

    # A fatal parse error (typically a header libclang couldn't locate) leaves a
    # partial AST and hence malformed bindings, so warn loudly.
    if n := _report_diagnostics(tu, header_path):
        print(
            f"warning: {n} fatal libclang error(s) parsing {header_path}; generated "
            "bindings will be malformed — the include flags are likely wrong "
            "for this toolchain (see the errors above)",
            file=sys.stderr,
        )

    collect_bound_types(tu)
    classes, enums, drops = extract_from_header(tu, header_path)
    _report_drops(header_path, drops)
    if not classes and not enums:
        return None
    return generate_bindings(header_path, classes, enums, ns=args.namespace, extra_dir=args.extra_dir)


def main(argv=None):
    args = parse_args(argv)

    # Module-composition mode: emit the NB_MODULE entry point and exit.
    if args.emit_module is not None:
        units = [(ns, init) for ns, _, init in (u.rpartition("#") for u in args.unit)]
        _write_or_print(generate_module(units, args.emit_module), args.output)
        return

    headers = list(args.headers)
    if args.dir:
        headers.extend(_find_headers(args.dir, args.recursive))
    if not headers:
        print("No headers specified.", file=sys.stderr)
        sys.exit(1)

    set_bound_headers(args.bound_headers or headers)
    if args.extra_dir is not None:
        args.extra_dir = str(Path(args.extra_dir).resolve())

    cc_entries = None if args.no_cmake else _load_compile_commands(Path(args.build_dir))
    if not args.no_cmake and cc_entries is None:
        print(
            f"warning: no compile_commands.json under {args.build_dir}; "
            "falling back to built-in include detection — build-type guards "
            "such as NDEBUG may not match your build "
            "(configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)",
            file=sys.stderr,
        )

    index = clang.Index.create()
    try:
        outputs = [code for h in headers if (code := _bindings_for_header(index, args, cc_entries, h))]
    except ExtraSectionError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)

    if not outputs:
        print("No bindings generated.", file=sys.stderr)
        return

    _write_or_print("\n// ============================================================\n".join(outputs), args.output)


if __name__ == "__main__":
    main()
