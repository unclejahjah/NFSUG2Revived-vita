"""Need for Speed Underground 2 (SPEED2.EXE 1.2, unpacked): the tables generate.py needs for this executable.

Addresses are this file's (SHA-256 below); codepdbh/nfsu2-recompiled documents the same build.
"""

EXE_NAME = 'SPEED2.EXE'
EXE_SHA256 = 'f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7'
MODULE = 'ug2'

# The blank-named section: the original base relocation blocks (the directory entry is zeroed). Discovery takes its
# pointer candidates from them.
RELOCATION_SECTION = 0x8f5000

SETJMP = None
LONGJMP_JUMPS = {}

# CRT exits: doexit 0x75d4c0 leaves through a function pointer or ExitProcess.
NO_RETURN = [
    0x75d593,   # exit
    0x75d5a4,   # _exit
    0x75bc7e,   # _amsg_exit (through the exit hook)
    0x75bca3,   # _amsg_exit variant (__crtExitProcess)
]

# Calls that write their return address into its stack slot (the translation leaves it unwritten): their native
# callee runs only for that caller.
RETURN_ADDRESS_SITES = [
    0x581475,   # GameLoop's second debug hook call (0x4022C0): free roam (src/ug2/freeroam.cpp), as NFSU2FreeRoam.asi
]

# Guest functions replaced by hand-written native code (native::sub_<address>, src/ug2/native.cpp).
NATIVE_OVERRIDES = [
    0x75bed4,   # _ftol2: st0 to a 64-bit integer, truncating (fistp, corrected toward zero)
    0x75c4d0,   # _chkstk / _alloca_probe: page-probing stack allocation
    0x6f5fc5,   # Cyrix probe (sahf/div/lahf): the translated div keeps the flags, which reads as a Cyrix
    0x75cfd0,   # CRT memcpy: `neg ecx; jmp [ecx*4 + 0x75d268]` reads its table downwards; discovery keeps one entry
    0x760df0,   # CRT memmove: the same code (table 0x761088)
    # Widescreen (src/ug2/widescreen.cpp)
    0x5bf610,   # racing resolution (thiscall; width*, height*): the display size
    0x537480,   # FEng package render (thiscall, package argument): HUD or menu flush
    0x51a5d0,   # FERenderObject draw (thiscall): canvas mapping of the object
    0x5c7ec0,   # projection (thiscall, view): hor+ factors of the player views
    0x5bd4a0,   # draw queue material setup (cdecl; material, packet flags): D3D material or effect vectors
    # Front touch (src/ug2/padbind.cpp)
    0x50b460,   # FE mouse cursor draw (cdecl; x, y): skipped with front_touch=on
    # Forwarding placeholders for the hot-function conversions (src/ug2/native/stubs.cpp, tools/native/placeholders.txt)
    0x5cf420, 0x5bc300, 0x61b670, 0x5bdde0, 0x5c5930, 0x5d6570,
    0x5bc210, 0x4013f0, 0x5bcc30, 0x4905a0, 0x5ceb30, 0x5dd490,
    0x4122c0, 0x5d3330, 0x4198c0, 0x43c2b0, 0x5e72e0, 0x623080,
    0x5764d0, 0x4895e0, 0x441000, 0x5d69b0, 0x5d64d0, 0x5aa480,
    0x5a38e0, 0x440d40, 0x4275e0, 0x401aa0, 0x5d6240, 0x620ae0,
    0x5d6ad0, 0x59bb00, 0x5a8790, 0x5bd140, 0x595040, 0x5114f0,
    0x5dd360, 0x5abdf0,
    # Garage camera orbit with the right stick (src/ug2/native/sub_4a5d90.cpp)
    0x4a5d90,
    # GameLoop's debug hook (empty, ret), called at 0x581470 and 0x581475 between World::DoTimestep and the camera,
    # audio and render: the call at 0x581475 is where the PC's NFSU2FreeRoam.asi runs free roam (src/ug2/freeroam.cpp)
    0x4022c0,
    # Vita Settings in the game's own screens (src/ug2/menu_entry.cpp): the setups of UI_Main, UI_OptionsMain and
    # UI_Options (thiscall; their constructors call them directly) and the game's text of a label hash (edx)
    0x4aee30, 0x4b6230, 0x4b8470, 0x4ff9d0,
    # ... and the race pause menu's options list, UI_PauseOptions (0x4d9418 calls it directly)
    0x4c4bd0,
    # Medium graphics features: flares, shadow hull, headlight pass, particles
    0x490920, 0x5c5580, 0x60cee0, 0x5bdaa0, 0x48bdf0, 0x492e10,
    0x5c0e10, 0x4881d0, 0x5bb430, 0x61e700,
    # Medium helpers: shadow outline buffer, vector/box helpers, flare and car-render callees, post/fog
    0x5be130, 0x5be0f0, 0x487210, 0x487270, 0x43d610, 0x5bc450, 0x5c7470, 0x5dbb00,
    0x5bc4a0, 0x5ba8b0, 0x5be060, 0x5bdfe0, 0x5be180, 0x401650, 0x5aad20, 0x43d0c0,
    # Car physics: drivetrain (src/ug2/native/sub_594300.cpp)
    0x594300, 0x594840, 0x594a30, 0x594da0,
    # Car physics: one wheel's contact and tyre forces (src/ug2/native/sub_5a8280.cpp, sub_59d600.cpp)
    0x5a8280, 0x59d600,
    # Car physics: shape bounds and vector helpers (src/ug2/native/sub_5a19f0.cpp, sub_43d1e0.cpp, sub_43c570.cpp)
    0x5a19f0, 0x43d1e0, 0x43c570,
    # D3DX (include/winapi/d3dx9.h): native effects without the guest effect compiler or shader objects
    0x640aac,   # D3DXCreateEffectPool(ppPool), ret 4
    0x640c02,   # D3DXCreateEffectFromResourceA(device, module, name, defines, include, flags, pool, ppEffect, ppErrors)
    0x63cedb,   # D3DXMatrixMultiply(out, a, b): jmp [0x803e74], the PSGP stub
    0x63d12c,   # D3DXMatrixTranspose(out, m): jmp [0x803f00], the PSGP stub
    # EA SND mixer outside the execution lock (src/ug2/audio_mixer.cpp)
    0x725c8e,   # render loop (&output pointer, frames)
    0x729574,   # voice render (&output pointer, frames)
    0x721e13,   # SND tick
    0x721de6,   # millisecond callbacks
    0x72382f,   # stream notifications
    0x72c360,   # mix kernel (x87)
    0x72c480,   # convert kernel (x87)
    0x733700,   # linear resampler, mono (x87)
    0x734bf4,   # EA-XA decoder (x87)
    0x72cac7,   # output module, float to 16-bit
    0x7216c6,   # SND output rate (device, rate): audio_mix_rate
    # Voice render outside the SND mutex (src/ug2/audio_split.cpp): voice end, and the SND functions other threads wait
    # in while that render runs: voices, streams, aux effects, SND heap frees
    0x7292ac,
    0x728df9, 0x728f75, 0x728fd2, 0x72905d, 0x729087, 0x7290af, 0x72d3cb, 0x72d3ec, 0x72d4ac, 0x72d548, 0x72d8e9,
    0x723138, 0x7231d4, 0x723515, 0x723608, 0x723649, 0x72366a, 0x723699, 0x7237ef, 0x72387b, 0x72c31e,
    0x722442, 0x727a12, 0x7279d6,
    0x72653c, 0x7268bf, 0x726cee, 0x726db3,
    0x724894,
    # Audio bank load and unload: bank-code lookups are cached between them (src/ug2/bankcode.cpp)
    0x7212e5,   # bank loader
    0x7211ab,   # bank unload
    # Hitch log timing (src/ug2/hitch.cpp)
    0x574670,   # decompression dispatcher (cdecl; source, destination)
    0x5ce170,   # texture creation of a texture record (thiscall)
]

# Calls emitted inline with generate.py --inline-ftol2 (codegen/control.py INLINE): address -> helper.
INLINE_HELPERS = {
    0x75bed4: 'ftol2',
}

# Native overrides that never run guest code, wait or set cpu.terminate (generate.py --pure-natives).
PURE_NATIVES = [
    0x75bed4,   # _ftol2
    0x75c4d0,   # _chkstk
    0x6f5fc5,   # Cyrix probe
    0x75cfd0,   # memcpy
    0x760df0,   # memmove
]

# Function entries discovery misses. Audio bank library table 0x817FE0: the data-pointer scan reads these entries as
# 16-bit pairs.
EXTRA_ENTRIES = [0x7201c2, 0x7201fe, 0x72023c, 0x72025f, 0x7202fb, 0x720376, 0x7203bd]

# Widescreen. Slots past .rdata (zero bytes inside its raw size) written by widescreen.cpp:
# 0x7E7FE0 2D x scale, 0x7E7FE4 2D x offset, 0x7E7FE8 controls picture width, 0x7E7FEC / 0x7E7FF0 projection.
PATCHES = [
    # 2D polygon 0x5C5020 and line quads 0x50DCD0: x scale [0x79AC10] (1/320) -> [0x7E7FE0]
    (0x50dd10, '10ac7900', 'e07f7e00'),
    (0x50dd36, '10ac7900', 'e07f7e00'),
    (0x50dd84, '10ac7900', 'e07f7e00'),
    (0x5c507c, '10ac7900', 'e07f7e00'),
    (0x5c50bb, '10ac7900', 'e07f7e00'),
    (0x5c5134, '10ac7900', 'e07f7e00'),
    (0x5c51ad, '10ac7900', 'e07f7e00'),
    # and their x offset [0x784250] (1.0) -> [0x7E7FE4]
    (0x50dd52, '50427800', 'e47f7e00'),
    (0x50dd8c, '50427800', 'e47f7e00'),
    (0x5c5082, '50427800', 'e47f7e00'),
    (0x5c50ce, '50427800', 'e47f7e00'),
    (0x5c5147, '50427800', 'e47f7e00'),
    (0x5c51c0, '50427800', 'e47f7e00'),
    # Cutscene letterbox bars 0x5CBDC0: canvas x 0..640 -> -320..960 (past both screen edges up to 8:3)
    (0x5cbe71, '00000000', '0000a0c3'),
    (0x5cbeb1, '00000000', '0000a0c3'),
    (0x5cbee5, '00000000', '0000a0c3'),
    (0x5cbf1d, '00000000', '0000a0c3'),
    (0x5cbe89, '00002044', '00007044'),
    (0x5cbea1, '00002044', '00007044'),
    (0x5cbef5, '00002044', '00007044'),
    (0x5cbf05, '00002044', '00007044'),
    # Alpha-clear, fade and flash quad 0x5C7140 (through 0x5C5020): canvas x 0..640 -> -320..960
    (0x5c725a, '00000000', '0000a0c3'),
    (0x5c728a, '00000000', '0000a0c3'),
    (0x5c726a, '00002044', '00007044'),
    (0x5c727a, '00002044', '00007044'),
    # Controls picture 0x5C3120: its width follows [0x7E7FE8] (4:3 width) instead of the screen width [0x870980]
    (0x5c315e, '80098700', 'e87f7e00'),
    # Projection 0x5C7EC0: x scale 1/tan -> [0x7E7FEC]/tan, vertical half angle factor 0.5 -> [0x7E7FF0]
    (0x5c7fea, '50427800', 'ec7f7e00'),
    (0x5c7ffc, '60427800', 'f07f7e00'),
]
THREAD_SEGMENTS = []

# Net x87 pushes of functions the depth analysis cannot follow.
FPU_DELTAS = {
    0x75ca0d: 0,    # CRT pow core (_CIpow 0x75c9b0): fnsave 0x75cb1d / frstor 0x75cb29 restore the depth they save
}

# Functions that save the caller's flags on entry (pushfd) and restore them before returning (popfd).
FLAGS_PRESERVED = [
    0x6f5f8e,   # CPUID support probe: pushfd at entry, popfd at 0x6f5fa7 before ret
]

# Entries that no indirect call reaches although a data dword points at them: no base relocation covers the dword.
NOT_INDIRECT = [
    0x409000,   # relocation block header at 0x923548 (page RVA 0x409000)
    0x412000,   # relocation block header at 0x923718 (page RVA 0x412000)
    0x470066,   # 16-bit values at 0x7cfb54
    0x591a37,   # packed values at 0x7f01d0
    0x64252f,   # '/%d' of format strings at 0x7949b9, 0x795302, 0x797d4c; return address of the call at 0x64252a
]

# Targets of indirect jumps whose callers read flags (site -> entries).
JUMP_TARGETS = {
    # CRT x87 class dispatch: jmp [edx + 0x10 + class] through the descriptors 0x81aa7a, 0x81aa9a, 0x81aaba, 0x81a4aa
    # (sinh, cosh, tanh, exp)
    0x7654f5: [0x769910, 0x76558c, 0x7655a2, 0x769984, 0x769962, 0x76559d, 0x769991, 0x769929, 0x7699b1, 0x765291,
               0x7653d1],
    # and through descriptor 0x819eb0 (fmod, two operands)
    0x765581: [0x75ede4, 0x765636, 0x7655cf, 0x765594, 0x7655cd, 0x7655f7],
}

# SIMD code behind feature tests the runtime fails (no MMX, SSE, SSE2 or 3DNow! in cpuid or IsProcessorFeaturePresent)
# or behind flags this executable never sets; these ranges trap if reached. The VP6 MMX routines run and are
# translated (codegen/mmx.py).
SIMD_GATED = [
    (0x68978d, 0x695137, 'D3DX PSGP: filled by 0x670fdf, 0x671152, 0x6713a1 from 0x641713 (IPFP 6/7, cpuid SSE2)'),
    (0x6d395f, 0x6d3f77, 'D3DX PSGP SSE/3DNow! helpers, called only by the PSGP routines'),
    (0x6d4460, 0x6d465e, 'D3DX PSGP 3DNow! helpers, called only by the PSGP routines'),
    (0x6de800, 0x6dec69, 'EA render SSE/3DNow! commands: 0x6da570 picks them by [0x81cb38]/[0x81cb54], never written'),
    (0x6e0990, 0x6e0f8d, 'EA render SSE/3DNow! paths behind [0x81cb38]/[0x81cb54], never written'),
    (0x6ffdb4, 0x700059, 'movie YUV MMX converters, formats 2-7: 0x5bf9b0 passes [0x800580] = 1, never written'),
    (0x715d38, 0x715e0e, 'VP6 SSE2 (0x708216; 0x71d02a: cpuid 1 edx bit 26)'),
    (0x717410, 0x719296, 'VP6 SSE2 (0x708216)'),
    (0x71a831, 0x71ae37, 'VP6 SSE2 (0x708216)'),
    (0x71e9e7, 0x71fb03, 'VP6 SSE2 (0x70d01a)'),
    (0x72c3e0, 0x72c47f, 'audio SSE: 0x728da4, [0x8b460f] bit 1 (0x729d16: cpuid 1 edx bit 25)'),
    (0x7302dd, 0x730c66, 'audio MPEG MMX: 0x730c66 behind [0x8b4a2c] (0x72d951: cpuid 1 edx bit 23)'),
    (0x733460, 0x7339d1, 'audio SSE: 0x72d369, [0x8b460f] bit 1'),
    (0x734aff, 0x734bf4, 'audio SSE: 0x734ce9, [0x8b4a20] bit 1 (0x72ccde: cpuid 1 edx bit 25)'),
    (0x75c9b0, 0x75ca04, 'CRT _CIpow: SSE2 path behind [0x8ba480] (0x7646d1: cpuid 1 edx bit 26)'),
    (0x75eb50, 0x75ee9b, 'CRT libm SSE2 paths behind [0x8ba480]'),
    (0x76469c, 0x765276, 'CRT SSE2 OS probe and pow kernel behind [0x8ba480]'),
    (0x76aa20, 0x76acdd, 'CRT libm SSE2 kernel behind [0x8ba480]'),
]

# Bank routines are translated by bankcode.py (library table 0x817FE0, loader 0x7212E5) and run by
# src/ug2/bankcode.cpp; routines it cannot translate are interpreted there.
BANK_LIBRARY_TABLE = 0x817FE0
BANK_DIR = 'SOUND'
# Library functions the translated routines call in place instead of returning to the runner at each call: the pure
# ones of src/ug2/audio_mixer.cpp (s_pure). Those in BANK_NATIVE_LIBRARY run natively (src/ug2/banklib.cpp).
BANK_DIRECT_LIBRARY = [
    0x71feef, 0x71fefb, 0x71ff03, 0x71ff0b, 0x7201fe, 0x72023c, 0x72025f, 0x7202fb, 0x720340, 0x720376,
    0x7203bd, 0x7203eb, 0x720434, 0x7204f3, 0x7205f3, 0x720684, 0x72069e, 0x7206cc, 0x7206f2, 0x720702,
    0x720728, 0x720738, 0x720767, 0x720784, 0x7207a0, 0x7207aa, 0x7207b4, 0x7207bf, 0x7207d3, 0x720e48,
    0x720f32, 0x720fd1, 0x720ff8, 0x72100b, 0x72101f,
]
BANK_NATIVE_LIBRARY = [
    0x71feef, 0x71fefb, 0x720340, 0x7203bd, 0x7203eb, 0x7204f3, 0x720684, 0x720738, 0x720767, 0x720784, 0x7207bf,
    0x720f32, 0x720fd1, 0x720ff8,
]
