"""End-to-end: build an email-matching regex and JIT it."""
from __future__ import annotations

import shutil
import pytest
import mim
import mim.plug.regex as regex


@pytest.mark.slow
@pytest.mark.needs_clang
def test_python_package_works(tmp_path, monkeypatch):
    if shutil.which("clang") is None:
        pytest.skip("clang not on PATH")

    monkeypatch.chdir(tmp_path)

    driver = mim.Driver()
    builder = regex.RegBuilder(driver, "regex", mim.Level.Error)

    alpha = builder.range("a", "z") | builder.range("A", "Z")  # [a-zA-Z]
    under_hyph = builder.lit("_") | builder.lit("-")  # [_\-]
    dot_under_hyph = builder.lit(".").disj(
        [builder.lit("_"), builder.lit("-")]
    )  # [._\-]
    an = builder.alnum()
    # Local: [a-zA-Z0-9](?:[a-zA-Z0-9]*[._\-]+[a-zA-Z0-9])*[a-zA-Z0-9]*
    local = an + (an["*"] + dot_under_hyph["+"] + an)["*"] + an["*"]

    # Domain: [a-zA-Z0-9](?:[a-zA-Z0-9]*[_\-]+[a-zA-Z0-9])*[a-zA-Z0-9]*
    domain = an + (an["*"] + under_hyph["+"] + an)["*"] + an["*"]

    # Subdomain: (?:(?:[a-zA-Z0-9]*[_\-]+[a-zA-Z0-9])*[a-zA-Z0-9]+\.)*
    subdomain = ((an["*"] + under_hyph["+"] + an)["*"] + an["+"] + builder.lit("."))[
        "*"
    ]

    # Top Level Domain (.com, .de, etc...): [a-zA-Z][a-zA-Z]+
    tld = alpha + alpha["+"]

    email = local + builder.lit("@") + domain + builder.lit(".") + subdomain + tld
    library = email.jit()

    assert library["match_func"]("test@test.de".encode("utf-8"))
