import platform as pf
import subprocess
import os
import tempfile
from ctypes import CDLL, cdll
from abc import ABC
from typing import Iterable


class JIT(ABC):

    def __init__(self, so_name: str, exports: Iterable[str] = ()):
        self.so_name = so_name
        self.ll_name = so_name + ".ll"
        self.exports = list(exports)
        self._lib = None
        self._so_dir = None

    def _get_so_path(self) -> str:
        system = pf.system()
        if system == "Windows":
            if self._so_dir is None:
                self._so_dir = tempfile.mkdtemp(prefix=f"{self.so_name}-")
            return os.path.join(self._so_dir, self.so_name + ".dll")

        ext = ".dylib" if system == "Darwin" else ".so"
        return os.path.join(".", self.so_name + ext)

    def _compile_so(self):
        so_path = self._get_so_path()
        cmd = ["clang", self.ll_name, "-o", so_path, "-shared", "-Wno-override-module"]
        if pf.system() == "Windows":
            for name in self.exports:
                cmd += ["-Xlinker", f"/EXPORT:{name}"]
        subprocess.run(cmd, check=True)
        return so_path

    def compile_and_load(self) -> CDLL:
        so_path = self._compile_so()
        self._lib = cdll.LoadLibrary(so_path)
        return self._lib
