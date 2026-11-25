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
# The native backends' symbol and local-label prefixes of the host's object format, as FileCheck's [[SYM]] and [[LBL]].
config.substitutions.append(('%asmdefs', '-DSYM=_ -DLBL=L' if sys.platform == 'darwin' else '-DSYM= -DLBL=.L'))
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

# Git-for-Windows bash occasionally fails to launch a freshly-built exe (a Windows loader hiccup
# resolving a load-time DLL), surfacing as a spurious exit-127 with no output before mim even runs.
# Retry only on Windows; a genuine regression still fails every attempt, so this cannot mask real
# failures.
if sys.platform == "win32":
    config.test_retry_attempts = 2

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

# A native backend emits assembly for the host, so its binaries can be assembled and run here.
# Tests gate on `native` and spell the backend as `%native`.
# Static and cross builds have no native backend plugins.
native = None
if not getattr(config, 'x86', True):
    config.excludes.update({'x86', 'arm'})
else:
    machine = platform.machine().lower()
    if sys.platform == 'linux' and machine in ('x86_64', 'amd64'):
        native = 'x86'
        config.available_features.add('x86_64-linux')
    elif sys.platform in ('linux', 'darwin') and machine in ('aarch64', 'arm64'):
        native = 'arm'
if native:
    config.available_features.add('native')
    config.substitutions.append(('%native', native))

# `--param <backend>_k=<n>` (`x86_k`, `arm_k`) compiles every `-p <backend> … -X <backend>:o=` run with `-X <backend>:k=<n>`, so the expected exit codes check the spiller at small k.
# On a host whose native backend it is, this includes the `%native` spelling of the shared backend tests.
for backend in ('x86', 'arm'):
    k = lit_config.params.get(f'{backend}_k')
    if not k:
        continue
    if not k.isdigit():
        lit_config.fatal(f"{backend}_k expects a register count, got '{k}'")
    spellings = [backend] + (['%native'] if native == backend else [])
    # lit escapes backslashes in replacements, so no backreferences.
    for spelling in spellings:
        for after in (rf'(?<=-p {spelling})', r'(?<=\.ll)'):
            config.substitutions.insert(0, (f'{after} -X {spelling}:o=', f' -X {backend}:k={k} -X {spelling}:o='))
# CUDA support (needs CUDA toolkit) is detected once by CMake and passed in via
# lit.site.cfg.py. Tests gate on this via `// REQUIRES: cuda`.
if getattr(config, 'cuda_can_run_on_device', False):
    config.available_features.add('cuda')
