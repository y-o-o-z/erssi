#!/usr/bin/env python3
"""The defaults that /HELP pages give for settings match the C defaults.

A help page names a default as "name (default: value)" or "name (type;
default: value)"; several names can share one, "a, b (default: x)" or
"a, b (default: x and y)". Each such name is looked up in the
settings_add_*() calls under src/ and its C default compared. Where the
Shellter default configuration (irssi.conf) sets another value, the page
says so after a comma, as in "(default: OFF, the Shellter default
configuration sets ON)"; only the part before the comma is the default.

Prints one TAP line per setting. Usage: check-help-defaults.py <source dir>"""
import re
import sys
from pathlib import Path

ADD_RE = re.compile(
    r'settings_add_(str|bool|int|choice|time|size|level)'
    r'(?:_module\s*\(\s*"[^"]*"\s*,|\s*\()\s*"[^"]*"\s*,\s*'
    r'([A-Za-z_][A-Za-z0-9_]*|"[^"]*")\s*,\s*'
    r'("(?:[^"\\]|\\.)*"|[A-Za-z_][A-Za-z0-9_]*|-?\d+)'
    r'(?:\s*,\s*"([^"]*)")?\s*\)')
DEFINE_RE = re.compile(r'^#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(?:"([^"]*)"|(-?\d+)\b)', re.M)
ENUM_RE = re.compile(r'enum\s*[A-Za-z_0-9]*\s*\{([^}]*)\}', re.S)
# the name directly before "(", optionally with more names before it
HELP_RE = re.compile(
    r'((?:[a-z][a-z0-9_]*,\s+)*[a-z][a-z0-9_]*)\s+\((?:[^()]*?;\s*)?default:\s*([^,;)]*)')


def c_sources(src):
    # the fuzz harnesses register stand-ins for some settings
    return [p for p in sorted((src / "src").rglob("*.[ch]")) if "fe-fuzz" not in p.parts]


def enum_index(texts, ident):
    for text in texts:
        for body in ENUM_RE.findall(text):
            names = [re.sub(r'/\*.*?\*/', '', n, flags=re.S).strip()
                     for n in body.split(",")]
            idx = -1
            for n in names:
                if not n:
                    continue
                m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)\s*(?:=\s*(-?\d+))?$', n)
                if not m:
                    break
                idx = int(m.group(2)) if m.group(2) else idx + 1
                if m.group(1) == ident:
                    return idx
    return None


def c_defaults(src):
    texts = [p.read_text(errors="replace") for p in c_sources(src)]
    defines = {}
    for text in texts:
        for name, string, number in DEFINE_RE.findall(text):
            defines[name] = string or number
    found = {}
    for text in texts:
        for kind, name, value, choices in ADD_RE.findall(text):
            name = name[1:-1] if name.startswith('"') else defines.get(name)
            if name is None:
                continue
            if kind == "bool":
                value = {"TRUE": "on", "FALSE": "off"}.get(value)
            elif kind == "choice":
                idx = int(value) if re.match(r'-?\d+$', value) else enum_index(texts, value)
                opts = choices.split(";")
                value = opts[idx] if idx is not None and 0 <= idx < len(opts) else None
            elif value.startswith('"'):
                value = value[1:-1]
            elif re.match(r'-?\d+$', value) is None:
                value = defines.get(value)
            if value is not None:
                found.setdefault(name, set()).add(value.lower())
    return found


def help_defaults(src):
    for page in sorted((src / "docs" / "help" / "in").glob("*.in")):
        text = page.read_text(errors="replace")
        for names, value in HELP_RE.findall(text):
            names = [n.strip() for n in names.split(",")]
            value = " ".join(value.split()).lower()
            values = value.split(" and ") if len(names) > 1 and " and " in value else [value] * len(names)
            if len(values) != len(names):
                values = [value] * len(names)
            for n, v in zip(names, values):
                yield page.name, n, "" if v == "empty" else v


def main():
    src = Path(sys.argv[1])
    known = c_defaults(src)
    checks = list(help_defaults(src))
    if not checks:
        print("1..1\nnot ok 1 no \"(default: ...)\" found in docs/help/in")
        return 1
    print(f"1..{len(checks)}")
    failed = 0
    for i, (page, name, value) in enumerate(checks, 1):
        c = known.get(name)
        if c is None:
            failed += 1
            print(f"not ok {i} {page}: {name}: no settings_add_*() for it under src/")
        elif value not in c:
            failed += 1
            shown = " or ".join(repr(v) for v in sorted(c))
            print(f"not ok {i} {page}: {name}: help says default {value!r}, the code has {shown}")
        else:
            print(f"ok {i} {page}: {name} = {value!r}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
