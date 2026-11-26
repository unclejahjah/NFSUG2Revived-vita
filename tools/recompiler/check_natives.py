#!/usr/bin/env python3
"""Fails when the generated code lacks a NATIVE_OVERRIDES entry: older code links without the native, silently.

Usage: check_natives.py <games/ug2.py> <generated folder>
"""
import ast
import glob
import os
import re
import sys


def overrides(game_file):
    tree = ast.parse(open(game_file).read(), game_file)
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(getattr(t, 'id', None) == 'NATIVE_OVERRIDES' for t in node.targets):
            return set(ast.literal_eval(node.value))
    sys.exit('%s: no NATIVE_OVERRIDES' % game_file)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    wanted = overrides(sys.argv[1])
    wired = set()
    for path in glob.glob(os.path.join(sys.argv[2], '*.cpp')):
        with open(path, errors='replace') as f:
            wired.update(int(m, 16) for m in re.findall(r'RC_NATIVE\(0x([0-9a-f]+)', f.read()))
    missing = sorted(wanted - wired)
    if missing:
        sys.exit('%s is older than NATIVE_OVERRIDES: no RC_NATIVE for %s. Regenerate it (tools/build.py --regenerate).'
                 % (sys.argv[2], ', '.join('0x%x' % a for a in missing)))


if __name__ == '__main__':
    main()
