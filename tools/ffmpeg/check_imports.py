"""Checks that a binary loads FFmpeg only through delay-load (EVER's way).

    py -3.13 tools\\ffmpeg\\check_imports.py <RedemptionEditorCore.dll | render_bench.exe> ...

A /DELAYLOAD name that matches no import (linker warning LNK4199, for example
after an FFmpeg update renamed avcodec-62.dll) silently turns the import into
a normal one: the core would then fail to load in the game whenever the DLLs
are not on the search path. This reads the PE import tables (stdlib only) and
fails when an FFmpeg DLL is a normal import or is missing from the delay-load
imports. Prints CHECK_IMPORTS,ok=1 or the offending names.
"""

from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

FFMPEG = re.compile(r"^(avutil|avcodec|avformat|avfilter|avdevice|swscale|swresample|postproc)-\d+\.dll$", re.I)


def rva_to_offset(sections: list[tuple[int, int, int, int]], rva: int) -> int:
    for va, vsize, raw, rawsize in sections:
        if va <= rva < va + max(vsize, rawsize):
            return raw + (rva - va)
    raise ValueError(f"rva {rva:#x} outside the sections")


def cstring(data: bytes, offset: int) -> str:
    end = data.index(b"\0", offset)
    return data[offset:end].decode("ascii", "replace")


def imports(path: Path) -> tuple[list[str], list[str]]:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    sections_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise ValueError("not PE32+")
    directories = optional + 112
    import_rva = struct.unpack_from("<I", data, directories + 8 * 1)[0]
    delay_rva = struct.unpack_from("<I", data, directories + 8 * 13)[0]
    sections = []
    table = optional + optional_size
    for i in range(sections_count):
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, table + 40 * i + 8)
        sections.append((va, vsize, raw, rawsize))
    normal: list[str] = []
    if import_rva:
        offset = rva_to_offset(sections, import_rva)
        while True:
            name_rva = struct.unpack_from("<I", data, offset + 12)[0]
            if not name_rva:
                break
            normal.append(cstring(data, rva_to_offset(sections, name_rva)))
            offset += 20
    delayed: list[str] = []
    if delay_rva:
        offset = rva_to_offset(sections, delay_rva)
        while True:
            name_rva = struct.unpack_from("<I", data, offset + 4)[0]
            if not name_rva:
                break
            delayed.append(cstring(data, rva_to_offset(sections, name_rva)))
            offset += 32
    return normal, delayed


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 2
    bad = 0
    for name in argv:
        normal, delayed = imports(Path(name))
        static = [n for n in normal if FFMPEG.match(n)]
        lazy = [n for n in delayed if FFMPEG.match(n)]
        ok = not static and len(lazy) >= 6
        bad += not ok
        print(f"CHECK_IMPORTS,ok={int(ok)},file={name},static_ffmpeg={';'.join(static) or '-'},delay_ffmpeg={';'.join(lazy) or '-'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
