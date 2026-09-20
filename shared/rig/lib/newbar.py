#!/usr/bin/env python3
"""newbar.py "<before>" "<after>" - the y of a wooden plank that appeared.

Both arguments are `ui.py bars` output. Prints the y (points) of the lowest
plank present in `after` but not in `before`, or -1.

This is how the rig knows a card is LEGAL without knowing the rules: selecting
a card that forms a valid move makes an action plank appear, and selecting one
that does not makes nothing appear. Comparing the two answers is the whole
test.
"""
import ast
import sys


def bars(text):
    return ast.literal_eval(text.split("BARS ")[1].strip())


def main():
    before, after = bars(sys.argv[1]), bars(sys.argv[2])
    old = [y for y, _ in before]
    fresh = [y for y, _ in after if all(abs(y - o) > 12 for o in old)]
    print(fresh[-1] if fresh else -1)


if __name__ == "__main__":
    main()
