#!/usr/bin/env python3
"""docs/erssi.1 documents every command-line option of erssi.

The options are the GOptionEntry tables of every file under src/ that
calls args_register(), except the bot (fe-none) and the fuzz targets
(fe-fuzz), which are other programs. Each option shown by --help must have
an entry of its own in the page: a ".TP" whose next line, a ".B" or ".BI",
names --<long name>. A mention in the text of another option does not
count. Hidden options are internal and only checked when the page has an
entry for them anyway. The page must also not name irssi's home directory,
~/.irssi: erssi uses ~/.erssi.

Prints TAP. Usage: check-man-options.py <source dir>"""
import re
import sys
from pathlib import Path

# programs of their own, not erssi; args.c defines args_register()
EXCLUDED = ("src/fe-none/", "src/fe-fuzz/", "src/core/args.c")
# { "long-name", 'c' or 0, FLAGS, G_OPTION_ARG_...
ENTRY_RE = re.compile(r'\{\s*"([a-z][a-z0-9-]*)",\s*(?:\'.\'|0)\s*,\s*([^,]+),\s*G_OPTION_ARG_')
# an option entry: ".TP", then the option line, roff writing --name as \-\-name
ENTRY_LINE_RE = re.compile(r'^\.TP[ \t]*\n(\.BI?[ \t][^\n]*)', re.M)
LONG_RE = re.compile(r'\\-\\-([a-z][a-z0-9\\-]*)')


def option_sources(root):
    """the files under src/ that register command-line options"""
    found = []
    for path in sorted((root / "src").rglob("*.c")):
        rel = path.relative_to(root).as_posix()
        if rel.startswith(EXCLUDED):
            continue
        if "args_register(" in path.read_text(errors="replace"):
            found.append(rel)
    return found


def main():
    root = Path(sys.argv[1])
    options = []
    for src in option_sources(root):
        for m in ENTRY_RE.finditer((root / src).read_text(errors="replace")):
            options.append((m.group(1), "G_OPTION_FLAG_HIDDEN" in m.group(2), src))
    page = root / "docs" / "erssi.1"
    if not options or not page.exists():
        print("1..1\nnot ok 1 no options found, or docs/erssi.1 is missing")
        return 1
    text = page.read_text(errors="replace")
    documented = set()
    for m in ENTRY_LINE_RE.finditer(text):
        # ".BI "\-c, \-\-connect=" SERVER" and ".B "\-\-update"" alike
        for name in LONG_RE.findall(m.group(1)):
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
