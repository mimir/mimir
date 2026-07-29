#!/usr/bin/env python3
"""
Generate nanobind Python bindings for C++ headers using libclang (from venv).

Usage:
    ./gen_nanobind_bindings.py path/to/header.h                    # stdout
    ./gen_nanobind_bindings.py path/to/header.h -o bind.cpp        # to file
    ./gen_nanobind_bindings.py --dir include/mim --recursive       # batch
    ./gen_nanobind_bindings.py --dir include/mim --recursive
        --namespace mim -I /custom/include

Requires: the project venv at REPO_ROOT/.venv/ with `clang` (libclang Python bindings) installed.
"""

import argparse
import difflib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from functools import lru_cache
from pathlib import Path
from typing import Optional

# TODO: change this, this is way too convoluted
# ---------------------------------------------------------------------------
#  Bootstrap: use the project venv so we get the bundled libclang.so
# ---------------------------------------------------------------------------

_SCRIPT_DIR = Path(__file__).resolve().parent          # scripts/
_REPO_ROOT  = _SCRIPT_DIR.parent                       # repo root

# When run via CMake (using the build venv's Python), sys.prefix already
# points at the venv root.  When invoked manually with the system Python,
# fall back to the repo-root .venv/ for the libclang bootstrap.
if sys.prefix != sys.base_prefix:
    _VENV = Path(sys.prefix)
else:
    _VENV = _REPO_ROOT / ".venv"

# Determine the `site-packages` inside the venv
_py_ver = f"python{sys.version_info.major}.{sys.version_info.minor}"
_venv_site = _VENV / "lib" / _py_ver / "site-packages"

if _venv_site.is_dir() and str(_venv_site) not in sys.path:
    sys.path.insert(0, str(_venv_site))
    # Ensure the bundled libclang.so can be found by ctypes
    _clang_native = _venv_site / "clang" / "native"
    if _clang_native.is_dir():
        _lib = _clang_native / "libclang.so"
        if _lib.exists():
            os.environ.setdefault("LIBCLANG_LIBRARY_FILE", str(_lib))

try:
    import clang.cindex # pyright: ignore[reportMissingImports]
except ImportError:
    print(
        "ERROR: clang.cindex not found in project venv.\n"
        f"  Run: {_VENV}/bin/pip install clang",
        file=sys.stderr,
    )
    sys.exit(1)
except clang.cindex.LibclangError as e:
    print(f"ERROR: libclang not loadable from venv: {e}", file=sys.stderr)
    sys.exit(1)

clang = clang.cindex
CursorKind = clang.CursorKind
TypeKind = clang.TypeKind


# ---------------------------------------------------------------------------
#  Type helpers
# ---------------------------------------------------------------------------

def _type_spelling(t) -> str:
    raw = t.spelling
    raw = raw.replace("class ", "").replace("struct ", "").replace("enum ", "")
    return raw


def _is_def_ptr(typestr: str) -> bool:
    norm = typestr.replace(" ", "")
    return "Def*" in norm


# ---------------------------------------------------------------------------
#  Extraction helpers
# ---------------------------------------------------------------------------

def is_public(cursor) -> bool:
    try:
        return cursor.access_specifier == clang.AccessSpecifier.PUBLIC
    except Exception:
        return True


def is_deleted(cursor) -> bool:
    try:
        raw = cursor.raw_comment or ""
        extent = str(cursor.extent.end)
        return "= delete" in extent or "= delete" in raw
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
        if parent is not None and parent.kind in (CursorKind.CLASS_DECL, CursorKind.STRUCT_DECL):
            return parent.spelling, decl
    except Exception:
        pass
    return None


def _resolve_param_type(p) -> Optional[str]:
    """Spelling to emit for a parameter, or ``None`` to skip the whole method.

    - function-pointer params are unbindable → skip;
    - a nested *record* param is spelled unqualified by libclang and is usually
      an unbound type → skip;
    - a nested *enum* param is qualified with its enclosing class (e.g.
      ``Level`` → ``Log::Level``) so it resolves at namespace scope.
    """
    if _type_is_fn_ptr(p.type):
        return None
    spelling = _type_spelling(p.type)
    nd = _nested_decl(p.type)
    if nd:
        parent_name, decl = nd
        if decl.kind != CursorKind.ENUM_DECL:
            return None
        return re.sub(rf"\b{re.escape(decl.spelling)}\b", f"{parent_name}::{decl.spelling}", spelling, count=1)
    return spelling


def _is_unresolved_return(ret: str) -> bool:
    """True if *ret* is not a concrete, bindable type.

    Catches undeduced `auto`/`decltype(auto)` and dependent template parameters
    such as `Vector<R>` (a single uppercase-letter template argument), which
    arise from the MIM_PROJ mixin methods and cannot be bound as written.
    """
    if re.search(r"\b(auto|decltype)\b", ret):
        return True
    # A template argument that is a lone uppercase letter is a template
    # parameter (e.g. `Vector<R>`), never a concrete type in this codebase.
    return bool(re.search(r"<\s*[A-Z]\s*[,>]", ret))


def _type_is_fn_ptr(t) -> bool:
    """True if *t* is a raw function pointer (or function) type.

    nanobind has no type caster for raw function pointers (e.g. the `Normalizer`
    and `Backend` typedefs), so a method exposing one cannot be bound.
    """
    try:
        canon = t.get_canonical()
        if canon.kind == TypeKind.POINTER:
            return canon.get_pointee().kind == TypeKind.FUNCTIONPROTO
        return canon.kind == TypeKind.FUNCTIONPROTO
    except Exception:
        return False


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


def _samefile(a, b):
    try:
        return os.path.samefile(a, b)
    except OSError:
        return os.path.abspath(a) == os.path.abspath(b)


def _param_default(p) -> Optional[str]:
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

class MethodInfo:
    def __init__(
        self,
        name: str,
        return_type: str,
        params: list[tuple[str, str]],
        class_name: str = "",
        is_const: bool = False,
        is_static: bool = False,
        is_constructor: bool = False,
        is_field: bool = False,
        defaults: Optional[list[Optional[str]]] = None,
    ):
        self.name = name
        self.return_type = return_type
        self.params = params
        self.class_name = class_name
        self.is_const = is_const
        self.is_static = is_static
        self.is_constructor = is_constructor
        self.is_field = is_field
        # Parallel to `params`: the source default value for each parameter, or None.
        self.defaults = defaults if defaults is not None else [None] * len(params)

    @property
    def py_name(self) -> str:
        return _camel_to_snake(self.name)

    @property
    def cpp_ref(self) -> str:
        if self.is_constructor:
            return ""
        return f"&{self.class_name}::{self.name}"


def _camel_to_snake(name: str) -> str:
    s = re.sub(r"(?<=[a-z])(?=[A-Z])", "_", name)
    s = re.sub(r"(?<=[A-Z])(?=[A-Z][a-z])", "_", s)
    return s.lower()


def _extract_params(cursor):
    """Return ``(params, defaults)`` for a function cursor, or ``None`` to skip.

    ``None`` signals that a parameter type is unbindable (see `_resolve_param_type`),
    so the whole method/constructor must be dropped.
    """
    params, defaults = [], []
    for p in cursor.get_children():
        if p.kind == CursorKind.PARM_DECL:
            pt = _resolve_param_type(p)
            if pt is None:
                return None
            params.append((pt, p.spelling))
            defaults.append(_param_default(p))
    return params, defaults


def _extract_method(cursor, class_name: str) -> Optional[MethodInfo]:
    if is_deleted(cursor):
        return None

    if cursor.kind == CursorKind.DESTRUCTOR:
        return None

    if cursor.spelling.startswith("operator"):
        return None

    if not is_public(cursor):
        return None

    if cursor.kind == CursorKind.CONSTRUCTOR:
        if _is_copy_or_move_ctor(cursor, class_name):
            return None
        extracted = _extract_params(cursor)
        if extracted is None:
            return None
        params, defaults = extracted
        return MethodInfo(
            name=cursor.spelling,
            return_type="",
            params=params,
            class_name=class_name,
            is_constructor=True,
            defaults=defaults,
        )

    is_static = is_const = False
    try:
        is_static = cursor.is_static_method()
    except Exception:
        pass
    try:
        is_const = cursor.is_const_method()
    except Exception:
        pass

    if _type_is_fn_ptr(cursor.result_type):
        return None
    ret = _type_spelling(cursor.result_type)
    # Skip methods whose return type libclang could not resolve to a concrete
    # bindable type (the MIM_PROJ mixin methods): either a bare `auto`/`decltype`
    # or a dependent template parameter such as `Vector<R>`. The generated
    # binding would be uncompilable/unconvertible, so leave it to an nbextra
    # substitute.
    if _is_unresolved_return(ret):
        return None
    extracted = _extract_params(cursor)
    if extracted is None:
        return None
    params, defaults = extracted

    return MethodInfo(
        name=cursor.spelling,
        return_type=ret,
        params=params,
        class_name=class_name,
        is_const=is_const,
        is_static=is_static,
        defaults=defaults,
    )


def extract_from_header(tu, header_path: str):
    classes: dict = {}
    enums: list = []

    def walk(cursor):
        loc = cursor.location
        if not loc.file or not _samefile(str(loc.file), header_path):
            return
        kind = cursor.kind

        if kind in (CursorKind.CLASS_DECL, CursorKind.STRUCT_DECL):
            # Skip forward declarations / opaque declarations
            try:
                if not cursor.is_definition():
                    return
            except Exception:
                pass
            name = cursor.spelling
            if not name or name.startswith("__") or name.startswith("("):
                return
            # Skip template specializations (e.g. `template<> struct fe::is_bit_enum<mim::Dep>`
            # or `formatter<...>`): they are not real, bindable classes.
            if "<" in cursor.displayname:
                return
            is_abstract = False
            try:
                is_abstract = cursor.is_abstract_record()
            except Exception:
                pass
            bases = []
            methods = []
            is_exception = False
            for child in cursor.get_children():
                ck = child.kind
                if ck == CursorKind.CXX_BASE_SPECIFIER:
                    raw = child.type.spelling.replace("class ", "").replace("struct ", "").strip()
                    # A class deriving from std::exception is bound as a Python
                    # exception, not as an ordinary class.
                    if "exception" in raw:
                        is_exception = True
                    # Only keep bases that are themselves bindable mim classes.
                    # Foreign (`fe::SymPool`, `std::true_type`) and template mixin
                    # bases (`fe::RuntimeCast<Def>`) are not registered nanobind
                    # types, so binding against them would not compile.
                    if "::" in raw or "<" in raw:
                        continue
                    bases.append(raw)
                elif ck in (CursorKind.CXX_METHOD, CursorKind.CONSTRUCTOR):
                    mi = _extract_method(child, name)
                    if mi:
                        methods.append(mi)
                elif ck == CursorKind.FIELD_DECL and is_public(child):
                    ft = _type_spelling(child.type)
                    methods.append(MethodInfo(
                        name=child.spelling, return_type=ft, params=[], class_name=name,
                        is_field=True, is_const=child.type.is_const_qualified(),
                    ))
                elif ck == CursorKind.ENUM_DECL and is_public(child):
                    # Nested enums (e.g. `Log::Level`) are bound at module scope.
                    enums.append(child)
            classes[name] = {
                "bases": bases,
                "methods": methods,
                "is_exception": is_exception,
                "is_abstract": is_abstract,
            }

        elif kind == CursorKind.ENUM_DECL:
            enums.append(cursor)

        elif kind == CursorKind.NAMESPACE:
            for c in cursor.get_children():
                walk(c)

    for child in tu.cursor.get_children():
        walk(child)

    return classes, enums


# ---------------------------------------------------------------------------
#  Code generation
# ---------------------------------------------------------------------------

INCLUDES_TEMPLATE = """\
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>
"""


def _returns_lvalue_ref(ret_type: str) -> bool:
    norm = ret_type.replace(" ", "")
    return norm.endswith("&") and not norm.endswith("&&")


def _returns_pointer(ret_type: str) -> bool:
    return "*" in ret_type


def _returns_def_vector(ret_type: str) -> bool:
    # e.g. `Vector<const Def*>` / `mim::Vector<mim::Def const*, 4>` (DefVec): a
    # custom container nanobind can't convert. We copy it into a std::vector so
    # it reaches Python as a list of Def.
    norm = ret_type.replace(" ", "")
    return "Vector<" in norm and "Def*" in norm


def _needs_ref_policy(ret_type: str) -> bool:
    # A method returning a reference or pointer to a bound C++ object must hand
    # Python a non-owning handle rather than copying the object into it.
    # Value returns are moved by nanobind's default `automatic` policy and need
    # no annotation.
    return _returns_lvalue_ref(ret_type) or _returns_pointer(ret_type)


def _policy_suffix(ret_type: str, static: bool) -> str:
    if not _needs_ref_policy(ret_type):
        return ""
    # `reference_internal` ties the result's lifetime to `self`; a static method
    # has no `self`, so fall back to a plain non-owning `reference`.
    pol = "reference" if static else "reference_internal"
    return f", nb::rv_policy::{pol}"


def _gen_constructor_binding(mi: MethodInfo, indent: str = "    ") -> str:
    args = ", ".join(pt for pt, _ in mi.params)
    if not args:
        return f'{indent}.def(nb::init<>())'
    return f'{indent}.def(nb::init<{args}>())'


def _gen_field_binding(mi: MethodInfo, indent: str = "    ") -> str:
    # Public data members are exposed as writable properties, unless the field
    # is const (or a reference we can't rebind), in which case it is read-only.
    accessor = "def_ro" if (mi.is_const or "&" in mi.return_type) else "def_rw"
    return f'{indent}.{accessor}("{mi.py_name}", &{mi.class_name}::{mi.name})'


# QoL parameter substitutions: expose a Python-friendly container type in the
# lambda signature and let an implicit C++ conversion forward it to the wrapped
# call. `Defs` (= Span<const Def*>) is implicitly constructible from any vector,
# so accepting `std::vector<const Def*>` makes methods like `world.cn([...])`
# take a plain Python list.
_PARAM_TYPE_MAP = {
    "Defs": "std::vector<const mim::Def*>",
}


def _map_param_type(pt: str) -> str:
    return _PARAM_TYPE_MAP.get(pt.strip(), pt)


# Default values simple enough to reproduce verbatim in a nb::arg() default.
_SAFE_DEFAULT = re.compile(r"^(true|false|nullptr|-?\d+[uUlL]*|0[xX][0-9a-fA-F]+[uUlL]*)$")


def _arg_spec(mi: MethodInfo) -> str:
    """`, nb::arg(...)` clause carrying parameter default values, or "".

    Only emitted when every defaulted parameter has a literal we can reproduce;
    otherwise the parameters simply stay required (C++ guarantees defaults are
    trailing, so we never emit a default ahead of a non-default one).
    """
    if not mi.params:
        return ""
    defaults = mi.defaults
    first = next((i for i, d in enumerate(defaults) if d is not None), None)
    if first is None:
        return ""
    if not all(_SAFE_DEFAULT.match(d or "") for d in defaults[first:]):
        return ""
    parts = []
    for i, (_, pn) in enumerate(mi.params):
        name = pn or f"a{i}"
        if defaults[i] is not None:
            parts.append(f'nb::arg("{name}") = {defaults[i]}')
        else:
            parts.append(f'nb::arg("{name}")')
    return ", " + ", ".join(parts)


def _lambda_params(mi: MethodInfo) -> tuple[str, str]:
    """Return the (declaration, call) strings for a method's parameters.

    Unnamed parameters are given synthetic names so they can be forwarded to
    the wrapped call.
    """
    lam_params = []
    call_args = []
    for idx, (pt, pn) in enumerate(mi.params):
        name = pn or f"a{idx}"
        lam_params.append(f"{_map_param_type(pt)} {name}")
        call_args.append(name)
    return ", ".join(lam_params), ", ".join(call_args)


def _gen_lambda_wrapper(mi: MethodInfo, indent: str = "    ") -> str:
    # Bind instance methods through a call-site lambda rather than a
    # pointer-to-member. Taking `&Class::method` is ambiguous whenever the
    # method has template or const/non-const overloads, so `overload_cast`
    # fails; a lambda lets ordinary C++ overload resolution pick the right one.
    params_str, args_str = _lambda_params(mi)
    self_param = f"const {mi.class_name}& self" if mi.is_const else f"{mi.class_name}& self"
    sep = ", " if params_str else ""
    call = f"self.{mi.name}({args_str})"
    # A `Vector<const Def*>` return is copied into a std::vector so it converts
    # to a Python list; elements stay tied to `self` (reference_internal).
    if _returns_def_vector(mi.return_type):
        lam = f'[]({self_param}{sep}{params_str}) {{ auto _v = {call}; return std::vector<const mim::Def*>(_v.begin(), _v.end()); }}'
        return f'{indent}.def("{mi.py_name}", {lam}, nb::rv_policy::reference_internal{_arg_spec(mi)})'
    # An lvalue-reference return is bound through its address so it reaches
    # Python as a non-owning pointer instead of being copied by value (which
    # fails for non-copyable or forward-declared types such as World/Driver).
    policy = _policy_suffix(mi.return_type, static=False)
    body = "&" + call if _returns_lvalue_ref(mi.return_type) else call
    return f'{indent}.def("{mi.py_name}", []({self_param}{sep}{params_str}) {{ return {body}; }}{policy}{_arg_spec(mi)})'


def _gen_static_lambda(mi: MethodInfo, indent: str = "    ") -> str:
    # Same rationale as _gen_lambda_wrapper, but for static methods there is no
    # `self`, so we call `Class::method(args)` directly.
    params_str, args_str = _lambda_params(mi)
    policy = _policy_suffix(mi.return_type, static=True)
    body = f"{mi.class_name}::{mi.name}({args_str})"
    if _returns_lvalue_ref(mi.return_type):
        body = "&" + body
    return f'{indent}.def_static("{mi.py_name}", []({params_str}) {{ return {body}; }}{policy}{_arg_spec(mi)})'


def _generate_method_binding(mi: MethodInfo, indent: str = "    ") -> str:
    if mi.is_constructor:
        return _gen_constructor_binding(mi, indent)
    if mi.is_field:
        return _gen_field_binding(mi, indent)
    if mi.is_static:
        return _gen_static_lambda(mi, indent)
    return _gen_lambda_wrapper(mi, indent)


def _enum_qualified_name(cursor) -> str:
    """Enum name qualified by any enclosing classes (e.g. `Log::Level`).

    The enclosing namespace is dropped because the binding is emitted inside it.
    """
    parts = [cursor.spelling]
    p = cursor.semantic_parent
    while p is not None and p.kind in (CursorKind.CLASS_DECL, CursorKind.STRUCT_DECL):
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


def _empty_extra() -> dict:
    """A fresh, empty set of .nbextra injection sections.

    A factory (not a shared constant) because the `classes`/`skips` values are
    mutated in place downstream.
    """
    return {"includes": "", "classes": {}, "standalone": "", "skips": {}}


def _parse_extra_file(content: str, stem: str) -> dict:
    """Parse a structured .nbextra file into injection sections.

    Section headers are bracketed tags on their own line:
        [include]           — extra #include lines appended after the standard nanobind includes
        [class:ClassName]   — method-chain fragment appended to the named class binding
        [skip:ClassName]    — whitespace-separated method names to drop from that class
                              (C++ name or snake_case py_name); pair with [class:...] to
                              substitute a hand-written binding for something the generator
                              can't express
        [standalone]        — raw code injected inside init_* after all class/enum blocks

    Content before the first section header is ignored.
    Unrecognised tags emit a warning and their bodies are discarded.
    """
    result = _empty_extra()
    current_key: str | None = None
    current_lines: list[str] = []

    def _flush() -> None:
        body = "\n".join(current_lines).strip()
        current_lines.clear()
        if not body or current_key is None:
            return
        if current_key == "include":
            result["includes"] = body
        elif current_key == "standalone":
            result["standalone"] = body
        elif current_key.startswith("class:"):
            cls = current_key[len("class:"):]
            result["classes"][cls] = body
        elif current_key.startswith("skip:"):
            cls = current_key[len("skip:"):]
            result["skips"].setdefault(cls, set()).update(body.split())
        else:
            print(f"warning: {stem}.nbextra: unrecognised section [{current_key}] — skipped", file=sys.stderr)

    for line in content.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]") and len(stripped) > 2:
            _flush()
            current_key = stripped[1:-1].strip()
        else:
            current_lines.append(line)
    _flush()
    return result


def _load_extra(header_path: str, extra_dir: str | None) -> dict:
    """Load and parse the companion .nbextra file for *header_path*, if any."""
    if not extra_dir:
        return _empty_extra()
    stem = os.path.splitext(os.path.basename(header_path))[0]
    extra_path = os.path.join(extra_dir, stem + ".nbextra")
    try:
        with open(extra_path) as f:
            return _parse_extra_file(f.read(), stem)
    except OSError:
        return _empty_extra()


class ExtraSectionError(Exception):
    """A .nbextra section targets a class that will never receive it."""


def _check_extra_sections(extra: dict, classes: dict, header_path: str, stem: str) -> None:
    """Reject `[class:X]`/`[skip:X]` sections that cannot be applied.

    Both are matched against the C++ class name verbatim, so a typo or a stale
    name left behind by a rename yields a module that still compiles but is
    missing every binding the section declared.
    """
    header = os.path.basename(header_path)
    problems = []

    for kind, names in (("class", extra["classes"]), ("skip", extra["skips"])):
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
    for name in extra["classes"]:
        if classes.get(name, {}).get("is_exception"):
            problems.append(
                f"  [class:{name}] targets an exception class registered via "
                f"nb::exception<{name}>, which accepts no .def(...) chain"
            )

    if problems:
        known = ", ".join(sorted(classes)) or "(none)"
        raise ExtraSectionError(
            f"{stem}.nbextra: unusable section(s):\n"
            + "\n".join(problems)
            + f"\nClasses available from {header}: {known}"
        )


def generate_bindings(header_path: str, classes: dict, enums: list, ns: str = "", extra_dir: str | None = None) -> str:
    extra = _load_extra(header_path, extra_dir)

    lines = []
    lines.append(INCLUDES_TEMPLATE)
    if extra["includes"]:
        lines.append(extra["includes"])
        lines.append("")
    lines.append(f'#include "{os.path.relpath(header_path, os.getcwd())}"')
    # The two runtime hub types are cross-referenced by most accessors (e.g.
    # `Def::world()`, `World::driver()`); include their full definitions so
    # nanobind can cast references/pointers to them (their headers are on the
    # PUBLIC include path of libmim).
    lines.append("#include <mim/driver.h>")
    lines.append("#include <mim/world.h>")
    lines.append("")
    lines.append("namespace nb = nanobind;")
    lines.append("")

    header_stem = os.path.splitext(os.path.basename(header_path))[0]
    func_name = f"init_{header_stem}"

    if ns:
        lines.append(f"namespace {ns} {{")
        lines.append("")

    lines.append(f"void {func_name}(nb::module_& m) {{")
    lines.append("    // clang-format off")

    for cursor in enums:
        lines.append("")
        lines.append(_gen_enum_binding(cursor))

    # An unmatched [class:X]/[skip:X] is almost always a typo or a stale name
    # after a C++ rename. Silently dropping the section produces a module that
    # compiles cleanly but is missing every binding the section declared, so
    # fail loudly here — before emitting anything.
    _check_extra_sections(extra, classes, header_path, header_stem)

    for class_name, info in classes.items():
        # A class deriving from std::exception is registered as a Python
        # exception rather than an ordinary class.
        if info.get("is_exception"):
            lines.append("")
            lines.append(f'    nb::exception<{class_name}>(m, "{class_name}");')
            continue

        bases = info["bases"]
        base_spec = _base_spec(bases)
        # Def and everything deriving from it is never_destruct —
        # the World owns all Def lifetimes.
        suffix = ", nb::never_destruct()" if (class_name == "Def" or "Def" in bases) else ""

        class_extra = extra["classes"].get(class_name, "")

        lines.append("")
        cls_start = f'    nb::class_<{class_name}{base_spec}>(m, "{class_name}"{suffix})'
        methods = info["methods"]

        # Abstract classes cannot be constructed from Python (placement-new of an
        # abstract type is ill-formed), so drop their constructor bindings.
        if info.get("is_abstract"):
            methods = [m for m in methods if not m.is_constructor]

        # Explicit [skip:Class] denylist: drop members the generator can't
        # express (matched by C++ name or snake_case py_name); a companion
        # [class:Class] section can substitute a hand-written binding.
        _skip = extra["skips"].get(class_name)
        if _skip:
            methods = [m for m in methods if m.name not in _skip and m.py_name not in _skip]

        # nanobind rejects a Python name carrying both static and instance
        # overloads (e.g. Def::zonk() const vs. static Def::zonk(Defs)).
        # Keep the instance overloads and drop the colliding static ones.
        _static_py = {m.py_name for m in methods if m.is_static and not m.is_field}
        _instance_py = {m.py_name for m in methods if not m.is_static and not m.is_constructor and not m.is_field}
        _drop_static = _static_py & _instance_py
        if _drop_static:
            methods = [m for m in methods if not (m.is_static and m.py_name in _drop_static)]

        if not methods and not class_extra:
            lines.append(cls_start + ";")
            continue

        lines.append(cls_start)

        for i, mi in enumerate(methods):
            binding = _generate_method_binding(mi)
            if i == 0:
                lines[-1] = lines[-1] + " " + binding.strip()
            else:
                lines.append(binding)

        if class_extra:
            if lines[-1].rstrip().endswith(";"):
                lines[-1] = lines[-1].rstrip()[:-1].rstrip()
            lines.append(class_extra.strip())

        if not lines[-1].rstrip().endswith(";"):
            lines[-1] = lines[-1] + ";"

    if extra["standalone"]:
        lines.append("")
        lines.append(extra["standalone"])

    lines.append("")
    lines.append("    // clang-format on")
    lines.append("}")

    if ns:
        lines.append("")
        lines.append(f"}} // namespace {ns}")

    lines.append("")
    return "\n".join(lines)


def generate_module(units: list[tuple[str, str]], module_name: str = "_mim") -> str:
    """Emit the ``NB_MODULE`` entry point that composes every init function.

    *units* is an ordered list of ``(namespace, init_name)`` pairs; the calls are
    emitted in that order (foundational types first so signatures resolve to
    Python class names).
    """
    lines = ["#include <nanobind/nanobind.h>", "", "namespace nb = nanobind;", ""]

    # Group forward declarations by namespace, preserving first-seen order.
    order: list[str] = []
    by_ns: dict[str, list[str]] = {}
    for ns, init in units:
        if ns not in by_ns:
            by_ns[ns] = []
            order.append(ns)
        by_ns[ns].append(init)
    for ns in order:
        decls = " ".join(f"void {i}(nb::module_&);" for i in by_ns[ns])
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

_CC_SEPARATE_FLAGS = ("-I", "-isystem", "-iquote", "-D", "-U", "-include")
_CC_GLUED_PREFIXES = ("-I", "-isystem", "-iquote", "-D", "-U", "-std=", "-include")


def _load_compile_commands(build_dir: Path) -> Optional[list]:
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


def _rel_under(path, anchor: str) -> Optional[Path]:
    """Path relative to `<repo>/<anchor>`, or None if it does not live there."""
    try:
        return Path(path).resolve().relative_to(_anchor_base(anchor))
    except ValueError:
        return None


def _match_cc_entry(header_path: str, entries: list) -> Optional[dict]:
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


def _flags_from_cc_entry(entry: dict) -> list:
    """Extract the preprocessor-relevant flags ."""
    if "arguments" in entry:
        raw = list(entry["arguments"])
    else:
        raw = shlex.split(entry.get("command", ""))

    out, i = [], 0
    while i < len(raw):
        a = raw[i]
        if a in _CC_SEPARATE_FLAGS and i + 1 < len(raw):
            out.append(a)
            out.append(raw[i + 1])
            i += 2
            continue
        if a.startswith(_CC_GLUED_PREFIXES):
            out.append(a)
        i += 1
    return out


def _resource_include_from(resource_dir: str) -> Optional[str]:
    """`-isystem` flag for a clang resource dir, if it holds the builtin headers."""
    inc = Path(resource_dir.strip()) / "include"
    return f"-isystem{inc}" if (inc / "stddef.h").is_file() else None


def _clang_resource_include() -> Optional[str]:
    """Locate libclang's builtin headers (stddef.h et al.) as an `-isystem` flag.

    compile_commands.json never records the resource dir, and the pip `libclang`
    wheel ships only the shared library — not its builtin headers — so libclang
    cannot find `stddef.h` on its own.  Ask a real clang where they live
    (`clang -print-resource-dir`), which is correct on Linux, macOS and Windows
    alike; fall back to scanning the usual install locations if none is on PATH.
    """
    # Primary, portable: ask a clang executable for its resource dir.
    cc = os.environ.get("CC", "")
    candidates = ([cc] if "clang" in os.path.basename(cc) else []) + ["clang", "clang++"]
    for exe in candidates:
        path = shutil.which(exe)
        if not path:
            continue
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


def _write_or_print(code: str, output: Optional[str]) -> None:
    if output:
        # CMake's file(MAKE_DIRECTORY) runs at configure time only, so the
        # output directory may be gone on a later build; recreate it here.
        os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
        with open(output, "w") as f:
            f.write(code)
        print(f"Written to {output}")
    else:
        print(code)


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

    # Base clang arguments shared by every header.
    base_args = ["-x", "c++-header"]
    if args.extra_args:
        base_args.extend(args.extra_args.split())
    for inc in args.includes or []:
        base_args.append(f"-I{inc}")
    # libclang's own builtin headers need to be included as well.
    res_flag = _clang_resource_include()
    if res_flag:
        base_args.append(res_flag)

    # Get all Cmake relevant includes and flags
    cc_entries = None
    if not args.no_cmake:
        cc_entries = _load_compile_commands(Path(args.build_dir))
        if cc_entries is None:
            print(
                f"warning: no compile_commands.json under {args.build_dir}; "
                "falling back to built-in include detection — build-type guards "
                "such as NDEBUG may not match your build "
                "(configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)",
                file=sys.stderr,
            )

    # Hardcoded Fallbacks should you not use the cmake flag.
    # TODO: Maybe change this to an argument in the future?
    fallback_includes = [
        f"-I{_REPO_ROOT / 'include'}",
        f"-I{_REPO_ROOT / 'submodules' / 'fe' / 'include'}",
        f"-I{_REPO_ROOT / 'submodules' / 'abseil-cpp'}",
        f"-I{_REPO_ROOT / 'build' / 'include'}",
        f"-I{_REPO_ROOT / 'build'}",
    ]

    def _args_for_header(hdr: str) -> list:
        clang_args = list(base_args)
        if cc_entries:
            entry = _match_cc_entry(hdr, cc_entries)
            if entry:
                clang_args.extend(_flags_from_cc_entry(entry))
                return clang_args
        clang_args.extend(fallback_includes)
        return clang_args

    # Resolve extras directory
    extra_dir = args.extra_dir
    if extra_dir is not None:
        extra_dir = str(Path(extra_dir).resolve())

    idx = clang.Index.create()
    outputs = []

    for hdr in headers:
        if not _header_has_decls(hdr):
            continue

        clang_args = _args_for_header(hdr)
        tu = idx.parse(hdr, clang_args)
        if not tu:
            print(f"ERROR: failed to parse {hdr}", file=sys.stderr)
            continue

        classes, enums = extract_from_header(tu, hdr)
        if not classes and not enums:
            continue

        try:
            code = generate_bindings(hdr, classes, enums, ns=args.namespace, extra_dir=extra_dir)
        except ExtraSectionError as e:
            print(f"ERROR: {e}", file=sys.stderr)
            sys.exit(1)
        outputs.append(code)

    if not outputs:
        print("No bindings generated.", file=sys.stderr)
        return

    combined = "\n// ============================================================\n".join(outputs)

    _write_or_print(combined, args.output)


if __name__ == "__main__":
    main()
