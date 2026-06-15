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
