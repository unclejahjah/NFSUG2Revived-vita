// Memory usage log: guest arena areas, port heap, kernel free sizes and texture heap use in one line.

#include <lib/memlog.h>
#include <lib/memmap.h>
#include <platform/config.h>
#include <platform/switches.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/time.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#ifdef __vita__
#include <malloc.h>
#include <psp2/kernel/sysmem.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#endif

namespace win32 { namespace memlog
{

std::atomic<bool> g_enabled{false};

namespace
{

uint64_t s_intervalUs = 0;
uint64_t s_nextUs = 0;                  // game thread only
uint32_t (*s_frameCounter)() = nullptr;
std::atomic<uint64_t> s_lastLoadUs{0};
const uint64_t LoadGapUs = 2000000;     // one world load opens its files several times

uint32_t heapKb()
{
#ifdef __vita__
    return uint32_t(mallinfo().uordblks / 1024);
#elif defined(__APPLE__)
    malloc_statistics_t stats;
    malloc_zone_statistics(nullptr, &stats);
    return uint32_t(stats.size_in_use / 1024);
#else
    return 0;
#endif
}

}

void event(const char* why)
{
    if (!g_enabled.load(std::memory_order_relaxed))
        return;
    if (strcmp(why, "world load") == 0)
    {
        const uint64_t now = pf::ticksUs();
        const uint64_t last = s_lastLoadUs.exchange(now, std::memory_order_relaxed);
        if (last && now - last < LoadGapUs)
            return;
    }
    const MemMap::Usage u = MemMap::usage();
    uint32_t userFree = 0, cdramFree = 0, phycontFree = 0;
#ifdef __vita__
    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetFreeMemorySize(&info) >= 0)
    {
        userFree = uint32_t(std::max(0, info.size_user)) / 1024;
        cdramFree = uint32_t(std::max(0, info.size_cdram)) / 1024;
        phycontFree = uint32_t(std::max(0, info.size_phycont)) / 1024;
    }
#endif
    const pf::gxm::TextureMemory t = pf::gxm::Device::get().textureMemory();
    const unsigned seconds = unsigned(pf::ticksUs() / 1000000u);
    const unsigned frame = s_frameCounter ? s_frameCounter() : 0;
    pf::logInfo("memory: t=%u frame=%u why=%s guest_used=%u guest_free=%u guest_largest=%u guest_ranges=%u "
                "runtime_used=%u runtime_free=%u runtime_largest=%u blocks=%u heap=%u user_free=%u cdram_free=%u "
                "phycont_free=%u textures=%u tex_cache=%u tex_total=%u tex_live=%u tex_pending=%u tex_pending_n=%u "
                "tex_hole=%u tex_failed=%u tex_instant=%u tex_queued=%u tex_rescued=%u tex_peak=%u tex_pending_peak=%u",
                seconds, frame, why, unsigned(u.guestUsed), unsigned(u.guestFree), unsigned(u.guestLargest), unsigned(u.guestRanges), unsigned(u.runtimeUsed),
                unsigned(u.runtimeFree), unsigned(u.runtimeLargest), unsigned(u.blocks), unsigned(heapKb()),
                unsigned(userFree), unsigned(cdramFree), unsigned(phycontFree),
                unsigned(t.usedBytes / 1024), unsigned(pf::perf::cacheBytes() / 1024), unsigned(t.totalBytes / 1024),
                unsigned((t.usedBytes - t.pendingBytes) / 1024), unsigned(t.pendingBytes / 1024),
                unsigned(t.pendingCount), unsigned(t.largestHole / 1024), unsigned(t.failures),
                unsigned(t.instantFrees), unsigned(t.queuedFrees), unsigned(t.rescued),
                unsigned(t.peakUsedBytes / 1024), unsigned(t.peakPendingBytes / 1024));
}

void tick()
{
    const uint64_t now = pf::ticksUs();
    if (now < s_nextUs)
        return;
    s_nextUs = now + s_intervalUs;
    event("interval");
}

void configure(uint32_t (*frameCounter)())
{
    // Written with the info log, so it appears only when log_info=on as well.
    if (!NFSU_DEV_BOOL("memory_log", false))
        return;
    const int seconds = std::max(1, NFSU_DEV_INT("memory_log_seconds", 10));
    s_intervalUs = uint64_t(seconds) * 1000000u;
    s_nextUs = pf::ticksUs() + s_intervalUs;
    s_frameCounter = frameCounter;
    pf::logInfo("memory log: every %d s (memory_log=on); needs log_info=on", seconds);
    g_enabled.store(true, std::memory_order_relaxed);
    event("start");
}

}}
