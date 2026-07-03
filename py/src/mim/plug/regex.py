from __future__ import annotations

import ctypes
from typing import List

from .. import Def, Driver, Level
from .._plugins.regex import regex
from ..plugin import MimPlugin
from .core import core
from .mem import mem


def __getattr__(name):
    return getattr(regex, name)


class RegBuilder(MimPlugin):

    def __init__(self, driver: Driver, libname: str, log_level=Level.Warn, initialize=True):
        super().__init__(driver, log_level, libname)
        self.lvl = log_level
        self.world = self.driver.world()
        if initialize:
            # `ll` must be loaded in the same pass as `compile` so that `ll.mim`'s
            # `import compile` resolves `%compile.Phase`; its emit phase produces `<libname>.ll`.
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
        self.match_func = self.world.mut_con([
            self.world.call(mem.M, self.world.lit_nat_0()),
            self.world.call(
                mem.Ptr0,
                [self.world.arr(self.world.top_nat(), self.world.type_i8())],
            ),
            self.world.cn([
                self.world.call(mem.M, self.world.lit_nat_0()),
                self.world.type_bool(),
            ]),
        ]).set(function_name)

        self.match_func.externalize()
        state_mem, to_match, exit = self.match_func.var().projs(3)

        regex_mem, matched, pos = self.world.implicit_app(
            self._final_def,
            [state_mem, to_match, self.world.lit(self.world.type_idx(self.world.top_nat()), 0)],
        ).projs(3)
        last_elem_ptr = self.world.call(mem.lea, [to_match, pos])
        final_mem, last_elem = self.world.call(mem.load, [regex_mem, last_elem_ptr]).projs(2)
        eq_zero = self.world.call(core.icmp.e, [last_elem, self.world.lit_i8(0)])

        matched_and_end = self.world.call(
            core.bit2.and_,
            self.world.lit_nat_0(),
            [matched, eq_zero],
        )
        self.match_func.app(False, exit, [final_mem, matched_and_end])

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
