// EA SND voice render outside the SND mutex (audio_mixer_split); other threads wait in SND functions touching render
// state. Dev watch and check of their holds.

#include "audio_split.h"
#include "audio_mixer.h"

#include <lib/event.h>
#include <lib/teb.h>
#include <platform/config.h>
#include <platform/switches.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <recomp.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace ug2
{

namespace split
{

namespace
{

using x86::reg32;

const reg32 SndHandleSlot   = 0x8b4f38;
const reg32 VoiceArray      = 0x8b4a00;
const reg32 VoiceCount      = 0x8b47f8;
const reg32 ChannelCount    = 0x8b47f9;
const reg32 VoiceSize       = 0x5c;
const reg32 SndHeap         = 0x8b4674;     // +0 base, +4 record table (8-byte records at non-positive indexes), +0x14 -count
const reg32 Handles         = 0x8b466c;
const reg32 HandleCount     = 0x8b460c;
const reg32 Drivers         = 0x8b4724;
const reg32 PostFilters     = 0x8b4940;
const reg32 PostOutputs     = 0x8b4928;
const reg32 AuxCount        = 0x8180b8;
const reg32 AuxEffects      = 0x8b47cc;
const reg32 Notifications   = 0x8b40e8;     // count, 8-byte entries, then 32 stream object pointers at 0x8B43EC
const reg32 NotifyStreams   = 0x8b43ec;
const int   MaxModules      = 2048;
const int   MaxVoices       = 256;

enum State { Closed, Rendering, Finishing };

bool s_split = false;                       // audio_mixer_split
NFSU_DEV_VARC(bool, s_check, false, false);                       // audio_split_check
NFSU_DEV_VARC(bool, s_watch, false, false);                       // dev_snd_watch
std::atomic<int> s_state{Closed};
std::atomic<int> s_waiters{0};
pf::Semaphore* s_done = nullptr;
reg32 s_mixer = 0;                          // thread block of the mixer while its window is open
uint32_t s_opened = 0;
uint32_t s_generation[MaxVoices];           // bumped by voice starts and ends
uint32_t s_gainWrites[MaxVoices];

struct Counters
{
    std::atomic<uint32_t> windows{0}, windowUs{0}, paused{0}, refused{0}, waited{0}, waitedUs{0}, waitedMaxUs{0},
        back{0}, backUs{0}, backMaxUs{0}, skippedEnds{0}, relocked{0};
};
Counters s_count;
uint32_t s_countSince = 0;

void raiseMax(std::atomic<uint32_t>& max, uint32_t value)
{
    uint32_t old = max.load(std::memory_order_relaxed);
    while (value > old && !max.compare_exchange_weak(old, value, std::memory_order_relaxed))
        ;
}

// ---- Footprint: the memory the voice render reads and writes -------------------------------------------------------

struct KindName
{
    reg32       render;
    const char* name;
    bool        second;
    int         object;                     // 1: EA-XA state at +0x1C, 2: MPEG stream at +0x1C
};

const KindName s_kinds[] = {
    {0x72802d, "mpegA", false, 2}, {0x7282ef, "mpegB", false, 2}, {0x7285bd, "mpegC", false, 2},
    {0x72a138, "bufsrc", false, 0}, {0x72a2f3, "hold", false, 0}, {0x72a3d9, "sum", true, 0},
    {0x72a541, "delay", false, 0}, {0x72a672, "gain", false, 0}, {0x72a7a1, "filtA", false, 0},
    {0x72a8c5, "filtB", false, 0}, {0x72a9b3, "filtC", false, 0}, {0x72aa55, "filtD", false, 0},
    {0x72c941, "gate", false, 0}, {0x72cac7, "out16", false, 0}, {0x72d18d, "resamp", false, 0},
    {0x732866, "decloop", false, 0}, {0x732980, "decstream", false, 0}, {0x732b7e, "decbank", false, 0},
    {0x732c84, "xastream", false, 1}, {0x732dfb, "xaloop", false, 1}, {0x733023, "xasample", false, 1},
    {0x7330f5, "pcmstream", false, 0}, {0x7331ff, "pcmloop", false, 0}, {0x7332a3, "pcmsample", false, 0},
};
const int KindCount = int(sizeof(s_kinds) / sizeof(s_kinds[0]));

enum Label { LVoice, LGlobal, LNotify, LSndStream, LHandle, LDriver, LDecoder, LStream, LUnknown, LModule };

const char* labelName(int label)
{
    static const char* const names[] = {"voice", "global", "notify", "sndstream", "handle", "driver", "xastate",
                                        "mpegstream", "module?"};
    return label >= LModule ? s_kinds[label - LModule].name : names[label];
}

struct Range
{
    reg32   at;
    reg32   size;
    int     label;
};

struct Footprint
{
    std::vector<Range>      ranges;
    std::vector<uint8_t>    bytes;
};

std::unordered_map<reg32, reg32> s_sizes;

void loadSizes()
{
    s_sizes.clear();
    const reg32 heap = rc::ld32(SndHeap);
    if (!heap)
        return;
    const reg32 base = rc::ld32(heap);
    const reg32 table = rc::ld32(heap + 4);
    const x86::sreg32 count = x86::sreg32(rc::ld32(heap + 0x14));
    for (x86::sreg32 i = 0; i > count && i > -16384; --i)
    {
        const reg32 record = table + reg32(i) * 8;
        s_sizes[base + rc::ld32(record)] = rc::ld32(record + 4);
    }
}

reg32 blockSize(reg32 at, reg32 fallback)
{
    const auto found = s_sizes.find(at);
    return found == s_sizes.end() || found->second == 0 || found->second > 0x10000 ? fallback : found->second;
}

void addRange(Footprint& f, reg32 at, reg32 size, int label)
{
    if (at >= 0x10000 && size)
        f.ranges.push_back({at, size, label});
}

void addModule(Footprint& f, reg32 module, int depth, int& budget)
{
    if (!module || depth > 16 || --budget < 0)
        return;
    const reg32 render = rc::ld32(module);
    int kind = -1;
    for (int i = 0; i < KindCount; ++i)
        if (s_kinds[i].render == render)
            kind = i;
    addRange(f, module, blockSize(module, 0x60), kind < 0 ? LUnknown : LModule + kind);
    if (kind < 0)
        return;
    if (s_kinds[kind].object)
    {
        const reg32 object = rc::ld32(module + 0x1c);
        addRange(f, object, blockSize(object, s_kinds[kind].object == 1 ? 0x20 : 0x40),
                 s_kinds[kind].object == 1 ? LDecoder : LStream);
    }
    addModule(f, rc::ld32(module + 8), depth + 1, budget);
    if (s_kinds[kind].second)
        addModule(f, rc::ld32(module + 0xc), depth + 1, budget);
}

void capture(Footprint& f)
{
    f.ranges.clear();
    loadSizes();
    addRange(f, 0x8b47c0, 0x280, LGlobal);
    addRange(f, AuxCount, 4, LGlobal);
    addRange(f, Notifications, NotifyStreams - Notifications, LNotify);
    for (reg32 i = 0; i < 32; ++i)
    {
        const reg32 stream = rc::ld32(NotifyStreams + 4 * i);
        addRange(f, stream, blockSize(stream, 0x80), LSndStream);
    }
    const reg32 voices = rc::ld32(VoiceArray);
    const reg32 count = rc::ld8(VoiceCount);
    int budget = MaxModules;
    addRange(f, voices, count * VoiceSize, LVoice);
    for (reg32 i = 0; i < count; ++i)
        addModule(f, rc::ld32(voices + i * VoiceSize + 0x40), 0, budget);
    const reg32 channels = std::min<reg32>(rc::ld8(ChannelCount), 6);
    for (reg32 c = 0; c < channels; ++c)
    {
        addModule(f, rc::ld32(PostFilters + 4 * c), 0, budget);
        addModule(f, rc::ld32(PostOutputs + 4 * c), 0, budget);
    }
    const x86::sreg32 aux = x86::sreg32(rc::ld32(AuxCount));
    const reg32 effects = rc::ld32(AuxEffects);
    for (x86::sreg32 i = 0; i < aux && i < 16 && effects; ++i)
    {
        const reg32 effect = rc::ld32(effects + 4 * reg32(i));
        const reg32 table = effect ? rc::ld32(effect + 0x40) : 0;
        for (reg32 c = 0; table && c < channels; ++c)
            addModule(f, rc::ld32(table + 4 + 4 * c), 0, budget);
    }
    const x86::sreg32 handles = x86::sreg16(rc::ld16(HandleCount));
    if (handles > 0 && handles < 256)
    {
        addRange(f, rc::ld32(Handles), reg32(handles) * 0x90, LHandle);
        addRange(f, rc::ld32(Drivers), reg32(handles) * 0x88, LDriver);
    }
    size_t total = 0;
    for (const Range& r : f.ranges)
        total += r.size;
    f.bytes.resize(total);
    size_t at = 0;
    for (const Range& r : f.ranges)
    {
        memcpy(f.bytes.data() + at, rc::host(r.at), r.size);
        at += r.size;
    }
}

uint64_t fieldKey(const Range& r, reg32 offset)
{
    const reg32 word = offset & ~3u;
    const reg32 field = r.label == LVoice ? word % VoiceSize : r.label == LHandle ? word % 0x90
                      : r.label == LDriver ? word % 0x88 : r.label == LGlobal || r.label == LNotify ? r.at + word
                      : word;
    return (uint64_t(r.label) << 32) | field;
}

bool renderLabel(uint64_t key)
{
    const int label = int(key >> 32);
    return label != LHandle && label != LDriver;
}

// Fields changed since the capture, except those in skip.
void changedFields(const Footprint& before, const std::set<uint64_t>* skip, std::vector<uint64_t>& out)
{
    out.clear();
    size_t at = 0;
    for (const Range& r : before.ranges)
    {
        const uint8_t* old = before.bytes.data() + at;
        const uint8_t* now = rc::host(r.at);
        at += r.size;
        if (!memcmp(old, now, r.size))
            continue;
        reg32 last = ~0u;
        for (reg32 o = 0; o < r.size; ++o)
        {
            if (old[o] == now[o] || (o & ~3u) == last)
                continue;
            last = o & ~3u;
            const uint64_t key = fieldKey(r, o);
            if (!skip || !skip->count(key))
                out.push_back(key);
        }
    }
}

// ---- Watch and check ----------------------------------------------------------------------------------------------

struct Totals
{
    uint32_t holds = 0, waits = 0, waitUs = 0, holdUs = 0, holdMaxUs = 0, changedRender = 0, inWindow = 0,
             gatedHolds = 0, ungatedChanges = 0;
};

struct Holder
{
    reg32       thread = 0;
    int         depth = 0;
    uint32_t    start = 0;
    bool        inWindow = false;
    bool        gated = false;              // waited in, or passed, a gated function in this hold
    Footprint   before;
};

Holder s_holders[8];
std::map<reg32, Totals> s_threads;
std::map<uint64_t, uint32_t> s_fields;          // field key -> holds that changed it
std::map<uint64_t, uint32_t> s_ungated;         // check: fields in-window holds changed without passing a gate
std::set<uint64_t> s_mixerWrites;               // fields the mixer changed in render loops no other hold overlapped
uint32_t s_since = 0;
uint32_t s_otherHolds = 0;
Footprint s_loop;
uint32_t s_loopHolds = 0;
bool s_loopOpen = false;
uint32_t s_loops = 0, s_loopsClean = 0;
std::vector<uint64_t> s_changed;
Footprint s_render;                             // at the window's start, to learn what the render writes
uint32_t s_renderHolds = 0;

Holder* holderOf(reg32 thread, bool create)
{
    for (Holder& h : s_holders)
        if (h.thread == thread)
            return &h;
    if (!create)
        return nullptr;
    for (Holder& h : s_holders)
        if (!h.thread)
        {
            h.thread = thread;
            return &h;
        }
    return nullptr;
}

void logTop(const char* title, std::map<uint64_t, uint32_t>& fields)
{
    std::vector<std::pair<uint64_t, uint32_t>> sorted(fields.begin(), fields.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    char line[1400];
    int used = snprintf(line, sizeof(line), "%s:", title);
    for (size_t i = 0; i < sorted.size() && i < 48 && used < int(sizeof(line)) - 40; ++i)
        used += snprintf(line + used, sizeof(line) - used, " %s+%x %u", labelName(int(sorted[i].first >> 32)),
                         unsigned(sorted[i].first & 0xffffffff), sorted[i].second);
    pf::logInfo("%s", line);
    fields.clear();
}

void logWatch(uint32_t now)
{
    if (int32_t(now - s_since) < 5000000)
        return;
    s_since = now;
    for (const auto& t : s_threads)
    {
        const Totals& s = t.second;
        pf::logInfo("snd watch 5s: thread %08x: %u holds %.2f ms (max %.2f), %u waits %.2f ms, %u changed render "
                    "state; %u in the render window, %u of them in a gated function, %u changed render state without",
                    unsigned(t.first), s.holds, s.holdUs / 1000.0, s.holdMaxUs / 1000.0, s.waits, s.waitUs / 1000.0,
                    s.changedRender, s.inWindow, s.gatedHolds, s.ungatedChanges);
    }
    logTop("snd watch 5s fields", s_fields);
    if (s_check)
        logTop("split check 5s: fields changed in the window outside gated functions", s_ungated);
    std::map<uint64_t, uint32_t> mixer;
    for (uint64_t k : s_mixerWrites)
        mixer[k] = 1;
    pf::logInfo("snd watch: %u render loops, %u with no other hold, %u fields the mixer writes", s_loops, s_loopsClean,
                unsigned(s_mixerWrites.size()));
    logTop("snd watch: mixer-written fields", mixer);
    s_threads.clear();
}

void watch(bool acquired, uint32_t thread, uint32_t esp, uint32_t waitUs)
{
    RC_USE(esp);
    if (pf::perf::sndThread(thread))
        return;
    Holder* h = holderOf(thread, true);
    if (!h)
        return;
    const uint32_t now = pf::perf::now();
    if (acquired)
    {
        if (h->depth++ > 0)
            return;
        ++s_otherHolds;
        h->start = now;
        h->inWindow = s_state.load(std::memory_order_acquire) != Closed;
        h->gated = false;
        Totals& t = s_threads[thread];
        if (waitUs)
        {
            ++t.waits;
            t.waitUs += waitUs;
        }
        capture(h->before);
        return;
    }
    if (h->depth == 0 || --h->depth > 0)
        return;
    Totals& t = s_threads[thread];
    const uint32_t held = now - h->start;
    ++t.holds;
    t.holdUs += held;
    t.holdMaxUs = std::max(t.holdMaxUs, held);
    t.inWindow += h->inWindow;
    t.gatedHolds += h->inWindow && h->gated;
    changedFields(h->before, h->inWindow ? &s_mixerWrites : nullptr, s_changed);
    bool render = false;
    for (uint64_t k : s_changed)
    {
        ++s_fields[k];
        if (!renderLabel(k))
            continue;
        render = true;
        if (h->inWindow && !h->gated)
            ++s_ungated[k];
    }
    t.changedRender += render;
    t.ungatedChanges += render && h->inWindow && !h->gated;
    logWatch(now);
}

// ---- Window -------------------------------------------------------------------------------------------------------

void logCounts()
{
    const uint32_t now = pf::perf::now();
    if (int32_t(now - s_countSince) < 5000000)
        return;
    s_countSince = now;
    Counters& c = s_count;
    const uint32_t windows = c.windows.exchange(0);
    pf::logInfo("audio split 5s: %u voice renders outside the SND mutex (%.2f ms each), %u not split; %u paused at a voice "
                "end (%u ends left to another thread's stop, %u renders relocked after the audit); mixer waited %u "
                "times to take the mutex back, %.2f ms (max %.2f); other threads waited %u times in SND functions, "
                "%.2f ms (max %.2f)", windows, windows ? c.windowUs.exchange(0) / 1000.0 / windows : 0.0,
                c.refused.exchange(0), c.paused.exchange(0), c.skippedEnds.exchange(0), c.relocked.exchange(0),
                c.back.exchange(0), c.backUs.exchange(0) / 1000.0, c.backMaxUs.exchange(0) / 1000.0,
                c.waited.exchange(0), c.waitedUs.exchange(0) / 1000.0, c.waitedMaxUs.exchange(0) / 1000.0);
}

// Mixer: retakes the SND mutex; true if another thread held it.
bool takeBack(x86::CPU& cpu)
{
    const uint32_t holds = s_otherHolds;
    if (s_watch && holds == s_renderHolds)
    {
        changedFields(s_render, nullptr, s_changed);
        for (uint64_t k : s_changed)
            s_mixerWrites.insert(k);
    }
    s_state.store(Finishing, std::memory_order_seq_cst);
    const int waiters = s_waiters.load(std::memory_order_seq_cst);
    if (waiters > 0)
        s_done->post(waiters);
    const reg32 handle = rc::ld32(SndHandleSlot);
    const reg32 self = rc::ld32(cpu.efs + win32::teb::ThreadId);
    const uint32_t start = pf::perf::now();
    bool contended = false;
    if (win32::Event::wait(cpu, &handle, 1, false, 0, self) != 0)
    {
        contended = true;
        win32::Event::wait(cpu, &handle, 1, false, -1, self);
        const uint32_t waited = pf::perf::now() - start;
        ++s_count.back;
        s_count.backUs += waited;
        raiseMax(s_count.backMaxUs, waited);
    }
    s_state.store(Closed, std::memory_order_seq_cst);
    s_mixer = 0;
    if (pf::perf::enabled())
        pf::perf::sndAcquired(handle, cpu.efs);
    ++s_count.windows;
    s_count.windowUs += pf::perf::now() - s_opened;
    return contended || holds != s_otherHolds;
}

void bump(reg32 voice)
{
    if (voice < MaxVoices)
        ++s_generation[voice];
}

}

// Other threads: waits while the mixer renders unlocked.
void gate(win32::WinApplication* app, const x86::CPU& cpu)
{
    if (s_state.load(std::memory_order_seq_cst) == Closed || cpu.efs == s_mixer)
        return;
    if (s_watch)
        if (Holder* h = holderOf(cpu.efs, false))
            h->gated = true;
    if (s_state.load(std::memory_order_seq_cst) != Rendering)
        return;
    const uint32_t start = pf::perf::now();
    app->unlockContext(cpu);
    s_waiters.fetch_add(1, std::memory_order_seq_cst);
    while (s_state.load(std::memory_order_seq_cst) == Rendering)
        s_done->waitForUs(1000);
    s_waiters.fetch_sub(1, std::memory_order_seq_cst);
    app->lockContext(cpu);
    const uint32_t waited = pf::perf::now() - start;
    ++s_count.waited;
    s_count.waitedUs += waited;
    raiseMax(s_count.waitedMaxUs, waited);
}

bool on()
{
    return s_split;
}

uint32_t gainWrites(reg32 voice)
{
    return voice < MaxVoices ? s_gainWrites[voice] : 0;
}

Window::Window(win32::WinApplication* app, x86::CPU& cpu)
    :   m_cpu(cpu)
    ,   m_open(false)
{
    RC_USE(app);
    if (!s_split)
        return;
    const reg32 handle = rc::ld32(SndHandleSlot);
    const reg32 self = rc::ld32(cpu.efs + win32::teb::ThreadId);
    if (!win32::Event::ownsOnce(handle, self))
    {
        ++s_count.refused;
        return;
    }
    if (pf::perf::enabled())
        pf::perf::sndReleased(handle, cpu.efs);
    if (s_watch)
    {
        capture(s_render);
        s_renderHolds = s_otherHolds;
    }
    s_mixer = cpu.efs;
    s_opened = pf::perf::now();
    s_state.store(Rendering, std::memory_order_seq_cst);
    win32::Event::releaseMutex(handle, self);
    m_open = true;
}

Window::~Window()
{
    if (m_open && s_mixer == m_cpu.efs)
        takeBack(m_cpu);
    if (m_open)
        logCounts();
}

void loopBegin(const x86::CPU& cpu)
{
    RC_USE(cpu);
    if (!s_watch)
        return;
    capture(s_loop);
    s_loopHolds = s_otherHolds;
    s_loopOpen = true;
}

void loopEnd(const x86::CPU& cpu)
{
    RC_USE(cpu);
    if (!s_loopOpen)
        return;
    s_loopOpen = false;
    ++s_loops;
    if (s_loopHolds != s_otherHolds)
        return;
    ++s_loopsClean;
    changedFields(s_loop, nullptr, s_changed);
    for (uint64_t k : s_changed)
        s_mixerWrites.insert(k);
}

void configure(win32::WinApplication& app)
{
    RC_USE(app);
    s_split = pf::Config::getBool("audio_mixer_split", true)
              && strcmp(NFSU_DEV_STRING("audio_mixer", "unlocked"), "unlocked") == 0
              && !NFSU_DEV_BOOL("audio_snd_yield", false);
    NFSU_DEV_SET(s_check, s_split && NFSU_DEV_BOOL("audio_split_check", false) && pf::perf::enabled());
    NFSU_DEV_SET(s_watch, (NFSU_DEV_BOOL("dev_snd_watch", false) || s_check) && pf::perf::enabled());
    if (s_split)
    {
        s_done = new pf::Semaphore("snd_split", 0);
        pf::logInfo("audio split: voice renders outside the SND mutex (audio_mixer_split)%s",
                    s_check ? ", other threads' changes checked (audio_split_check)" : "");
    }
    if (s_watch)
    {
        pf::perf::setSndWatch(&watch);
        pf::logInfo("snd watch: SND mutex holds of other threads, with the mixer state they change (dev_snd_watch)");
    }
}

}

void sub_7292ac_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_728df9_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_728fd2_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_72905d_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_729087_guest(win32::WinApplication* app, x86::CPU& cpu);

#define UG2_SPLIT_GATED(address)                                                \
    void sub_##address##_guest(win32::WinApplication* app, x86::CPU& cpu);     \
    namespace native {                                                          \
    void sub_##address(win32::WinApplication* app, x86::CPU& cpu)              \
    {                                                                           \
        split::gate(app, cpu);                                                  \
        sub_##address##_guest(app, cpu);                                        \
    }                                                                           \
    }

UG2_SPLIT_GATED(728f75)
UG2_SPLIT_GATED(7290af)
UG2_SPLIT_GATED(72d3cb)
UG2_SPLIT_GATED(72d3ec)
UG2_SPLIT_GATED(72d4ac)
UG2_SPLIT_GATED(72d548)
UG2_SPLIT_GATED(72d8e9)
UG2_SPLIT_GATED(723138)
UG2_SPLIT_GATED(7231d4)
UG2_SPLIT_GATED(723515)
UG2_SPLIT_GATED(723608)
UG2_SPLIT_GATED(723649)
UG2_SPLIT_GATED(72366a)
UG2_SPLIT_GATED(723699)
UG2_SPLIT_GATED(7237ef)
UG2_SPLIT_GATED(72387b)
UG2_SPLIT_GATED(72c31e)
UG2_SPLIT_GATED(722442)
UG2_SPLIT_GATED(727a12)
UG2_SPLIT_GATED(7279d6)
UG2_SPLIT_GATED(72653c)
UG2_SPLIT_GATED(7268bf)
UG2_SPLIT_GATED(726cee)
UG2_SPLIT_GATED(726db3)
UG2_SPLIT_GATED(724894)

namespace native
{

using x86::reg32;

// Voice start (voice index, ...).
void sub_728df9(win32::WinApplication* app, x86::CPU& cpu)
{
    split::gate(app, cpu);
    split::bump(rc::ld32(cpu.esp + 4));
    sub_728df9_guest(app, cpu);
}

// Voice teardown (voice index).
void sub_728fd2(win32::WinApplication* app, x86::CPU& cpu)
{
    split::gate(app, cpu);
    split::bump(rc::ld32(cpu.esp + 4));
    sub_728fd2_guest(app, cpu);
}

// Voice gain setters (voice index, channel, gain).
void sub_72905d(win32::WinApplication* app, x86::CPU& cpu)
{
    split::gate(app, cpu);
    const reg32 voice = rc::ld32(cpu.esp + 4);
    if (voice < split::MaxVoices)
        ++split::s_gainWrites[voice];
    sub_72905d_guest(app, cpu);
}

void sub_729087(win32::WinApplication* app, x86::CPU& cpu)
{
    split::gate(app, cpu);
    const reg32 voice = rc::ld32(cpu.esp + 4);
    if (voice < split::MaxVoices)
        ++split::s_gainWrites[voice];
    sub_729087_guest(app, cpu);
}

// Render voice end (index): retakes the SND mutex first.
void sub_7292ac(win32::WinApplication* app, x86::CPU& cpu)
{
    if (split::s_mixer && split::s_mixer == cpu.efs)
    {
        const reg32 index = rc::ld32(cpu.esp + 4);
        const uint32_t generation = index < split::MaxVoices ? split::s_generation[index] : 0;
        ++split::s_count.paused;
        if (split::takeBack(cpu))
        {
            if (index < split::MaxVoices && split::s_generation[index] != generation)
            {
                ++split::s_count.skippedEnds;
                cpu.esp += 4;
                return;
            }
            if (mixer::reaudit(app, cpu))
                ++split::s_count.relocked;
        }
    }
    sub_7292ac_guest(app, cpu);
}

}

}
