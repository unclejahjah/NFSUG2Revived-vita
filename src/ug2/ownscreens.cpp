// Port-owned FEng screens: package copies loaded through the chunk loader, and the port screen table behind the
// native lookups 0x50B790 and 0x50B840.

#include "ownscreens.h"

#include <lib/file.h>
#include <lib/memmap.h>
#include <platform/log.h>
#include <recomp.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace ug2 {

void sub_50b790_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_50b840_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_570d70_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5425a0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace ownscreens {

namespace {

using x86::reg32;

const reg32 PackageList = 0x8637b0;     // FEng package records; +4 the first node, a record's node is at +4
const reg32 LoadChunks = 0x577ab0;      // cdecl (data, bytes)
const reg32 OptionsEntry = 0x7f7e00;    // the game's screen table entry of UI_Options.fng, 28 bytes
const reg32 EntrySize = 28;
const reg32 ChunkRaw = 0x30203;         // FRONTB-style chunk holding an unpacked package (GlobalB.lzc's UI_PC_Help_Bar.fng)

struct Copy
{
    std::string source, name;
    Edit edit;
    bool done = false;
};

struct Replacement
{
    std::string name;
    reg32 hash = 0, chunk = 0;      // chunk: ours, built at the first registration
    Replace edit;
    bool done = false, failed = false;
};

std::vector<Copy> s_copies;
std::vector<Replacement> s_replacements;
std::map<reg32, reg32> s_registered;    // a loaded chunk to ours, until the loader unloads it
std::map<std::string, reg32> s_names;
std::map<reg32, reg32> s_entries;       // package hash to our entry
bool s_pending = false;

reg32 guestMemory(size_t bytes)
{
    return (new win32::MemMap(reg32(bytes)))->getBlockStart();
}

reg32 call(win32::WinApplication* app, x86::CPU& cpu, reg32 address, std::initializer_list<reg32> args)
{
    const reg32 saved = cpu.esp;
    reg32 sp = saved;
    for (const reg32* it = args.end(); it != args.begin();)
        rc::st32(sp -= 4, *--it);
    rc::st32(sp -= 4, 0);
    cpu.esp = sp;
    app->dynamic_call(address, cpu);
    cpu.esp = saved;
    return cpu.eax;
}

// Name of a raw package chunk: the game hashes the string at +0x28 of the package.
std::string rawName(reg32 chunk)
{
    std::string name;
    for (reg32 at = chunk + 8 + 0x28, c; (c = rc::ld8(at)) != 0 && name.size() < 80; ++at)
        name += char(c);
    return name;
}

// Finds a record without 0x51BB70's move to the front.
reg32 findRecord(reg32 hash)
{
    const reg32 head = PackageList + 4;
    for (reg32 node = rc::ld32(head), n = 0; node && node != head && n < 4096; node = rc::ld32(node), ++n)
    {
        const reg32 record = node - 4, chunk = rc::ld32(record + 0x10);
        if (!chunk)
            continue;
        const reg32 kind = rc::ld32(chunk);
        if (kind == fepackage::ChunkPackage && rc::ld32(chunk + 8) == hash)
            return record;
        if (kind == ChunkRaw && fepackage::nameHash(rawName(chunk)) == hash)
            return record;
    }
    return 0;
}

bool readChunk(reg32 chunk, fepackage::Package* package)
{
    const reg32 size = rc::ld32(chunk + 4);
    if (rc::ld32(chunk) == ChunkRaw)
    {
        std::vector<uint8_t> bytes(size);
        for (reg32 i = 0; i < size; ++i)
            bytes[i] = rc::ld8(chunk + 8 + i);
        return package->parse(bytes);
    }
    std::vector<uint8_t> packed(size > 4 ? size - 4 : 0), bytes;
    for (reg32 i = 0; i < packed.size(); ++i)
        packed[i] = rc::ld8(chunk + 12 + i);
    return fepackage::unpack(packed.data(), packed.size(), &bytes) && package->parse(bytes);
}

bool readRecord(reg32 record, fepackage::Package* package)
{
    return readChunk(rc::ld32(record + 0x10), package);
}

bool registerCopy(win32::WinApplication* app, x86::CPU& cpu, Copy& copy)
{
    const reg32 record = findRecord(fepackage::nameHash(copy.source));
    if (!record)
        return false;
    copy.done = true;
    fepackage::Package package;
    if (!readRecord(record, &package))
    {
        pf::logError("ownscreens: %s (record %08x) does not unpack or parse; %s not registered", copy.source.c_str(), unsigned(record),
                     copy.name.c_str());
        return true;
    }
    package.rename(copy.name);
    if (copy.edit)
        copy.edit(package);
    const std::vector<uint8_t> data = fepackage::packageChunk(copy.name, package.write());
    const reg32 at = guestMemory(data.size());
    for (size_t i = 0; i < data.size(); ++i)
        rc::st8(at + reg32(i), data[i]);
    call(app, cpu, LoadChunks, { at, reg32(data.size()) });
    const reg32 mine = findRecord(fepackage::nameHash(copy.name));
    pf::logInfo("ownscreens: %s registered as a copy of %s: %u objects, %u bytes, chunk %08x hash %08x, record %08x, "
                "table entry %08x (ours %08x)", copy.name.c_str(), copy.source.c_str(), unsigned(package.objectCount()),
                unsigned(data.size()), unsigned(at), unsigned(fepackage::nameHash(copy.name)), unsigned(mine),
                unsigned(mine ? rc::ld32(mine + 0x20) : 0), unsigned(entryOf(fepackage::nameHash(copy.name))));
    return true;
}

}

bool loadPackage(const std::string& name, fepackage::Package* out)
{
    const reg32 record = findRecord(fepackage::nameHash(name));
    return record && readRecord(record, out);
}

bool loadFromArchive(const std::string& guestPath, const std::string& name, fepackage::Package* out)
{
    FILE* file = fopen(win32::File::hostPath(guestPath.c_str()).c_str(), "rb");
    if (!file)
        return false;
    std::vector<uint8_t> bytes, chunks;
    uint8_t block[65536];
    for (size_t n; (n = fread(block, 1, sizeof(block), file)) > 0;)
        bytes.insert(bytes.end(), block, block + n);
    fclose(file);
    return fepackage::unpack(bytes.data(), bytes.size(), &chunks) && fepackage::findPackage(chunks, name, out);
}

void addReplacement(const std::string& name, const Replace& edit)
{
    Replacement r;
    r.name = name;
    r.hash = fepackage::nameHash(name);
    r.edit = edit;
    s_replacements.push_back(r);
}

reg32 replacementFor(reg32 chunk)
{
    if (s_replacements.empty() || !chunk)
        return 0;
    const reg32 kind = rc::ld32(chunk);
    if (kind != fepackage::ChunkPackage && kind != ChunkRaw)
        return 0;
    const reg32 hash = kind == ChunkRaw ? fepackage::nameHash(rawName(chunk)) : rc::ld32(chunk + 8);
    for (Replacement& r : s_replacements)
    {
        if (hash != r.hash || r.failed)
            continue;
        if (r.chunk)
        {
            s_registered[chunk] = r.chunk;
            pf::logInfo("ownscreens: %s registered again, chunk %08x -> %08x", r.name.c_str(), unsigned(chunk), unsigned(r.chunk));
            return r.chunk;
        }
        r.done = true;
        r.failed = true;
        fepackage::Package package;
        if (!readChunk(chunk, &package))
        {
            pf::logError("ownscreens: %s chunk %08x does not unpack or parse; kept as it is", r.name.c_str(), unsigned(chunk));
            return 0;
        }
        if (!r.edit(package))
            return 0;
        r.failed = false;
        const std::vector<uint8_t> bytes = package.write();
        std::vector<uint8_t> data;
        if (kind == ChunkRaw)
        {
            data.assign(8 + bytes.size(), 0);
            const reg32 header[2] = { ChunkRaw, reg32(bytes.size()) };
            memcpy(&data[0], header, 8);
            memcpy(&data[8], bytes.data(), bytes.size());
        }
        else
            data = fepackage::packageChunk(r.name, bytes);
        const reg32 at = guestMemory(data.size());
        for (size_t i = 0; i < data.size(); ++i)
            rc::st8(at + reg32(i), data[i]);
        r.chunk = at;
        s_registered[chunk] = at;
        pf::logInfo("ownscreens: %s replaced when the loader registered it (%s chunk): %u objects, %u bytes, chunk %08x -> %08x",
                    r.name.c_str(), kind == ChunkRaw ? "raw" : "packed", unsigned(package.objectCount()), unsigned(data.size()), unsigned(chunk),
                    unsigned(at));
        return at;
    }
    return 0;
}

reg32 unloadFor(reg32 chunk)
{
    const auto found = s_registered.find(chunk);
    if (found == s_registered.end())
        return 0;
    const reg32 ours = found->second;
    s_registered.erase(found);
    return ours;
}

ReplacementState replacementState(const std::string& name)
{
    ReplacementState state;
    for (const Replacement& r : s_replacements)
    {
        if (r.name != name || !r.done)
            continue;
        state.done = true;
        if (const reg32 record = findRecord(r.hash))
        {
            state.recordOurs = rc::ld32(record + 0x10) == r.chunk;
            state.instance = rc::ld32(record + 0x18);
        }
    }
    return state;
}

reg32 guestName(const std::string& name)
{
    auto found = s_names.find(name);
    if (found != s_names.end())
        return found->second;
    const reg32 at = guestMemory(name.size() + 1);
    for (size_t i = 0; i <= name.size(); ++i)
        rc::st8(at + reg32(i), uint8_t(name.c_str()[i]));
    s_names[name] = at;
    return at;
}

void addCopy(const std::string& source, const std::string& name, const Edit& edit)
{
    Copy copy;
    copy.source = source;
    copy.name = name;
    copy.edit = edit;
    s_copies.push_back(copy);
    s_pending = true;
}

void addScreen(const std::string& name, reg32 constructor)
{
    const reg32 entry = guestMemory(EntrySize);
    for (reg32 i = 0; i < EntrySize; i += 4)
        rc::st32(entry + i, rc::ld32(OptionsEntry + i));
    rc::st32(entry, guestName(name));
    rc::st32(entry + 4, constructor);
    s_entries[fepackage::nameHash(name)] = entry;
}

bool registered(const std::string& name)
{
    for (const Copy& c : s_copies)
    {
        if (c.name == name)
            return c.done;
    }
    return false;
}

void frame(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_pending)
        return;
    const x86::CPU saved = cpu;
    s_pending = false;
    for (Copy& copy : s_copies)
    {
        if (!copy.done && !registerCopy(app, cpu, copy))
            s_pending = true;
    }
    cpu = saved;
}

reg32 hashOfName(const std::string& name)
{
    return fepackage::nameHash(name);
}

reg32 entryOf(reg32 hash)
{
    auto found = s_entries.find(hash);
    return found == s_entries.end() ? 0 : found->second;
}

}

namespace native {

using x86::reg32;

// Screen entry by package hash (cdecl): the port table first.
void sub_50b840(win32::WinApplication* app, x86::CPU& cpu)
{
    if (const reg32 entry = ownscreens::entryOf(rc::ld32(cpu.esp + 4)))
    {
        cpu.eax = entry;
        cpu.esp += 4;
        return;
    }
    sub_50b840_guest(app, cpu);
}

// Screen constructor by package hash: the port table first.
void sub_50b790(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = ownscreens::entryOf(rc::ld32(cpu.esp + 4));
    if (!entry)
    {
        sub_50b790_guest(app, cpu);
        return;
    }
    const reg32 ret = cpu.esp, data = ret - 0x20;
    const reg32 fields[5] = { rc::ld32(entry), rc::ld32(ret + 8), rc::ld32(entry + 8), rc::ld32(entry + 12), rc::ld32(ret + 12) };
    for (reg32 i = 0; i < 5; ++i)
        rc::st32(data + 4 * i, fields[i]);
    cpu.esp = data;
    rc::st32(cpu.esp -= 4, data);
    rc::st32(cpu.esp -= 4, 0);
    app->dynamic_call(rc::ld32(entry + 4), cpu);
    cpu.esp = ret + 4;
}

// Chunk register (cdecl; chunk): a replaced package chunk goes in instead of the loaded one.
void sub_570d70(win32::WinApplication* app, x86::CPU& cpu)
{
    if (const reg32 replacement = ownscreens::replacementFor(rc::ld32(cpu.esp + 4)))
        rc::st32(cpu.esp + 4, replacement);
    sub_570d70_guest(app, cpu);
}

// Package chunk unload (cdecl; chunk): unloads our registered chunk.
void sub_5425a0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (const reg32 ours = ownscreens::unloadFor(rc::ld32(cpu.esp + 4)))
        rc::st32(cpu.esp + 4, ours);
    sub_5425a0_guest(app, cpu);
}

}

}
