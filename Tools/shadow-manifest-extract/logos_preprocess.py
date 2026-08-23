#!/usr/bin/env python3
"""Turn the small Logos surface used by Shadow into parseable metadata.

This is intentionally a scanner, not a Logos compiler.  It handles the
directives that matter to the manifest (`%hook`, `%hookf`, `%group`, `%init`,
`%end`) and records Objective-C method selectors with source lines.  Method
bodies are skipped; the next Clang pass consumes the normalized declarations.
"""

import argparse
import json
import re
import sys


DIRECTIVE_RE = re.compile(r"^\s*%(hook|hookf|group|init|end)\b(.*)$")
METHOD_RE = re.compile(r"^\s*([-+])\s*\(([^)]*)\)")


def strip_comments(text):
    out = []
    i = 0
    in_string = False
    while i < len(text):
        c = text[i]
        if in_string:
            out.append(c)
            if c == "\\" and i + 1 < len(text):
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_string = False
            i += 1
            continue
        if c == '"':
            in_string = True
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < len(text) and text[i + 1] == "/":
            while i < len(text) and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if c == "/" and i + 1 < len(text) and text[i + 1] == "*":
            out.extend("  ")
            i += 2
            while i < len(text) and not (text[i] == "*" and i + 1 < len(text) and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            if i < len(text):
                out.extend("  ")
                i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def selector_from_signature(signature):
    tail = signature[signature.find(")") + 1:]
    labels = re.findall(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*:", tail)
    if labels:
        return "".join(label + ":" for label in labels)
    match = re.search(r"\b([A-Za-z_][A-Za-z0-9_]*)\b", tail)
    return match.group(1) if match else None


def parse_text(text, source="<text>"):
    clean = strip_comments(text)
    lines = clean.splitlines()
    hooks = []
    current_group = None
    current = None
    pending = []

    for number, line in enumerate(lines, 1):
        directive = DIRECTIVE_RE.match(line)
        if directive:
            kind, rest = directive.groups()
            rest = rest.strip()
            if kind == "group":
                current_group = rest or None
            elif kind == "hook":
                current = {"class": rest.split()[0], "group": current_group,
                           "line": number, "methods": []}
                hooks.append(current)
            elif kind == "hookf":
                current = {"function": rest.split(",", 1)[-1].rstrip(") "),
                           "group": current_group, "line": number,
                           "methods": [], "function_hook": True}
                hooks.append(current)
            elif kind == "init":
                pending.append({"initializer": rest, "group": current_group,
                                "line": number})
            elif kind == "end":
                current = None
            continue

        if not current:
            continue
        method = METHOD_RE.match(line)
        if not method:
            continue
        signature = line
        while "{" not in signature and ";" not in signature and number < len(lines):
            number += 1
            signature += " " + lines[number - 1].strip()
        selector = selector_from_signature(signature)
        if selector:
            current["methods"].append({
                "selector": selector,
                "kind": "instance" if method.group(1) == "-" else "class",
                "return_type": method.group(2).strip(),
                "line": number,
            })

    return {"source": source, "hooks": hooks, "initializers": pending}


def parse_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as stream:
        return parse_text(stream.read(), path)


def normalized_source(metadata):
    """Emit a declaration-only ObjC translation unit for Clang's AST parser."""
    out = []
    for hook in metadata["hooks"]:
        cls = hook.get("class")
        if not cls:
            continue
        # A standalone interface is valid without SDK headers. A category
        # would require the original class declaration and makes Clang reject
        # a source-only extraction fixture before it can emit an AST.
        out.append(f"@interface {cls}")
        for method in hook["methods"]:
            labels = method["selector"].split(":")
            labels = [label for label in labels if label]
            if ":" in method["selector"]:
                declaration = "".join(f"{label}:(id)value " for label in labels).rstrip()
            else:
                declaration = method["selector"]
            sign = "-" if method["kind"] == "instance" else "+"
            out.append(f"{sign} (id){declaration};")
        out.append("@end")
    return "\n".join(out) + ("\n" if out else "")


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("--out", help="write metadata JSON instead of stdout")
    parser.add_argument("--normalized-source", help="write declaration-only ObjC")
    args = parser.parse_args(argv)
    metadata = parse_file(args.source)
    payload = json.dumps(metadata, indent=2) + "\n"
    if args.out:
        with open(args.out, "w", encoding="utf-8") as stream:
            stream.write(payload)
    else:
        sys.stdout.write(payload)
    if args.normalized_source:
        with open(args.normalized_source, "w", encoding="utf-8") as stream:
            stream.write(normalized_source(metadata))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
