from collections.abc import Callable
from contextlib import contextmanager
from enum import IntEnum
from functools import wraps
import sys
from typing import IO, ParamSpec, TypeVar, TypeAlias

from ._mim import *
from ._mim import Def, Sym, World
from .callable import MimCallable
from .jit import JIT
from .plugin import MimPlugin

P = ParamSpec("P")
R = TypeVar("R")


def print_mim_error(exc: BaseException, file: IO[str] | None = None) -> None:
    if file is None:
        file = sys.stderr

    print(exc, file=file)


@contextmanager
def guard_mim_errors(*, file: IO[str] | None = None, reraise: bool = False):
    try:
        yield
    except MIM_Error as exc:
        print_mim_error(exc, file=file)
        if reraise:
            raise


def catch_mim_errors(
    func: Callable[P, R] | None = None,
    *,
    default=None,
    file: IO[str] | None = None,
    reraise: bool = False,
):
    def decorator(wrapped: Callable[P, R]) -> Callable[P, R | None]:
        @wraps(wrapped)
        def guarded(*args: P.args, **kwargs: P.kwargs) -> R | None:
            try:
                return wrapped(*args, **kwargs)
            except MIM_Error as exc:
                print_mim_error(exc, file=file)
                if reraise:
                    raise
                return default

        return guarded

    if func is None:
        return decorator

    return decorator(func)


CallArg: TypeAlias = Def | list[Def] | tuple[Def, ...]


def call(self: World, callee: Def | Sym | str | IntEnum, *args: CallArg) -> Def:
    if isinstance(callee, IntEnum):
        callee = self.annex(callee.value)
    else:
        assert False

    for arg in args:
        operands = list(arg) if isinstance(arg, (list, tuple)) else [arg]
        callee = self.implicit_app(callee, operands)
    return callee


World.call = call

from . import plug
from .plug.regex import MimRegex, RegBuilder
