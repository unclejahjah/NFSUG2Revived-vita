# Third-party code and licensing

This repository's own code is licensed under the MIT License ([LICENSE](LICENSE)), which covers the port's own
source. The parts below come from other projects and keep their own terms.

## UG1VITA

This project grew out of [UG1VITA](https://github.com/NDRWhun/UG1VITA) by NDRWhun, the PS Vita port of Need for
Speed Underground, and still contains code from it. UG1VITA is MIT licensed; its copyright notice is kept in
[LICENSE](LICENSE) beside this project's own, as that license requires.

## pefile

`tools/recompiler/disasm/pefile.py` and `tools/recompiler/disasm/ordlookup/`, from the pefile project (Ero Carrera,
version 2018.8.8), MIT License. The license text is in `tools/recompiler/disasm/LICENSE.pefile`.

## Capstone

The recompiler disassembles with Capstone (BSD 3-Clause License), version pinned in
`tools/recompiler/requirements.txt`. It is not part of the repository: pip or `tools/recompiler/bootstrap.py`
installs it into `.pydeps/`.

## LLVM compiler-rt

The optional profile-guided build links compiler-rt's profile runtime (Apache License 2.0 with LLVM Exceptions). It
is not part of the repository: `cmake/pgo-runtime.cmake` downloads it for the compiler's version.

## NFSU-XtendedInput

The right stick's car orbit in the garage (the game's own car orbit pad events) and menu navigation kept to the
d-pad and left stick follow the approach of [NFSU-XtendedInput](https://github.com/xan1242/NFSU-XtendedInput) by
xan1242, MIT License, Copyright (c) 2022 Lovro Pleše. The port reimplements the behaviour natively
(`src/ug2/garage_stick.h`, `src/ug2/native/sub_4a5d90.cpp`, `src/ug2/native/sub_571d50.cpp`) and contains none of
its code.

## FFmpeg

The optional native VP6 movie decoder (`movies=vp6`) links libavcodec and libavutil from FFmpeg 7.1.2
(https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.xz), under the GNU Lesser General Public License, version 2.1 or later.
The build uses no GPL or nonfree options: `tools/vita/build_ffmpeg_vp6.py` downloads that release, checks its SHA-256,
applies the port's changes to it and builds only the VP6 decoders as a static library. The changes are the patches in
`tools/vita/ffmpeg-patches/` (also under the LGPL 2.1 or later): the coefficients of a frame parsed on a second thread,
Huffman tables kept while their model is unchanged, and a NEON version of one inverse DCT. The port is open source and
built from these scripts, so anyone can rebuild it with their own copy of FFmpeg: replace the library in
`build-ffmpeg-vp6/install` (or point `NFSU_FFMPEG_DIR` at it) and build again; a copy without these patches lacks
`ff_vp56_set_split`, which `src/ug2/vp6native.cpp` calls. FFmpeg's license texts are in its source release.

## Noto Sans

The start-up screen draws its text with the Vita's own system font when the firmware's font library is available,
and otherwise with Noto Sans Regular (Copyright 2022 The Noto Project Authors), SIL Open Font License
1.1. Its Latin glyphs are rasterised by `tools/fonts/make_atlas.py` into `src/ug2/font_atlas.h`; the license text
is in `src/ug2/fonts/OFL.txt`.

## Mods tab descriptions

The start-up screen's Mods tab quotes each feature's description from its mod's readme, credited to the mod's authors:
EU Car Content's from the NFSU2 Extra Options readme, by the ExOpts Team.

## mINI

`src/ug2/mini/ini.h` is mINI 0.9.20 (https://github.com/metayeti/mINI) by Danijel Durakovic, MIT License; the license
text is in `src/ug2/mini/LICENSE`. It reads and edits the mods' ini files. It is locally modified to add `//` comments
(comment lines, and trailing comments at `;` or the last ` //`, which a changed value keeps); each change is marked
`NFSU2VITA patch`.

## NFSU2 Extra Options

The EU Car Content feature, which lifts the game's regional car lock, is based on the Unlock Regional Cars option of
[NFSU2 Extra Options](https://github.com/ExOptsTeam/NFSU2ExOpts) by the ExOpts Team (nlgxzef and others), GPLv3, and
is switched by that mod's own settings file. The port makes the same change to the game's data that Extra Options
makes, the 0xFF byte in each of the four region list entries (`src/ug2/mods_runtime.cpp`), and contains none of Extra
Options' code.

## Need for Speed Underground 2

Game code, binaries and assets are the property of Electronic Arts Inc. and its licensors and are not part of this
repository. Users supply their own copy of the game; the code generated from its executable (`src/ug2/generated/`)
is not tracked.
