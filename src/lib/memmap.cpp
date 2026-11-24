#include <lib/memmap.h>
#include <lib/guestmodule.h>
#include <lib/mutex.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <ug2_layout.h>
#ifdef __vita__
#include <psp2/kernel/sysmem.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#endif
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace win32
{

// Guest heap, thread stacks and shim buffers: the free user memory at startup less s_reserveMemory (graphics
// heaps and pools, host thread stacks, system libraries allocated later), between s_minMemory and s_maxMemory.
[[maybe_unused]] static const x86::reg32 s_minMemory = 96*1024*1024;
static const x86::reg32 s_maxMemory = 192*1024*1024;
[[maybe_unused]] static const x86::reg32 s_reserveMemory = 64*1024*1024;
// End of the highest image section (the generated layout) or of the range kept for translated DLLs
// (lib/guestmodule.h), page-aligned.
static const x86::reg32 s_sectionLimit = (std::max(ug2::layout::imageEnd, ModuleRangeEnd) + 4095) & ~4095u;
// One byte per 4 KB block of the simulated memory, page-aligned.
static x86::reg32 blockMapSize(x86::reg32 memory)
{
    return (memory / 4096 + 4095) & ~4095u;
}
// Set by allocArena. Layout: sections, block map, simulated memory.
static x86::reg32 s_simulatedMemory = 0;
static x86::reg32 s_blockMapSize = 0;
static x86::reg32 s_arenaSize = 0;
#ifdef __vita__
static SceKernelFreeMemorySizeInfo s_freeAtStart;
#endif

x86::reg8* MemMap::s_blocks = nullptr;
x86::reg32 MemMap::s_blockCount = 0;
x86::reg32 MemMap::s_blockSize = 0;
x86::reg32 MemMap::s_sectionSize = 0;
x86::reg32 MemMap::s_addressOffset = 0;
x86::reg32 MemMap::s_lowFree = 0;
x86::reg32 MemMap::s_highFree = 0;
x86::reg32 MemMap::s_runtimeStart = 0;
bool MemMap::s_split = true;
MemMap::Reclaim MemMap::s_reclaim = nullptr;
Mutex*     MemMap::s_lock = nullptr;
x86::reg32 MemMap::s_liveCount = 0;

#ifdef __vita__
static SceUID s_arenaBlock = -1;

// The guest address space is one zeroed memblock; pages are always accessible.
// Runs before main, so failure is reported by MemMap::init once logging is up.
static SceUID allocBlock(x86::reg32 memory)
{
    s_simulatedMemory = memory;
    s_blockMapSize = blockMapSize(memory);
    s_arenaSize = s_sectionLimit + s_blockMapSize + memory;
    return sceKernelAllocMemBlock("guest", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW, s_arenaSize, nullptr);
}

static x86::reg8* allocArena()
{
    x86::reg32 memory = s_minMemory;
    memset(&s_freeAtStart, 0, sizeof(s_freeAtStart));
    s_freeAtStart.size = sizeof(s_freeAtStart);
    if (sceKernelGetFreeMemorySize(&s_freeAtStart) >= 0)
    {
        const x86::reg32 overhead = s_sectionLimit + blockMapSize(s_maxMemory) + s_reserveMemory;
        const x86::reg32 free = x86::reg32(std::max(0, s_freeAtStart.size_user));
        if (free > overhead + s_minMemory)
            memory = std::min(s_maxMemory, (free - overhead) & ~(1024u * 1024 - 1));
    }
    s_arenaBlock = allocBlock(memory);
    if (s_arenaBlock < 0 && memory > s_minMemory)
        s_arenaBlock = allocBlock(s_minMemory);
    if (s_arenaBlock < 0)
        return nullptr;
    void* base = nullptr;
    sceKernelGetMemBlockBase(s_arenaBlock, &base);
    memset(base, 0, s_arenaSize);
    return static_cast<x86::reg8*>(base);
}

static void freeArena()
{
    if (s_arenaBlock >= 0)
    {
        sceKernelFreeMemBlock(s_arenaBlock);
        s_arenaBlock = -1;
    }
}
#else
// Host: the 4 GB guest address space and a guard are reserved inaccessible, so stray guest accesses fault; the
// arena, of the Vita's largest size, is read-write and zeroed by the kernel.
static const size_t s_reservationSize = (size_t(1) << 32) + 64 * 1024;
static void* s_reservation = nullptr;
static int s_arenaError = 0;

static x86::reg8* allocArena()
{
    s_simulatedMemory = s_maxMemory;
    s_blockMapSize = blockMapSize(s_maxMemory);
    s_arenaSize = s_sectionLimit + s_blockMapSize + s_maxMemory;
    void* base = mmap(nullptr, s_reservationSize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED)
    {
        s_arenaError = errno;
        return nullptr;
    }
    const size_t page = size_t(sysconf(_SC_PAGESIZE));
    if (mprotect(base, (size_t(s_arenaSize) + page - 1) & ~(page - 1), PROT_READ | PROT_WRITE) != 0)
    {
        s_arenaError = errno;
        munmap(base, s_reservationSize);
        return nullptr;
    }
    s_reservation = base;
    return static_cast<x86::reg8*>(base);
}

static void freeArena()
{
    if (s_reservation)
    {
        munmap(s_reservation, s_reservationSize);
        s_reservation = nullptr;
    }
}
#endif

x86::reg8* const g_guestBase = allocArena();

// One bit per block of the simulated memory, set while the block is free; kept with s_blocks under s_lock.
static const x86::reg32 s_freeWordCount = s_maxMemory / 4096 / 32;
static uint32_t s_freeBits[s_freeWordCount];
// The block that owns each 4 KB block of the simulated memory, null while free; under s_lock.
static MemMap* s_owners[s_maxMemory / 4096];

static void markBlocks(x86::reg32 begin, x86::reg32 count, bool free)
{
    x86::reg32 b = begin;
    const x86::reg32 end = begin + count;
    while (b < end)
    {
        const x86::reg32 bit = b & 31;
        const x86::reg32 n = std::min<x86::reg32>(32 - bit, end - b);
        const uint32_t mask = (n == 32 ? ~0u : ((1u << n) - 1)) << bit;
        if (free)
            s_freeBits[b >> 5] |= mask;
        else
            s_freeBits[b >> 5] &= ~mask;
        b += n;
    }
}

// First block from b on whose free bit equals free, or limit.
static x86::reg32 nextBlock(x86::reg32 b, x86::reg32 limit, bool free)
{
    if (b >= limit)
        return limit;
    x86::reg32 word = b >> 5;
    uint32_t bits = (free ? s_freeBits[word] : ~s_freeBits[word]) & (~0u << (b & 31));
    while (!bits)
    {
        if (++word >= (limit + 31) >> 5)
            return limit;
        bits = free ? s_freeBits[word] : ~s_freeBits[word];
    }
    return std::min<x86::reg32>((word << 5) + x86::reg32(__builtin_ctz(bits)), limit);
}

// Last block below e whose free bit equals free, plus 1; 0 when there is none.
static x86::reg32 previousBlockEnd(x86::reg32 e, bool free)
{
    if (e == 0)
        return 0;
    x86::reg32 word = (e - 1) >> 5;
    const x86::reg32 top = (e - 1) & 31;
    uint32_t bits = (free ? s_freeBits[word] : ~s_freeBits[word]) & (top == 31 ? ~0u : ((2u << top) - 1));
    while (!bits)
    {
        if (word == 0)
            return 0;
        --word;
        bits = free ? s_freeBits[word] : ~s_freeBits[word];
    }
    return (word << 5) + 32 - x86::reg32(__builtin_clz(bits));
}

// Lowest b with [b, b + count) free: the same block plain first fit from block 0 finds.
x86::reg32 MemMap::findLow(x86::reg32 count)
{
    x86::reg32 b = s_lowFree;
    bool first = true;
    while (b + count <= s_blockCount)
    {
        b = nextBlock(b, s_blockCount, true);
        if (b == s_blockCount)
            break;
        if (first)
        {
            s_lowFree = b;
            first = false;
        }
        const x86::reg32 end = b + count;
        if (end > s_blockCount)
            break;
        const x86::reg32 s = nextBlock(b, end, false);
        if (s == end)
            return b;
        b = s + 1;
    }
    return x86::reg32(-1);
}

// Highest b with [b, b + count) free below limit.
x86::reg32 MemMap::findHigh(x86::reg32 count, x86::reg32 limit)
{
    bool first = limit >= s_highFree;
    x86::reg32 e = first ? s_highFree : limit;
    while (e >= count)
    {
        e = previousBlockEnd(e, true);
        if (first)
        {
            s_highFree = e;
            first = false;
        }
        if (e < count)
            break;
        const x86::reg32 begin = e - count;
        const x86::reg32 s = std::max(previousBlockEnd(e, false), begin);
        if (s == begin)
            return begin;
        e = s - 1;
    }
    return x86::reg32(-1);
}

MemMap::MemMap(x86::reg32 size, pf::perf::Alloc kind, Unplaced)
    :   m_block(x86::reg32(-1))
    ,   m_blockCount(size / 4096 + (size % 4096 != 0))
    ,   m_kind(kind)
{
    RC_ASSERT(m_blockCount >= 1);
}

MemMap::MemMap(x86::reg32 size, Placement placement, pf::perf::Alloc kind)
    :   MemMap(size, kind, Unplaced())
{
    if (!place(size, placement))
    {
        report(size, kind, placement);
        pf::fatal("guest memory exhausted (requested %u bytes)", size);
    }
}

MemMap* MemMap::tryCreate(x86::reg32 size, Placement placement, pf::perf::Alloc kind)
{
    MemMap* memMap = new MemMap(size, kind, Unplaced());
    if (memMap->place(size, placement))
        return memMap;
    delete memMap;
    return nullptr;
}

bool MemMap::place(x86::reg32 size, Placement placement)
{
    if (m_blockCount > s_blockCount)
        return false;
    const bool timed = pf::perf::timed();
    const bool searchTimed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    uint32_t searchUs = 0;
    const bool large = s_split && placement == Low && m_blockCount >= LargeSize / 4096;
    bool anywhere = false;
    for (;;)
    {
        s_lock->lock();
        const uint32_t searchStart = searchTimed ? pf::perf::now() : 0;
        if (placement == High)
            m_block = findHigh(m_blockCount, s_blockCount);
        else if (large)
            m_block = findHigh(m_blockCount, anywhere ? s_blockCount : s_runtimeStart);
        else
            m_block = findLow(m_blockCount);
        if (searchTimed)
            searchUs += pf::perf::now() - searchStart;
        if (m_block != x86::reg32(-1))
            break;
        s_lock->unlock();
        if (s_reclaim && s_reclaim())
            continue;
        if (!large || anywhere)
            return false;
        anywhere = true;
    }
    memset(s_blocks + m_block, 1, m_blockCount);
    markBlocks(m_block, m_blockCount, false);
    std::fill(s_owners + m_block, s_owners + m_block + m_blockCount, this);
    ++s_liveCount;
    if (m_block == s_lowFree)
        s_lowFree = m_block + m_blockCount;
    if (m_block + m_blockCount == s_highFree)
        s_highFree = m_block;
    s_lock->unlock();
    if (timed)
        pf::perf::guestBlock(pf::perf::Alloc(m_kind), size, pf::perf::now() - start, searchUs);
    return true;
}

MemMap::~MemMap()
{
    if (m_block == x86::reg32(-1))
        return;
    s_lock->lock();
    std::fill(s_owners + m_block, s_owners + m_block + m_blockCount, nullptr);
    --s_liveCount;
    memset(s_blocks + m_block, 0, m_blockCount);
    markBlocks(m_block, m_blockCount, true);
    if (m_block < s_lowFree)
        s_lowFree = m_block;
    if (m_block + m_blockCount > s_highFree)
        s_highFree = m_block + m_blockCount;
    s_lock->unlock();
}

void MemMap::setReclaim(Reclaim reclaim)
{
    s_reclaim = reclaim;
}

static MemMap::Report s_reports[4];

void MemMap::addReport(Report report)
{
    for (MemMap::Report& slot : s_reports)
    {
        if (!slot)
        {
            slot = report;
            return;
        }
    }
}

static void logFreeMemory(const char* when)
{
#ifdef __vita__
    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetFreeMemorySize(&info) >= 0)
        pf::logInfo("memory: free %s: user %d KB, cdram %d KB, phycont %d KB", when, info.size_user / 1024,
                    info.size_cdram / 1024, info.size_phycont / 1024);
#else
    RC_USE(when);
#endif
}

struct FreeRanges
{
    x86::reg32 blocks = 0, count = 0, largest = 0;
};

// Free ranges of [begin, end); s_lock held.
static FreeRanges freeRanges(x86::reg32 begin, x86::reg32 end)
{
    FreeRanges f;
    for (x86::reg32 b = nextBlock(begin, end, true); b < end; )
    {
        const x86::reg32 stop = nextBlock(b, end, false);
        f.blocks += stop - b;
        f.largest = std::max(f.largest, stop - b);
        ++f.count;
        b = nextBlock(stop, end, true);
    }
    return f;
}

void MemMap::report(x86::reg32 requested, pf::perf::Alloc requestKind, Placement placement)
{
    static const int Largest = 8;
    static const x86::reg32 Cells = 48;
    x86::reg32 kindBlocks[pf::perf::AllocCount] = {};
    x86::reg32 kindCounts[pf::perf::AllocCount] = {};
    x86::reg32 kindFirst[pf::perf::AllocCount] = {};
    x86::reg32 kindLast[pf::perf::AllocCount] = {};
    const MemMap* largest[Largest] = {};
    size_t live = 0;
    s_lock->lock();
    const FreeRanges all = freeRanges(0, s_blockCount);
    const FreeRanges guest = freeRanges(0, s_runtimeStart);
    const FreeRanges runtime = freeRanges(s_runtimeStart, s_blockCount);
    for (x86::reg32 b = nextBlock(0, s_blockCount, false); b < s_blockCount;
         b = nextBlock(b + s_owners[b]->m_blockCount, s_blockCount, false))
    {
        const MemMap* m = s_owners[b];
        const int kind = m->m_kind >= 0 && m->m_kind < pf::perf::AllocCount ? m->m_kind : 0;
        if (!kindCounts[kind])
            kindFirst[kind] = m->getBlockStart();
        kindLast[kind] = m->getBlockStart() + m->getBlockSize();
        kindBlocks[kind] += m->m_blockCount;
        ++kindCounts[kind];
        for (int i = 0; i < Largest; ++i)
        {
            if (!largest[i] || largest[i]->m_blockCount < m->m_blockCount)
            {
                for (int j = Largest - 1; j > i; --j)
                    largest[j] = largest[j - 1];
                largest[i] = m;
                break;
            }
        }
    }
    live = s_liveCount;
    char line[512];
    int length = 0;
    for (int i = 0; i < Largest && largest[i]; ++i)
    {
        length += snprintf(line + length, sizeof(line) - length, " %s %u KB at 0x%08x;",
                           pf::perf::allocName(pf::perf::Alloc(largest[i]->m_kind)),
                           largest[i]->m_blockCount * 4, largest[i]->getBlockStart());
        if (length >= int(sizeof(line)))
            break;
    }
    char map[Cells + 1];
    const x86::reg32 perCell = (s_blockCount + Cells - 1) / Cells;
    for (x86::reg32 c = 0; c < Cells; ++c)
    {
        const x86::reg32 begin = std::min(c * perCell, s_blockCount), end = std::min(begin + perCell, s_blockCount);
        const x86::reg32 used = end - begin - freeRanges(begin, end).blocks;
        map[c] = begin == end ? ' ' : !used ? '.' : used == end - begin ? '#' : char('0' + used * 10 / (end - begin));
    }
    map[Cells] = 0;
    s_lock->unlock();

    const bool large = s_split && placement == Low && requested >= LargeSize;
    pf::logInfo("memory: guest exhausted, %u bytes requested (%s, %s); arena %u KB, free %u KB in %u ranges, largest "
                "%u KB, %u blocks live", requested, pf::perf::allocName(requestKind),
                placement == High ? "runtime, highest fit" : large ? "large, highest fit" : "lowest fit",
                s_simulatedMemory / 1024, all.blocks * 4, all.count, all.largest * 4, unsigned(live));
    pf::logInfo("memory: guest area 0x%08x %u KB, free %u KB in %u ranges, largest %u KB; runtime area 0x%08x %u KB, "
                "free %u KB in %u ranges, largest %u KB", s_addressOffset, s_runtimeStart * 4, guest.blocks * 4,
                guest.count, guest.largest * 4, s_addressOffset + s_runtimeStart * 4096,
                (s_blockCount - s_runtimeStart) * 4, runtime.blocks * 4, runtime.count, runtime.largest * 4);
    for (int kind = 0; kind < pf::perf::AllocCount; ++kind)
    {
        if (kindCounts[kind])
            pf::logInfo("memory: %s %u blocks, %u KB, 0x%08x to 0x%08x", pf::perf::allocName(pf::perf::Alloc(kind)),
                        kindCounts[kind], kindBlocks[kind] * 4, kindFirst[kind], kindLast[kind]);
    }
    pf::logInfo("memory: largest blocks:%s", line);
    pf::logInfo("memory: map from 0x%08x, %u KB per cell, tenths used (. free, # full): %s", s_addressOffset,
                perCell * 4, map);
    logFreeMemory("now");
    for (MemMap::Report r : s_reports)
    {
        if (r)
            r();
    }
}

MemMap::Usage MemMap::usage()
{
    s_lock->lock();
    const x86::reg32 split = s_split ? s_runtimeStart : s_blockCount;
    const FreeRanges guest = freeRanges(0, split);
    const FreeRanges runtime = freeRanges(split, s_blockCount);
    Usage u;
    u.blocks = s_liveCount;
    s_lock->unlock();
    u.guestUsed = (split - guest.blocks) * 4;
    u.guestFree = guest.blocks * 4;
    u.guestLargest = guest.largest * 4;
    u.guestRanges = guest.count;
    u.runtimeUsed = (s_blockCount - split - runtime.blocks) * 4;
    u.runtimeFree = runtime.blocks * 4;
    u.runtimeLargest = runtime.largest * 4;
    u.runtimeRanges = runtime.count;
    return u;
}

// Null for an address outside every block.
MemMap* MemMap::findBlock(x86::reg32 memIndex)
{
    if (memIndex < s_addressOffset || memIndex >= s_addressOffset + s_simulatedMemory)
        return nullptr;
    s_lock->lock();
    MemMap* result = s_owners[(memIndex - s_addressOffset) / 4096];
    s_lock->unlock();
    return result;
}

bool MemMap::isGuestRange(x86::reg32 address, x86::reg32 size)
{
    return x86::reg64(address) + size <= x86::reg64(s_addressOffset) + s_simulatedMemory;
}

void MemMap::init(x86::reg32 baseAddress, const std::vector<Section>& sections, bool split, x86::reg32 runtimeSize)
{
#ifdef __vita__
    if (!g_guestBase)
        pf::fatal("guest memory allocation of %u bytes failed: 0x%08x", s_arenaSize, unsigned(s_arenaBlock));
    pf::logInfo("guest memory: %u bytes at %p, %u KB for blocks (free user memory at startup %d KB, cdram %d KB)",
                s_arenaSize, static_cast<void*>(g_guestBase), s_simulatedMemory / 1024, s_freeAtStart.size_user / 1024,
                s_freeAtStart.size_cdram / 1024);
#else
    if (!g_guestBase)
        pf::fatal("guest address space reservation failed: %s", strerror(s_arenaError));
    pf::logInfo("guest memory: %u bytes at %p of a 4 GB reservation, %u KB for blocks", s_arenaSize,
                static_cast<void*>(g_guestBase), s_simulatedMemory / 1024);
#endif
    logFreeMemory("after the guest memory");
    s_blockCount = s_simulatedMemory / 4096;
    s_blockSize = s_blockMapSize;
    s_lowFree = 0;
    s_highFree = s_blockCount;
    s_split = split;
    s_runtimeStart = s_blockCount - std::min(runtimeSize / 4096, s_blockCount / 4);
    pf::logInfo("guest memory: %s, runtime area %u KB at the top", split ? "blocks of 4096 KB and more placed from "
                "the top (guest_memory_split=on)" : "every block placed lowest fit (guest_memory_split=off)",
                (s_blockCount - s_runtimeStart) * 4);
    markBlocks(0, s_blockCount, true);
    x86::reg32 maxEnd = 0;
    for (std::vector<Section>::const_iterator it = sections.begin(); it != sections.end(); ++it)
    {
        x86::reg32 end = it->baseAddress + it->size;
        if (end > maxEnd)
            maxEnd = end;
    }
    s_sectionSize = (maxEnd + 4095) & ~4095u;
    if (s_sectionSize > s_sectionLimit)
        pf::fatal("image sections end at 0x%08x, past the guest arena limit 0x%08x", s_sectionSize, s_sectionLimit);
    s_blocks = g_guestBase + s_sectionSize;
    s_addressOffset = s_sectionSize + s_blockSize;
    for (std::vector<Section>::const_iterator it = sections.begin(); it != sections.end(); ++it)
    {
        RC_ASSERT(it->baseAddress >= baseAddress);
        RC_ASSERT(it->baseAddress + it->size <= s_sectionSize);
        if (it->data)
        {
            memcpy(g_guestBase + it->baseAddress, it->data, it->size);
        }
    }
    s_lock = new Mutex("MemMap");
}

void MemMap::fini()
{
    std::vector<MemMap*> live;
    for (x86::reg32 b = nextBlock(0, s_blockCount, false); b < s_blockCount;
         b = nextBlock(b + s_owners[b]->m_blockCount, s_blockCount, false))
        live.push_back(s_owners[b]);
    for (MemMap* m : live)
        delete m;
    RC_ASSERT(s_liveCount == 0);
    s_blocks = nullptr;
    s_blockCount = 0;
    s_blockSize = 0;
    s_addressOffset = 0;
    delete s_lock;
    s_lock = nullptr;
    freeArena();
}

void MemMap::fillDebugGraph(x86::reg16* graph)
{
    s_lock->lock();
    const x86::reg32 blocksPerPixel = (s_blockCount+639)/640;
    for (x86::reg32 b = 0; b < s_blockCount; b += blocksPerPixel)
    {
        x86::reg16 c = 0;
        for (x86::reg32 b1 = b; b1 < b+blocksPerPixel; ++b1)
        {
            if (s_blocks[b1])
                c++;
        }
        x86::reg16 r = (0x1f*c/blocksPerPixel) << (5+6);
        x86::reg16 g = (0x3f*(blocksPerPixel-c)/blocksPerPixel) << 5;
        graph[b/blocksPerPixel + 640*0] = r|g;
        graph[b/blocksPerPixel + 640*1] = r|g;
        graph[b/blocksPerPixel + 640*2] = r|g;
    }
    s_lock->unlock();
}

void MemMap::fillDebugGraph(x86::reg32* graph)
{
    s_lock->lock();
    const x86::reg32 blocksPerPixel = (s_blockCount+639)/640;
    for (x86::reg32 b = 0; b < s_blockCount; b += blocksPerPixel)
    {
        x86::reg16 c = 0;
        for (x86::reg32 b1 = b; b1 < b+blocksPerPixel; ++b1)
        {
            if (s_blocks[b1])
                c++;
        }
        x86::reg32 r = (0xff*c/blocksPerPixel) << 24;
        x86::reg32 g = (0xff*(blocksPerPixel-c)/blocksPerPixel) << 16;
        graph[b/blocksPerPixel + 640*0] = r|g;
        graph[b/blocksPerPixel + 640*1] = r|g;
        graph[b/blocksPerPixel + 640*2] = r|g;
    }
    s_lock->unlock();
}

}
