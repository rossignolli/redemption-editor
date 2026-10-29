# Third-party notices

Redemption Editor is licensed under the GNU General Public License, version 3 ([LICENSE](LICENSE)). It includes or
links the third-party software below, each under its own licence, and credits the projects whose ideas or data it
uses. A release ZIP carries this file next to the binaries.

## ReShade

The add-on API headers (`external/reshade/include`, a git submodule of https://github.com/crosire/reshade, version
6.6.2) are compiled into `RedemptionEditor.addon64` and `RedemptionEditorCore.dll`. ReShade itself is installed by the
player and is not part of the release. BSD 3-Clause License (`external/reshade/LICENSE.md`):

```
Copyright 2014 Patrick Mours. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
  * Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## MinHook

The detour library, compiled into `RedemptionEditor.asi` from `external/reshade/deps/minhook`. BSD 2-Clause License
(`external/reshade/deps/minhook/LICENSE.txt`):

```
MinHook - The Minimalistic API Hooking Library for x64/x86
Copyright (C) 2009-2017 Tsuda Kageyu.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

================================================================================
Portions of this software are Copyright (c) 2008-2009, Vyacheslav Patkov.
================================================================================
Hacker Disassembler Engine 32 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

-------------------------------------------------------------------------------
Hacker Disassembler Engine 64 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Dear ImGui

The editor's interface is drawn with the Dear ImGui of the running ReShade, through ReShade's function table; the UI
harness (`ReplayEditorUiRuntime.vcxproj`, a test tool, not shipped) compiles Dear ImGui from
`external/reshade/deps/imgui`. MIT License (`external/reshade/deps/imgui/LICENSE.txt`):

```
The MIT License (MIT)

Copyright (c) 2014-2025 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## stb_truetype and stb_rect_pack

`editor/display_font.h` compiles Dear ImGui's copies of stb_truetype 1.26 and stb_rect_pack 1.01
(`imstb_truetype.h`, `imstb_rectpack.h`, by Sean Barrett) into `RedemptionEditorCore.dll` to draw the title font.
They are dual licensed, MIT or public domain (Unlicense); the MIT text, as it appears at the end of those files:

```
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Zstandard (zstd) 1.5.6

Clip compression, compiled into `RedemptionEditor.asi`, `ReplayCoreTests.exe` and `ReplayClipTool.exe` from the
amalgamated `external/zstd/zstd.c` (official release tarball `zstd-1.5.6.tar.gz`, sha256
8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1). zstd is dual licensed, BSD or GPLv2; this project
uses it under the BSD License (`external/zstd/LICENSE`):

```
BSD License

For Zstandard software

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook, nor Meta, nor the names of its contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## LZMA SDK 24.09

The packed clip container (`src/clip_pack.cpp`, the clips the background compressor writes), compiled into
`RedemptionEditor.asi`, `ReplayCoreTests.exe` and `ReplayClipTool.exe` from the C encoder and decoder in
`external/lzma` (Igor Pavlov, https://github.com/ip7z/7zip tag 24.09, folder `C/`, files unchanged; sources and
hashes in `external/lzma/README.txt`). Public domain: each file states "Igor Pavlov : Public domain" in its header,
and the repository's `DOC/License.txt` says: "Some files are 'public domain' files, if 'public domain' status is
stated in source file."

## FFmpeg and its codec libraries

The video export (`editor/render_export/libav_encoder.*`) links the FFmpeg 8 libraries (libavcodec, libavformat,
libavfilter, libavutil, libswscale, libswresample) as DLLs that ship in `RedemptionEditor\dlls\` and are loaded at run
time. They are built with vcpkg from `external/ffmpeg/vcpkg.json` (`tools/ffmpeg/install_ffmpeg.bat`). That build
enables the `gpl` feature with x264 and x265, so **the shipped FFmpeg DLLs are covered by the GNU General Public
License, version 2 or later**. The other libraries of that build (libaom, libvpx, Opus, Vorbis, Ogg, LAME, Intel
libvpl, the AMD AMF and NVIDIA codec headers, zlib) keep their own licences, which vcpkg installs in
`share\<port>\copyright` of the FFmpeg build folder.

Source code: https://ffmpeg.org and the vcpkg ports at the baseline commit pinned in `external/ffmpeg/vcpkg.json`
(https://github.com/microsoft/vcpkg); `tools/ffmpeg/install_ffmpeg.bat` rebuilds the same set from that source.

## Windows Imaging Component

The editor reads its painted art with the Windows Imaging Component (`editor/ui_art.h`), and the screenshots of a
problem report are encoded with it (`src/qa_jpeg.h`). It is part of Windows; nothing of it is distributed.

## Script Hook RDR2 SDK and native names

`RedemptionEditor.asi` is built against the Script Hook RDR2 SDK by Alexander Blade (http://www.dev-c.com) and links
its `ScriptHookRDR2.lib`. The SDK may not be redistributed and is not in this repository; players install the Script
Hook RDR2 runtime themselves. The native declarations come from the community native database
(https://alloc8or.re/rdr3/nativedb/), generated into the SDK's `natives.h` at build time
([docs/building.md](docs/building.md)).

## Redemption (title font)

`editor/fonts/Redemption.otf`, shipped as `RedemptionEditor\fonts\Redemption.otf`: the face of the editor's titles.
Its own name table says: "Copyright (C) Rockstar North. All rights reserved." (manufacturer Rockstar North, vendor
URL http://www.rockstarnorth.com/). It carries no licence field. It is not covered by this project's licence.

## Painted interface art

`editor/ui/*.png`, shipped as `RedemptionEditor\ui\`: the panel edges, the timeline strips and its marks, the menu and
tool icons, the loading cylinder, the reel mark and the ornaments. Made for this project with Google Flow and the
project's own drawing scripts; no third-party licence applies. Without these files the editor draws a flat look.

## Project site

`site/` is the project's web page. `site/assets/SOURCES.txt` lists the source of each image: the card and background
art is Rockstar Games artwork from the official Red Dead Redemption 2 page, (c) Rockstar Games, used for
illustration and not covered by this project's licence; the videos, their posters and the remaining files were made
for the project.

## Simple Camera

The free camera's drone flight, banking, acrobatic quaternion rotation and collision passes, the time-aware spline
path, the camera shake model and presets and the world markers follow Simple Camera, a GTA V free camera and photo
mode mod by crxhvrd (https://github.com/crxhvrd/simplecamera, commit 13c92ef). No file was copied: the logic was
rewritten for RDR2 (`src/free_cam.*`, `src/camera_effects.*`, `src/camera_track.cpp`, `src/camera_overlay.*`). MIT
License:

```
MIT License

Copyright (c) 2026 crxhvrd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## REPlus

The Spline blend's ideas (Hermite tangents from the neighbouring markers with natural ends, Fritsch-Carlson monotone
tangents, the speed trapezoid of the ease, the B-spline camera weight) follow the spline of REPlus by crxhvrd
(https://github.com/crxhvrd/REPlus), licensed under the GNU General Public License, version 3. Its code was read for
the mathematics only; `src/camera_spline.*` is this project's own code.

## rdr3_discoveries

`src/weapon_components.h` lists weapon component names from femga's public RDR2 research collection
(https://github.com/femga/rdr3_discoveries, `weapons/weapon_components.lua`) with their joaat hashes, which the
recording tests on each weapon. Only the names (game identifiers) are used; no code of that repository is included.

## Trademarks

Red Dead Redemption, Rockstar Games and Rockstar North are trademarks of Take-Two Interactive Software, Inc.
Redemption Editor is not affiliated with, endorsed or sponsored by Rockstar Games or Take-Two Interactive.
