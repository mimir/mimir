import lit.formats
import os

config.name = 'mim regression'
config.test_format = lit.formats.ShTest(True)

config.suffixes = ['.mim']

if not getattr(config, 'test_source_root', None):
    config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.my_obj_root, 'test')

config.substitutions.append(('%mim', config.mim))
config.substitutions.append(('%FileCheck', '"{}"'.format(config.filecheck)))

# inhert env vars
config.environment = os.environ

config.available_features.add("always")
