#!/usr/bin/env python3
"""Print a CANGO1 log as text, or as SavvyCAN GVRET CSV.

Usage:
  cango_convert.py file.cango
  cango_convert.py file.cango --csv [out.csv]
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cango_codec import format_gvret_csv, format_log


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print("usage: cango_convert.py file.cango [--csv [out.csv]]", file=sys.stderr)
        return 2
    blob = Path(args[0]).read_bytes()
    if "--csv" in args:
        text = format_gvret_csv(blob)
        idx = args.index("--csv")
        if idx + 1 < len(args) and not args[idx + 1].startswith("-"):
            Path(args[idx + 1]).write_text(text)
        else:
            sys.stdout.write(text)
        return 0
    sys.stdout.write(format_log(blob))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
