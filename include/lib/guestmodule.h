// Game DLLs translated next to SPEED2.EXE (tools/recompiler/games/srv.py): today one, server.dll, EA's LAN server,
// which the game loads when a player hosts a LAN game (0x5FA300: LoadLibraryA("server"), then StartServer,
// StopServer and IsServerRunning through GetProcAddress).
//
// The code is translated as if the DLL were loaded at a fixed guest base, in a range of the guest address space kept
// for it below the heap (ModuleRangeBegin..ModuleRangeEnd). LoadLibraryA maps the user's own file there the way the
// Windows loader does: the file must be the one the code was translated from (SHA-256), headers and sections are
// copied, its base relocations applied for the new base (the same bytes the translation saw), the import address
// table gets the fake addresses whose shims the module registered at startup, and DllMain runs. The last FreeLibrary
// runs DllMain(DLL_PROCESS_DETACH); the next load maps a fresh image, so .data starts over as on Windows.
//
// Handles of these modules are their base addresses, as on Windows.
#ifndef LIB_GUESTMODULE_H_
#define LIB_GUESTMODULE_H_

#include <lib/winapp.h>
#include <winapi/types.h>

namespace win32
{

// The guest range kept for translated DLLs: above SPEED2.EXE's highest section (0x932000), always committed.
constexpr x86::reg32 ModuleRangeBegin = 0x940000;
constexpr x86::reg32 ModuleRangeEnd = 0x9a0000;

// What tools/recompiler writes for a translated DLL (<module>.cpp); without the file at build time only its name.
struct GuestModule
{
    struct Import
    {
        x86::reg32 iat;             // import address table slot
        x86::reg32 fake;            // the address its shim is registered at
    };
    struct Export
    {
        const char* name;
        x86::reg32 address;
    };

    const char* fileName;           // lower case, e.g. "server.dll"
    const char* sha256;             // of the file the code was translated from; empty: not translated
    x86::reg32 fileSize;
    x86::reg32 base;                // the guest base it was translated at
    x86::reg32 preferredBase;       // the file's own ImageBase
    x86::reg32 sizeOfImage;
    x86::reg32 entryPoint;          // DllMain (the CRT's _DllMainCRTStartup)
    const Import* imports;
    x86::reg32 importCount;
    const Export* exports;
    x86::reg32 exportCount;
    void (*registerMethods)(WinApplication* app);   // the translated functions and the import shims; null: none
};

namespace guestmodule
{

// At startup: the translated functions and import shims of every module.
void registerAll(WinApplication* app);
// LoadLibraryA: a translated DLL by file name (any directory and letter case, ".dll" implied), mapped on its first
// load. 0 when the name is no translated DLL (*known false) or it cannot be loaded (*known true, last error set).
HMODULE load(WinApplication* app, x86::CPU& cpu, const char* name, bool* known);
// Whether LoadLibraryA of the name would find the translated file: translated, and the file in the game folder is
// the one it was translated from (read and checked once).
bool available(const char* name);
// GetModuleHandleA: the handle of a loaded one, else 0.
HMODULE find(const char* name);
// GetProcAddress of a module handle; *known false when the handle is no module of these.
x86::reg32 procAddress(HMODULE handle, const char* name, bool* known);
// FreeLibrary; false when the handle is no module of these.
bool free(WinApplication* app, x86::CPU& cpu, HMODULE handle);
// GetModuleFileNameA: the file's guest path for a module handle, or null.
const char* fileName(HMODULE handle);

}

}

// The translated DLLs (generated code).
namespace srv { extern const win32::GuestModule module; }

#endif
