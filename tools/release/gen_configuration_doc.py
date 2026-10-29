#!/usr/bin/env python3
"""Writes docs/configuration.md from src/ini_public.h, so the page and
the shipped RedemptionEditor.ini cannot drift apart.

Standard library only. From the project root:

    py -3 tools/release/gen_configuration_doc.py          # (re)write the page
    py -3 tools/release/gen_configuration_doc.py --check  # exit 1 when it is stale

The text comes from kShippedIni (its sections, keys, defaults and comments),
the key list from kPublicReadKeys and the action names of [Keys] from
src/editor_keys.h kActions. A public key missing from kShippedIni is an error,
unless it is a support switch (kSupportSwitches): those are read but not
shipped, and the page only says they exist (the list is for support,
docs/release/troubleshooting-keys.md).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INI_HEADER = ROOT / "src" / "ini_public.h"
KEYS_HEADER = ROOT / "src" / "editor_keys.h"
# The private checkout keeps the public pages under public/; the public tree has them in docs/.
OUT = ROOT / ("public/docs" if (ROOT / "public" / "docs").is_dir() else "docs") / "configuration.md"

KEY_LINE = re.compile(r"^(;?)([A-Za-z][A-Za-z0-9]*)=(.*)$")

SECTION_INTRO = {
    "RedemptionEditor": "Recording, clips, the world state, logging and reports. The commented lines at the end are "
    "the editor's own preferences: the editor writes them when you change them in its menus.",
    "Keys": "The keys of every action. The free camera's movement keys (W A S D, Q E, Z C, the arrows, "
    "Page Up and Page Down, Shift and Ctrl) and the editor's own keys (Esc, Enter, Backspace, Space, Tab, Del, "
    "Ctrl+S, Ctrl+Z, Ctrl+Y, Ctrl+E, Ctrl+C, Ctrl+V, F1) are fixed.",
}


def shipped_ini(header: str) -> str:
    match = re.search(r'kShippedIni\[\]\s*=\s*R"INI\((.*?)\)INI"', header, re.S)
    if not match:
        raise SystemExit("kShippedIni not found in src/ini_public.h")
    return match.group(1)


def public_keys(header: str) -> list[tuple[str, str]]:
    match = re.search(r"kPublicReadKeys\[\]\s*=\s*\{(.*?)\n\s*\};", header, re.S)
    if not match:
        raise SystemExit("kPublicReadKeys not found in src/ini_public.h")
    return re.findall(r'\{"(\w+)",\s*"(\w+)"\}', match.group(1))


def support_switches(header: str) -> set[str]:
    match = re.search(r"kSupportSwitches\[\]\s*=\s*\{(.*?)\n\s*\};", header, re.S)
    if not match:
        raise SystemExit("kSupportSwitches not found in src/ini_public.h")
    return set(re.findall(r'\{"(\w+)",\s*"[^"]*"\}', match.group(1)))


def key_actions(header: str) -> dict[str, str]:
    match = re.search(r"kActions\[kActionCount\]\s*=\s*\{(.*?)\n\s*\};", header, re.S)
    if not match:
        raise SystemExit("kActions not found in src/editor_keys.h")
    rows = re.findall(r'\{Action::\w+,\s*"(\w+)",\s*"[^"]*",\s*"([^"]*)"(?:,\s*"\w+")?\}', match.group(1))
    return {name: text for name, text in rows}


class Block:
    """Keys that share one comment."""

    def __init__(self, comment: list[str]):
        self.comment = comment
        self.keys: list[tuple[str, str, bool]] = []  # key, default, commented


def parse(text: str):
    """[(section, intro paragraphs, [Block])], plus the file's own header comment."""
    sections = []
    header: list[str] = []
    section = None
    comment: list[str] = []
    block = None
    for raw in text.splitlines():
        line = raw.rstrip()
        if line.startswith("[") and line.endswith("]"):
            if section is None:
                header = comment
            section = (line[1:-1], [], [])
            sections.append(section)
            comment, block = [], None
            continue
        if not line:
            block = None
            continue
        key = KEY_LINE.match(line)
        if key and section is not None:
            if block is None or comment:
                block = Block(comment)
                section[2].append(block)
                comment = []
            block.keys.append((key.group(2), key.group(3), key.group(1) == ";"))
            continue
        if line.startswith(";"):
            body = line[1:].strip()
            if not body:
                # A lone ";" closes a paragraph that belongs to the section.
                if section is not None and comment:
                    section[1].append(" ".join(comment))
                    comment = []
                continue
            if block is not None and not comment:
                block = None
            comment.append(body)
    return header, sections


def cell(text: str) -> str:
    return text.replace("|", "\\|") if text else ""


def code(text: str) -> str:
    return f"`{text}`" if text else "(empty)"


def render(header_text: str, keys_text: str) -> str:
    ini = shipped_ini(header_text)
    actions = key_actions(keys_text)
    intro, sections = parse(ini)

    seen = set()
    out = [
        "# Configuration",
        "",
        "<!-- Generated by tools/release/gen_configuration_doc.py from src/ini_public.h. Do not edit by hand. -->",
        "",
        "Redemption Editor reads `RedemptionEditor.ini` in the game folder (next to `RDR2.exe`) when the game starts. "
        "When the file is missing, the mod writes it with the text below (the same file as `dist/RedemptionEditor.ini`). "
        "Every value in it is the default: a key you delete reads the same value.",
        "",
    ]
    if intro:
        out += ["> " + " ".join(intro), ""]
    for name, paragraphs, blocks in sections:
        out += [f"## [{name}]", ""]
        if name in SECTION_INTRO:
            out += [SECTION_INTRO[name], ""]
        for paragraph in paragraphs:
            out += [paragraph, ""]
        if name == "Keys":
            for block in blocks:
                if block.comment:
                    out += [" ".join(block.comment), ""]
                out += ["| Key | Default | Action |", "|---|---|---|"]
                for key, value, _ in block.keys:
                    seen.add((name, key))
                    out.append(f"| `{key}` | {code(value)} | {cell(actions.get(key, ''))} |")
                out.append("")
            continue
        # Runs of one-key blocks become one table with the comment as a column.
        run: list[Block] = []

        def flush_run():
            if not run:
                return
            out.extend(["| Key | Default | What it does |", "|---|---|---|"])
            for one in run:
                key, value, _ = one.keys[0]
                out.append(f"| `{key}` | {code(value)} | {cell(' '.join(one.comment))} |")
            out.append("")
            run.clear()

        for block in blocks:
            for key, _, _ in block.keys:
                seen.add((name, key))
            if len(block.keys) == 1:
                run.append(block)
                continue
            flush_run()
            if block.comment:
                out += [" ".join(block.comment), ""]
            commented = all(c for _, _, c in block.keys)
            out += ["| Key | Default |" + (" Note |" if commented else ""), "|---|---|" + ("---|" if commented else "")]
            for key, value, is_commented in block.keys:
                note = " written by the editor |" if commented else ""
                out.append(f"| `{key}` | {code(value)} |{note}")
            out.append("")
        flush_run()

    support = support_switches(header_text)
    missing = []
    for section, key in public_keys(header_text):
        if (section, key) in seen:
            continue
        if section == "RedemptionEditor" and key in support:
            continue
        missing.append(f"[{section}] {key}")
    if missing:
        raise SystemExit("public keys missing from kShippedIni: " + ", ".join(missing))

    out += [
        "## Where the mod writes",
        "",
        "Everything the mod writes goes to one base folder: the game folder when the mod can write there, else "
        "`Documents\\Rockstar Games\\Red Dead Redemption 2\\Redemption Editor`, else `%LOCALAPPDATA%\\RedemptionEditor`. "
        "In the base folder:",
        "",
        "| What | Where |",
        "|---|---|",
        "| Clips and their thumbnails | `ReplayClips\\` |",
        "| Projects | `ReplayProjects\\` |",
        "| Report ZIPs and problem marks | `ReplayReports\\` |",
        "| Developer reports | `ReplayDebug\\` |",
        "| The log and the crash log | `RedemptionEditor.log`, `RedemptionEditor.crash.log` |",
        "| The settings you change in the editor (only when the base is not the game folder) | `RedemptionEditor.ini` |",
        "",
        "Crash dumps always go to `%LOCALAPPDATA%\\RedemptionEditor\\crashes`, and exported videos to "
        "`Documents\\Rockstar Games\\Red Dead Redemption 2\\Videos\\Redemption Editor` (`[Export] Folder` moves them).",
        "",
        "**A game in Program Files.** Windows does not let a game that runs without administrator rights write to "
        "`C:\\Program Files` or `C:\\Program Files (x86)` (the Rockstar Games Launcher's default, and Steam's). At start the "
        "mod writes, reopens and deletes a test file in the game folder and in each data folder above; when one of them is "
        "refused (or Windows quietly redirects the write to its VirtualStore), the base is the Documents folder. You don't "
        "need to change any folder permissions. Clips and projects already in the game folder still show in the gallery "
        "and open; a clip or project you change is saved to the base folder under the same name. The "
        "`RedemptionEditor.ini` of the game folder is still read, and the settings you change in the editor go to the "
        "base folder's `RedemptionEditor.ini`, whose keys win. If no folder takes writes, a notice says where saving "
        "failed and why. The log line `STORAGE_BASE` names the base and the reason.",
        "",
        "## Advanced switches",
        "",
        "Advanced switches exist for support; we'll tell you if you need one. They are not in the file, and every one "
        "is on by default. If an issue or a crash report reply asks you to add one, put the line it gives you under "
        "`[RedemptionEditor]` and restart the game.",
        "",
        "## Developer keys",
        "",
        "The public build reads only the keys above and the advanced switches. A developer build (`/p:RedemptionDev=true`, see "
        "[building.md](building.md)) also reads the research and tuning keys; see "
        "[release/ini-and-dev-build.md](release/ini-and-dev-build.md).",
        "",
    ]
    return "\r\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="exit 1 when the page is stale")
    args = parser.parse_args()
    text = render(INI_HEADER.read_text(encoding="utf-8"), KEYS_HEADER.read_text(encoding="utf-8"))
    current = OUT.read_bytes().decode("utf-8") if OUT.exists() else None
    if args.check:
        if current != text:
            print(f"GEN_CONFIGURATION_DOC,stale={OUT.relative_to(ROOT).as_posix()}")
            return 1
        print("GEN_CONFIGURATION_DOC,ok")
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(text.encode("utf-8"))
    print(f"GEN_CONFIGURATION_DOC,written={OUT.relative_to(ROOT).as_posix()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
