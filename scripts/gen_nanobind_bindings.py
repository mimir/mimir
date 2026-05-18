#!/usr/bin/env python3
"""
Generate nanobind Python bindings for C++ headers using libclang (from venv).

Usage:
    ./gen_nanobind_bindings.py path/to/header.h                    # stdout
    ./gen_nanobind_bindings.py path/to/header.h -o bind.cpp        # to file
    ./gen_nanobind_bindings.py --dir include/mim --recursive       # batch
    ./gen_nanobind_bindings.py --dir include/mim --recursive \\
        --namespace mim -I /custom/include

Requires: the project venv at REPO_ROOT/.venv/ with `clang` (libclang Python bindings) installed.
"""

import argparse
import os
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
#  Bootstrap: use the project venv so we get the bundled libclang.so
# ---------------------------------------------------------------------------

_SCRIPT_DIR = Path(__file__).resolve().parent          # scripts/
_REPO_ROOT  = _SCRIPT_DIR.parent                       # repo root
_VENV       = _REPO_ROOT / ".venv"

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

# Now try to import – the bundled libclang.so is found automatically.
try:
    import clang.cindex
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


def is_pure_virtual(cursor) -> bool:
    try:
        return cursor.is_pure_virtual()
    except Exception:
        return False


def _samefile(a, b):
    try:
        return os.path.samefile(a, b)
    except OSError:
        return os.path.abspath(a) == os.path.abspath(b)


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
    ):
        self.name = name
        self.return_type = return_type
        self.params = params
        self.class_name = class_name
        self.is_const = is_const
        self.is_static = is_static
        self.is_constructor = is_constructor

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
        params = []
        for p in cursor.get_children():
            if p.kind == CursorKind.PARM_DECL:
                params.append((_type_spelling(p.type), p.spelling))
        return MethodInfo(
            name=cursor.spelling,
            return_type="",
            params=params,
            class_name=class_name,
            is_constructor=True,
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

    ret = _type_spelling(cursor.result_type)
    params = []
    for p in cursor.get_children():
        if p.kind == CursorKind.PARM_DECL:
            params.append((_type_spelling(p.type), p.spelling))

    return MethodInfo(
        name=cursor.spelling,
        return_type=ret,
        params=params,
        class_name=class_name,
        is_const=is_const,
        is_static=is_static,
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
            if not name or name.startswith("__"):
                return
            bases = []
            methods = []
            for child in cursor.get_children():
                ck = child.kind
                if ck == CursorKind.CXX_BASE_SPECIFIER:
                    raw = child.type.spelling.replace("class ", "").replace("struct ", "").strip()
                    bases.append(raw)
                elif ck in (CursorKind.CXX_METHOD, CursorKind.CONSTRUCTOR):
                    mi = _extract_method(child, name)
                    if mi:
                        methods.append(mi)
                elif ck == CursorKind.FIELD_DECL and is_public(child):
                    ft = _type_spelling(child.type)
                    methods.append(MethodInfo(
                        name=child.spelling, return_type=ft, params=[], class_name=name
                    ))
            classes[name] = {"bases": bases, "methods": methods}

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


def _needs_ref_policy(ret_type: str) -> bool:
    return _is_def_ptr(ret_type)


def _gen_constructor_binding(mi: MethodInfo, indent: str = "    ") -> str:
    args = ", ".join(pt for pt, _ in mi.params)
    if not args:
        return f'{indent}.def(nb::init<>())'
    return f'{indent}.def(nb::init<{args}>())'


def _gen_static_binding(mi: MethodInfo, indent: str = "    ") -> str:
    policy = ", nb::rv_policy::reference_internal" if _needs_ref_policy(mi.return_type) else ""
    return f'{indent}.def_static("{mi.py_name}", {mi.cpp_ref}{policy})'


def _gen_overload_binding(mi: MethodInfo, indent: str = "    ") -> str:
    args_str = ", ".join(pt for pt, _ in mi.params)
    policy = ", nb::rv_policy::reference_internal" if _needs_ref_policy(mi.return_type) else ""

    if not args_str:
        if mi.is_const:
            return f'{indent}.def("{mi.py_name}", nb::overload_cast<>({mi.cpp_ref}, nb::const_){policy})'
        return f'{indent}.def("{mi.py_name}", nb::overload_cast<>({mi.cpp_ref}){policy})'

    if mi.is_const:
        return f'{indent}.def("{mi.py_name}", nb::overload_cast<{args_str}>({mi.cpp_ref}, nb::const_){policy})'
    return f'{indent}.def("{mi.py_name}", nb::overload_cast<{args_str}>({mi.cpp_ref}){policy})'


def _gen_simple_binding(mi: MethodInfo, indent: str = "    ") -> str:
    policy = ", nb::rv_policy::reference_internal" if _needs_ref_policy(mi.return_type) else ""
    return f'{indent}.def("{mi.py_name}", {mi.cpp_ref}{policy})'


def _gen_lambda_wrapper(mi: MethodInfo, indent: str = "    ") -> str:
    lam_params = []
    call_args = []
    for pt, pn in mi.params:
        lam_params.append(f"{pt} {pn}")
        call_args.append(pn)
    params_str = ", ".join(lam_params)
    args_str = ", ".join(call_args)
    const_marker = " const" if mi.is_const else ""
    policy = ", nb::rv_policy::reference_internal" if _needs_ref_policy(mi.return_type) else ""

    prefix = f"{params_str}, " if params_str else ""
    body = f"self.{mi.name}({args_str})"
    return f'{indent}.def("{mi.py_name}", []({mi.class_name}& self{", " + params_str if params_str else ""}){const_marker} {{ return {body}; }}{policy})'


def _generate_method_binding(mi: MethodInfo, overloaded_names: set, indent: str = "    ") -> str:
    if mi.is_constructor:
        return _gen_constructor_binding(mi, indent)
    if mi.is_static:
        return _gen_static_binding(mi, indent)

    is_overloaded = mi.name in overloaded_names
    has_params = bool(mi.params)
    needs_lambda = has_params and any(
        "vector<" in pt or "span<" in pt or "View<" in pt or "Defs" in pt or "Muts" in pt
        for pt, _ in mi.params
    )

    if needs_lambda:
        return _gen_lambda_wrapper(mi, indent)
    if is_overloaded:
        return _gen_overload_binding(mi, indent)
    if has_params:
        return _gen_overload_binding(mi, indent)
    return _gen_simple_binding(mi, indent)


def _gen_enum_binding(cursor, indent: str = "    ") -> str:
    name = cursor.spelling
    lines = [f'{indent}nb::enum_<{name}>(m, "{name.split("::")[-1]}")']
    for child in cursor.get_children():
        if child.kind == CursorKind.ENUM_CONSTANT_DECL:
            lines.append(f'{indent}    .value("{child.spelling}", {name}::{child.spelling})')
    lines[-1] += ";"
    return "\n".join(lines)


def _base_spec(bases: list[str]) -> str:
    if not bases:
        return ""
    return f", {bases[0].split('::')[-1]}"


def _compute_overloaded_names(classes: dict) -> set:
    overloaded: set = set()
    for info in classes.values():
        seen = defaultdict(int)
        for m in info["methods"]:
            if not m.is_constructor:
                seen[m.name] += 1
        for name, cnt in seen.items():
            if cnt > 1:
                overloaded.add(name)
    return overloaded


def generate_bindings(header_path: str, classes: dict, enums: list, ns: str = "") -> str:
    lines = []
    lines.append(INCLUDES_TEMPLATE)
    lines.append(f'#include "{os.path.relpath(header_path, os.getcwd())}"')
    lines.append("")
    lines.append("namespace nb = nanobind;")
    lines.append("")

    header_stem = os.path.splitext(os.path.basename(header_path))[0]
    func_name = f"init_{header_stem}"
    overloaded = _compute_overloaded_names(classes)

    if ns:
        lines.append(f"namespace {ns} {{")
        lines.append("")

    lines.append(f"void {func_name}(nb::module_& m) {{")
    lines.append("    // clang-format off")

    for cursor in enums:
        lines.append("")
        lines.append(_gen_enum_binding(cursor))

    for class_name, info in classes.items():
        bases = info["bases"]
        base_spec = _base_spec(bases)
        suffix = ", nb::never_destruct()" if "Def" in bases else ""

        lines.append("")
        cls_start = f'    nb::class_<{class_name}{base_spec}>(m, "{class_name}"{suffix})'
        methods = info["methods"]

        if not methods:
            lines.append(cls_start + ";")
            continue

        lines.append(cls_start)

        for i, mi in enumerate(methods):
            binding = _generate_method_binding(mi, overloaded)
            if i == 0:
                lines[-1] = lines[-1] + " " + binding.strip()
            else:
                lines.append(binding)

        if not lines[-1].rstrip().endswith(";"):
            lines[-1] = lines[-1] + ";"

    lines.append("")
    lines.append("    // clang-format on")
    lines.append("}")

    if ns:
        lines.append("")
        lines.append(f"}} // namespace {ns}")

    lines.append("")
    return "\n".join(lines)


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
    p.add_argument("-I", action="append", dest="includes", default=[], help="Include paths")
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


def main(argv=None):
    args = parse_args(argv)

    headers = list(args.headers)
    if args.dir:
        headers.extend(_find_headers(args.dir, args.recursive))
    if not headers:
        print("No headers specified.", file=sys.stderr)
        sys.exit(1)

    # Clang arguments
    clang_args = ["-x", "c++-header"]
    if args.extra_args:
        clang_args.extend(args.extra_args.split())
    for inc in args.includes or []:
        clang_args.append(f"-I{inc}")

    # Auto-detect repository include paths
    for d in [
        _REPO_ROOT / "include",
        _REPO_ROOT / "submodules" / "fe" / "include",
        _REPO_ROOT / "submodules" / "abseil-cpp",
        _REPO_ROOT / "build" / "include",
        _REPO_ROOT / "build",
    ]:
        p = str(d)
        if p not in clang_args:
            clang_args.append(f"-I{p}")

    # Clang builtin headers (e.g. /usr/lib/clang/22/include)
    for clang_ver_dir in sorted(Path("/usr/lib/clang").iterdir(), reverse=True):
        res_incl = clang_ver_dir / "include"
        if res_incl.is_dir() and (res_incl / "stddef.h").exists():
            clang_args.append(f"-isystem{res_incl}")
            break

    idx = clang.Index.create()
    outputs = []

    for hdr in headers:
        if not _header_has_decls(hdr):
            continue

        tu = idx.parse(hdr, clang_args)
        if not tu:
            print(f"ERROR: failed to parse {hdr}", file=sys.stderr)
            continue

        classes, enums = extract_from_header(tu, hdr)
        if not classes and not enums:
            continue

        code = generate_bindings(hdr, classes, enums, ns=args.namespace)
        outputs.append(code)

    if not outputs:
        print("No bindings generated.", file=sys.stderr)
        return

    combined = "\n// ============================================================\n".join(outputs)

    if args.output:
        with open(args.output, "w") as f:
            f.write(combined)
        print(f"Written to {args.output}")
    else:
        print(combined)


if __name__ == "__main__":
    main()
