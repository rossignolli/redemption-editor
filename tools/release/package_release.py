#!/usr/bin/env python3
"""Builds the user package of the Redemption Editor from the public binaries.

Windows Python, standard library only. From the repository root, after building
RedemptionEditor.sln (or ReplayProbe, ReplayEditorLoader and ReplayEditor) in
Release x64:

    py -3 tools\\release\\package_release.py [--bin DIR] [--asi-dir DIR] [--editor-dir DIR] [--ffmpeg-root DIR] [--out DIR]

--bin is the output folder of the build (default bin\\Release); --asi-dir and
--editor-dir override it for the ASI and for the add-on and core; --ffmpeg-root is the
FFmpeg build the core was linked with (the FfmpegRoot property; default
external\\ffmpeg\\installed\\x64-windows-release), for its licences. Writes
<out>\\RedemptionEditor-<VERSION>.zip (default out: build\\package) with, at its
root, what goes next to RDR2.exe:

    RedemptionEditor.asi       the Script Hook RDR2 script
    RedemptionEditor.addon64   the ReShade add-on (a loader)
    RedemptionEditorCore.dll   the editor the add-on loads
    RedemptionEditor\\dlls\\    the FFmpeg libraries of the video export
    RedemptionEditor\\fonts\\   the editor's title font
    RedemptionEditor\\ui\\      the editor's painted art
    RedemptionEditor\\lang\\    the languages (en.lang, the translators' template, and each translation)
    RedemptionEditor.ini       the default settings (dist\\RedemptionEditor.ini)
    README.md, LICENSE, THIRD_PARTY_NOTICES.md
    THIRD_PARTY_LICENSES\\      the licences of the FFmpeg build (a GPL build)
    SHA256SUMS.txt

It refuses an ASI that does not carry this checkout's VERSION, a developer
build (flavor=dev) and a core that imports FFmpeg without delay-load. It only
reads the build folders and writes the zip.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASI = "RedemptionEditor.asi"
ADDON = "RedemptionEditor.addon64"
CORE = "RedemptionEditorCore.dll"
INI = "RedemptionEditor.ini"
DOCS = ("README.md", "LICENSE", "THIRD_PARTY_NOTICES.md")
FFMPEG_ROOT = ROOT / "external" / "ffmpeg" / "installed" / "x64-windows-release"
# vcpkg helper and build-tool ports: none of their code ships.
FFMPEG_TOOL_PORTS = ("vcpkg-", "pkgconf", "ffmpeg-bin2c")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fail(code: str, **fields: object) -> int:
    extra = "".join(f",{key}={value}" for key, value in fields.items())
    print(f"PACKAGE_RELEASE,error={code}{extra}")
    return 2


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bin", type=Path, default=ROOT / "bin" / "Release")
    parser.add_argument("--asi-dir", type=Path)
    parser.add_argument("--editor-dir", type=Path)
    parser.add_argument("--ffmpeg-root", type=Path, default=FFMPEG_ROOT)
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "package")
    args = parser.parse_args(argv)
    asi_dir = args.asi_dir or args.bin
    editor_dir = args.editor_dir or args.bin
    ffmpeg_share = args.ffmpeg_root / "share"

    version = (ROOT / "VERSION").read_text(encoding="ascii").strip()
    asi = asi_dir / ASI
    if not asi.is_file():
        return fail("asi_missing", path=asi)
    asi_bytes = asi.read_bytes()
    if version.encode("ascii") + b"\x00" not in asi_bytes:
        return fail("asi_version_mismatch", version=version, path=asi)
    if b"flavor=dev\x00" in asi_bytes or b"flavor=public\x00" not in asi_bytes:
        return fail("asi_not_public_build", path=asi, hint="build without /p:RedemptionDev=true")
    for name in (ADDON, CORE):
        if not (editor_dir / name).is_file():
            return fail("editor_missing", path=editor_dir / name)

    sys.path.insert(0, str(ROOT / "tools" / "ffmpeg"))
    import check_imports  # noqa: E402 - tools/ffmpeg/check_imports.py

    normal, _ = check_imports.imports(editor_dir / CORE)
    if any(check_imports.FFMPEG.match(name) for name in normal):
        return fail("ffmpeg_static_import", core=editor_dir / CORE)

    bundle = editor_dir / "RedemptionEditor"
    dlls = sorted(d for d in (bundle / "dlls").glob("*.dll") if not d.name.lower().startswith("pkgconf"))
    if not dlls:
        return fail("ffmpeg_dlls_missing", path=bundle / "dlls", hint="build ReplayEditor (its post-build copies them)")
    fonts = sorted(p.name for p in (ROOT / "editor" / "fonts").glob("*.otf"))
    art = sorted(p.name for p in (ROOT / "editor" / "ui").glob("*.png"))
    langs = sorted(p.name for p in (ROOT / "editor" / "lang").glob("*.lang"))
    missing = [f"fonts/{n}" for n in fonts if not (bundle / "fonts" / n).is_file()]
    missing += [f"ui/{n}" for n in art if not (bundle / "ui" / n).is_file()]
    missing += [f"lang/{n}" for n in langs if not (bundle / "lang" / n).is_file()]
    if missing:
        return fail("bundle_incomplete", path=bundle, files=";".join(missing), hint="build ReplayEditor")
    if not ffmpeg_share.is_dir():
        return fail("ffmpeg_licenses_missing", path=ffmpeg_share, hint="tools\\ffmpeg\\install_ffmpeg.bat")
    # In the private checkout README.md is the developer's page; the player's is
    # public/README.md (the public tree has it at its root).
    readme = ROOT / "public" / "README.md"
    if not readme.is_file():
        readme = ROOT / "README.md"
    for name in DOCS + (INI,):
        source = readme if name == "README.md" else ROOT / ("dist" if name == INI else "") / name
        if not source.is_file():
            return fail("file_missing", path=source)

    files: dict[str, Path] = {ASI: asi, ADDON: editor_dir / ADDON, CORE: editor_dir / CORE}
    for dll in dlls:
        files[f"RedemptionEditor/dlls/{dll.name}"] = dll
    for name in fonts:
        files[f"RedemptionEditor/fonts/{name}"] = bundle / "fonts" / name
    for name in art:
        files[f"RedemptionEditor/ui/{name}"] = bundle / "ui" / name
    for name in langs:
        files[f"RedemptionEditor/lang/{name}"] = bundle / "lang" / name
    files[INI] = ROOT / "dist" / INI
    for name in DOCS:
        files[name] = readme if name == "README.md" else ROOT / name
    for port in sorted(ffmpeg_share.iterdir()):
        if (port / "copyright").is_file() and not port.name.startswith(FFMPEG_TOOL_PORTS):
            files[f"THIRD_PARTY_LICENSES/{port.name}.txt"] = port / "copyright"

    sums = "".join(f"{sha256(path)}  {name}\r\n" for name, path in sorted(files.items()))
    args.out.mkdir(parents=True, exist_ok=True)
    target = args.out / f"RedemptionEditor-{version}.zip"
    temporary = target.with_suffix(".zip.tmp")
    with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, path in files.items():
            archive.write(path, name)
        archive.writestr("SHA256SUMS.txt", sums)
    temporary.replace(target)
    print(f"PACKAGE_RELEASE,ok=1,version={version},files={len(files) + 1},zip={target},sha256={sha256(target)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
