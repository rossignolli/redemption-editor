#!/usr/bin/env python3
"""Static check of the public build's settings (release 1.0).

docs/release/ini-and-dev-build.md. Standard library only. From the project root:

    py -3.13 tools/check_public_build.py              # check, exit 1 on a finding
    py -3.13 tools/check_public_build.py --write-ini  # also (re)write dist/RedemptionEditor.ini

What it checks:
  1. Every ini read the public build compiles (the ClCompile items of ReplayProbe,
     ReplayEditor and ReplayEditorLoader without the RedemptionDev condition, and every
     header of src/ and editor/), outside `#if defined(REDEMPTION_DEV)` blocks, names a
     key of src/ini_public.h kPublicReadKeys. Dev reads (DevIniInt, DevIniString,
     DevIniIntW, devKey) are not reads of the public build.
  2. kPublicReadKeys has no key the public build never reads.
  3. Every key of kPublicReadKeys is in kShippedIni (an active line, or a commented
     ";Key=" line of the editor preferences) or is a support switch (kSupportSwitches:
     read, never shipped; docs/release/troubleshooting-keys.md), never both. Every
     support switch is a [RedemptionEditor] key of kPublicReadKeys, is on, is listed in
     docs/release/troubleshooting-keys.md (when that private page is there), and no
     developer key is one.
  4. Where a read's default is a plain literal, the shipped value (or the support
     switch's value) is the same.
  5. dist/RedemptionEditor.ini is kShippedIni with CRLF line ends.
  6. The public ASI has no bench (review #19): ReplayProbe compiles src/replay_bench.cpp
     and src/bench_script.cpp only with the RedemptionDev condition, no public line of
     its sources names the bench executor, its fault commands or its inbox, and the
     public entry points of src/replay_bench.h are inline (the inert ones).
  7. The language files (tools/i18n/lang_table.py --check): editor/lang/en.lang
     is the code's English, every translation parses and keeps its {names}.
A read whose key is not a literal must be one of the known table-driven reads
(DYNAMIC below); any other is reported.
"""

import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECTS = ("ReplayProbe.vcxproj", "ReplayEditor.vcxproj", "ReplayEditorLoader.vcxproj")
HEADER_DIRS = ("src", "editor")
DIST_INI = os.path.join("dist", "RedemptionEditor.ini")
SUPPORT_DOC = os.path.join("docs", "release", "troubleshooting-keys.md")
# Files that implement the readers themselves (their GetPrivateProfile calls take
# the caller's key).
READER_FILES = {os.path.normpath("src/mod_identity.h")}

# Table-driven reads: (file, regex on the code line, section, where the keys come from).
DYNAMIC = (
    ("script.cpp", r"IniString\(info\.iniKey", "RedemptionEditor", "kFreeCamFields"),
    ("script.cpp", r"IniString\(sp::kIniKeys\[index\]", "RedemptionEditor", "camera_spline.h kIniKeys"),
    ("script.cpp", r"IniInt\(name, fallback", "RedemptionEditor", "key() lambdas (their literal calls are checked)"),
    ("script.cpp", r"IniSection\(section\.data\(\)", None, "IniSectionText: the section logged as read (CONFIG_INI)"),
    ("src/editor_keys_ini.h", r"ProfileStringA\(kIniSection, key", "Keys", "editor_keys.h kActions"),
)

READ_LITERAL = re.compile(r'\bIni(Int|IntW|String|StringW)\(\s*L?"(\w+)"\s*,\s*([^,]+?)\s*,')
READ_ANY = re.compile(r'\b(Ini(?:Int|IntW|String|StringW|Section)|(?:GetPrivate)?Profile(?:Int|String|Section)[AW]?)\(')
LAMBDA = re.compile(r'(?<![\w.])key\(\s*"(\w+)"\s*,\s*([^)]+)\)')
# GetPrivateProfile* and the layered mod_identity.h Profile* (the player's settings over the game folder's).
GPP = re.compile(r'\b(?:GetPrivate)?Profile(?:Int|String)[AW]?\(\s*L?"(\w+)"\s*,\s*L?"(\w+)"\s*,\s*(L?"[^"]*"|[^,]+?)\s*,')


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8", errors="replace") as handle:
        return handle.read()


def strip_comments(text):
    """Comments blanked (newlines kept), strings and chars kept."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
            continue
        if c == 'R' and text.startswith('R"', i):
            m = re.match(r'R"([^(]*)\(', text[i:])
            if m:
                end = text.find(')' + m.group(1) + '"', i)
                end = n if end < 0 else end + len(m.group(1)) + 2
                out.append(text[i:end])
                i = end
                continue
        if c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def public_lines(text):
    """(line number, line) of the code compiled without REDEMPTION_DEV."""
    stack = []  # per #if: 'dev', 'pub' or 'other'; the branch state flips at #else
    lines = []
    for number, line in enumerate(text.split('\n'), 1):
        s = line.strip()
        if s.startswith('#'):
            d = re.sub(r'^#\s*', '#', s)
            if re.match(r'#(if\s+defined\s*\(\s*REDEMPTION_DEV\s*\)|ifdef\s+REDEMPTION_DEV(?!\w))', d):
                stack.append('dev')
            elif re.match(r'#(if\s+!\s*defined\s*\(\s*REDEMPTION_DEV\s*\)|ifndef\s+REDEMPTION_DEV(?!\w))', d):
                stack.append('pub')
            elif re.match(r'#if', d):
                stack.append('other')
            elif re.match(r'#el', d) and stack:
                stack[-1] = {'dev': 'pub', 'pub': 'dev'}.get(stack[-1], 'other')
            elif re.match(r'#endif', d) and stack:
                stack.pop()
            continue
        if 'dev' in stack:
            continue
        lines.append((number, line))
    return lines


def public_sources():
    files = set()
    for project in PROJECTS:
        tree = ET.parse(os.path.join(ROOT, project))
        for item in tree.iter():
            if not item.tag.endswith('ClCompile') or 'Include' not in item.attrib:
                continue
            path = item.attrib['Include'].replace('\\', '/')
            if path.startswith('external/') or not path.endswith('.cpp'):
                continue
            if 'RedemptionDev' in item.attrib.get('Condition', ''):
                continue
            files.add(os.path.normpath(path))
    for folder in HEADER_DIRS:
        for directory, _, names in os.walk(os.path.join(ROOT, folder)):
            for name in names:
                if name.endswith('.h') or name.endswith('.inl'):
                    rel = os.path.relpath(os.path.join(directory, name), ROOT)
                    if 'ui_harness' not in rel:
                        files.add(os.path.normpath(rel))
    return sorted(files)


# 6. The bench: the executor, its scenario parser, its hooks and fault commands, its inbox.
BENCH_SOURCES = (os.path.normpath('src/replay_bench.cpp'), os.path.normpath('src/bench_script.cpp'))
BENCH_SYMBOLS = re.compile(r'\b(BenchHooks|g_benchHooks|g_benchThrowNextTick|g_benchHoldNormalExit|BenchCameraOp|'
                           r'REPLAY_BENCH_DIR|kInboxDir|BENCH_READY|ParseScript)\b|replay::bench::Attach\b|\\\\inbox')
BENCH_ENTRY = re.compile(r'\b(Attach|Update|ScrubOverride|OwnCamera)\s*\(')


def bench_findings():
    findings = []
    asi = []
    tree = ET.parse(os.path.join(ROOT, 'ReplayProbe.vcxproj'))
    for item in tree.iter():
        if not item.tag.endswith('ClCompile') or 'Include' not in item.attrib:
            continue
        path = os.path.normpath(item.attrib['Include'].replace('\\', '/'))
        dev = 'RedemptionDev' in item.attrib.get('Condition', '')
        if path in BENCH_SOURCES and not dev:
            findings.append(f'ReplayProbe.vcxproj: {path} is compiled by the public build (the bench is developer only)')
        if not dev and path.endswith('.cpp') and not path.startswith('external'):
            asi.append(path)
    for rel in sorted(asi):
        if rel in BENCH_SOURCES:
            continue
        for number, line in public_lines(strip_comments(read(rel))):
            m = BENCH_SYMBOLS.search(line)
            if m:
                findings.append(f'{rel}:{number}: the public build references the bench ({m.group(0)}): {line.strip()[:100]}')
    header = os.path.normpath('src/replay_bench.h')
    for number, line in public_lines(strip_comments(read(header))):
        if BENCH_ENTRY.search(line) and not line.strip().startswith('inline '):
            findings.append(f'{header}:{number}: a bench entry point of the public build that is not the inline inert one: {line.strip()[:100]}')
    return findings


def lang_findings():
    """7. tools/i18n/lang_table.py's check, as findings of this one."""
    sys.path.insert(0, os.path.join(ROOT, "tools", "i18n"))
    import lang_table
    found, notes = [], []
    lang_table.check(lang_table.code_texts(found), found, notes)
    return ["lang: " + f for f in found]


def shipped():
    header = read('src/ini_public.h')
    m = re.search(r'kShippedIni\[\] = R"INI\((.*?)\)INI";', header, re.S)
    text = m.group(1)
    section = ''
    lines = {}
    for raw in text.split('\n'):
        line = raw.rstrip()
        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1]
            continue
        km = re.match(r'(;?)(\w+)=(.*)$', line)
        if km:
            lines[(section, km.group(2))] = (km.group(3), km.group(1) == ';')
    block = re.search(r'kPublicReadKeys\[\] = \{(.*?)\n\s*\};', header, re.S).group(1)
    reads = re.findall(r'\{"(\w+)", "(\w+)"\}', block)
    block = re.search(r'kSupportSwitches\[\] = \{(.*?)\n\s*\};', header, re.S).group(1)
    support = dict(re.findall(r'\{"(\w+)", "([^"]*)"\}', block))
    return text, lines, set(reads), support


def table_keys():
    """The keys of the table-driven reads."""
    script = read('script.cpp')
    block = script[script.index('kFreeCamFields['):]
    block = block[:block.index('};')]
    freecam = [k for k in re.findall(r'\{"(\w*)",', block) if k]
    spline = re.findall(r'"(\w+)"', re.search(r'kIniKeys\[\] = \{(.*?)\};', read('src/camera_spline.h'), re.S).group(1))
    actions = re.findall(r'\{Action::\w+, "(\w+)"', read('src/editor_keys.h'))
    return [('RedemptionEditor', k) for k in freecam + spline] + [('Keys', k) for k in actions]


def documented(lines, support, section, key):
    """(value, commented) of a key: its shipped line, else its support switch."""
    if (section, key) in lines:
        return lines[(section, key)]
    if section == 'RedemptionEditor' and key in support:
        return (support[key], True)
    return None


def support_findings(lines, whitelist, support):
    """3. The support switches: read, on, never shipped, never a developer key, listed for support."""
    findings = []
    for section, key in sorted(lines):
        if section == 'Troubleshooting':
            findings.append(f'[Troubleshooting] {key}: the shipped ini has a [Troubleshooting] section (the switches stay out of it)')
        elif section == 'RedemptionEditor' and key in support:
            findings.append(f'[RedemptionEditor] {key}: a support switch in kShippedIni (support switches are read, not shipped)')
    for key, value in sorted(support.items()):
        if ('RedemptionEditor', key) not in whitelist:
            findings.append(f'[RedemptionEditor] {key}: in kSupportSwitches but not in kPublicReadKeys')
        if not value or value in ('0', 'off'):
            findings.append(f'[RedemptionEditor] {key}: support switch is not on by default ("{value}")')
    dev_doc = os.path.join(ROOT, 'docs', 'release', 'ini-and-dev-build.md')
    if os.path.exists(dev_doc):
        dev_keys = set(re.findall(r'^\| `(\w+)` \| \w+ \| Dev', read(os.path.join('docs', 'release', 'ini-and-dev-build.md')), re.M))
        for key in sorted(dev_keys & (set(support) | {k for s, k in whitelist})):
            findings.append(f'{key}: a developer key (ini-and-dev-build.md section 6) is public')
    if os.path.exists(os.path.join(ROOT, SUPPORT_DOC)):
        listed = set(re.findall(r'^\| `(\w+)` \|', read(SUPPORT_DOC), re.M))
        for key in sorted(set(support) - listed):
            findings.append(f'{SUPPORT_DOC}: support switch {key} is not listed')
        for key in sorted(listed - set(support)):
            findings.append(f'{SUPPORT_DOC}: lists {key}, which is not a support switch')
    return findings


def literal(value):
    v = value.strip()
    if v.startswith('L"') or v.startswith('"'):
        return v.lstrip('L').strip('"')
    if v in ('true', 'false'):
        return '1' if v == 'true' else '0'
    if re.fullmatch(r'-?\d+', v):
        return v
    return None


def main():
    findings = []
    notes = []
    text, lines, whitelist, support = shipped()
    reads = {}  # (section, key) -> [(file, line, default)]
    for rel in public_sources():
        if rel in READER_FILES:
            continue
        code = strip_comments(read(rel))
        for number, line in public_lines(code):
            literal_hits = 0
            for m in READ_LITERAL.finditer(line):
                reads.setdefault(('RedemptionEditor', m.group(2)), []).append((rel, number, m.group(3)))
                literal_hits += 1
            for m in GPP.finditer(line):
                reads.setdefault((m.group(1), m.group(2)), []).append((rel, number, m.group(3)))
                literal_hits += 1
            if rel == 'script.cpp':
                for m in LAMBDA.finditer(line):
                    reads.setdefault(('RedemptionEditor', m.group(1)), []).append((rel, number, m.group(2)))
            calls = [m for m in READ_ANY.finditer(line) if not line[max(0, m.start() - 3):m.start()].endswith('Dev')]
            if len(calls) > literal_hits:
                if any(r == rel.replace('\\', '/') and re.search(p, line) for r, p, _, _ in DYNAMIC):
                    continue
                findings.append(f'{rel}:{number}: a read this check cannot name: {line.strip()[:120]}')
    for section, key in table_keys():
        reads.setdefault((section, key), []).append(('(table)', 0, ''))

    for (section, key), sites in sorted(reads.items()):
        where = ', '.join(f'{f}:{n}' for f, n, _ in sites[:3])
        if (section, key) not in whitelist:
            findings.append(f'[{section}] {key}: read by the public build ({where}) but not in kPublicReadKeys')
            continue
        doc = documented(lines, support, section, key)
        if doc is None:
            findings.append(f'[{section}] {key}: in kPublicReadKeys but neither in kShippedIni nor in kSupportSwitches')
            continue
        for f, n, default in sites:
            want = literal(default) if default else None
            if want is None:
                if default:
                    notes.append(f'[{section}] {key}: default "{default.strip()}" at {f}:{n} is not a literal; shipped "{doc[0]}" checked by hand')
                continue
            got = doc[0]
            same = got == want or (re.fullmatch(r'-?\d+', want or '') and re.fullmatch(r'-?\d+', got or '') and int(got) == int(want))
            if not same:
                findings.append(f'[{section}] {key}: shipped "{got}" but the code default at {f}:{n} is "{want}"')
    for section, key in sorted(whitelist):
        if (section, key) not in reads:
            findings.append(f'[{section}] {key}: in kPublicReadKeys but the public build never reads it')
    findings.extend(support_findings(lines, whitelist, support))
    findings.extend(bench_findings())
    findings.extend(lang_findings())

    expected = text.replace('\r\n', '\n').replace('\n', '\r\n')
    dist = os.path.join(ROOT, DIST_INI)
    if '--write-ini' in sys.argv:
        os.makedirs(os.path.dirname(dist), exist_ok=True)
        with open(dist, 'w', encoding='ascii', newline='') as handle:
            handle.write(expected)
        notes.append(f'wrote {DIST_INI}')
    if not os.path.exists(dist):
        findings.append(f'{DIST_INI} is missing (run with --write-ini)')
    else:
        with open(dist, encoding='ascii', newline='') as handle:
            if handle.read() != expected:
                findings.append(f'{DIST_INI} differs from src/ini_public.h kShippedIni (run with --write-ini)')

    print(f'PUBLIC_BUILD_CHECK,reads={len(reads)},whitelist={len(whitelist)},shipped_keys={len(lines)},'
          f'support_switches={len(support)},findings={len(findings)},notes={len(notes)}')
    for note in notes:
        print('NOTE ' + note)
    for finding in findings:
        print('FAIL ' + finding)
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
