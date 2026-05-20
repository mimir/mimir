from __future__ import annotations

from collections.abc import Callable, Iterator
from enum import IntEnum
from typing import IO, Any, ParamSpec, TypeAlias, TypeVar, overload

from . import _mim
from ._mim import *

P = ParamSpec("P")
R = TypeVar("R")

CallArg: TypeAlias = Def | list[Def] | tuple[Def, ...]


def print_mim_error(exc: BaseException, file: IO[str] | None = ...) -> None: ...
def guard_mim_errors(*, file: IO[str] | None = ..., reraise: bool = ...) -> Iterator[None]: ...

@overload
def catch_mim_errors(
    func: Callable[P, R],
    *,
    default: Any = ...,
    file: IO[str] | None = ...,
    reraise: bool = ...,
) -> Callable[P, R | None]: ...
@overload
def catch_mim_errors(
    func: None = ...,
    *,
    default: Any = ...,
    file: IO[str] | None = ...,
    reraise: bool = ...,
) -> Callable[[Callable[P, R]], Callable[P, R | None]]: ...


class Def(_mim.Def):
    @overload
    def __getitem__(self, index: int) -> Def: ...
    @overload
    def __getitem__(self, index: tuple[int, int]) -> Def: ...


class World(_mim.World):
    def call(self, callee: Def | Sym | str | IntEnum, *args: CallArg) -> Def: ...
