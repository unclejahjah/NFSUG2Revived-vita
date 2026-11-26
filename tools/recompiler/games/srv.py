"""EA's LAN server of Need for Speed Underground 2 (server.dll of the 1.2 patch): the tables generate.py needs for it.

SPEED2.EXE loads it when a player hosts a LAN game (0x5FA300: LoadLibraryA("server"), then StartServer,
StopServer and IsServerRunning through GetProcAddress). The port translates it as a second guest module at the fixed
base below (its preferred base 0x10000000 is guest heap): include/lib/guestmodule.h reserves that range, and
src/lib/guestmodule.cpp maps the user's own file there at LoadLibraryA, relocated, as the Windows loader does.

Addresses are those at the guest base: preferred address - 0x10000000 + 0x940000.
"""

EXE_NAME = 'server.dll'
EXE_SHA256 = 'a5f50324adeabedca748449965e5e0f409b0cfce065cd0ffb7975d06d55ba9e4'
MODULE = 'srv'
# A DLL: translated at this base, registered as a library (no Application class, no entry thread).
LIBRARY = True
REBASE = 0x940000

SETJMP = None
LONGJMP_JUMPS = {}
NO_RETURN = []
RETURN_ADDRESS_SITES = []
NATIVE_OVERRIDES = []
EXTRA_ENTRIES = []
PATCHES = []
THREAD_SEGMENTS = []
FPU_DELTAS = {}
FLAGS_PRESERVED = []
NOT_INDIRECT = []
JUMP_TARGETS = {}
SIMD_GATED = []
BANK_LIBRARY_TABLE = None
