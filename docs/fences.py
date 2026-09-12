#!/usr/bin/env python3

"""Doxygen input filter: wraps every Mim and EBNF listing in a `<lang>-code` div.

Doxygen discards the language of a fenced code block, so `docs/mim.js` and `docs/ebnf.js` need the wrapper to tell such a listing from any other verbatim block.
"""

import re
import sys

WRAP = re.compile(r'^([ \t]*)(?:```(mim|ebnf)\n.*?^[ \t]*```|\\include "[^"]*\.(mim)")$', re.M | re.S)


def wrap(match):
    indent, lang = match.group(1), match.group(2) or match.group(3)
    return f'{indent}<div class="{lang}-code">\n\n{match.group(0)}\n\n{indent}</div>'


def main():
    with open(sys.argv[1], encoding='utf-8') as file:
        sys.stdout.write(WRAP.sub(wrap, file.read()))


if __name__ == '__main__':
    main()
