#!/usr/bin/env python3
"""Print a CANGO1 log as text. Usage: cango_convert.py file.cango"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cango_codec import format_log


def main():
    if len(sys.argv) != 2:
        print("usage: cango_convert.py file.cango", file=sys.stderr)
        return 2
    sys.stdout.write(format_log(Path(sys.argv[1]).read_bytes()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
