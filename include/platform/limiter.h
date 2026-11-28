#ifndef PLATFORM_LIMITER_H_
#define PLATFORM_LIMITER_H_

#include <cstdint>

// limiter_log=on: where a frame's time goes on the game thread, the render thread and the GPU, summed from the race
// start and logged once with the benchmark summary.
namespace pf { namespace limiter {

enum Sum
{
    GameFrames,
    GameSlotWait,       // opening a frame: slot wait, fence and relock included
    GameFenceWait,      // of it: sceGxmNotificationWait on the slot's previous frame
    GameCpu,            // game thread CPU time
    RenderFrames,
    RenderIdle,         // waiting for a frame from the game thread
    RenderBusy,         // from taking a frame to its display queue entry
    RenderCpu,          // render thread CPU time of RenderBusy
    RenderResolve,      // resolve, blend ops, plan optimisation
    RenderPlan,         // plan, programs, sort
    RenderLists,        // command list recording, worker wait included
    RenderListWait,     // of it: stealing or waiting for the workers' jobs
    RenderScenes,       // begin scene, execute lists, end scene
    RenderEndScene,     // of it: inside sceGxmEndScene
    RenderBeginScene,   // of it: inside sceGxmBeginScene
    RenderQueue,        // inside sceGxmDisplayQueueAddEntry
    WorkerJobs,         // list workers recording jobs (wall)
    GpuFrames,          // frames with a known completion time, after one with a known completion time
    GpuBusy,            // of those: later of first end scene and previous completion, until completion
    GpuIdle,            // of those: previous completion until the first end scene, when later
    GpuSpanFrames,      // every frame
    GpuSpan,            // first end scene until the display callback starts (an upper bound of the GPU time)
    Flips,
    FlipWait,           // display callback: set frame buffer and vblank wait
    SumCount
};

#ifdef DEV_SETTINGS
extern bool g_on;
inline bool on() { return g_on; }
#else
constexpr bool g_on = false;
constexpr bool on() { return false; }
#endif
// Main thread, before other threads start.
void init();
void add(Sum sum, uint32_t value);
// Game thread, once per frame: adds its CPU time.
void gameFrame();
// Display callback end: counts the flip interval in vblanks.
void flipped(uint32_t nowUs);
// The calling thread's CPU time in microseconds.
uint64_t threadCpu();
// debug_scene_finish=on: the GPU is awaited after each scene.
bool sceneFinish();
// Render thread: one scene's GPU time, by target and order.
void scene(uint32_t width, uint32_t height, bool display, uint32_t ordinal, uint32_t us, uint32_t draws);
// Zeroes the sums (race start).
void start();
// Logs the per-frame split and the present interval histogram.
void report(const uint32_t* frameUs, uint32_t frames);

}}

#endif
