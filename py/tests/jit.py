from __future__ import annotations

from pathlib import Path
from unittest.mock import patch

import mim.jit as mim_jit


def test_windows_jit_dll_path_uses_tempdir(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)

    jit = mim_jit.JIT("regex_test")
    with patch.object(mim_jit.pf, "system", return_value="Windows"):
        dll_path = Path(jit._get_so_path())
        assert Path(jit._get_so_path()) == dll_path

    assert dll_path.is_absolute()
    assert dll_path.parent != tmp_path
    assert dll_path.name == "regex_test.dll"
