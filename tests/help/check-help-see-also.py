#!/usr/bin/env python3
"""Every topic under "See also" in a /HELP page has a page of its own.

The list follows "%9See also:%9" on the same line or on the lines after it,
up to a blank line. An entry such as "SET quit_message" points to the page
of its first word. /HELP finds a topic by its file name in docs/help, so a
name without docs/help/in/<name>.in prints "No help for <name>".

Prints one TAP line per page with a "See also". Usage:
check-help-see-also.py <source dir>"""
import re
import sys
from pathlib import Path

SEE_ALSO_RE = re.compile(r'%9See also:%9[ \t]*(.*?)(?:\n[ \t]*\n(?=\S)|\Z)', re.S)


def main():
    pages = Path(sys.argv[1]) / "docs" / "help" / "in"
    have = {p.stem.lower() for p in pages.glob("*.in")}
    checks = []
    for page in sorted(pages.glob("*.in")):
        m = SEE_ALSO_RE.search(page.read_text(errors="replace"))
        if m is None:
            continue
        entries = [e.strip() for e in re.split(r'[,\n]', m.group(1)) if e.strip()]
        missing = [e for e in entries if e.split()[0].lstrip("$").lower() not in have]
        checks.append((page.name, entries, missing))
    if not checks:
        print("1..1\nnot ok 1 no \"See also\" found in docs/help/in")
        return 1
    print(f"1..{len(checks)}")
    failed = 0
    for i, (name, entries, missing) in enumerate(checks, 1):
        if not entries:
            failed += 1
            print(f"not ok {i} {name}: an empty \"See also\"")
        elif missing:
            failed += 1
            print(f"not ok {i} {name}: no help page for {', '.join(missing)}")
        else:
            print(f"ok {i} {name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
