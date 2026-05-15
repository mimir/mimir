# first source venv:
# source ../build/.venv/bin/activate
import mim
import mim.plug.regex as regex
from pathlib import Path

driver = mim.Driver()
mim_path = (Path(__file__).resolve().parent / "../build/lib/mim").resolve()
driver.add_search_path(str(mim_path))

builder = regex.RegBuilder(driver, "regex_demo", mim.Level.Error)
pattern = (builder.lit("a") + builder.lit("b") + builder.lit("c"))["+"]
matcher = pattern.jit()["match_func"]

print(matcher(b"abc"))    # True
print(matcher(b"abcabc")) # True
print(matcher(b"xyz"))    # False
