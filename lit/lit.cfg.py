import os, platform, subprocess, sys
sys.path.insert(0, os.path.dirname(__file__))
from mim_sh_test import MimShTest
from mim_roundtrip_test import MimRoundTripTest

config.name = 'mim regression'
# lit 23 deprecates the external shell, but our RUN lines rely on `$?` and `VAR=`, which the
# internal shell does not implement.
try:
    config.test_format = MimShTest(True, force_execute_external=True)
except TypeError:
    config.test_format = MimShTest(True)

config.suffixes = ['.mim']

if not getattr(config, 'test_source_root', None):
    config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.my_obj_root, 'lit', 'out')

# `--param roundtrip=mim|ast` ignores the RUN lines and checks instead that each input's `-o`/`--output-ast` output round-trips.
roundtrip = lit_config.params.get('roundtrip')
if roundtrip:
    # Optimizing twice is not idempotent - but the dump has to be.
    flags = {'mim': ['--no-opt', '--output-mim'], 'ast': ['--output-ast']}
    if roundtrip not in flags:
        lit_config.fatal(f"unknown roundtrip mode '{roundtrip}'; expected one of: {', '.join(flags)}")
    config.name = f'mim roundtrip-{roundtrip}'
    config.test_format = MimRoundTripTest(*flags[roundtrip])
    config.test_exec_root = os.path.join(config.my_obj_root, 'roundtrip', roundtrip)
    # `UNSUPPORTED: roundtrip` opts out an input that only compiles with flags from its RUN lines.
    config.available_features.add('roundtrip')

config.substitutions.append(('%mim', config.mim))
config.substitutions.append(('%FileCheck', '"{}"'.format(config.filecheck)))
# Parent of the plugin directory, so that a test can spell a plugin as the path `mim/<plugin>`.
# os.path.join would mix in a backslash on Windows, which the external bash then swallows as an escape.
config.substitutions.append(('%libdir', config.my_obj_root + '/lib'))
# Path-list separator (`:` on Unix, `;` on Windows).
config.substitutions.append(('%pathsep', os.pathsep))
# MSVC's link.exe lacks the compiler-rt builtins (e.g. `__umodti3` for an `i128` `urem`) that LLVM lowers to libcalls.
clang_rt = ''
if sys.platform == 'win32':
    try:
        lib = subprocess.run(['clang', '-print-libgcc-file-name', '--rtlib=compiler-rt'], capture_output=True, text=True).stdout.strip()
        if os.path.exists(lib):
            clang_rt = '"{}"'.format(lib.replace('\\', '/'))
    except OSError:
        pass
config.substitutions.append(('%clang_rt', clang_rt))

# inherit env vars
config.environment = os.environ

config.available_features.add("always")

# Gate expensive tests on optimized builds via `// REQUIRES: release`.
if getattr(config, 'build_type', '').lower() in ('release', 'relwithdebinfo', 'minsizerel'):
    config.available_features.add("release")
# 16-bit floating-point support (needs std::float16_t) is detected once by CMake and passed in via
# lit.site.cfg.py. Tests emitting math.F16 literals gate on this via `// REQUIRES: fp16`.
if getattr(config, 'fp16', False):
    config.available_features.add("fp16")
# operating system is detected using sys.platform (see https://docs.python.org/3/library/sys.html#sys.platform)
# Tests that only work on e.g. Linux operating systems specify this via `// REQUIRES: system-linux`.
config.available_features.add(f"system-{sys.platform}")
# The native x86 backend emits SysV/ELF assembly; tests run its binaries via `%if x86_64-linux %{ ... %}`.
# Static and cross builds have no x86 plugin.
if not getattr(config, 'x86', True):
    config.excludes.add('x86')
elif sys.platform == 'linux' and platform.machine() == 'x86_64':
    config.available_features.add('x86_64-linux')
# `--param x86_k=<n>` compiles every `-p x86 … -X x86:o=` run with `-X x86:k=<n>`, so the expected exit codes check the spiller at small k.
x86_k = lit_config.params.get('x86_k')
if x86_k:
    if not x86_k.isdigit():
        lit_config.fatal(f"x86_k expects a register count, got '{x86_k}'")
    # lit escapes backslashes in replacements, so no backreferences.
    for after in (r'(?<=-p x86)', r'(?<=\.ll)'):
        config.substitutions.insert(0, (after + ' -X x86:o=', f' -X x86:k={x86_k} -X x86:o='))
# CUDA support (needs CUDA toolkit) is detected once by CMake and passed in via
# lit.site.cfg.py. Tests gate on this via `// REQUIRES: cuda`.
if getattr(config, 'cuda_can_run_on_device', False):
    config.available_features.add('cuda')
