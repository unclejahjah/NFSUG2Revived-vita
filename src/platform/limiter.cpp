#include <platform/limiter.h>
#include <platform/config.h>
#include <platform/switches.h>
#include <platform/log.h>
#include <platform/perf.h>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

#include <atomic>
#include <cstdio>

namespace pf { namespace limiter {

#ifdef DEV_SETTINGS
bool g_on = false;
#endif

namespace
{

const uint32_t VblankUs = 16667;
const uint32_t FlipBins = 8;
const uint32_t BinUs = 2000, Bins = 40;

std::atomic<uint32_t> s_sums[SumCount];
std::atomic<uint32_t> s_flipBins[FlipBins];
std::atomic<uint32_t> s_lastFlip{0};
std::atomic<uint32_t> s_generation{1};
struct SceneSum
{
    uint32_t key, count, us, draws;
};
const uint32_t MaxScenes = 48;
SceneSum s_scenes[MaxScenes];   // render thread
uint32_t s_sceneCount = 0;
NFSU_DEV_VAR(bool, s_sceneFinish, false);
uint64_t s_gameCpu = 0;         // game thread
uint32_t s_gameGeneration = 0;

double per(Sum sum, Sum count)
{
    const uint32_t n = s_sums[count].load(std::memory_order_relaxed);
    return n ? double(s_sums[sum].load(std::memory_order_relaxed)) / 1000.0 / n : 0.0;
}

}

void init()
{
#ifdef DEV_SETTINGS
    g_on = pf::Config::getBool("limiter_log", false);
#endif
    NFSU_DEV_SET(s_sceneFinish, g_on && NFSU_DEV_BOOL("debug_scene_finish", false));
    if (s_sceneFinish)
        logInfo("limiter: debug: GPU finished after every scene (debug_scene_finish): frame times are not normal");
    if (g_on)
        logInfo("limiter: on (limiter_log): frame time split logged with the benchmark summary");
}

void add(Sum sum, uint32_t value)
{
    s_sums[sum].fetch_add(value, std::memory_order_relaxed);
}

uint64_t threadCpu()
{
#ifdef __vita__
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    return sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0 ? 0 : info.runClocks;
#else
    return 0;
#endif
}

void gameFrame()
{
    const uint64_t cpu = threadCpu();
    const uint32_t generation = s_generation.load(std::memory_order_relaxed);
    if (s_gameGeneration == generation)
    {
        add(GameCpu, uint32_t(cpu - s_gameCpu));
        add(GameFrames, 1);
    }
    s_gameGeneration = generation;
    s_gameCpu = cpu;
}

void flipped(uint32_t nowUs)
{
    const uint32_t last = s_lastFlip.exchange(nowUs, std::memory_order_relaxed);
    if (!last)
        return;
    const uint32_t vblanks = (nowUs - last + VblankUs / 2) / VblankUs;
    s_flipBins[vblanks < FlipBins ? vblanks : FlipBins - 1].fetch_add(1, std::memory_order_relaxed);
}

bool sceneFinish()
{
    return s_sceneFinish;
}

void scene(uint32_t width, uint32_t height, bool display, uint32_t ordinal, uint32_t us, uint32_t draws)
{
    const uint32_t key = width << 20 | height << 8 | (display ? 0x80 : 0) | (ordinal & 0x7f);
    uint32_t i = 0;
    while (i < s_sceneCount && s_scenes[i].key != key)
        ++i;
    if (i == MaxScenes)
        return;
    if (i == s_sceneCount)
        s_scenes[s_sceneCount++] = SceneSum{ key, 0, 0, 0 };
    ++s_scenes[i].count;
    s_scenes[i].us += us;
    s_scenes[i].draws += draws;
}

void start()
{
    s_sceneCount = 0;
    for (auto& s : s_sums)
        s.store(0, std::memory_order_relaxed);
    for (auto& b : s_flipBins)
        b.store(0, std::memory_order_relaxed);
    s_lastFlip.store(0, std::memory_order_relaxed);
    s_generation.fetch_add(1, std::memory_order_relaxed);
}

void report(const uint32_t* frameUs, uint32_t frames)
{
    if (!g_on)
        return;
    uint64_t total = 0;
    uint32_t bins[Bins] = {};
    for (uint32_t i = 0; i < frames; ++i)
    {
        total += frameUs[i];
        const uint32_t bin = frameUs[i] / BinUs;
        ++bins[bin < Bins ? bin : Bins - 1];
    }
    const double frame = frames ? double(total) / 1000.0 / frames : 0.0;
    const double gameWall = s_sums[GameFrames] ? double(total) / 1000.0 / s_sums[GameFrames] : 0.0;
    logInfo("limiter: present interval %.2f ms over %u frames | game thread per frame: %.2f ms = cpu %.2f + slot wait "
            "%.2f (fence %.2f) + other %.2f (%u frames)", frame, unsigned(frames), gameWall, per(GameCpu, GameFrames),
            per(GameSlotWait, GameFrames), per(GameFenceWait, GameFrames),
            gameWall - per(GameCpu, GameFrames) - per(GameSlotWait, GameFrames), unsigned(s_sums[GameFrames]));
    logInfo("limiter: render thread per frame: idle %.2f + busy %.2f (cpu %.2f) = resolve %.2f + plan %.2f + lists "
            "%.2f (worker wait %.2f) + scenes %.2f (begin %.2f, end %.2f) + queue %.2f | list workers %.2f (%u frames)",
            per(RenderIdle, RenderFrames), per(RenderBusy, RenderFrames), per(RenderCpu, RenderFrames),
            per(RenderResolve, RenderFrames), per(RenderPlan, RenderFrames), per(RenderLists, RenderFrames),
            per(RenderListWait, RenderFrames), per(RenderScenes, RenderFrames), per(RenderBeginScene, RenderFrames),
            per(RenderEndScene, RenderFrames), per(RenderQueue, RenderFrames), per(WorkerJobs, RenderFrames),
            unsigned(s_sums[RenderFrames]));
    logInfo("limiter: gpu per frame: busy %.2f + idle %.2f (%u frames with known completion) | first end scene to "
            "display callback %.2f (%u frames) | flip wait %.2f (%u flips)", per(GpuBusy, GpuFrames),
            per(GpuIdle, GpuFrames), unsigned(s_sums[GpuFrames]), per(GpuSpan, GpuSpanFrames),
            unsigned(s_sums[GpuSpanFrames]), per(FlipWait, Flips), unsigned(s_sums[Flips]));
    char text[512];
    int n = 0;
    for (uint32_t i = 0; i < FlipBins; ++i)
        n += snprintf(text + n, sizeof(text) - size_t(n), " %u:%u", unsigned(i), unsigned(s_flipBins[i]));
    logInfo("limiter: flip intervals in vblanks (count):%s", text);
    n = 0;
    for (uint32_t i = 0; i < Bins && n < int(sizeof(text)) - 16; ++i)
        if (bins[i])
            n += snprintf(text + n, sizeof(text) - size_t(n), " %u:%u", unsigned(i * BinUs / 1000), unsigned(bins[i]));
    logInfo("limiter: present intervals, %u ms bins from (count):%s", unsigned(BinUs / 1000), text);
    const uint32_t renderFrames = s_sums[RenderFrames] ? s_sums[RenderFrames].load() : 1;
    for (uint32_t i = 0; i < s_sceneCount; ++i)
    {
        const SceneSum& s = s_scenes[i];
        logInfo("limiter: scene %s%ux%u #%u: gpu %.2f ms per frame (%.2f ms in %u scenes), %.1f draws",
                s.key & 0x80 ? "display " : "t", unsigned(s.key >> 20), unsigned(s.key >> 8 & 0xfff),
                unsigned(s.key & 0x7f), double(s.us) / 1000.0 / renderFrames, double(s.us) / 1000.0 / s.count,
                unsigned(s.count), double(s.draws) / s.count);
    }
}

}}
