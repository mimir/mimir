import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from mim_sh_test import MimShTest

config.name = 'mim regression'
config.test_format = MimShTest(True)

config.suffixes = ['.mim']

if not getattr(config, 'test_source_root', None):
    config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.my_obj_root, 'test')

config.substitutions.append(('%mim', config.mim))
config.substitutions.append(('%FileCheck', '"{}"'.format(config.filecheck)))

# inherit env vars
config.environment = os.environ

config.available_features.add("always")

# Gate expensive tests on optimized builds via `// REQUIRES: release`.
if getattr(config, 'build_type', '').lower() in ('release', 'relwithdebinfo', 'minsizerel'):
    config.available_features.add("release")
# 16-bit floating-point support (needs std::float16_t) is detected once by CMake and passed in via
# lit.site.cfg.py. Tests emitting %math.F16 literals gate on this via `// REQUIRES: fp16`.
if getattr(config, 'fp16', False):
    config.available_features.add("fp16")
# operating system is detected using sys.platform (see https://docs.python.org/3/library/sys.html#sys.platform)
# Tests that only work on e.g. Linux operating systems specify this via `// REQUIRES: system-linux`.
config.available_features.add(f"system-{sys.platform}")
# CUDA support (needs CUDA toolkit) is detected once by CMake and passed in via
# lit.site.cfg.py. Tests gate on this via `// REQUIRES: cuda`.
if getattr(config, 'cuda_can_run_on_device', False):
    config.available_features.add('cuda')
