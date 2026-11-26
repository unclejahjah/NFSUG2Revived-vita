#include <lib/heap.h>
#include <lib/memmap.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace win32 { namespace heap
{

namespace
{

const x86::reg32 Granularity = 16;
// reserve(): requests up to this much smaller than the reserved size are lent the block too (a CRT's own header).
const x86::reg32 ReserveSlack = 64;
const x86::reg32 ChunkSize = 1024 * 1024;
// Blocks up to this size come from size classes; larger ones are guest memory blocks of their own.
const x86::reg32 LargeSize = 256 * 1024;
const x86::reg32 KeepSize = 32 * 1024 * 1024;

struct Block
{
    x86::reg32  size;       // requested
    int         sizeClass;  // -1 for a large block
};

struct State
{
    pf::LwMutex                                 mutex{"heap", true};    // recursive for the report
    std::vector<x86::reg32>                     classSizes;     // ascending
    std::vector<std::vector<x86::reg32>>        freeLists;      // per class
    std::unordered_map<x86::reg32, Block>       blocks;         // live blocks by address
    std::unordered_map<x86::reg32, MemMap*>     large;
    // reserve(): one large block kept for a request of reservedSize (less up to ReserveSlack) (the hosting server's state object), so
    // that request still fits when guest memory is fragmented later. Lent out and given back, never released.
    MemMap*                                     reserved = nullptr;
    x86::reg32                                  reservedSize = 0;
    bool                                        reservedLent = false;
    // A released block of KeepSize or more (the track streamer's pool, 36 to 60 MB) is kept for the next request of
    // KeepSize or more that fits in it: the next track load gets the same range back. Between the two the guest heap
    // is fragmented by what the menus and a hosted LAN server (17 MB) hold, and the 60 MB pool of a free-roam LAN
    // race no longer fitted anywhere else (2026-11-26). Freed when another request finds no room.
    MemMap*                                     kept = nullptr;
    bool                                        keptLent = false;
    std::vector<MemMap*>                        chunks;
    x86::reg32                                  chunkNext = 0;  // next free byte of the last chunk
    x86::reg32                                  chunkEnd = 0;
    x86::reg32                                  used = 0;
    x86::reg32                                  smallChunks = 0;

    State()
    {
        // 16-byte steps to 1 KB, then about 1/8 growth, 16-byte aligned, up to LargeSize.
        for (x86::reg32 size = Granularity; size <= 1024; size += Granularity)
            classSizes.push_back(size);
        for (x86::reg32 size = 1024; size < LargeSize; )
        {
            size = (size + size / 8 + Granularity - 1) & ~(Granularity - 1);
            classSizes.push_back(size < LargeSize ? size : LargeSize);
        }
        freeLists.resize(classSizes.size());
        MemMap::addReport(&report);
    }

    static void report();

    int classOf(x86::reg32 size) const
    {
        size_t low = 0, high = classSizes.size();
        while (low < high)
        {
            const size_t mid = (low + high) / 2;
            if (classSizes[mid] < size)
                low = mid + 1;
            else
                high = mid;
        }
        return int(low);
    }

    // Carves a block of a class out of the current chunk; a new chunk when it does not fit, smaller than ChunkSize
    // when no free range of ChunkSize is left.
    x86::reg32 carve(x86::reg32 size)
    {
        if (chunkEnd - chunkNext < size)
        {
            MemMap* chunk = MemMap::tryCreate(ChunkSize, MemMap::Low, pf::perf::AllocHeapChunk);
            if (!chunk)
            {
                if (!smallChunks++)
                    pf::logInfo("heap: no free range of %u KB in guest memory, chunks of the block size from here",
                                ChunkSize / 1024);
                chunk = new MemMap(size, MemMap::Low, pf::perf::AllocHeapChunk);
            }
            chunks.push_back(chunk);
            chunkNext = chunk->getBlockStart();
            chunkEnd = chunkNext + chunk->getBlockSize();
        }
        const x86::reg32 address = chunkNext;
        chunkNext += size;
        return address;
    }
};

State& state()
{
    static State s_state;
    return s_state;
}

// Called from the guest memory exhaustion report, possibly by the thread that holds the mutex.
void State::report()
{
    State& s = state();
    pf::LwLockGuard guard(s.mutex);
    x86::reg32 chunkBytes = 0, freeBytes = 0, largeBytes = 0;
    for (const MemMap* chunk : s.chunks)
        chunkBytes += chunk->getBlockSize();
    for (size_t c = 0; c < s.freeLists.size(); ++c)
        freeBytes += x86::reg32(s.freeLists[c].size()) * s.classSizes[c];
    for (const auto& block : s.large)
        largeBytes += block.second->getBlockSize();
    pf::logInfo("memory: heap %u KB in live blocks; %u chunks %u KB (%u KB free in size classes, %u KB uncarved, %u "
                "small); %u large blocks %u KB", s.used / 1024, unsigned(s.chunks.size()), chunkBytes / 1024,
                freeBytes / 1024, (s.chunkEnd - s.chunkNext) / 1024, s.smallChunks, unsigned(s.large.size()),
                largeBytes / 1024);
    if (s.kept)
        pf::logInfo("memory: heap keeps %u KB for the next track load (%s)", s.kept->getBlockSize() / 1024,
                    s.keptLent ? "lent" : "idle");
}

}

x86::reg32 allocate(x86::reg32 size, bool zero)
{
    State& s = state();
    const x86::reg32 rounded = size ? (size + Granularity - 1) & ~(Granularity - 1) : Granularity;
    x86::reg32 address;
    int sizeClass = -1;
    {
        pf::LwLockGuard guard(s.mutex);
        if (s.reserved && !s.reservedLent && size <= s.reservedSize && size + ReserveSlack > s.reservedSize)
        {
            address = s.reserved->getBlockStart();
            s.reservedLent = true;
        }
        else if (rounded <= LargeSize)
        {
            sizeClass = s.classOf(rounded);
            std::vector<x86::reg32>& list = s.freeLists[sizeClass];
            if (!list.empty())
            {
                address = list.back();
                list.pop_back();
            }
            else
            {
                address = s.carve(s.classSizes[sizeClass]);
            }
        }
        else if (s.kept && !s.keptLent && rounded >= KeepSize && rounded <= s.kept->getBlockSize())
        {
            address = s.kept->getBlockStart();
            s.keptLent = true;
        }
        else
        {
            MemMap* block = MemMap::tryCreate(rounded, MemMap::Low, pf::perf::AllocHeapLarge);
            if (!block && s.kept && !s.keptLent)
            {
                pf::logInfo("memory: no room for %u KB: the kept %u KB block let go", rounded / 1024,
                            s.kept->getBlockSize() / 1024);
                delete s.kept;
                s.kept = nullptr;
            }
            if (!block)
                block = new MemMap(rounded, MemMap::Low, pf::perf::AllocHeapLarge);
            address = block->getBlockStart();
            s.large[address] = block;
        }
        s.blocks[address] = Block{size, sizeClass};
        s.used += size;
    }
    if (zero)
        memset(g_guestBase + address, 0, size);
    return address;
}

bool release(x86::reg32 address)
{
    State& s = state();
    MemMap* large = nullptr;
    {
        pf::LwLockGuard guard(s.mutex);
        const auto it = s.blocks.find(address);
        if (it == s.blocks.end())
            return false;
        s.used -= it->second.size;
        if (s.reserved && address == s.reserved->getBlockStart())
        {
            s.reservedLent = false;     // kept for the next request of its size
        }
        else if (s.kept && address == s.kept->getBlockStart())
        {
            s.keptLent = false;
        }
        else if (it->second.sizeClass >= 0)
        {
            s.freeLists[it->second.sizeClass].push_back(address);
        }
        else
        {
            const auto block = s.large.find(address);
            large = block->second;
            s.large.erase(block);
            if (!s.kept && large->getBlockSize() >= KeepSize)
            {
                s.kept = large;     // for the next track load
                s.keptLent = false;
                large = nullptr;
            }
        }
        s.blocks.erase(it);
    }
    delete large;
    return true;
}

bool unreserve()
{
    State& s = state();
    MemMap* reserved = nullptr;
    {
        pf::LwLockGuard guard(s.mutex);
        if (!s.reserved || s.reservedLent)
            return false;
        reserved = s.reserved;
        s.reserved = nullptr;
        s.reservedSize = 0;
    }
    delete reserved;
    return true;
}

bool letGoKept()
{
    State& s = state();
    MemMap* kept = nullptr;
    {
        pf::LwLockGuard guard(s.mutex);
        if (!s.kept || s.keptLent)
            return false;
        kept = s.kept;
        s.kept = nullptr;
    }
    pf::logInfo("memory: the kept %u KB block let go", kept->getBlockSize() / 1024);
    delete kept;
    return true;
}

bool reserve(x86::reg32 size)
{
    State& s = state();
    pf::LwLockGuard guard(s.mutex);
    if (s.reserved)
        return s.reservedSize == size;
    const x86::reg32 rounded = (size + Granularity - 1) & ~(Granularity - 1);
    s.reserved = MemMap::tryCreate(rounded, MemMap::Low, pf::perf::AllocHeapLarge);
    if (!s.reserved)
        return false;
    s.reservedSize = size;
    return true;
}

x86::reg32 size(x86::reg32 address)
{
    State& s = state();
    pf::LwLockGuard guard(s.mutex);
    const auto it = s.blocks.find(address);
    return it == s.blocks.end() ? 0xffffffff : it->second.size;
}

x86::reg32 used()
{
    State& s = state();
    pf::LwLockGuard guard(s.mutex);
    return s.used;
}

}}
