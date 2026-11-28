// The game queues its texture releases and does 3 a frame, so a left world stays in memory while the next one loads.
// The queue is emptied each frame within a time budget, and completely when a world load starts (textures.h).

#include "textures.h"

#include <lib/memlog.h>
#include <platform/config.h>
#include <platform/switches.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <recomp.h>

#include <cstdio>

namespace ug2 { namespace textures {

namespace
{

using x86::reg32;

const reg32 NoTextures = 0x0087079C;        // nonzero byte: the game creates and releases no texture
const reg32 PeekMessageSlot = 0x00783330;   // called every frame by 0x5BF1C0
const reg32 FramesRendered = 0x0086514C;
const reg32 ReleaseQueue = 0x008763B0;      // list head: Direct3D objects the game releases 3 a frame
const reg32 ReleaseSome = 0x005CE4C0;       // releases up to 3 of them; called every frame before PeekMessage
const uint32_t ReleaseBudgetUs = 4000;
const reg32 GameState = 0x008654A4;         // 4 then 5 while a world loads (5 alone from free roam to a race), 6 in the world
const reg32 LoadStartState = 4, LoadingState = 5;

uint32_t s_retries = 0, s_retried = 0;      // texture_retry: creations tried, textures made
win32::MethodPtr s_peekMessage = nullptr;
NFSU_DEV_VARC(bool, s_releaseAll, true, true);                   // texture_release_all
NFSU_DEV_VARC(bool, s_loadDrain, true, true);                    // texture_load_drain
bool s_firstCreation = false;               // the next creation is the first of a world load
reg32 s_gameState = 0;
int s_thread = 0;

bool logged(uint32_t count)
{
    return count <= 4 || count % 64 == 0;
}

uint32_t queueLength()
{
    uint32_t n = 0;
    for (reg32 node = rc::ld32(ReleaseQueue); node != ReleaseQueue && node && n < 100000; node = rc::ld32(node))
        ++n;
    return n;
}

// With memory_log: the queue and the texture heap at a point of a world load.
void logQueue(const char* when)
{
    if (!win32::memlog::g_enabled.load(std::memory_order_relaxed))
        return;
    const pf::gxm::TextureMemory m = pf::gxm::Device::get().textureMemory();
    pf::logInfo("textures: %s: release queue %u, tex_live %u KB, tex_pending %u KB, tex_total %u KB, frame %u", when,
                unsigned(queueLength()), unsigned((m.usedBytes - m.pendingBytes) / 1024),
                unsigned(m.pendingBytes / 1024), unsigned(m.totalBytes / 1024), unsigned(rc::ld32(FramesRendered)));
}

// Empties the game's release queue: within a time budget, or all of it.
void drainReleases(win32::WinApplication* app, x86::CPU& cpu, bool all)
{
    if (rc::ld32(ReleaseQueue) == ReleaseQueue || rc::ld8(NoTextures) || pf::Thread::currentId() != s_thread)
        return;
    const uint32_t start = pf::ticksUsLow();
    do
    {
        const x86::CPU saved = cpu;
        cpu.esp -= 4;
        app->dynamic_call(ReleaseSome, cpu);
        cpu = saved;
    } while (rc::ld32(ReleaseQueue) != ReleaseQueue && (all || pf::ticksUsLow() - start < ReleaseBudgetUs));
}

void peekMessageHook(win32::WinApplication* app, x86::CPU& cpu)
{
    s_peekMessage(app, cpu);
    const reg32 gameState = rc::ld32(GameState);
    if (gameState != s_gameState && pf::Thread::currentId() == s_thread)
    {
        char when[48];
        snprintf(when, sizeof(when), "game state %u -> %u", unsigned(s_gameState), unsigned(gameState));
        logQueue(when);
        const bool loadStart = gameState == LoadStartState || (gameState == LoadingState && s_gameState != LoadStartState);
        s_gameState = gameState;
        if (loadStart && s_loadDrain)
        {
            // What the game has queued goes before the new world's textures are made.
            drainReleases(app, cpu, true);
            pf::gxm::Device::get().collectTextures();
            logQueue("world load start, after the drain");
        }
        s_firstCreation = loadStart;
    }
    if (s_releaseAll)
        drainReleases(app, cpu, false);
}

}

void configure(win32::WinApplication& app)
{
    NFSU_DEV_SET(s_releaseAll, NFSU_DEV_BOOL("texture_release_all", true));
    NFSU_DEV_SET(s_loadDrain, NFSU_DEV_BOOL("texture_load_drain", true));
    s_thread = pf::Thread::currentId();
    pf::logInfo("textures: the game's queued texture releases are done %s (texture_release_all=%s), and %s when a world "
                "load starts (texture_load_drain=%s)", s_releaseAll ? "as they come, up to 4 ms a frame" : "3 a frame",
                s_releaseAll ? "on" : "off", s_loadDrain ? "all freed" : "left queued", s_loadDrain ? "on" : "off");
    if (s_releaseAll || s_loadDrain)
        s_peekMessage = app.replaceMethod(rc::ld32(PeekMessageSlot), peekMessageHook);
}

void creating()
{
    if (!s_firstCreation)
        return;
    s_firstCreation = false;
    logQueue("first creation of the world load");
}

void retried(bool created)
{
    ++s_retries;
    if (created && logged(++s_retried))
        pf::logInfo("textures: a record without a texture has one now (texture_retry, %u of %u creations)",
                    unsigned(s_retried), unsigned(s_retries));
}

}}
