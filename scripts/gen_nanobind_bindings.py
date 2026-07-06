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
import json
import os
import re
import shlex
import sys
from collections import defaultdict
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


def _parse_extra_file(content: str, stem: str) -> dict:
    """Parse a structured .nbextra file into injection sections.

    Section headers are bracketed tags on their own line:
        [include]           — extra #include lines appended after the standard nanobind includes
        [class:ClassName]   — method-chain fragment appended to the named class binding
        [standalone]        — raw code injected inside init_* after all class/enum blocks

    Content before the first section header is ignored.
    Unrecognised tags emit a warning and their bodies are discarded.
    """
    result: dict = {"includes": "", "classes": {}, "standalone": ""}
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
    _empty: dict = {"includes": "", "classes": {}, "standalone": ""}
    if not extra_dir:
        return _empty
    stem = os.path.splitext(os.path.basename(header_path))[0]
    extra_path = os.path.join(extra_dir, stem + ".nbextra")
    try:
        with open(extra_path) as f:
            return _parse_extra_file(f.read(), stem)
    except OSError:
        return _empty


def generate_bindings(header_path: str, classes: dict, enums: list, ns: str = "", extra_dir: str | None = None) -> str:
    extra = _load_extra(header_path, extra_dir)

    lines = []
    lines.append(INCLUDES_TEMPLATE)
    if extra["includes"]:
        lines.append(extra["includes"])
        lines.append("")
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

    used_class_extras: set[str] = set()

    for class_name, info in classes.items():
        bases = info["bases"]
        base_spec = _base_spec(bases)
        # Def and everything deriving from it is never_destruct —
        # the World owns all Def lifetimes.
        suffix = ", nb::never_destruct()" if (class_name == "Def" or "Def" in bases) else ""

        class_extra = extra["classes"].get(class_name, "")
        if class_extra:
            used_class_extras.add(class_name)

        lines.append("")
        cls_start = f'    nb::class_<{class_name}{base_spec}>(m, "{class_name}"{suffix})'
        methods = info["methods"]

        if not methods and not class_extra:
            lines.append(cls_start + ";")
            continue

        lines.append(cls_start)

        for i, mi in enumerate(methods):
            binding = _generate_method_binding(mi, overloaded)
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

    for cls_name in extra["classes"]:
        if cls_name not in used_class_extras:
            print(
                f"warning: {header_stem}.nbextra: [class:{cls_name}] did not match "
                f"any class parsed from {os.path.basename(header_path)}",
                file=sys.stderr,
            )

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


def _rel_under(path, anchor: str) -> Optional[Path]:
    """Path relative to `<repo>/<anchor>`, or None if it does not live there."""
    base = (_REPO_ROOT / anchor).resolve()
    try:
        return Path(path).resolve().relative_to(base)
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


def _clang_resource_include() -> Optional[str]:
    """Locate libclang's builtin headers (stddef.h et al.) as an `-isystem` flag.

    compile_commands.json never lists the compiler resource dir, so libclang
    would otherwise fail to find its own builtin headers.
    """
    base = Path("/usr/lib/clang")
    if not base.is_dir():
        return None
    for ver_dir in sorted(base.iterdir(), reverse=True):
        res_incl = ver_dir / "include"
        if res_incl.is_dir() and (res_incl / "stddef.h").exists():
            return f"-isystem{res_incl}"
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

        code = generate_bindings(hdr, classes, enums, ns=args.namespace, extra_dir=extra_dir)
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
