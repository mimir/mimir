#!/usr/bin/env python3

"""Doxygen input filter: wraps every Mim listing in a `mim-code` div.

Doxygen discards the language of a fenced code block, so `docs/mim.js` needs the wrapper to tell a Mim listing from any other verbatim block.
"""

import re
import sys

WRAP = re.compile(r'^([ \t]*)(?:```mim\n.*?^[ \t]*```|\\include "[^"]*\.mim")$', re.M | re.S)


def wrap(match):
    indent = match.group(1)
    return f'{indent}<div class="mim-code">\n\n{match.group(0)}\n\n{indent}</div>'


def main():
    with open(sys.argv[1], encoding='utf-8') as file:
        sys.stdout.write(WRAP.sub(wrap, file.read()))


if __name__ == '__main__':
    main()
