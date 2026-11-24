// Loading the translated game DLLs (include/lib/guestmodule.h). Every call comes from guest code, with the execution
// lock held, so the state here needs no lock of its own; DllMain runs on the loading thread, as under the Windows
// loader lock.
#include <lib/guestmodule.h>
#include <lib/file.h>
#include <winapi/support.h>
#include <platform/log.h>
#include <platform/sha256.h>
#include <recomp.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <strings.h>
#include <vector>

namespace win32
{
namespace guestmodule
{

namespace
{

const x86::reg32 DLL_PROCESS_DETACH = 0;
const x86::reg32 DLL_PROCESS_ATTACH = 1;
const x86::reg32 ERROR_MOD_NOT_FOUND = 126;
const x86::reg32 ERROR_BAD_EXE_FORMAT = 193;
const x86::reg32 ERROR_DLL_INIT_FAILED = 1114;

struct Loaded
{
    const GuestModule* module;
    int refs;                       // LoadLibraryA less FreeLibrary; mapped while above 0
    bool refused;                   // the file is not the translated one (logged once)
    int checked;                    // available(): 0 not asked yet, 1 the file is there, -1 not
    std::string path;               // the guest path it was read from
};

Loaded s_modules[] = {
    { &srv::module, 0, false, 0, std::string() },
};

bool translated(const GuestModule* m)
{
    return m->sha256[0] && m->registerMethods;
}

// "server", "SERVER.DLL", "C:\x\server.dll" -> "server.dll" (".dll" is implied without an extension, a trailing dot
// means none).
std::string normalName(const char* name)
{
    const char* b = name;
    for (const char* p = name; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            b = p + 1;
    std::string n(b);
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    if (!n.empty() && n.back() == '.')
        n.pop_back();
    else if (n.find('.') == std::string::npos)
        n += ".dll";
    return n;
}

Loaded* byName(const char* name)
{
    const std::string n = normalName(name);
    for (Loaded& l : s_modules)
        if (n == l.module->fileName)
            return &l;
    return nullptr;
}

Loaded* byHandle(HMODULE handle)
{
    for (Loaded& l : s_modules)
        if (translated(l.module) && handle == l.module->base && l.refs > 0)
            return &l;
    return nullptr;
}

uint32_t rd32(const std::vector<uint8_t>& b, size_t o)
{
    return o + 4 <= b.size() ? uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16
                               | uint32_t(b[o + 3]) << 24 : 0;
}
uint16_t rd16(const std::vector<uint8_t>& b, size_t o)
{
    return o + 2 <= b.size() ? uint16_t(b[o] | b[o + 1] << 8) : 0;
}

// The file next to SPEED2.EXE (the current directory is the game folder), or the name as the game gave it.
bool readFile(const char* name, const GuestModule* m, std::vector<uint8_t>& data, std::string& path)
{
    const std::string candidates[] = { name, m->fileName };
    for (const std::string& c : candidates)
    {
        File f(c.c_str(), GENERIC_READ, 0, OPEN_EXISTING);
        if (!static_cast<void*>(f))
            continue;
        data.resize(f.size());
        x86::reg32 got = 0;
        if (!data.empty() && (!f.read(data.data(), x86::reg32(data.size()), &got) || got != data.size()))
            continue;
        path = c;
        return true;
    }
    return false;
}

// Headers and sections at the module's base, relocated, the import address table bound. 0 or a Win32 error.
x86::reg32 map(WinApplication* app, Loaded& l, const std::vector<uint8_t>& file)
{
    const GuestModule* m = l.module;
    const uint32_t nt = rd32(file, 0x3c);
    const uint32_t opt = nt + 24;
    const uint16_t sectionCount = rd16(file, nt + 6);
    const uint16_t optSize = rd16(file, nt + 20);
    const uint32_t imageSize = rd32(file, opt + 56);
    const uint32_t headerSize = rd32(file, opt + 60);
    // The hash pins the file: these only guard against a broken translation.
    if (rd32(file, nt) != 0x4550 || rd32(file, opt + 28) != m->preferredBase || imageSize != m->sizeOfImage
        || m->base + rd32(file, opt + 16) != m->entryPoint || m->base < ModuleRangeBegin
        || m->base + imageSize > ModuleRangeEnd)
    {
        pf::logError("%s: its layout is not the translated one", m->fileName);
        return ERROR_BAD_EXE_FORMAT;
    }
    x86::reg8* const image = g_guestBase + m->base;
    memset(image, 0, imageSize);
    memcpy(image, file.data(), std::min<size_t>(headerSize, file.size()));
    for (uint32_t i = 0, sh = opt + optSize; i < sectionCount; i++, sh += 40)
    {
        const uint32_t virtualSize = rd32(file, sh + 8), address = rd32(file, sh + 12);
        const uint32_t rawSize = rd32(file, sh + 16), rawOffset = rd32(file, sh + 20);
        const uint32_t n = std::min(rawSize, virtualSize ? virtualSize : rawSize);
        if (address + n > imageSize || size_t(rawOffset) + n > file.size())
            return ERROR_BAD_EXE_FORMAT;
        memcpy(image + address, file.data() + rawOffset, n);
    }
    // Base relocations (data directory 5): HIGHLOW only, as the translation checked.
    const uint32_t relocations = rd32(file, opt + 96 + 5 * 8), relocationSize = rd32(file, opt + 96 + 5 * 8 + 4);
    const uint32_t delta = m->base - m->preferredBase;
    uint32_t fixups = 0;
    for (uint32_t off = 0; off + 8 <= relocationSize; )
    {
        uint32_t page, blockSize;
        memcpy(&page, image + relocations + off, 4);
        memcpy(&blockSize, image + relocations + off + 4, 4);
        if (blockSize < 8 || off + blockSize > relocationSize)
            break;
        for (uint32_t k = 8; k + 2 <= blockSize; k += 2)
        {
            uint16_t e;
            memcpy(&e, image + relocations + off + k, 2);
            const uint32_t at = page + (e & 0xfff);
            if (e >> 12 == 0)
                continue;
            if (e >> 12 != 3 || at + 4 > imageSize)
                return ERROR_BAD_EXE_FORMAT;
            uint32_t v;
            memcpy(&v, image + at, 4);
            v += delta;
            memcpy(image + at, &v, 4);
            fixups++;
        }
        off += blockSize;
    }
    for (x86::reg32 i = 0; i < m->importCount; i++)
        app->getMemory<x86::reg32>(m->imports[i].iat) = m->imports[i].fake;
    pf::logInfo("%s: mapped at 0x%08x (0x%x bytes, %u relocations, %u imports bound)", m->fileName, m->base,
                imageSize, fixups, m->importCount);
    return 0;
}

// DllMain(base, reason, 0), stdcall, on this thread. Its result.
x86::reg32 callEntry(WinApplication* app, x86::CPU& cpu, const GuestModule* m, x86::reg32 reason)
{
    const x86::reg32 eip = cpu.ip;
    cpu.esp -= 16;
    app->getMemory<x86::reg32>(cpu.esp + 12) = 0;
    app->getMemory<x86::reg32>(cpu.esp + 8) = reason;
    app->getMemory<x86::reg32>(cpu.esp + 4) = m->base;
    app->getMemory<x86::reg32>(cpu.esp + 0) = 0;
    cpu.ip = m->entryPoint;
    app->dynamic_call(m->entryPoint, cpu);
    cpu.ip = eip;
    return cpu.eax;
}

}

void registerAll(WinApplication* app)
{
    for (Loaded& l : s_modules)
        if (translated(l.module))
            l.module->registerMethods(app);
}

HMODULE load(WinApplication* app, x86::CPU& cpu, const char* name, bool* known)
{
    Loaded* l = byName(name);
    *known = l != nullptr;
    if (!l)
        return 0;
    const GuestModule* m = l->module;
    if (l->refs > 0)
    {
        l->refs++;
        return m->base;
    }
    if (!translated(m))
    {
        pf::logInfo("LoadLibraryA: %s was not translated: hosting a LAN game is not available (the build had no "
                    "%s next to SPEED2.EXE)", name, m->fileName);
        setLastError(app, cpu, ERROR_MOD_NOT_FOUND);
        return 0;
    }
    std::vector<uint8_t> file;
    std::string path;
    if (!readFile(name, m, file, path))
    {
        pf::logInfo("LoadLibraryA: %s is not in the game folder: hosting a LAN game is not available (put the "
                    "%s of the PC game, patch 1.2, next to SPEED2.EXE)", name, m->fileName);
        setLastError(app, cpu, ERROR_MOD_NOT_FOUND);
        return 0;
    }
    const std::string hash = pf::sha256Hex(file.data(), file.size());
    if (hash != m->sha256)
    {
        if (!l->refused)
            pf::logError("LoadLibraryA: %s (%u bytes, SHA-256 %s) is not the file this build was translated from "
                         "(%u bytes, SHA-256 %s): it is not loaded, so hosting a LAN game is not available", name,
                         unsigned(file.size()), hash.c_str(), unsigned(m->fileSize), m->sha256);
        l->refused = true;
        setLastError(app, cpu, ERROR_BAD_EXE_FORMAT);
        return 0;
    }
    if (const x86::reg32 error = map(app, *l, file))
    {
        setLastError(app, cpu, error);
        return 0;
    }
    l->path = path;
    l->refs = 1;
    if (!callEntry(app, cpu, m, DLL_PROCESS_ATTACH))
    {
        pf::logError("LoadLibraryA: %s: DllMain failed", m->fileName);
        l->refs = 0;
        setLastError(app, cpu, ERROR_DLL_INIT_FAILED);
        return 0;
    }
    pf::logInfo("LoadLibraryA: %s loaded from %s (from 0x%08x)", m->fileName, path.c_str(), callerAddress(app, cpu));
    return m->base;
}

bool available(const char* name)
{
    Loaded* l = byName(name);
    if (!l || !translated(l->module))
        return false;
    if (!l->checked)
    {
        std::vector<uint8_t> file;
        std::string path;
        const bool there = readFile(name, l->module, file, path);
        l->checked = there && pf::sha256Hex(file.data(), file.size()) == l->module->sha256 ? 1 : -1;
        pf::logInfo("%s: %s", l->module->fileName, l->checked > 0 ? "in the game folder, hosting LAN games works"
                    : there ? "in the game folder, but not the file this build was translated from: no hosting"
                    : "not in the game folder: no hosting of LAN games");
    }
    return l->checked > 0;
}

HMODULE find(const char* name)
{
    const Loaded* l = byName(name);
    return l && l->refs > 0 ? l->module->base : 0;
}

x86::reg32 procAddress(HMODULE handle, const char* name, bool* known)
{
    const Loaded* l = byHandle(handle);
    *known = l != nullptr;
    if (!l)
        return 0;
    for (x86::reg32 i = 0; i < l->module->exportCount; i++)
        if (!strcmp(l->module->exports[i].name, name))
            return l->module->exports[i].address;
    return 0;
}

bool free(WinApplication* app, x86::CPU& cpu, HMODULE handle)
{
    Loaded* l = byHandle(handle);
    if (!l)
        return false;
    if (--l->refs == 0)
    {
        callEntry(app, cpu, l->module, DLL_PROCESS_DETACH);
        pf::logInfo("FreeLibrary: %s unloaded", l->module->fileName);
    }
    return true;
}

const char* fileName(HMODULE handle)
{
    const Loaded* l = byHandle(handle);
    return l ? l->path.c_str() : nullptr;
}

}
}
