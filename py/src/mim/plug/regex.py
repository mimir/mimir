from __future__ import annotations

import ctypes
from typing import List

from .. import Def, Driver, LogLevel
from .._plugins.regex import regex
from ..plugin import MimPlugin
from .core import core
from .mem import mem


def __getattr__(name):
    return getattr(regex, name)


class RegBuilder(MimPlugin):

    def __init__(self, driver: Driver, libname: str, log_level=LogLevel.Warn, initialize=True):
        super().__init__(driver, log_level, libname)
        self.lvl = log_level
        self.world = self.driver.world()
        if initialize:
            driver.load_plugins(["regex", "ll"])

    def _char_lit(self, lit) -> Def:
        return self.world.lit_i8(ord(lit))

    def _set_final_def(self, d: Def):
        self._final_def = d

    def lit(self, lit: str) -> "MimRegex":
        if len(lit) > 1:
            defs = [MimRegex(self.world.call(regex.lit, self._char_lit(c)), self) for c in lit]
            acc = defs[0]
            for re in defs[1::]:
                acc = acc + re
            return MimRegex(acc.def_, self)
        return MimRegex(self.world.call(regex.lit, self._char_lit(lit)), self)

    def alnum(self) -> "MimRegex":
        exp = self.range("a", "z") | self.range("A", "Z") | self.range("0", "9")
        return MimRegex(exp.def_, self)

    def alpha(self):
        return MimRegex((self.range("a", "z") | self.range("A", "Z")).def_, self)

    def range(self, left, right) -> "MimRegex":
        d = self.world.call(regex.range, [self._char_lit(left), self._char_lit(right)])
        return MimRegex(d, self)

    def build(self):
        function_name = "match_func"
        mem_t = self.world.call(mem.M, self.world.lit_nat_0())
        str_t = self.world.call(mem.Ptr0, [self.world.annex(mem.Char)])

        strlen = self.world.mut_con([mem_t, str_t, self.world.cn([mem_t, self.world.type_i64()])]).set("strlen")

        self.match_func = self.world.mut_con([
            mem_t,
            str_t,
            self.world.cn([mem_t, self.world.type_bool()]),
        ]).set(function_name)

        self.match_func.externalize()
        state_mem, c_str, exit = self.match_func.var().projs(3)

        match_len = self.world.mut_con([mem_t, self.world.type_i64()])
        self.match_func.app(False, strlen, [state_mem, c_str, match_len])
        state_mem, length = match_len.var().projs(2)

        n = self.world.call(core.bitcast, self.world.type_nat(), length)
        to_match = self.world.call(mem.assume, [c_str, n])

        regex_mem, matched, pos = self.world.implicit_app(
            self._final_def,
            [state_mem, to_match, self.world.call(regex.pos, n, self.world.lit_nat_0())],
        ).projs(3)
        at_end = self.world.call(core.icmp.e, [pos, self.world.call(regex.pos, n, n)])

        matched_and_end = self.world.call(
            core.bit2.and_,
            self.world.lit_nat_0(),
            [matched, at_end],
        )
        match_len.app(False, exit, [regex_mem, matched_and_end])

        self.register_func(function_name, [ctypes.c_char_p], ctypes.c_bool)


class MimRegex:

    def __init__(self, def_: Def, builder: RegBuilder):
        self.def_ = def_
        self.builder_ = builder
        self.world = self.builder_.world

    def _conj(self, expr: List["MimRegex"]) -> "MimRegex":
        new_expr = [self.def_]
        new_expr.extend([x.def_ for x in expr])
        d = self.world.call(regex.conj, new_expr)
        return MimRegex(d, self.builder_)

    def star(self) -> "MimRegex":
        d = self.world.call(regex.quant.star, self.def_)
        return MimRegex(d, self.builder_)

    def plus(self) -> "MimRegex":
        d = self.world.call(regex.quant.plus, self.def_)
        return MimRegex(d, self.builder_)

    def any(self) -> "MimRegex":
        d = self.world.call(regex.any)
        return MimRegex(d, self.builder_)

    def __invert__(self) -> "MimRegex":
        d = self.world.call(regex.quant.optional, self.def_)
        return MimRegex(d, self.builder_)

    def __neg__(self) -> "MimRegex":
        d = self.world.call(regex.not_, self.def_)
        return MimRegex(d, self.builder_)

    def disj(self, expr: List["MimRegex"]) -> "MimRegex":
        d = self.world.call(regex.disj, [self.def_] + [x.def_ for x in expr])
        return MimRegex(d, self.builder_)

    def __or__(self, other: "MimRegex") -> "MimRegex":
        d = self.world.call(regex.disj, [self.def_] + [other.def_])
        return MimRegex(d, self.builder_)

    def empty(self) -> "MimRegex":
        d = self.world.call(regex.empty)
        return MimRegex(d, self.builder_)

    def __add__(self, other: "MimRegex") -> "MimRegex":
        d = self._conj([other]).def_
        return MimRegex(d, self.builder_)

    def __mul__(self, times: int) -> "MimRegex":
        acc = self
        for _ in range(times):
            acc = acc + self
        return MimRegex(acc.def_, self.builder_)

    def __getitem__(self, postfix_operator: str) -> "MimRegex":
        if len(postfix_operator) != 1:
            raise ValueError("only strings of size 1 are valid")
        if postfix_operator not in ["*", "+", "?"]:
            raise ValueError("postfix_operator has to match either *, ? or +")

        match postfix_operator:
            case "*":
                d = self.world.call(regex.quant.star, self.def_)
                return MimRegex(d, self.builder_)
            case "+":
                d = self.world.call(regex.quant.plus, self.def_)
                return MimRegex(d, self.builder_)
            case "?":
                d = self.world.call(regex.quant.optional, self.def_)
                return MimRegex(d, self.builder_)
            case _:
                raise RuntimeError(f"Unexpected Identifier {postfix_operator}")

    def jit(self):
        self.builder_._set_final_def(self.def_)
        return self.builder_.jit()
