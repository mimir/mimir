import difflib
import itertools
import os
import shlex

import lit.formats
import lit.Test
import lit.TestRunner
import lit.util


class MimRoundTripTest(lit.formats.FileBasedTest):
    """Checks that re-emitting an input's `mim <flags>` output reproduces it byte for byte."""

    def __init__(self, *flags):
        self.flags = list(flags)

    def run(self, cmd, cwd, timeout):
        try:
            return lit.util.executeCommand(cmd, cwd=cwd, timeout=timeout)
        except lit.util.ExecuteCommandTimeoutException as e:
            return e.out, e.err + "\nreached timeout", -1

    def execute(self, test, litConfig):
        if test.config.unsupported:
            return lit.Test.Result(lit.Test.UNSUPPORTED, "test is unsupported")

        # lit 23 moved the per-test timeout to the suite config; see lit.site.cfg.py.in.
        timeout = getattr(test.config, "maxIndividualTestTime", None) or litConfig.maxIndividualTestTime or 0
        mim = shlex.split(test.config.mim)
        src = test.getSourcePath()
        name = os.path.basename(src)
        # The file stem is the module name, so each emitted file keeps the input's basename.
        base = lit.TestRunner.getTempPaths(test)[1] + ".rt"
        outs = [os.path.join(base, str(i), name) for i in (1, 2)]
        for out in outs:
            os.makedirs(os.path.dirname(out), exist_ok=True)

        _, err, code = self.run(mim + [src] + self.flags + [outs[0]], os.path.dirname(src), timeout)
        if code != 0:
            return lit.Test.Result(lit.Test.UNSUPPORTED, "input does not compile on its own:\n" + err)

        # The copy lives elsewhere, so relative imports must still resolve against the input's directory.
        _, err, code = self.run(mim + ["-I", os.path.dirname(src), outs[0]] + self.flags + [outs[1]], base, timeout)
        if code != 0:
            return lit.Test.Result(lit.Test.FAIL, f"re-reading {outs[0]} failed:\n{err}")

        with open(outs[0], "rb") as f1, open(outs[1], "rb") as f2:
            b1, b2 = f1.read(), f2.read()
        if b1 == b2:
            return lit.Test.Result(lit.Test.PASS)

        l1 = b1.decode("utf-8", "replace").splitlines(keepends=True)
        l2 = b2.decode("utf-8", "replace").splitlines(keepends=True)
        limit = 100
        diff = list(itertools.islice(difflib.unified_diff(l1, l2, outs[0], outs[1]), limit + 1))
        if len(diff) > limit:
            diff[limit:] = ["... more diff lines\n"]
        return lit.Test.Result(lit.Test.FAIL, "".join(diff))
