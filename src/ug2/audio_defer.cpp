// Callback thread for the EA SND notification delivery (0x72382F) and tick hook (0x73B9C0): a ring filled by the mixer
// without locks, drained in order under the execution lock. Hook requests are ring entries too.

#include "audio_defer.h"
#include "audio_mixer.h"

#include <lib/event.h>
#include <lib/scheduler.h>
#include <lib/teb.h>
#include <lib/thread.h>
#include <platform/config.h>
#include <platform/switches.h>
#include <platform/cores.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <recomp.h>

#include <atomic>

namespace ug2 { namespace sndq
{

namespace
{

using x86::reg32;

const reg32 Notifications   = 0x8b40e8;    // count; entries of 8 bytes follow at 0x8B40EC
const reg32 Queue           = 0x8b40ec;
const reg32 Streams         = 0x8b43ec;
const reg32 TickHook        = 0x73b9c0;
const reg32 EntryPoint      = 0x7ffe0010;   // registered method that runs the callback loop
const uint32_t RingSize     = 2048;
const uint32_t RingMask     = RingSize - 1;
const uint32_t FenceWaitUs  = 2000;
const reg32 SndHandle       = 0x8b4f38;     // the SND mutex handle
const reg32 SndUnlock       = 0x72430f;     // [0x81808C]: releases it

struct Entry
{
    reg32       function;
    reg32       args[3];
    uint32_t    count;
    uint32_t    queuedUs;
};

Entry                   s_ring[RingSize];
std::atomic<uint32_t>   s_head{0};          // moved under s_pop
std::atomic<uint32_t>   s_tail{0};          // moved by the mixer only
pf::LwMutex*            s_pop = nullptr;
int                     s_fencing = 0;      // under s_pop
std::atomic<int>        s_inflight{0};
std::atomic<bool>       s_awake{false};
std::atomic<bool>       s_hookPending{false};
std::atomic<uint32_t>   s_hookTail{0};      // ring tail when the pending hook was requested
std::atomic<uint32_t>   s_hookUs{0};
std::atomic<int>        s_active{0};        // deliveries running now
NFSU_DEV_VARC(bool, s_log, false, false);
NFSU_DEV_VARC(bool, s_deferNotify, true, false);
NFSU_DEV_VARC(bool, s_deferHook, true, true);
NFSU_DEV_VARC(bool, s_inlineWithHooks, false, true);
NFSU_DEV_VARC(bool, s_hookRelease, false, true);      // audio_hook_release
NFSU_DEV_VARC(uint32_t, s_hookWaitUs, 10000, 10000);       // audio_hook_wait_ms
bool                    s_releasing = false;        // under the execution lock
win32::MethodPtr        s_unlock = nullptr;
pf::Semaphore*          s_timer = nullptr;
struct Stats
{
    std::atomic<uint32_t> delivered{0}, delaySumUs{0}, delayMaxUs{0}, over5ms{0}, over20ms{0}, overlaps{0};
    std::atomic<uint32_t> hookSets{0}, hookRuns{0}, hookLate{0}, hookLateEntries{0}, hookDelayMaxUs{0}, hookOver5ms{0};
    std::atomic<uint32_t> fences{0}, fenceWaits{0}, fenceTimeouts{0}, fenceWaitUs{0}, fenceNaps{0}, fenceDelivered{0};
    std::atomic<uint32_t> lockedBusy{0}, lockedOther{0};
    std::atomic<uint32_t> releases{0}, releaseDelivered{0}, fallbacks{0};
};
Stats s_stats;

void raiseMax(std::atomic<uint32_t>& max, uint32_t value)
{
    uint32_t old = max.load(std::memory_order_relaxed);
    while (value > old && !max.compare_exchange_weak(old, value, std::memory_order_relaxed))
        ;
}
std::atomic<bool>       s_stop{false};
std::atomic<reg32>      s_thread{0};        // thread block of the callback thread
std::atomic<reg32>      s_mixer{0};         // thread block of the last mixer thread
std::atomic<int>        s_state{0};         // 0 none, 1 starting, 2 running
NFSU_DEV_VARC(bool, s_enabled, false, true);
win32::WinApplication*  s_app = nullptr;
pf::Semaphore*          s_wake = nullptr;
pf::Semaphore*          s_nap = nullptr;
pf::Semaphore*          s_exited = nullptr;
pf::Thread*             s_host = nullptr;

void wake()
{
    if (!s_awake.exchange(true, std::memory_order_acq_rel))
        s_wake->post();
}

bool popRing(Entry& entry, bool fencer)
{
    pf::LwLockGuard guard(*s_pop);
    const uint32_t head = s_head.load(std::memory_order_relaxed);
    if ((s_fencing && !fencer) || head == s_tail.load(std::memory_order_acquire))
        return false;
    entry = s_ring[head & RingMask];
    if (!fencer)
        s_inflight.fetch_add(1, std::memory_order_seq_cst);
    s_head.store(head + 1, std::memory_order_seq_cst);
    return true;
}

void deliver(win32::WinApplication* app, x86::CPU& cpu, const Entry& entry)
{
    if (s_log)
    {
        const uint32_t delay = pf::perf::now() - entry.queuedUs;
        s_stats.delivered++;
        s_stats.delaySumUs += delay;
        raiseMax(s_stats.delayMaxUs, delay);
        if (delay > 5000)
            s_stats.over5ms++;
        if (delay > 20000)
            s_stats.over20ms++;
        if (s_active.fetch_add(1) > 0)
            s_stats.overlaps++;
    }
    if (pf::perf::enabled())
    {
        pf::perf::deferLatency(pf::perf::now() - entry.queuedUs);
        pf::perf::deferCount(pf::perf::DeferDelivered, 1);
    }
    const reg32 saved = cpu.esp;
    reg32 esp = saved;
    for (uint32_t i = entry.count; i-- > 0;)
    {
        esp -= 4;
        rc::st32(esp, entry.args[i]);
    }
    esp -= 4;
    rc::st32(esp, 0);
    cpu.esp = esp;
    app->dynamic_call(entry.function, cpu);
    cpu.esp = saved;
    if (s_log)
        s_active.fetch_sub(1);
}

void drain(win32::WinApplication* app, x86::CPU& cpu)
{
    Entry entry;
    while (!cpu.terminate && popRing(entry, false))
    {
        deliver(app, cpu, entry);
        s_inflight.fetch_sub(1, std::memory_order_release);
    }
}

void runHook(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_hookPending.exchange(false, std::memory_order_acq_rel))
        return;
    if (s_log)
    {
        const uint32_t late = s_head.load(std::memory_order_acquire) - s_hookTail.load(std::memory_order_relaxed);
        const uint32_t delay = pf::perf::now() - s_hookUs.load(std::memory_order_relaxed);
        s_stats.hookRuns++;
        if (int32_t(late) > 0)
        {
            s_stats.hookLate++;
            s_stats.hookLateEntries += late;
        }
        raiseMax(s_stats.hookDelayMaxUs, delay);
        if (delay > 5000)
            s_stats.hookOver5ms++;
    }
    if (pf::perf::enabled())
        pf::perf::deferCount(pf::perf::DeferHookRuns, 1);
    const reg32 saved = cpu.esp;
    cpu.esp -= 4;
    rc::st32(cpu.esp, 0);
    app->dynamic_call(TickHook, cpu);
    cpu.esp = saved;
}

bool work()
{
    return s_hookPending.load(std::memory_order_acquire) ||
           (!s_fencing && s_head.load(std::memory_order_acquire) != s_tail.load(std::memory_order_acquire));
}

void logStats()
{
    static uint32_t next = 0;
    const uint32_t now = pf::perf::now();
    if (!s_log || int32_t(now - next) < 0)
        return;
    next = now + 5000000;
    Stats& t = s_stats;
    const uint32_t n = t.delivered.exchange(0);
    pf::logInfo("audio defer stats 5s: %u delivered, delay avg %u max %u us, >5ms %u, >20ms %u, overlapped %u; hook sets %u "
                "runs %u (late vs later notifications %u, %u entries), hook delay max %u us, >5ms %u; fences %u, waited %u "
                "(%u timed out, %u naps, %u us), %u delivered by fences; locked notification batches %u queue busy, "
                "%u other callbacks; at SND release %u times, %u delivered; %u fallback wakes",
                n, n ? t.delaySumUs.exchange(0) / n : 0, t.delayMaxUs.exchange(0), t.over5ms.exchange(0),
                t.over20ms.exchange(0), t.overlaps.exchange(0), t.hookSets.exchange(0), t.hookRuns.exchange(0),
                t.hookLate.exchange(0), t.hookLateEntries.exchange(0), t.hookDelayMaxUs.exchange(0),
                t.hookOver5ms.exchange(0), t.fences.exchange(0), t.fenceWaits.exchange(0), t.fenceTimeouts.exchange(0),
                t.fenceNaps.exchange(0), t.fenceWaitUs.exchange(0), t.fenceDelivered.exchange(0),
                t.lockedBusy.exchange(0), t.lockedOther.exchange(0), t.releases.exchange(0),
                t.releaseDelivered.exchange(0), t.fallbacks.exchange(0));
}

// audio_hook_release: sleeps until the oldest queued entry is due; true when one still is.
bool waitOldest()
{
    for (;;)
    {
        if (s_stop.load(std::memory_order_acquire))
            return true;
        const uint32_t head = s_head.load(std::memory_order_acquire);
        if (head == s_tail.load(std::memory_order_acquire))
            return false;
        const int32_t left = int32_t(s_ring[head & RingMask].queuedUs + s_hookWaitUs - pf::perf::now());
        if (left <= 0)
        {
            if (s_log)
                s_stats.fallbacks++;
            return true;
        }
        s_timer->waitForUs(uint32_t(left));
    }
}

// Final SND mutex release by a thread with the execution lock: queued hooks run first, as the PC mixer would run them
// right after it took the mutex.
void unlockHook(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_releasing && s_head.load(std::memory_order_acquire) != s_tail.load(std::memory_order_acquire)
        && s_inflight.load(std::memory_order_acquire) == 0 && !s_fencing && !mixer::unlocked(cpu)
        && win32::Event::ownsOnce(rc::ld32(SndHandle), rc::ld32(cpu.efs + win32::teb::ThreadId)))
    {
        s_releasing = true;
        if (s_log)
            s_stats.releases++;
        Entry entry;
        while (!cpu.terminate && popRing(entry, true))
        {
            deliver(app, cpu, entry);
            if (s_log)
                s_stats.releaseDelivered++;
        }
        s_releasing = false;
        logStats();
    }
    s_unlock(app, cpu);
}

void loop(win32::WinApplication* app, x86::CPU& cpu)
{
    s_thread.store(cpu.efs, std::memory_order_release);
    s_state.store(2, std::memory_order_release);
    for (;;)
    {
        app->unlockContext(cpu);
        s_wake->wait();
        while (s_hookRelease && !s_stop.load(std::memory_order_acquire))
        {
            s_awake.store(false, std::memory_order_seq_cst);
            if (waitOldest())
                break;
            s_wake->wait();
        }
        app->lockContext(cpu);
        if (s_stop.load(std::memory_order_acquire) || cpu.terminate)
            break;
        s_awake.store(false, std::memory_order_seq_cst);
        do
        {
            drain(app, cpu);
            runHook(app, cpu);
        }
        while (!cpu.terminate && work());
        logStats();
    }
    s_thread.store(0, std::memory_order_release);
}

int threadEntry(void*)
{
    win32::Thread::registerHostThread();
    win32::scheduler::raiseCurrentLevel(win32::scheduler::LevelTimeCritical);
    if (pf::perf::enabled())
        pf::perf::trackThread("snd_callbacks");
    alignas(16) x86::CPU cpu{};
    s_app->runThread(cpu, EntryPoint, {}, true, true);
    s_exited->post();
    return 0;
}

void start(win32::WinApplication* app)
{
    int expected = 0;
    if (!s_state.compare_exchange_strong(expected, 1))
        return;
    s_app = app;
    s_stop.store(false);
    s_host = new pf::Thread("snd_callbacks", &threadEntry, nullptr, win32::scheduler::hostPriorityOf(15), 256 * 1024,
                            pf::cores::mask(pf::cores::Helper));
    s_host->start();
}

}

void configure(win32::WinApplication& app)
{
    const bool requested = NFSU_DEV_BOOL("audio_defer", true);
    const bool unlocked = NFSU_DEV_STRING("audio_mixer", "unlocked")[0] == 'u';
    NFSU_DEV_SET(s_enabled, requested && unlocked);
    NFSU_DEV_SET(s_log, NFSU_DEV_BOOL("audio_defer_log", false));
    NFSU_DEV_SET(s_deferNotify, NFSU_DEV_BOOL("audio_defer_notify", false));
    NFSU_DEV_SET(s_deferHook, NFSU_DEV_BOOL("audio_defer_hook", true));
    NFSU_DEV_SET(s_inlineWithHooks, NFSU_DEV_BOOL("audio_inline_with_hooks", false)
                     || NFSU_DEV_BOOL("audio_engine_inline", true));
    NFSU_DEV_SET(s_hookRelease, NFSU_DEV_BOOL("audio_hook_release", true) && s_deferHook && !s_deferNotify);
    NFSU_DEV_SET(s_hookWaitUs, uint32_t(NFSU_DEV_INT("audio_hook_wait_ms", 10)) * 1000);
    pf::logInfo("audio defer: %s (audio_defer=%s, audio_mixer=%s), tick hooks %s", s_enabled ? "on" : "off",
                requested ? "on" : "off", unlocked ? "unlocked" : "locked",
                !s_enabled || !s_hookRelease ? "on the callback thread" : "at the SND mutex release");
    if (s_enabled && s_hookRelease)
        pf::logInfo("audio defer: callback thread runs hooks left %u ms", unsigned(s_hookWaitUs / 1000));
    if (!s_enabled)
        return;
    s_app = &app;
    s_pop = new pf::LwMutex("snd_pop");
    s_wake = new pf::Semaphore("snd_wake", 0);
    s_nap = new pf::Semaphore("snd_nap", 0);
    s_exited = new pf::Semaphore("snd_exit", 0);
    s_timer = new pf::Semaphore("snd_timer", 0);
    app.registerMethod(EntryPoint, {"snd_callbacks", &loop});
    if (s_hookRelease)
        s_unlock = app.replaceMethod(SndUnlock, &unlockHook);
}

bool enabled()
{
    return s_enabled;
}

bool idle()
{
    return !s_enabled || s_state.load(std::memory_order_acquire) != 2
        || (s_head.load(std::memory_order_acquire) == s_tail.load(std::memory_order_acquire)
            && s_inflight.load(std::memory_order_acquire) == 0);
}

bool inlineAllowed()
{
    return idle() || (s_inlineWithHooks && !s_deferNotify);
}

bool logging()
{
    return s_log;
}

void countLocked(bool busy)
{
    if (s_log)
        (busy ? s_stats.lockedBusy : s_stats.lockedOther)++;
}

void attach(win32::WinApplication* app, x86::CPU& cpu)
{
    if (s_state.load(std::memory_order_acquire) == 0)
        start(app);
    if (s_mixer.load(std::memory_order_relaxed) != cpu.efs)
    {
        s_mixer.store(cpu.efs, std::memory_order_relaxed);
        fence(app, cpu);
    }
}

bool hookDeferred()
{
    return s_enabled && s_deferHook;
}

bool queueNotifications()
{
    if (!s_deferNotify)
        return false;
    const uint32_t count = uint32_t(rc::ld32(Notifications));
    const uint32_t tail = s_tail.load(std::memory_order_relaxed);
    const uint32_t head = s_head.load(std::memory_order_acquire);
    if (count > RingSize - (tail - head) || s_state.load(std::memory_order_acquire) != 2)
    {
        if (pf::perf::enabled())
            pf::perf::deferCount(pf::perf::DeferFallbacks, 1);
        return false;
    }
    const uint32_t now = pf::perf::now();
    for (uint32_t i = 0; i < count; ++i)
    {
        const reg32 slot = Queue + 8 * i;
        const reg32 kind = rc::ld16(slot);
        const reg32 index = rc::ld16(slot + 2);
        const reg32 value = rc::ld32(slot + 4);
        const reg32 object = rc::ld32(Streams + 4 * index);
        Entry& entry = s_ring[(tail + i) & RingMask];
        entry.queuedUs = now;
        entry.args[2] = rc::ld32(object + 0x5c);
        if (kind)
        {
            entry.function = rc::ld32(object + 0x54);
            entry.args[0] = value;
            entry.args[1] = entry.args[2];
            entry.count = 2;
        }
        else
        {
            entry.function = rc::ld32(object + 0x58);
            entry.args[0] = index;
            entry.args[1] = value;
            entry.count = 3;
        }
    }
    rc::st32(Notifications, 0);
    s_tail.store(tail + count, std::memory_order_release);
    if (pf::perf::enabled())
    {
        pf::perf::deferCount(pf::perf::DeferQueued, count);
        pf::perf::deferDepth(tail + count - s_head.load(std::memory_order_relaxed));
    }
    wake();
    return true;
}

void requestHook()
{
    if (s_state.load(std::memory_order_acquire) != 2)
        return;
    const uint32_t tail = s_tail.load(std::memory_order_relaxed);
    if (RingSize - (tail - s_head.load(std::memory_order_acquire)) > 0)
    {
        Entry& entry = s_ring[tail & RingMask];
        entry.function = TickHook;
        entry.count = 0;
        entry.queuedUs = pf::perf::now();
        if (s_log)
            s_stats.hookSets++;
        s_tail.store(tail + 1, std::memory_order_release);
        if (pf::perf::enabled())
            pf::perf::deferCount(pf::perf::DeferHookSets, 1);
        wake();
        return;
    }
    if (!s_hookPending.load(std::memory_order_acquire))
    {
        s_hookTail.store(s_tail.load(std::memory_order_relaxed), std::memory_order_relaxed);
        s_hookUs.store(pf::perf::now(), std::memory_order_relaxed);
    }
    if (s_log)
        s_stats.hookSets++;
    s_hookPending.store(true, std::memory_order_release);
    if (pf::perf::enabled())
        pf::perf::deferCount(pf::perf::DeferHookSets, 1);
    wake();
}

void fence(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_enabled || s_state.load(std::memory_order_acquire) != 2)
        return;
    if (s_head.load(std::memory_order_acquire) == s_tail.load(std::memory_order_acquire) &&
        s_inflight.load(std::memory_order_acquire) == 0)
        return;
    {
        pf::LwLockGuard guard(*s_pop);
        ++s_fencing;
    }
    if (s_log)
        s_stats.fences++;
    // The mixer holds the SND mutex here; an in-flight hook usually waits for it, so waiting would only time out.
    if (s_thread.load(std::memory_order_acquire) != cpu.efs && s_mixer.load(std::memory_order_relaxed) != cpu.efs)
    {
        const uint32_t start = pf::perf::now();
        uint32_t naps = 0;
        while (s_inflight.load(std::memory_order_acquire) != 0 && pf::perf::now() - start < FenceWaitUs)
        {
            app->unlockContext(cpu);
            s_nap->waitForUs(100);
            app->lockContext(cpu);
            ++naps;
        }
        if (s_log && naps)
        {
            s_stats.fenceWaits++;
            s_stats.fenceNaps += naps;
            s_stats.fenceWaitUs += pf::perf::now() - start;
            if (s_inflight.load(std::memory_order_acquire) != 0)
                s_stats.fenceTimeouts++;
        }
    }
    Entry entry;
    while (!cpu.terminate && popRing(entry, true))
    {
        deliver(app, cpu, entry);
        if (s_log)
            s_stats.fenceDelivered++;
    }
    {
        pf::LwLockGuard guard(*s_pop);
        --s_fencing;
    }
    if (work())
        wake();
}

void shutdown()
{
    if (s_state.load() != 2)
        return;
    s_stop.store(true);
    s_wake->post();
    s_timer->post();
    if (s_exited->waitFor(500))
        s_host->join();
}

}}
