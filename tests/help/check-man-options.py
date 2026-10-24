#!/usr/bin/env python3
"""docs/erssi.1 documents every command-line option of erssi.

The options are the GOptionEntry tables that the erssi binary registers
(src/core/core.c, src/core/session.c, src/fe-common/core/fe-common-core.c,
src/fe-ansi/irssi.c). Each option shown by --help must have a ".B" or ".BI"
entry naming --<long name> in the page; hidden ones are internal and only
checked when the page names them anyway. The page must also not name
irssi's home directory, ~/.irssi: erssi uses ~/.erssi.

Prints TAP. Usage: check-man-options.py <source dir>"""
import re
import sys
from pathlib import Path

SOURCES = [
    "src/core/core.c",
    "src/core/session.c",
    "src/fe-common/core/fe-common-core.c",
    "src/fe-ansi/irssi.c",
]
# { "long-name", 'c' or 0, FLAGS, G_OPTION_ARG_...
ENTRY_RE = re.compile(r'\{\s*"([a-z][a-z0-9-]*)",\s*(?:\'.\'|0)\s*,\s*([^,]+),\s*G_OPTION_ARG_')
# the option as roff writes it: \-\-name
MAN_RE = re.compile(r'^\.BI?\s+"?[^\n]*?\\-\\-([a-z][a-z0-9\\-]*)', re.M)


def main():
    root = Path(sys.argv[1])
    options = []
    for src in SOURCES:
        for m in ENTRY_RE.finditer((root / src).read_text(errors="replace")):
            options.append((m.group(1), "G_OPTION_FLAG_HIDDEN" in m.group(2), src))
    page = root / "docs" / "erssi.1"
    if not options or not page.exists():
        print("1..1\nnot ok 1 no options found, or docs/erssi.1 is missing")
        return 1
    text = page.read_text(errors="replace")
    documented = set()
    for m in MAN_RE.finditer(text):
        documented.add(m.group(1).replace("\\-", "-"))
        # ".BI "\-c, \-\-connect=" SERVER" and ".B "\-\-update"" alike
        for name in re.findall(r'\\-\\-([a-z][a-z0-9\\-]*)', m.group(0)):
            documented.add(name.replace("\\-", "-"))

    checks = []
    for name, hidden, src in options:
        if name in documented:
            checks.append((True, f"--{name} ({src})"))
        elif hidden:
            checks.append((True, f"--{name} ({src}) # SKIP hidden, not in the page"))
        else:
            checks.append((False, f"--{name} ({src}) has no entry in docs/erssi.1"))
    old_home = [i for i, line in enumerate(text.splitlines(), 1) if "~/.irssi" in line]
    checks.append((not old_home, "docs/erssi.1 names ~/.erssi, not ~/.irssi"
                   + (f" (lines {old_home})" if old_home else "")))
    page_name = re.search(r'^\.TH\s+(\S+)', text, re.M)
    checks.append((page_name is not None and page_name.group(1).lower() == "erssi",
                   "docs/erssi.1 is the page of erssi (.TH ERSSI)"))

    print(f"1..{len(checks)}")
    failed = 0
    for i, (ok, what) in enumerate(checks, 1):
        failed += not ok
        print(f"{'ok' if ok else 'not ok'} {i} {what}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
