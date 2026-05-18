from __future__ import annotations
from pathlib import Path

import pytest
import mim


@pytest.fixture
def driver() -> mim.Driver:
    return mim.Driver()


@pytest.fixture
def world(driver: mim.Driver):
    return driver.world()


@pytest.fixture
def regex_world():
    driver = mim.Driver()
    driver.load_plugins(["compile", "mem", "core", "math", "regex", "opt"])
    return driver.world()
