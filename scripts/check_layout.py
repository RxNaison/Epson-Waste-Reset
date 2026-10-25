#!/usr/bin/env python3
"""Checks the code layout CONTRIBUTING.md describes, over include/ewr/:

- ewr:: holds types: a free function declared directly in ewr:: fails, except
  ToString(enum) overloads, which ADL finds next to their enum.
- no static-only classes: a class whose only members are static functions is
  a namespace in disguise.
- a public header never includes protocol/, transport/ or update/.

Run from the repository root; exits 1 on any finding.
"""
import re
import sys
from pathlib import Path

INTERNAL = re.compile(r'#include\s+"(protocol|transport|update)/')
ACCESS = re.compile(r"^(public|private|protected)\s*:\s*")


def strip(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', text)
    text = re.sub(r"'(?:[^'\\\n]|\\.)*'", "''", text)
    return "\n".join(l for l in text.split("\n") if not l.lstrip().startswith("#"))


def function_name(statement):
    """The name a statement declares as a function, or None."""
    s = ACCESS.sub("", statement.strip())
    if not s or s.startswith(("using ", "typedef ", "friend ", "return ", "static_assert")) or "(" not in s:
        return None
    if re.match(r"(enum|struct|class|union|namespace)\b", s):
        return None
    head = s[:s.index("(")]
    if "=" in head:
        return None  # a variable with an initializer that happens to call something
    m = re.search(r"([A-Za-z_~][\w:]*)\s*$", head)
    return m.group(1) if m else None


def check(path):
    findings = []
    raw = path.read_text(encoding="utf-8")
    for n, line in enumerate(raw.split("\n"), 1):
        if INTERNAL.search(line):
            findings.append(f"{path}:{n}: public header includes an internal one: {line.strip()}")

    code = strip(raw)
    stack = []      # dicts: kind ('ns', 'class', 'body'), name, and member counts for classes
    text = ""
    line = 1
    for ch in code:
        if ch == "\n":
            line += 1
        if ch not in "{};":
            text += ch
            continue

        statement = text.strip()
        text = ""
        top = stack[-1] if stack else {"kind": "ns", "name": ""}
        if ch == "}":
            if stack:
                done = stack.pop()
                if done["kind"] == "class" and done["static"] and not done["members"]:
                    findings.append(f"{path}: class {done['name']} has only static member functions")
            continue

        name = function_name(statement) if top["kind"] in ("ns", "class") else None
        at_ewr = top["kind"] == "ns" and all(e["kind"] == "ns" for e in stack) and \
            "::".join(e["name"] for e in stack) == "ewr"
        if name and at_ewr and "::" not in name and name != "ToString":
            findings.append(f"{path}:{line}: free function {name}() directly in ewr::")
        if top["kind"] == "class":
            body = ACCESS.sub("", statement)
            if name:
                if body.startswith("static "):
                    top["static"] += 1
                else:
                    top["members"] += 1
            elif body and not re.match(r"(using|typedef|friend|enum|struct|class|union)\b", body):
                top["members"] += 1  # a data member

        if ch == "{":
            m = re.search(r"\bnamespace\s*([\w:]*)\s*$", statement)
            c = re.search(r"\b(class|struct|union)\s+(\w+)[^()]*$", statement)
            if m and "(" not in statement:
                for part in (m.group(1) or "").split("::"):
                    stack.append({"kind": "ns", "name": part})
                # 'namespace a::b {' closes with one brace: keep one entry, named a::b
                if m.group(1) and "::" in m.group(1):
                    parts = m.group(1).split("::")
                    del stack[-len(parts):]
                    stack.append({"kind": "ns", "name": m.group(1)})
            elif c and not name:
                stack.append({"kind": "class", "name": c.group(2), "static": 0, "members": 0})
            else:
                if top["kind"] == "class" and statement.strip().startswith(("enum", "struct", "class")):
                    pass
                stack.append({"kind": "body", "name": ""})
    return findings


def main():
    headers = sorted(p for p in Path("include/ewr").glob("*.h") if p.name != "ewr_c.h")
    findings = [f for h in headers for f in check(h)]
    for f in findings:
        print(f)
    if findings:
        print(f"\n{len(findings)} layout finding(s). See 'Code layout' in CONTRIBUTING.md.")
        return 1
    print(f"layout ok: {len(headers)} public headers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
