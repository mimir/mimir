# To run, do this beforehand:
# source build/.venv/bin/activate
import mim
import mim.plug.regex as regex

driver = mim.Driver()
builder = regex.RegBuilder(driver, "regex_demo", mim.Level.Error)
pattern = (builder.lit("a") + builder.lit("b") + builder.lit("c"))["+"]
library = pattern.jit()
matcher = library["match_func"]

print(matcher(b"abc"))    # True
print(matcher(b"abcabc")) # True
print(matcher(b"xyz"))    # False
