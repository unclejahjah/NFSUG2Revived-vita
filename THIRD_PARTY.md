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

## nfsu2-web (NFSU2 Revived)

`src/lib/winapi/revived/winsock.c`, `net_lan.c`, `net_proto.h`, `lan_vfd.h`, `racenet.h`, `winsock.h`, the free-roam
mod in `src/lib/winapi/revived/roam/` (`roam_game.c`, `roam_core.c`, `roam_core.h`, `roam_plat.h`, `roam_web.c`,
`carsfx.h`, `names.h`; `roam_host_win.h` after the PC plugin's) and `extras/scripts/NFSU2FreeRoam.ini` come from
[nfsu2-web](https://github.com/th0mk/nfsu2-web)'s runtime, MIT License (Copyright (c) 2026 th0mk; portions
copyright (c) 2026 Talha Müderrisoğlu), with small changes for the Vita marked `NFSU2_VLAN_BACKEND`. The license
text is in `src/lib/winapi/revived/LICENSE.nfsu2-web`.

## Mbed TLS

The Revived link's TLS is Mbed TLS 3.6.2 (Apache License 2.0). It is not part of the repository:
`cmake/revived.cmake` downloads its release archive and checks its SHA-256 (or uses `NFSU_MBEDTLS`).

## Mozilla CA certificates

`third_party/cacert/cacert.pem` is Mozilla's list of CA certificates, as packaged by certifi 2026.10.18, Mozilla
Public License 2.0 (https://mozilla.org/MPL/2.0/).

## Need for Speed Underground 2

Game code, binaries and assets are the property of Electronic Arts Inc. and its licensors and are not part of this
repository. Users supply their own copy of the game; the code generated from its executable (`src/ug2/generated/`)
is not tracked.
