#!/usr/bin/env python3
"""The language files of the Redemption Editor (src/i18n.h).

Standard library only. From the project root:

    py -3 tools/i18n/lang_table.py --write   # (re)write editor/lang/en.lang from the code
    py -3 tools/i18n/lang_table.py --check   # exit 1 on a finding (tools/check_public_build.py runs it)
    py -3 tools/i18n/lang_table.py --missing pt-BR   # the keys a translation lacks
    py -3 tools/i18n/lang_table.py --file x.lang     # check one file, wherever it is

English is the source: every TR("key", "English"), TRF("key", "English", ...)
and TR_NOOP("key", "English") of the mod's sources. en.lang is written from
them (the translators' template; the mod never reads it). The check:
  1. a key has one English text, and en.lang is what the code says;
  2. every other editor/lang/<code>.lang parses, names only keys of en.lang,
     has every key, and keeps each text's {names};
  3. no file has an em or en dash;
  4. no text writes a key by its name: a key the player can move in [Keys]
     (the defaults of src/editor_keys.h kActions: Shift+F1, F4, K, ", .")
     is a literal in no file, and every {key_<name>} is an action's or a
     fixed key's (ActionInfo::text, kFixed), which the mod fills in.
"""

from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LANG_DIR = os.path.join(ROOT, "editor", "lang")
SOURCES = ("script.cpp", "src", "editor", os.path.join("tools", "ui_harness"))  # the fixtures use the editor's keys
SKIP = (os.path.join("editor", "lang"),)
CALL = re.compile(r'\b(TR|TRF|TR_NOOP)\(\s*"')
KEY = re.compile(r"^[A-Za-z0-9_.\-]+$")
DASHES = ("\u2013", "\u2014")
HEADER = """# Redemption Editor: English, the source of every translation.
# Written by tools/i18n/lang_table.py from the code (TR and TRF): do not edit
# this file by hand.
#
# To translate: copy this file to <code>.lang in this folder (pt-BR.lang,
# fr.lang, de.lang: the game's language codes) and translate each text after
# "=". Keep the key before "=" and every {name} in the text: the mod puts a
# value there. A text with spaces at its start or end is in double quotes;
# \\n is a new line. A key left out shows the English. Lines that start with
# "#" are comments. The file is UTF-8.
#
# No text writes a keyboard key by its name: {key_record}, {key_save}... is
# the key the player set in RedemptionEditor.ini [Keys] (or a fixed key), put
# in by the mod. Keep each {key_...} like any other {name}.
"""


def literal(text: str, at: int) -> tuple[str, int]:
    """A C++ string literal (and the ones right after it) from the quote at `at`."""
    out = []
    while True:
        assert text[at] == '"'
        at += 1
        while text[at] != '"':
            c = text[at]
            if c == "\\":
                e = text[at + 1]
                out.append({"n": "\n", "t": "\t", "\\": "\\", '"': '"', "'": "'"}.get(e, "\\" + e))
                at += 2
                continue
            out.append(c)
            at += 1
        at += 1
        m = re.match(r"\s*\"", text[at:])
        if not m:
            return "".join(out), at
        at += m.end() - 1


def strip_comments(text: str) -> str:
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
            continue
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def source_files() -> list[str]:
    files = []
    for entry in SOURCES:
        path = os.path.join(ROOT, entry)
        if os.path.isfile(path):
            files.append(path)
            continue
        for directory, _, names in os.walk(path):
            for name in sorted(names):
                full = os.path.join(directory, name)
                rel = os.path.relpath(full, ROOT)
                if name.endswith((".cpp", ".h", ".inl")) and not any(s in rel for s in SKIP):
                    files.append(full)
    return sorted(files)


def code_texts(findings: list[str]) -> dict[str, str]:
    """key -> English, from the sources."""
    texts: dict[str, str] = {}
    where: dict[str, str] = {}
    for path in source_files():
        with open(path, encoding="utf-8", errors="replace") as handle:
            code = strip_comments(handle.read())
        rel = os.path.relpath(path, ROOT)
        for m in CALL.finditer(code):
            if m.group(1) == "TR" and code[m.start() - 9:m.start()] == "#define ":
                continue
            key, at = literal(code, m.end() - 1)
            if not re.match(r"\s*,\s*\"", code[at:]):
                continue  # TR(key, a variable): a table's entry, read where it is written
            at = code.index('"', at)
            english, _ = literal(code, at)
            line = code.count("\n", 0, m.start()) + 1
            if not KEY.match(key):
                findings.append(f"{rel}:{line}: a bad key {key!r}")
            elif key in texts and texts[key] != english:
                findings.append(f"{rel}:{line}: key {key} has another English at {where[key]}: {texts[key]!r} vs {english!r}")
            else:
                texts.setdefault(key, english)
                where.setdefault(key, f"{rel}:{line}")
    return texts


def quote(value: str) -> str:
    """A text as a .lang line writes it (the parser of src/i18n.h reads it back)."""
    quoted = value != value.strip(" \t") or (len(value) >= 2 and value[0] == '"' and value[-1] == '"') or value == ""
    out = value.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")
    if quoted:
        return '"' + out.replace('"', '\\"') + '"'
    return out


def parse(text: str) -> tuple[dict[str, str], list[str]]:
    """The same rules as replay::i18n::Parse."""
    entries: dict[str, str] = {}
    problems = []
    if text.startswith("\ufeff"):
        text = text[1:]
    for number, raw in enumerate(text.split("\n"), 1):
        line = raw.rstrip("\r")
        s = line.lstrip(" \t")
        if not s or s.startswith("#"):
            continue
        m = re.match(r"([A-Za-z0-9_.\-]+)[ \t]*=(.*)$", s)
        if not m:
            problems.append(f"line {number}: no key or no '='")
            continue
        key, value = m.group(1), m.group(2).strip(" \t")
        quoted = len(value) >= 2 and value[0] == '"' and value[-1] == '"'
        if quoted:
            value = value[1:-1]
        out, i, bad = [], 0, False
        while i < len(value):
            c = value[i]
            if c != "\\":
                bad = bad or (c == '"' and quoted)
                out.append(c)
                i += 1
                continue
            if i + 1 >= len(value):
                bad = True
                break
            e = value[i + 1]
            if e in "nt\\\"":
                out.append({"n": "\n", "t": "\t"}.get(e, e))
            else:
                bad = True
            i += 2
        if bad:
            problems.append(f"line {number}: a bad escape or quote")
            continue
        entries[key] = "".join(out)
    return entries, problems


def placeholders(text: str) -> list[str]:
    text = text.replace("{{", "").replace("}}", "")
    return sorted(re.findall(r"\{([^{}]*)\}", text))


def en_text(texts: dict[str, str]) -> str:
    lines = [HEADER.rstrip("\n")]
    group = None
    for key in sorted(texts):
        head = key.split(".")[0]
        if head != group:
            group = head
            lines.append("")
            lines.append(f"# [{group}]")
        lines.append(f"{key} = {quote(texts[key])}")
    return ("\n".join(lines) + "\n").replace("\n", "\r\n")


KEYS_HEADER = os.path.join(ROOT, "src", "editor_keys.h")


def key_names() -> tuple[set[str], set[str]]:
    """The {key_<name>} names (actions' and fixed keys') and the remappable defaults as a text would write them."""
    with open(KEYS_HEADER, encoding="utf-8") as handle:
        code = handle.read()
    names: set[str] = set()
    literals: set[str] = set()
    labels = {"Comma": ",", "Period": ".", "LeftBracket": "[", "RightBracket": "]", "Backquote": "`"}
    for defaults, text in re.findall(r'\{Action::\w+, "\w+", "([^"]*)", "[^"]*", "(\w+)"\}', code):
        names.add(text)
        for token in filter(None, (t.strip() for t in defaults.split(","))):
            literals.add(token)
            mods, _, key = token.rpartition("+")
            prefix = mods + "+" if mods else ""
            if key in labels:
                literals.add(prefix + labels[key])
            elif re.fullmatch(r"Num\d", key):
                literals.add(prefix + "Num " + key[3:])
    names.update(re.findall(r'\{"(\w+)", fixed::\w+\}', code))
    return names, literals


def literal_keys(text: str, literals: set[str]) -> list[str]:
    """The remappable default keys a text writes by name."""
    found = []
    for literal in sorted(literals, key=len, reverse=True):
        if len(literal) == 1 and not literal.isalnum():
            continue  # "," "." "[" are punctuation: the key bar's pairs are looked for below
        # Not inside a word, a version (H.264), a name (GTA V) or an elision
        # (L'export); next to a CJK letter it still counts ("F4" then a kana).
        word = r"A-Za-z0-9_\u00C0-\u024F\u0370-\u04FF"  # Latin, Greek and Cyrillic letters
        before = r"(?<![" + word + r"+])" if "+" in literal else r"(?<![" + word + r".+\-/'\u2019])"
        pattern = before + re.escape(literal) + r"(?![" + word + r"+\-'\u2019]|\.[" + word + r"])"
        for m in list(re.finditer(pattern, text)):
            if (literal == "V" and text[max(0, m.start() - 4):m.start()] == "GTA ") or text.startswith("F\u5024", m.start()):
                continue  # GTA V; the Japanese f-number
            found.append(literal)
        text = re.sub(pattern, lambda m: " " * len(m.group(0)), text)
    for pair in (", .", "[ ]", "`"):
        if pair in text:
            found.append(pair)
    return found


def check_keys(name: str, content: str, findings: list[str]) -> None:
    """Item 4: no remappable key written by its name, and only known {key_<name>}."""
    names, literals = key_names()
    entries, _ = parse(content)
    for key, value in entries.items():
        for literal in literal_keys(value, literals):
            findings.append(f"{name}: {key} writes the key {literal!r} by its name (use its {{key_<name>}}, filled from [Keys])")
        for placeholder in placeholders(value):
            if placeholder.startswith("key_") and placeholder[4:] not in names:
                findings.append(f"{name}: {key} has {{{placeholder}}}, not a key of src/editor_keys.h (ActionInfo::text, kFixed)")


def check_translation(name: str, content: str, texts: dict[str, str], findings: list[str], notes: list[str]) -> None:
    """One translation: it parses, names only the code's keys, has them all, keeps every {name}, no dash."""
    for dash in DASHES:
        if dash in content:
            findings.append(f"{name}: an em or en dash (use a comma, a colon or parentheses)")
    entries, problems = parse(content)
    for problem in problems:
        findings.append(f"{name}: {problem}")
    for key, value in entries.items():
        if key not in texts:
            findings.append(f"{name}: {key} is not a key of the code")
        elif placeholders(value) != placeholders(texts[key]):
            findings.append(f"{name}: {key} has {placeholders(value)}, the English {placeholders(texts[key])}")
    missing = [key for key in texts if key not in entries]
    if missing:
        findings.append(f"{name}: {len(missing)} keys missing (first: {', '.join(missing[:5])})")
    notes.append(f"{name}: {len(entries)} of {len(texts)} keys")


def check(texts: dict[str, str], findings: list[str], notes: list[str]) -> None:
    en_path = os.path.join(LANG_DIR, "en.lang")
    if not os.path.exists(en_path):
        findings.append("editor/lang/en.lang is missing (run with --write)")
    else:
        with open(en_path, encoding="utf-8", newline="") as handle:
            if handle.read().replace("\r\n", "\n") != en_text(texts).replace("\r\n", "\n"):
                findings.append("editor/lang/en.lang differs from the code's English (run tools/i18n/lang_table.py --write)")
    for name in sorted(os.listdir(LANG_DIR)) if os.path.isdir(LANG_DIR) else []:
        if not name.endswith(".lang"):
            continue
        with open(os.path.join(LANG_DIR, name), encoding="utf-8", errors="replace") as handle:
            content = handle.read()
        if name == "en.lang":
            for dash in DASHES:
                if dash in content:
                    findings.append(f"editor/lang/{name}: an em or en dash (use a comma, a colon or parentheses)")
        check_keys("editor/lang/" + name, content, findings)
        if name != "en.lang":
            check_translation("editor/lang/" + name, content, texts, findings, notes)


def main(argv: list[str]) -> int:
    findings: list[str] = []
    notes: list[str] = []
    texts = code_texts(findings)
    if "--write" in argv:
        os.makedirs(LANG_DIR, exist_ok=True)
        with open(os.path.join(LANG_DIR, "en.lang"), "w", encoding="utf-8", newline="") as handle:
            handle.write(en_text(texts))
        notes.append(f"wrote editor/lang/en.lang ({len(texts)} keys)")
    if "--file" in argv:
        path = argv[argv.index("--file") + 1]
        with open(path, encoding="utf-8", errors="replace") as handle:
            content = handle.read()
            check_translation(os.path.basename(path), content, texts, findings, notes)
            check_keys(os.path.basename(path), content, findings)
        print(f"LANG_FILE_CHECK,keys={len(texts)},findings={len(findings)}")
        for note in notes:
            print("NOTE " + note)
        for finding in findings:
            print("FAIL " + finding)
        return 1 if findings else 0
    if "--missing" in argv:
        code = argv[argv.index("--missing") + 1]
        with open(os.path.join(LANG_DIR, code + ".lang"), encoding="utf-8") as handle:
            entries, _ = parse(handle.read())
        for key in sorted(texts):
            if key not in entries:
                print(f"{key} = {quote(texts[key])}")
        return 0
    check(texts, findings, notes)
    print(f"LANG_TABLE_CHECK,keys={len(texts)},findings={len(findings)}")
    for note in notes:
        print("NOTE " + note)
    for finding in findings:
        print("FAIL " + finding)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
