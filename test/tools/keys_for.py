#!/usr/bin/env python3
"""Turn text into ALPHABOX_KEYPIPE / ALPHABOX_KEYSCRIPT key tokens (US layout).

usage: keys_for.py [--enter] "<text>"
  Prints space-separated tokens, e.g.
    keys_for.py --enter "cmd /c dir"  ->  c m d space slash c space d i r enter
  Append to an ALPHABOX_KEYPIPE file to type the text into the guest. The guest
  keyboard layout must be US for the shifted symbols to come out right.
"""
import sys

PLAIN = {
    " ": "space", "/": "slash", "\\": "bslash", ".": "dot", ",": "comma",
    "-": "minus", "=": "equals", ";": "semicolon", "'": "quote",
    "[": "lbracket", "]": "rbracket", "`": "grave", "\t": "tab",
}
SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7",
    "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equals",
    ":": "semicolon", '"': "quote", "<": "comma", ">": "dot", "?": "slash",
    "|": "bslash", "{": "lbracket", "}": "rbracket", "~": "grave",
}


def tokens(text):
    out = []
    for ch in text:
        if ch.isascii() and (ch.islower() or ch.isdigit()):
            out.append(ch)
        elif ch.isascii() and ch.isupper():
            out.append("shift-" + ch.lower())
        elif ch in PLAIN:
            out.append(PLAIN[ch])
        elif ch in SHIFTED:
            out.append("shift-" + SHIFTED[ch])
        else:
            raise ValueError("no key for %r" % ch)
    return out


def main():
    args = sys.argv[1:]
    enter = "--enter" in args
    args = [a for a in args if a != "--enter"]
    if len(args) != 1:
        sys.exit(__doc__)
    toks = tokens(args[0]) + (["enter"] if enter else [])
    print(" ".join(toks))
    return 0


if __name__ == "__main__":
    sys.exit(main())
