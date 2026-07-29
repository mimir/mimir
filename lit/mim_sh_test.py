import os
import lit.formats

class MimShTest(lit.formats.ShTest):
    def execute(self, test, litConfig):
        stem = os.path.splitext(os.path.basename(test.getSourcePath()))[0]
        # replace (not append) so it's always the current test's stem
        subs = [s for s in test.config.substitutions if s[0] != r"%\{s:stem\}"]
        test.config.substitutions = subs + [(r"%\{s:stem\}", stem)]
        return super().execute(test, litConfig)
