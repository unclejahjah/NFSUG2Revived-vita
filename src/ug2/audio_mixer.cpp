// EA SND mixer render loop 0x725C8E outside the execution lock, with audits that retake it on failure. Native x87
// kernels, bit-identical to the guest's.

#include "audio_mixer.h"
#include "audio_cull.h"
#include "audio_defer.h"
#include "audio_kernels.h"
#include "audio_split.h"
#include "audio_stats.h"

#include <lib/event.h>
#include <lib/heap.h>
#include <lib/mutex.h>
#include <lib/teb.h>
#include <lib/winapp.h>
#include <platform/switches.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sampler.h>
#include <platform/sync.h>
#include <recomp.h>
#include <x86.h>

#include <algorithm>
#include <cmath>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

namespace ug2
{

void sub_725c8e_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_721e13_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_721de6_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_72382f_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_729574_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_72c360_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_72c480_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_733700_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_734bf4_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_72cac7_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_734a81(win32::WinApplication* app, x86::CPU& cpu);
void sub_7216c6_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace mixer
{

namespace
{

std::atomic<x86::reg32> s_thread{0};    // thread block (fs) in the render loop without the lock, else 0
win32::Mutex* s_lock = nullptr;         // the execution lock that thread gave up
bool s_changed = true;                  // SND state may have changed under the lock since the driver audit
NFSU_DEV_VARC(bool, s_unlocked, false, true);                // audio_mixer=unlocked
#ifdef DEV_SETTINGS
bool s_nativeKernels = true;            // audio_native_kernels
bool s_neonKernels = false;            // audio_neon_kernels
#else
const bool s_nativeKernels = AUDIO_FAST_PATHS != 0;
const bool s_neonKernels = AUDIO_FAST_PATHS != 0;
#endif
NFSU_DEV_VARC(bool, s_hash, false, false);                    // audio_hash
NFSU_DEV_VARC(bool, s_sndYield, false, false);                // audio_snd_yield
NFSU_DEV_VARC(bool, s_stay, false, true);                    // audio_mixer_stay
NFSU_DEV_VARC(FILE*, s_dump, nullptr, nullptr);                 // audio_dump: raw 16-bit output blocks
win32::Event* s_sndMutex = nullptr;     // the SND mutex while the loop runs with audio_snd_yield
bool s_midLocked = false;               // execution lock taken after a yield failed the render audit
pf::Semaphore* s_handoff = nullptr;

const x86::reg32 SndHandleSlot = 0x8b4f38;
const uint32_t   HandoffUs     = 500;

struct YieldStats
{
    uint32_t yields = 0, handedOver = 0, waitUs = 0, waitMaxUs = 0, relocked = 0, voiceChanged = 0, arrayChanged = 0;
    uint32_t since = 0;
};
YieldStats s_yieldStats;

void setLock(win32::WinApplication* app, const x86::CPU& cpu, win32::Mutex* lock)
{
    app->getMemory<win32::Mutex*>(cpu.efs + win32::teb::HostLock) = lock;
}

void logYields()
{
    YieldStats& t = s_yieldStats;
    const uint32_t now = pf::perf::now();
    if (int32_t(now - t.since) < 5000000)
        return;
    pf::logInfo("audio snd yield 5s: %u yields (%u taken by the waiter), mixer waited %u us (max %u), %u relocked after "
                "the audit, %u changed the current voice, %u the voice array", t.yields, t.handedOver, t.waitUs,
                t.waitMaxUs, t.relocked, t.voiceChanged, t.arrayChanged);
    t = YieldStats();
    t.since = now;
}

// Mixer without the lock: hands the SND mutex to a waiting game thread and takes it back; true when it did. A
// handoff nobody takes ends the yields for this render loop.
bool yieldSnd(win32::WinApplication* app, x86::CPU& cpu)
{
    win32::Event* const mutex = s_sndMutex;
    if (!mutex || mutex->normalWaiters() == 0)
        return false;
    const x86::reg32 handle = rc::ld32(SndHandleSlot);
    const x86::reg32 self = rc::ld32(cpu.efs + win32::teb::ThreadId);
    if (!win32::Event::ownsOnce(handle, self) || !win32::Event::releaseMutex(handle, self))
        return false;
    const bool timed = pf::perf::enabled();
    if (timed)
        pf::perf::sndReleased(handle, cpu.efs);
    const uint32_t start = pf::perf::now();
    bool taken = false;
    const int waiters = mutex->normalWaiters();
    while (!(taken = win32::Event::mutexOwner(handle) != 0 || mutex->normalWaiters() < waiters)
           && pf::perf::now() - start < HandoffUs)
        s_handoff->waitForUs(20);
    win32::Event::wait(cpu, &handle, 1, false, -1, self);
    if (!taken)
        s_sndMutex = nullptr;
    if (timed)
        pf::perf::sndAcquired(handle, cpu.efs);
    s_changed = true;
    const uint32_t waited = pf::perf::now() - start;
    YieldStats& t = s_yieldStats;
    ++t.yields;
    t.handedOver += taken;
    t.waitUs += waited;
    t.waitMaxUs = std::max(t.waitMaxUs, waited);
    return true;
}

}

bool unlocked(const x86::CPU& cpu)
{
    if (s_thread.load(std::memory_order_relaxed) != cpu.efs)
        return false;
    win32::Mutex* lock;
    memcpy(&lock, rc::host(cpu.efs + win32::teb::HostLock), sizeof(lock));
    return !lock;
}

Locked::Locked(win32::WinApplication* app, const x86::CPU& cpu, int cause)
    :   m_app(app)
    ,   m_cpu(cpu)
    ,   m_taken(unlocked(cpu))
    ,   m_cause(cause)
{
    if (!m_taken)
        return;
    s_thread.store(0, std::memory_order_relaxed);
    setLock(m_app, m_cpu, s_lock);
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    m_app->lockContext(m_cpu);
    if (timed)
        pf::perf::sndLockWait(pf::perf::SndCause(m_cause), pf::perf::now() - start);
}

Locked::~Locked()
{
    if (!m_taken)
        return;
    m_app->unlockContext(m_cpu);
    setLock(m_app, m_cpu, nullptr);
    s_changed = true;
    s_thread.store(m_cpu.efs, std::memory_order_relaxed);
}

}

namespace native
{

namespace
{

using x86::reg32;
using x86::sreg32;

inline reg32 argument(const x86::CPU& cpu, int n)
{
    return rc::ld32(cpu.esp + 4 + 4 * n);
}

inline double loadFloat(reg32 address)
{
    return double(rc::ldf32(address));
}

const reg32 VoiceArray      = 0x8b4a00;     // 0x5C-byte voices: +0 state (2 = playing), +0x40 module chain
const reg32 VoiceCount      = 0x8b47f8;     // byte
const reg32 ChannelCount    = 0x8b47f9;     // byte
const reg32 VoiceSize       = 0x5c;
reg32 s_yieldVoice          = 0;            // audio_snd_yield: last voice mixed; reset per voice render call
const reg32 PlayingVoice    = 2;
const reg32 MixKernel       = 0x8b4a04;
const reg32 ConvertKernel   = 0x8b4a08;
const reg32 VoiceDone       = 0x8b47f4;
const reg32 FreeHook        = 0x8b4a24;
const reg32 AuxHook         = 0x8b4800;     // called with the frame count when set
const reg32 PostHook        = 0x8b4a10;     // called with the channel buffers when [0x8B46C9] or [0x8B46CC] is set
const reg32 PostFlagA       = 0x8b46c9;     // byte
const reg32 PostFlagB       = 0x8b46cc;     // dword
const reg32 MemMoveSlot     = 0x818ba0;
const reg32 AuxCount        = 0x8180b8;     // dword
const reg32 AuxEffects      = 0x8b47cc;     // pointer to AuxCount effect objects
const reg32 PostFilters     = 0x8b4940;     // per channel, optional
const reg32 PostOutputs     = 0x8b4928;     // per channel
const reg32 MaxChannels     = 6;
const int   MaxDepth        = 16;
const int   MaxModules      = 4096;

const reg32 MixKernelFunction       = 0x72c360;
const reg32 ConvertKernelFunction   = 0x72c480;
const reg32 AuxHookFunction         = 0x72655a;
const reg32 MemMoveFunction         = 0x71fecc;
const reg32 VoiceDoneFunction       = 0x725378;
const reg32 FreeHookFunction        = 0x728cf3;
const reg32 AllocHookFunction       = 0x728cee;

enum Extra
{
    Plain,
    Decoder,        // EA-XA decoder object at +0x1C, its function at +0
    Resampler,      // kernel at +0x24
    Mpeg,           // decoder object [[+0x1C]], its vtable 0x7C6A4C
};

struct ModuleKind
{
    reg32   render;
    bool    secondInput;
    Extra   extra;
};

const ModuleKind s_modules[] = {
    {0x72802d, false, Mpeg},        // MPEG stream
    {0x7282ef, false, Mpeg},        // MPEG stream
    {0x7285bd, false, Mpeg},        // MPEG stream
    {0x72a138, false, Plain},       // buffer source
    {0x72a2f3, false, Plain},       // hold
    {0x72a3d9, true,  Plain},       // sum of two inputs
    {0x72a541, false, Plain},       // delay
    {0x72a672, false, Plain},       // gain
    {0x72a7a1, false, Plain},       // filter
    {0x72a8c5, false, Plain},       // filter
    {0x72a9b3, false, Plain},       // filter
    {0x72aa55, false, Plain},       // filter
    {0x72c941, false, Plain},       // stream-synchronised gate
    {0x72cac7, false, Plain},       // float -> 16-bit output
    {0x72d18d, false, Resampler},   // resampler
    {0x732866, false, Plain},       // decoded looping sample
    {0x732980, false, Plain},       // decoded stream
    {0x732b7e, false, Plain},       // decoded bank sample
    {0x732c84, false, Decoder},     // EA-XA stream
    {0x732dfb, false, Decoder},     // looping EA-XA sample
    {0x733023, false, Decoder},     // EA-XA sample
    {0x7330f5, false, Plain},       // PCM stream
    {0x7331ff, false, Plain},       // looping PCM sample
    {0x7332a3, false, Plain},       // PCM sample
};

const reg32 s_teardowns[] = { 0, 0x7281b7, 0x728414, 0x7286ef, 0x72a391, 0x72a5ba, 0x72ca1b, 0x732afd, 0x732bce, 0x733082 };
const reg32 MpegVtable = 0x7c6a4c;
const reg32 AllocHook = 0x8b4a1c;
const reg32 EaxaDecoder = 0x734bf4;                     // the x87 one; 0x734AFF is SSE
const reg32 s_resamplerKernels[] = { 0x733700, 0x733388 };  // x87 mono and stereo

const ModuleKind* moduleKind(reg32 render)
{
    for (const ModuleKind& kind : s_modules)
    {
        if (kind.render == render)
            return &kind;
    }
    return nullptr;
}

template <size_t N>
bool contains(const reg32 (&set)[N], reg32 value)
{
    for (reg32 entry : set)
    {
        if (entry == value)
            return true;
    }
    return false;
}

struct Audit
{
    const char* reason = nullptr;
    reg32       address = 0;

    bool fail(const char* what, reg32 value)
    {
        reason = what;
        address = value;
        return false;
    }
};

bool knownModule(reg32 module, int depth, int& budget, Audit& audit)
{
    if (!module)
        return true;
    if (depth > MaxDepth)
        return audit.fail("depth", module);
    if (--budget < 0)
        return audit.fail("module count", module);
    const reg32 render = rc::ld32(module);
    const ModuleKind* kind = moduleKind(render);
    if (!kind)
        return audit.fail("module", render);
    const reg32 teardown = rc::ld32(module + 4);
    if (!contains(s_teardowns, teardown))
        return audit.fail("teardown", teardown);
    if (kind->extra == Decoder)
    {
        const reg32 decoder = rc::ld32(module + 0x1c);
        if (!decoder)
            return audit.fail("decoder", 0);
        if (rc::ld32(decoder) != EaxaDecoder)
            return audit.fail("decoder", rc::ld32(decoder));
    }
    else if (kind->extra == Mpeg)
    {
        const reg32 stream = rc::ld32(module + 0x1c);
        const reg32 object = stream ? rc::ld32(stream) : 0;
        if (!object || rc::ld32(object) != MpegVtable)
            return audit.fail("mpeg decoder", object ? rc::ld32(object) : 0);
    }
    else if (kind->extra == Resampler && !contains(s_resamplerKernels, rc::ld32(module + 0x24)))
    {
        return audit.fail("resampler", rc::ld32(module + 0x24));
    }
    return knownModule(rc::ld32(module + 8), depth + 1, budget, audit)
        && (!kind->secondInput || knownModule(rc::ld32(module + 0xc), depth + 1, budget, audit));
}

bool knownRoot(reg32 module, int& budget, Audit& audit)
{
    if (!module)
        return audit.fail("no output", 0);
    return knownModule(module, 0, budget, audit);
}

bool renderAudited(Audit& audit)
{
    if (rc::ld32(MixKernel) != MixKernelFunction)
        return audit.fail("mix kernel", rc::ld32(MixKernel));
    if (rc::ld32(ConvertKernel) != ConvertKernelFunction)
        return audit.fail("convert kernel", rc::ld32(ConvertKernel));
    if (rc::ld32(MemMoveSlot) != MemMoveFunction)
        return audit.fail("memmove", rc::ld32(MemMoveSlot));
    const reg32 channels = rc::ld8(ChannelCount);
    if (channels > MaxChannels)
        return audit.fail("channels", channels);
    int budget = MaxModules;
    const reg32 voices = rc::ld32(VoiceArray);
    const reg32 voiceCount = rc::ld8(VoiceCount);
    bool playing = false;
    for (reg32 i = 0; i < voiceCount; ++i)
    {
        const reg32 voice = voices + i * VoiceSize;
        if (rc::ld8(voice) != PlayingVoice)
            continue;
        playing = true;
        if (!knownRoot(rc::ld32(voice + 0x40), budget, audit))
            return false;
    }
    if (playing)
    {
        if (rc::ld32(VoiceDone) != VoiceDoneFunction)
            return audit.fail("voice done", rc::ld32(VoiceDone));
        if (rc::ld32(AllocHook) != AllocHookFunction)
            return audit.fail("alloc hook", rc::ld32(AllocHook));
        if (rc::ld32(FreeHook) != FreeHookFunction)
            return audit.fail("free hook", rc::ld32(FreeHook));
    }
    for (reg32 channel = 0; channel < channels; ++channel)
    {
        const reg32 filter = rc::ld32(PostFilters + 4 * channel);
        if ((filter && !knownRoot(filter, budget, audit))
            || !knownRoot(rc::ld32(PostOutputs + 4 * channel), budget, audit))
            return false;
    }
    const reg32 hook = rc::ld32(AuxHook);
    if (hook == AuxHookFunction)
    {
        const sreg32 count = sreg32(rc::ld32(AuxCount));
        const reg32 effects = rc::ld32(AuxEffects);
        for (sreg32 i = 0; i < count; ++i)
        {
            const reg32 effect = rc::ld32(effects + 4 * i);
            if (!effect)
                continue;
            const reg32 table = rc::ld32(effect + 0x40);
            if (!table)
                return audit.fail("aux table", effect);
            for (reg32 channel = 0; channel < channels; ++channel)
            {
                if (!knownModule(rc::ld32(table + 4 + 4 * channel), 0, budget, audit))
                    return false;
            }
        }
    }
    else if (hook != 0)
    {
        return audit.fail("aux hook", hook);
    }
    if ((rc::ld8(PostFlagA) || rc::ld32(PostFlagB)) && rc::ld32(PostHook) != 0)
        return audit.fail("post hook", rc::ld32(PostHook));
    return true;
}

const reg32 Handles         = 0x8b466c;
const reg32 HandleCount     = 0x8b460c;     // int16
const reg32 Drivers         = 0x8b4724;
const reg32 DriverSize      = 0x88;
const reg32 TickHookCount   = 0x8b460a;     // int8; hooks at 0x8B4614
const reg32 TickHooks       = 0x8b4614;
const reg32 DeferredHook    = 0x73b9c0;     // the one game hook the callback thread may run

struct DriverSlot
{
    reg32   function;
    reg32   argument;
    reg32   software;
};

const DriverSlot s_driverSlots[] = {
    {0x6c, 0,    0x72a000},     // stop
    {0x70, 0x5c, 0x729e76},
    {0x74, 0x58, 0x729e24},
    {0x78, 0x60, 0x729ea3},
    {0x7c, 0x64, 0x729ef1},
    {0x80, 0x68, 0x729f1e},     // play position of the first voice
};
const reg32 DriverVoices    = 0x18;         // voice list: byte count, int16 voice numbers from +4
const reg32 s_positionQueries[] = { 0, 0x73329b, 0x7286e4, 0x73307a };  // voice +0x44

bool teardownsKnown(reg32 voice, int& budget, Audit& audit)
{
    for (reg32 module = rc::ld32(voice + 0x40); module; module = rc::ld32(module + 8))
    {
        if (--budget < 0)
            return audit.fail("module count", module);
        const reg32 teardown = rc::ld32(module + 4);
        if (!contains(s_teardowns, teardown))
            return audit.fail("teardown", teardown);
    }
    return true;
}

bool driverAudited(reg32 index, int& budget, Audit& audit)
{
    const reg32 driver = rc::ld32(Drivers) + index * DriverSize;
    for (const DriverSlot& slot : s_driverSlots)
    {
        const reg32 function = rc::ld32(driver + slot.function);
        if (!function)
            continue;
        if (function != slot.software)
            return audit.fail("driver function", function);
        if (slot.argument && rc::ld32(driver + slot.argument) != driver + DriverVoices)
            return audit.fail("driver voices", rc::ld32(driver + slot.argument));
    }
    const reg32 voices = rc::ld32(VoiceArray);
    const reg32 voiceCount = rc::ld8(VoiceCount);
    const reg32 listed = rc::ld8(driver + DriverVoices);
    for (reg32 k = 0; k < listed; ++k)
    {
        const reg32 number = reg32(x86::sreg16(rc::ld16(driver + DriverVoices + 4 + 2 * k)));
        if (number >= voiceCount)
            return audit.fail("handle voice", number);
        if (!teardownsKnown(voices + number * VoiceSize, budget, audit))
            return false;
    }
    return true;
}

bool driversChecked(Audit& audit)
{
    if (rc::ld32(FreeHook) != FreeHookFunction)
        return audit.fail("free hook", rc::ld32(FreeHook));
    const reg32 handles = rc::ld32(Handles);
    const sreg32 count = x86::sreg16(rc::ld16(HandleCount));
    int budget = MaxModules;
    for (sreg32 i = 0; i < count; ++i)
    {
        if (!rc::ld8(handles + reg32(i) * 0x90 + 0x6d))
            continue;
        if (!driverAudited(reg32(i), budget, audit))
            return false;
    }
    return true;
}

enum DriverState
{
    DriversUnchecked,
    DriversPassed,
    DriversFailed,
};
DriverState s_driverState = DriversUnchecked;
Audit s_driverAudit;

bool driversAudited(Audit& audit)
{
    if (mixer::s_changed)
    {
        mixer::s_changed = false;
        s_driverState = DriversUnchecked;
    }
    if (s_driverState == DriversUnchecked)
        s_driverState = driversChecked(s_driverAudit) ? DriversPassed : DriversFailed;
    if (s_driverState == DriversFailed)
        audit = s_driverAudit;
    return s_driverState == DriversPassed;
}

const reg32 PositionQueryOffset = 0x44;

bool positionAudited(reg32 instance, Audit& audit)
{
    const reg32 id = rc::ld32(instance + 8);
    const reg32 index = id & 0xff;
    const reg32 handle = rc::ld32(Handles) + index * 0x90;
    if (x86::sreg32(id) < 0 || x86::sreg32(index) >= x86::sreg16(rc::ld16(HandleCount)) || !rc::ld8(handle + 0x6d)
        || rc::ld32(handle) != id)
        return true;
    const reg32 driver = rc::ld32(Drivers) + index * DriverSize;
    if (!rc::ld32(driver + 0x80))
        return true;
    const reg32 number = reg32(x86::sreg16(rc::ld16(driver + DriverVoices + 4)));
    if (number >= rc::ld8(VoiceCount))
        return audit.fail("position voice", number);
    const reg32 query = rc::ld32(rc::ld32(VoiceArray) + number * VoiceSize + PositionQueryOffset);
    if (!contains(s_positionQueries, query))
        return audit.fail("position query", query);
    return true;
}

bool soundAudited(reg32 instance, Audit& audit)
{
    const sreg32 wanted = sreg32(rc::ld32(instance + 0x18));
    const sreg32 state = wanted < 0 ? 0 : wanted > 2 ? 2 : wanted;
    const sreg32 current = x86::sreg8(rc::ld8(instance + 0xc));
    const sreg32 sound = x86::sreg8(rc::ld8(instance + 0x10));
    if (sound > 0)
        return audit.fail("sound kind", reg32(sound));
    if (state != current && state == 1 && sound < 0 && !(current == 2 && rc::ld8(instance + 0xd) == 1))
        return audit.fail("sound start", instance);
    if (state == 1 && sound >= 0)
    {
        const reg32 parameters = rc::ld8(instance + 0xe);
        for (reg32 k = 0; k < parameters; ++k)
        {
            const reg32 kind = rc::ld8(instance + 0x1c + 0xc * k);
            if (kind >= 12)
                return audit.fail("parameter kind", kind);
        }
        if (!driversAudited(audit))
            return false;
        return !rc::ld8(instance + 0xf) || positionAudited(instance, audit);
    }
    return driversAudited(audit);
}

bool teardownAudited(reg32 instance, Audit& audit)
{
    if (!rc::ld32(instance + 0xc))
        return true;
    const reg32 entry = rc::ld32(instance);
    const reg32 data = rc::ld32(instance + 4);
    if (rc::ld8(entry + 0x25) || rc::ld8(entry + 0x26) || rc::ld8(entry + 0x27))
        return audit.fail("teardown objects", entry);
    const reg32 sounds = rc::ld8(entry + 0x24);
    for (reg32 k = 0; k < sounds; ++k)
    {
        const sreg32 kind = x86::sreg8(rc::ld8(rc::ld32(entry + 0x3c + 4 * k) + data + 0x10));
        if (kind > 0)
            return audit.fail("sound kind", reg32(kind));
    }
    return driversAudited(audit);
}

// Library entries 5 (0x720036), 38 (0x7200A2) and 39 (0x7201C2) on the paths that only read or clamp the argument's
// own data; their other paths reach the object layer (0x735430, 0x735490, 0x735550, 0x735080, 0x7356D0).
NFSU_DEV_VARC(bool, s_auditObjectCalls, true, true);             // audio_library_audit

const reg32 LibraryClamp    = 0x720036;
const reg32 LibraryRead     = 0x7200a2;
const reg32 LibraryUpdate   = 0x7201c2;

// Observer the game links to sound handles (0x47CCE3): [[arg] + 0xF8] = [value], a leaf store into a game object.
// The PC game runs it on the mixer thread with only the SND mutex, as here.
const reg32 ValueObserver   = 0x472cd0;
const int   MaxObservers    = 64;

// 0x735080 (this = handle): calls the observers [+8] (+8 function, +0xC argument) with the new value.
bool observersAudited(reg32 handle, Audit& audit)
{
    int budget = MaxObservers;
    for (reg32 node = rc::ld32(handle + 8); node; node = rc::ld32(node))
    {
        if (--budget < 0)
            return audit.fail("observer count", node);
        if (rc::ld32(node + 8) != ValueObserver)
            return audit.fail("observer", rc::ld32(node + 8));
    }
    return true;
}

bool objectCallAudited(reg32 target, reg32 object, Audit& audit)
{
    if (target == LibraryClamp)
    {
        const reg32 slot = rc::ld8(object + 8) ? object + 0xc + 8 * reg32(rc::ld8(object + 9)) : object + 0xc;
        return rc::ld32(slot) == 0 || audit.fail("library clamp", target);
    }
    if (target == LibraryRead)
    {
        const reg32 values = rc::ld8(object + 0xc) ? object + 0x10 + 8 * reg32(rc::ld8(object + 0xd)) : object + 0x10;
        const bool pending = rc::ld32(values + 4) != 0;
        const bool current = rc::ld32(values) != 0;
        const bool handle = rc::ld32(object + 8) != 0;
        // 0x7200C7: release when pending, create when current without a handle, notify when neither but a handle.
        if (pending ? !handle : current == handle)
            return true;
        if (!pending && !current)
            return observersAudited(rc::ld32(object + 8), audit);
        return audit.fail("library read", target);
    }
    if (target == LibraryUpdate)
        return rc::ld32(object + 0x10) == rc::ld32(object + 0x14) || audit.fail("library update", target);
    return audit.fail("library call", target);
}

bool tickAudited(Audit& audit)
{
    const reg32 hooks = rc::ld8(TickHookCount);
    if (x86::sreg8(hooks) > 0)
        return audit.fail("tick hooks", hooks);
    return driversAudited(audit);
}

const reg32 Callbacks       = 0x8b405c;     // nodes: +0 next, +8 function, +0xC argument
const reg32 BankTick        = 0x725520;
const reg32 BankRoutines    = 0x8b446c;     // nodes: +0 next, +8 routine, +0xC argument
const int   MaxNodes        = 4096;

bool callbacksAudited(win32::WinApplication* app, Audit& audit)
{
    int budget = MaxNodes;
    for (reg32 node = rc::ld32(Callbacks); node; node = rc::ld32(node))
    {
        if (--budget < 0)
            return audit.fail("callback count", node);
        if (rc::ld32(node + 8) != BankTick)
            return audit.fail("callback", rc::ld32(node + 8));
    }
    for (reg32 node = rc::ld32(BankRoutines); node; node = rc::ld32(node))
    {
        if (--budget < 0)
            return audit.fail("bank callback count", node);
        if (app->getMethodName(rc::ld32(node + 8)))
            return audit.fail("bank callback", rc::ld32(node + 8));
    }
    return true;
}

const reg32 Notifications   = 0x8b40e8;     // count; 8-byte entries from 0x8B40EC: u16 kind, u16 stream, value
const reg32 NotifyQueue     = 0x8b40ec;
const reg32 NotifyStreams   = 0x8b43ec;     // stream objects by index: +0x54 / +0x58 callbacks, +0x5C context
const reg32 NotifyCapacity  = (NotifyStreams - NotifyQueue) / 8;
const reg32 ChunkCallback   = 0x7224ac;     // frames consumed: SND stream lists only (0x722442, 0x727A12, 0x7279D6)
const reg32 EngineCallback  = 0x4777f0;     // engine grains (0x466D30): own object and the SND stream queue only

NFSU_DEV_VARC(bool, s_inlineNotifications, true, true);          // audio_inline_notifications
NFSU_DEV_VARC(bool, s_engineInline, false, true);                // audio_engine_inline

bool inlineCallback(reg32 callback)
{
    return callback == ChunkCallback || (s_engineInline && callback == EngineCallback);
}

// The callback of queued notification i, as 0x72382F picks it.
reg32 notifyCallback(reg32 i)
{
    const reg32 slot = NotifyQueue + 8 * i;
    const reg32 stream = rc::ld32(NotifyStreams + 4 * reg32(rc::ld16(slot + 2)));
    return rc::ld32(stream + (rc::ld16(slot) ? 0x54 : 0x58));
}

// audio_defer_log: callbacks of the notifications delivered under the lock, logged every 500 batches.
void countLockedCallbacks()
{
    struct Row
    {
        reg32    callback;
        uint32_t count;
    };
    static Row s_rows[16];
    static uint32_t s_batches = 0;
    const reg32 count = std::min<reg32>(rc::ld32(Notifications), NotifyCapacity);
    for (reg32 i = 0; i < count; ++i)
    {
        const reg32 callback = notifyCallback(i);
        for (Row& row : s_rows)
        {
            if (row.callback == callback || !row.callback)
            {
                row.callback = callback;
                ++row.count;
                break;
            }
        }
    }
    if (++s_batches < 500)
        return;
    s_batches = 0;
    for (Row& row : s_rows)
    {
        if (row.callback)
            pf::logInfo("audio: locked notification callback 0x%08x: %u in 500 batches", unsigned(row.callback),
                        unsigned(row.count));
        row = Row();
    }
}

// Delivers queued notification i as 0x72382F does.
void deliverNotification(win32::WinApplication* app, x86::CPU& cpu, reg32 i)
{
    const reg32 slot = NotifyQueue + 8 * i;
    const reg32 kind = rc::ld16(slot);
    const reg32 index = rc::ld16(slot + 2);
    const reg32 stream = rc::ld32(NotifyStreams + 4 * index);
    const reg32 saved = cpu.esp;
    reg32 esp = saved;
    const auto push = [&esp](reg32 value) { esp -= 4; rc::st32(esp, value); };
    push(rc::ld32(stream + 0x5c));
    push(rc::ld32(slot + 4));
    if (!kind)
        push(index);
    push(0);
    cpu.esp = esp;
    app->dynamic_call(rc::ld32(stream + (kind ? 0x54 : 0x58)), cpu);
    cpu.esp = saved;
}

// Delivers leading audited notifications unlocked; true when none remain.
bool deliverChunkNotifications(win32::WinApplication* app, x86::CPU& cpu)
{
    reg32 i = 0;
    while (i < NotifyCapacity && x86::sreg32(i) < x86::sreg32(rc::ld32(Notifications)) && inlineCallback(notifyCallback(i)))
    {
        deliverNotification(app, cpu, i);
        if (cpu.terminate)
            return true;
        ++i;
    }
    const reg32 count = rc::ld32(Notifications);
    if (x86::sreg32(i) >= x86::sreg32(count))
    {
        rc::st32(Notifications, 0);
        if (pf::perf::enabled() && i)
            pf::perf::audio(pf::perf::AudioNotificationsInline, i);
        return true;
    }
    if (i)
    {
        for (reg32 k = i; k < count && k < NotifyCapacity; ++k)
        {
            rc::st32(NotifyQueue + 8 * (k - i), rc::ld32(NotifyQueue + 8 * k));
            rc::st32(NotifyQueue + 8 * (k - i) + 4, rc::ld32(NotifyQueue + 8 * k + 4));
        }
        rc::st32(Notifications, count - i);
        if (pf::perf::enabled())
            pf::perf::audio(pf::perf::AudioNotificationsInline, i);
    }
    return false;
}

}

const reg32 MixerRunning    = 0x8b4721;     // byte; cleared to end the thread
const reg32 OutputChannelsA = 0x8b44df;     // bytes; their sum is the 16-bit channels of the output block
const reg32 OutputChannelsB = 0x8b44e0;
const reg32 LoopHookBefore  = 0x8b46c0;     // mixer thread loop hooks, unset in this game
const reg32 LoopHookAfter   = 0x8b46c4;

static void renderLoop(win32::WinApplication* app, x86::CPU& cpu);

// audio_mix_rate: the SND mix rate in Hz, or 0 for the game's (0x47ABA9: 22050 or 44100 by g_PerformanceLevel).
static reg32 mixRate()
{
    static const reg32 s_rate = []
    {
        const int rate = pf::Config::getInt("audio_mix_rate", 0);
        const int allowed[] = { 0, 22050, 24000, 32000, 44100, 48000 };
        if (std::find(std::begin(allowed), std::end(allowed), rate) != std::end(allowed))
            return reg32(rate);
        pf::logError("audio_mix_rate=%d: not 22050, 24000, 32000, 44100 or 48000; the game's rate is used", rate);
        return reg32(0);
    }();
    return s_rate;
}

// SND output rate (device, rate), set twice at sound start-up (0x47AB8A, 0x47AC0F).
void sub_7216c6(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 GameRate = 0x7ecbd8;
    const reg32 rate = mixRate();
    if (rate)
    {
        pf::logInfo("audio mix rate: %u Hz instead of %u (audio_mix_rate)", unsigned(rate),
                    unsigned(argument(cpu, 1)));
        rc::st32(cpu.esp + 8, rate);
        rc::st32(GameRate, rate);
    }
    sub_7216c6_guest(app, cpu);
}

// Render loop (&output pointer, frames); audio_mixer_stay keeps the thread unlocked.
void sub_725c8e(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!mixstats::on())
    {
        renderLoop(app, cpu);
        return;
    }
    mixstats::loopEnter(cpu, argument(cpu, 1));
    renderLoop(app, cpu);
    mixstats::loopLeave(cpu);
}

static void renderLoop(win32::WinApplication* app, x86::CPU& cpu)
{
#ifdef __vita__
    static std::atomic<int> s_tracked{0};
    static const bool s_sampled = pf::sampler::sampleMixer();
    const int hostId = pf::perf::enabled() || s_sampled ? sceKernelGetThreadId() : 0;
    if (hostId && s_tracked.exchange(hostId) != hostId)
    {
        if (pf::perf::enabled())
            pf::perf::trackThread("mixer");
        if (s_sampled)
            pf::sampler::retarget(hostId);
        SceKernelThreadInfo info;
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0)
            pf::logInfo("audio mixer: host thread 0x%x priority %d, affinity 0x%x", unsigned(hostId),
                        int(info.currentPriority), unsigned(info.currentCpuAffinityMask));
    }
#endif
    const reg32 slot = cpu.efs + win32::teb::HostLock;
    win32::Mutex* const lock = app->getMemory<win32::Mutex*>(slot);
    const bool stayed = !lock && mixer::unlocked(cpu) && mixer::s_stay;
    if (!mixer::s_unlocked || (!lock && !stayed))
    {
        sub_725c8e_guest(app, cpu);
        return;
    }
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    if (timed)
        pf::perf::sndIdentify(rc::ld32(0x8b4f38), cpu.efs);
    if (lock)
    {
        if (sndq::enabled())
            sndq::attach(app, cpu);
        app->unlockContext(cpu);
        app->getMemory<win32::Mutex*>(slot) = nullptr;
        mixer::s_lock = lock;
    }
    mixer::s_changed = true;
    mixer::s_thread.store(cpu.efs, std::memory_order_relaxed);
    mixer::s_sndMutex = mixer::s_sndYield ? win32::Event::find(rc::ld32(mixer::SndHandleSlot)) : nullptr;
    const reg32 output = mixer::s_dump ? rc::ld32(argument(cpu, 0)) : 0;
    const reg32 frames = argument(cpu, 1);
    split::loopBegin(cpu);
    sub_725c8e_guest(app, cpu);
    split::loopEnd(cpu);
    mixer::s_sndMutex = nullptr;
    if (mixer::s_dump)
        fwrite(rc::host(output), 2 * (rc::ld8(OutputChannelsA) + rc::ld8(OutputChannelsB)), frames, mixer::s_dump);
    if (timed)
    {
        pf::perf::audio(pf::perf::AudioLoopsUnlocked, 1);
        pf::perf::audio(pf::perf::AudioLoopUnlockedUs, pf::perf::now() - start);
    }
    if (mixer::s_stay && rc::ld8(MixerRunning) && !rc::ld32(LoopHookBefore) && !rc::ld32(LoopHookAfter) && !cpu.terminate)
        return;
    mixer::s_thread.store(0, std::memory_order_relaxed);
    app->getMemory<win32::Mutex*>(slot) = mixer::s_lock;
    const uint32_t relock = timed ? pf::perf::now() : 0;
    app->lockContext(cpu);
    if (timed)
        pf::perf::sndLockWait(pf::perf::SndLoopEnd, pf::perf::now() - relock);
}

void sub_721e13(win32::WinApplication* app, x86::CPU& cpu)
{
    const mixstats::Scoped scope(cpu, mixstats::Tick);
    Audit audit;
    if (sndq::hookDeferred() && mixer::unlocked(cpu) && rc::ld8(TickHookCount) == 1 && rc::ld32(TickHooks) == DeferredHook)
    {
        rc::st8(TickHookCount, 0);
        const bool audited = tickAudited(audit);
        if (audited)
            sub_721e13_guest(app, cpu);
        rc::st8(TickHookCount, 1);
        if (audited)
        {
            sndq::requestHook();
            return;
        }
    }
    if (!mixer::unlocked(cpu) || tickAudited(audit))
    {
        sub_721e13_guest(app, cpu);
        return;
    }
    {
        mixer::Locked locked(app, cpu, pf::perf::SndTick);
        sub_721e13_guest(app, cpu);
    }
    if (pf::perf::enabled())
    {
        pf::perf::audio(pf::perf::AudioTicksLocked, 1);
        pf::perf::audioLocked(audit.reason, audit.address);
    }
}

void sub_721de6(win32::WinApplication* app, x86::CPU& cpu)
{
    const mixstats::Scoped scope(cpu, mixstats::Callbacks);
    Audit audit;
    if (!mixer::unlocked(cpu) || callbacksAudited(app, audit))
    {
        sub_721de6_guest(app, cpu);
        return;
    }
    {
        mixer::Locked locked(app, cpu);
        sub_721de6_guest(app, cpu);
    }
    if (pf::perf::enabled())
    {
        pf::perf::audio(pf::perf::AudioCallbacksLocked, 1);
        pf::perf::audioLocked(audit.reason, audit.address);
    }
}

void notificationsBody(win32::WinApplication* app, x86::CPU& cpu)
{
    const bool allowed = sndq::inlineAllowed();
    if (s_inlineNotifications && mixer::unlocked(cpu) && x86::sreg32(rc::ld32(Notifications)) > 0 && allowed
        && deliverChunkNotifications(app, cpu))
    {
        cpu.esp += 4;
        return;
    }
    if (sndq::enabled())
    {
        if (!mixer::unlocked(cpu))
            sndq::fence(app, cpu);
        else if (x86::sreg32(rc::ld32(Notifications)) > 0)
        {
            if (sndq::queueNotifications())
            {
                cpu.esp += 4;
                return;
            }
            sndq::countLocked(!allowed);
            if (sndq::logging())
                countLockedCallbacks();
            mixer::Locked locked(app, cpu, pf::perf::SndNotifications);
            sndq::fence(app, cpu);
            sub_72382f_guest(app, cpu);
            return;
        }
    }
    if (!mixer::unlocked(cpu) || x86::sreg32(rc::ld32(Notifications)) <= 0)
    {
        sub_72382f_guest(app, cpu);
        return;
    }
    {
        mixer::Locked locked(app, cpu, pf::perf::SndNotifications);
        sub_72382f_guest(app, cpu);
    }
    if (pf::perf::enabled())
        pf::perf::audio(pf::perf::AudioNotificationsLocked, 1);
}

void sub_72382f(win32::WinApplication* app, x86::CPU& cpu)
{
    const mixstats::Scoped scope(cpu, mixstats::Notifications);
    notificationsBody(app, cpu);
    // The render loop's edi holds the frames still to render in this slot.
    if (mixer::s_sndYield && mixer::unlocked(cpu) && sreg32(cpu.edi) > 0 && !cpu.terminate)
        mixer::yieldSnd(app, cpu);
}

static void voiceRender(win32::WinApplication* app, x86::CPU& cpu);

// Voice render (&output pointer, frames).
void sub_729574(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!mixstats::on())
    {
        voiceRender(app, cpu);
        return;
    }
    mixstats::voicesEnter(cpu, argument(cpu, 1));
    voiceRender(app, cpu);
    mixstats::voicesLeave(cpu);
}

static void voiceRender(win32::WinApplication* app, x86::CPU& cpu)
{
    const cull::Voices silent(cpu);
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    Audit audit;
    if (mixer::unlocked(cpu) && renderAudited(audit))
    {
        s_yieldVoice = 0;
        {
            const split::Window window(app, cpu);
            sub_729574_guest(app, cpu);
        }
        if (mixer::s_midLocked)
        {
            mixer::s_midLocked = false;
            app->unlockContext(cpu);
            mixer::setLock(app, cpu, nullptr);
            mixer::s_changed = true;
            mixer::s_thread.store(cpu.efs, std::memory_order_relaxed);
        }
        if (timed)
        {
            pf::perf::audio(pf::perf::AudioRendersUnlocked, 1);
            pf::perf::audio(pf::perf::AudioRenderUnlockedUs, pf::perf::now() - start);
        }
        return;
    }
    const char* const reason = mixer::unlocked(cpu) ? audit.reason : "outside the render loop";
    {
        mixer::Locked locked(app, cpu);
        sub_729574_guest(app, cpu);
    }
    if (timed)
    {
        pf::perf::audio(pf::perf::AudioRendersLocked, 1);
        pf::perf::audio(pf::perf::AudioRenderLockedUs, pf::perf::now() - start);
        pf::perf::audioLocked(reason, audit.address);
    }
}

const reg32 VoiceScratch = 0x8b48d4;        // pointer to the voice buffer 0x7292BC mixes from

// Voice loop of 0x7292BC, before a voice's first mix: yields the SND mutex, then audits the render again.
void voiceYield(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 voice = cpu.esi;
    if (voice == s_yieldVoice)
        return;
    s_yieldVoice = voice;
    if (mixer::s_sndMutex->normalWaiters() == 0)
        return;
    const reg32 voices = rc::ld32(VoiceArray);
    const reg32 count = rc::ld8(VoiceCount);
    if (voice < voices || voice >= voices + count * VoiceSize || (voice - voices) % VoiceSize)
        return;
    const bool log = sndq::logging();
    static std::vector<uint8_t> s_before;
    if (log)
        s_before.assign(rc::host(voices), rc::host(voices) + count * VoiceSize);
    if (!mixer::yieldSnd(app, cpu))
        return;
    if (log)
    {
        const reg32 now = rc::ld32(VoiceArray);
        const reg32 offset = voice - voices;
        if (now != voices || rc::ld8(VoiceCount) != count || memcmp(s_before.data(), rc::host(voices), s_before.size()))
            ++mixer::s_yieldStats.arrayChanged;
        if (now != voices || memcmp(s_before.data() + offset, rc::host(voice), VoiceSize))
            ++mixer::s_yieldStats.voiceChanged;
    }
    Audit audit;
    if (!renderAudited(audit))
    {
        mixer::s_thread.store(0, std::memory_order_relaxed);
        mixer::setLock(app, cpu, mixer::s_lock);
        app->lockContext(cpu);
        mixer::s_midLocked = true;
        ++mixer::s_yieldStats.relocked;
    }
    if (log)
        mixer::logYields();
}

// Mix kernel: dst[i] += src[i] * gain (n, gain, src, dst).
void sub_72c360(win32::WinApplication* app, x86::CPU& cpu)
{
    if (cull::checking())
        cull::checkMix(cpu);
    const mixstats::Scoped scope(cpu, mixstats::Mix);
    if (mixstats::on())
    {
        float gain;
        const reg32 bits = argument(cpu, 1);
        memcpy(&gain, &bits, sizeof(gain));
        mixstats::mixCall(cpu, argument(cpu, 0), gain, argument(cpu, 2));
    }
    if (mixer::s_sndMutex && mixer::unlocked(cpu) && argument(cpu, 2) == rc::ld32(VoiceScratch))
        voiceYield(app, cpu);
    if (!mixer::s_nativeKernels)
    {
        sub_72c360_guest(app, cpu);
        return;
    }
    reg32 n = argument(cpu, 0);
    float gainValue;
    const reg32 gainBits = argument(cpu, 1);
    memcpy(&gainValue, &gainBits, sizeof(gainValue));
    const double gain = double(gainValue);
    const reg32 src = argument(cpu, 2);
    const reg32 dst = argument(cpu, 3);
    cpu.esp += 4;
    if (mixer::s_neonKernels)
    {
        kernels::mix(rc::host(0), n, gainValue, src, dst);
        return;
    }
    while (n & 3)
    {
        double value = loadFloat(src + n * 4 - 4);
        value *= gain;
        value += loadFloat(dst + n * 4 - 4);
        rc::stf32(dst + n * 4 - 4, float(value));
        if (n == 1)
            return;
        --n;
    }
    for (;;)
    {
        const reg32 s = src + n * 4;
        const reg32 d = dst + n * 4;
        double v0 = loadFloat(s - 16);
        v0 *= gain;
        double v1 = loadFloat(s - 12);
        v1 *= gain;
        double v2 = loadFloat(s - 8);
        v2 *= gain;
        double v3 = loadFloat(s - 4);
        v3 *= gain;
        v0 += loadFloat(d - 16);
        v1 += loadFloat(d - 12);
        v2 += loadFloat(d - 8);
        v3 += loadFloat(d - 4);
        rc::stf32(d - 16, float(v0));
        rc::stf32(d - 12, float(v1));
        rc::stf32(d - 4, float(v3));
        rc::stf32(d - 8, float(v2));
        const reg32 previous = n;
        n -= 4;
        if (!(sreg32(previous) > 4))
            return;
    }
}

// Convert kernel: 16-bit to float (n, src, dst).
void sub_72c480(win32::WinApplication* app, x86::CPU& cpu)
{
    const mixstats::Scoped scope(cpu, mixstats::Convert);
    mixstats::work(cpu, mixstats::Convert, argument(cpu, 0));
    if (!mixer::s_nativeKernels)
    {
        sub_72c480_guest(app, cpu);
        return;
    }
    reg32 n = argument(cpu, 0);
    const reg32 src = argument(cpu, 1);
    const reg32 dst = argument(cpu, 2);
    cpu.esp += 4;
    if (mixer::s_neonKernels)
    {
        kernels::convert(rc::host(0), n, src, dst);
        return;
    }
    while (n & 7)
    {
        const double value = double(x86::sreg16(rc::ld16(src + n * 2 - 2)));
        rc::stf32(dst + n * 4 - 4, float(value));
        if (n == 1)
            return;
        --n;
    }
    for (;;)
    {
        const reg32 s = src + n * 2;
        const reg32 d = dst + n * 4;
        double v[8];
        for (int i = 0; i < 8; ++i)
            v[i] = double(x86::sreg16(rc::ld16(s - 16 + 2 * i)));
        rc::stf32(d - 32, float(v[0]));
        rc::stf32(d - 8, float(v[6]));
        rc::stf32(d - 12, float(v[5]));
        rc::stf32(d - 16, float(v[4]));
        rc::stf32(d - 20, float(v[3]));
        rc::stf32(d - 24, float(v[2]));
        rc::stf32(d - 28, float(v[1]));
        rc::stf32(d - 4, float(v[7]));
        const reg32 previous = n;
        n -= 8;
        if (!(sreg32(previous) > 8))
            return;
    }
}

static void resampleCulled(win32::WinApplication* app, x86::CPU& cpu);
void resampleLinear(win32::WinApplication* app, x86::CPU& cpu);

// Linear resampler (n, src, dst, &position, &fraction, step, step fraction).
void sub_733700(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!mixstats::on())
    {
        resampleCulled(app, cpu);
        return;
    }
    const mixstats::Scoped scope(cpu, mixstats::Resample);
    const reg32 n = argument(cpu, 0);
    const reg32 positionAddress = argument(cpu, 3);
    const reg32 before = rc::ld32(positionAddress);
    resampleCulled(app, cpu);
    mixstats::work(cpu, mixstats::Resample, n, rc::ld32(positionAddress) - before);
}

static void resampleCulled(win32::WinApplication* app, x86::CPU& cpu)
{
    if (cull::skipKernel(cpu))
    {
        if (!cull::checking())
        {
            cull::advanceLinear(cpu);
            return;
        }
        const cull::KernelCheck check(cpu, false);
        resampleLinear(app, cpu);
        check.finish();
        return;
    }
    resampleLinear(app, cpu);
}

void resampleLinear(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!mixer::s_nativeKernels)
    {
        sub_733700_guest(app, cpu);
        return;
    }
    const reg32 FractionWord = 0x819170;
    const reg32 FractionScale = 0x819178;
    reg32 n = argument(cpu, 0);
    const reg32 src = argument(cpu, 1);
    reg32 dst = argument(cpu, 2);
    const reg32 positionAddress = argument(cpu, 3);
    const reg32 fractionAddress = argument(cpu, 4);
    const reg32 step = argument(cpu, 5);
    const reg32 stepFraction = argument(cpu, 6);
    cpu.esp += 4;
    reg32 fraction = rc::ld32(fractionAddress);
    reg32 position = rc::ld32(positionAddress);
    rc::st32(FractionWord, fraction);
    const reg32 high = rc::ld32(FractionWord + 4);
    const double scale = loadFloat(FractionScale);
    if (mixer::s_neonKernels && high == 0 &&
        kernels::resample(rc::host(0), n, src, dst, position, fraction, step, stepFraction, rc::ldf32(FractionScale),
                          FractionWord))
    {
        rc::st32(FractionWord, fraction);
        rc::st32(fractionAddress, fraction);
        rc::st32(positionAddress, position);
        return;
    }
    auto sample = [&]()
    {
        const reg32 at = src + position * 4;
        double value = loadFloat(at + 4);
        value -= loadFloat(at);
        const double weight = high == 0 ? double(fraction)
                                        : double(x86::sreg64((x86::reg64(high) << 32) | fraction));
        value *= weight;
        value *= scale;
        const reg32 next = fraction + stepFraction;
        const reg32 carry = next < fraction;
        fraction = next;
        value += loadFloat(at);
        position += step + carry;
        dst += 4;
        rc::st32(FractionWord, fraction);
        rc::stf32(dst - 4, float(value));
    };
    if (sreg32(n) >= 2)
    {
        n -= 2;
        for (;;)
        {
            sample();
            sample();
            const reg32 previous = n;
            n -= 2;
            if (!(sreg32(previous) >= 2))
                break;
        }
        n += 2;
    }
    while (n & 1)
    {
        sample();
        --n;
    }
    rc::st32(fractionAddress, fraction);
    rc::st32(positionAddress, position);
}

// Fast EA-XA loop; false leaves remaining frames to the reference.
bool eaxaFast(win32::WinApplication* app, x86::CPU& cpu, reg32 state)
{
    const reg32 CoefficientA = 0x819678;
    const reg32 CoefficientB = 0x819668;
    const reg32 NibbleTable = 0x819688;
    const reg32 TablesEnd = NibbleTable + 0x400;
    const reg32 RawFrame = 0xee;
    uint8_t* const m = rc::host(0);
    const reg32 callSlot = cpu.esp - 40 - 32 - 4 - 8;
    for (;;)
    {
        reg32 out = rc::ld32(state + 0x10);
        rc::st32(out - 4, rc::ld32(state + 4));
        rc::st32(out - 8, rc::ld32(state + 8));
        reg32 in = rc::ld32(state + 0xc);
        float y1 = rc::ldf32(out - 4);
        float y2 = rc::ldf32(out - 8);
        bool raw = false;
        while (sreg32(rc::ld32(state)) > 0)
        {
            if (rc::ld8(in) == RawFrame)
            {
                raw = true;
                break;
            }
            if ((out & 3) || out > 0u - 0x70 || in > 0u - 16 || state > 0u - 0x14 ||
                !kernels::apart(out, 0x70, in, 16) || !kernels::apart(out, 0x70, state, 0x14) ||
                !kernels::apart(out, 0x70, CoefficientB, TablesEnd - CoefficientB))
                return false;
            rc::st32(state, rc::ld32(state) - 0x1c);
            const reg32 header = rc::ld8(in);
            const float a = rc::ldf32((header >> 4) * 4 + CoefficientA);
            const float b = rc::ldf32((header >> 4) * 4 + CoefficientB);
            kernels::eaxaFrame(m + in, m + out, m + NibbleTable + ((header & 0xf) << 6), a, b, y1, y2);
            out += 0x70;
            in += 0xf;
            rc::stf32(state + 4, y1);
            rc::stf32(state + 8, y2);
            rc::st32(state + 0x10, out);
            rc::st32(state + 0xc, in);
        }
        if (!raw)
            break;
        rc::st32(state, rc::ld32(state) - 0x1c);
        const x86::CPU saved = cpu;
        rc::st32(callSlot + 4, state);
        cpu.esp = callSlot;
        sub_734a81(app, cpu);
        if (cpu.terminate)
            return true;
        const x86::FPU fpu = cpu.fpu;
        cpu = saved;
        cpu.fpu = fpu;
    }
    cpu.esp += 4;
    return true;
}

static void eaxa(win32::WinApplication* app, x86::CPU& cpu);

// EA-XA decoder (state); 0xEE frames go to 0x734A81.
void sub_734bf4(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!mixstats::on())
    {
        eaxa(app, cpu);
        return;
    }
    const mixstats::Scoped scope(cpu, mixstats::Eaxa);
    const reg32 state = argument(cpu, 0);
    const reg32 before = rc::ld32(state);
    eaxa(app, cpu);
    mixstats::work(cpu, mixstats::Eaxa, (before - rc::ld32(state)) / 0x1c);
}

static void eaxa(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!mixer::s_nativeKernels)
    {
        sub_734bf4_guest(app, cpu);
        return;
    }
    if (mixer::s_neonKernels && eaxaFast(app, cpu, argument(cpu, 0)))
        return;
    const reg32 CoefficientA = 0x819678;
    const reg32 CoefficientB = 0x819668;
    const reg32 NibbleTable = 0x819688;     // 16 rows of 16 floats, by shift and nibble
    const reg32 RawFrame = 0xee;
    const reg32 state = argument(cpu, 0);
    // The guest's own pushes lie between its return slot and the call's argument.
    const reg32 callSlot = cpu.esp - 40 - 32 - 4 - 8;
    reg32 out = rc::ld32(state + 0x10);
    rc::st32(out - 4, rc::ld32(state + 4));
    rc::st32(out - 8, rc::ld32(state + 8));
    reg32 in = rc::ld32(state + 0xc);
    while (sreg32(rc::ld32(state)) > 0)
    {
        rc::st32(state, rc::ld32(state) - 0x1c);
        if (rc::ld8(in) == RawFrame)
        {
            const x86::CPU saved = cpu;
            rc::st32(callSlot + 4, state);
            cpu.esp = callSlot;
            sub_734a81(app, cpu);
            if (cpu.terminate)
                return;
            const x86::FPU fpu = cpu.fpu;
            cpu = saved;
            cpu.fpu = fpu;
            out = rc::ld32(state + 0x10);
            rc::st32(out - 4, rc::ld32(state + 4));
            rc::st32(out - 8, rc::ld32(state + 8));
            in = rc::ld32(state + 0xc);
            continue;
        }
        const reg32 header = rc::ld8(in);
        const double a = loadFloat((header >> 4) * 4 + CoefficientA);
        const double b = loadFloat((header >> 4) * 4 + CoefficientB);
        const reg32 row = (header & 0xf) << 6;
        reg32 nibble = rc::ld8(in + 1) >> 4;
        out += 0x70;
        in += 0xf;
        for (reg32 i = reg32(-14); i != 0; ++i)
        {
            const reg32 at = out + i * 8;
            double previous = loadFloat(at - 4);
            previous *= b;
            double older = loadFloat(at - 8);
            older *= a;
            previous += loadFloat(row + nibble * 4 + NibbleTable);
            older += previous;
            nibble = rc::ld8(in + i) & 0xf;
            rc::stf32(at, float(older));
            previous = loadFloat(at);
            previous *= b;
            older = loadFloat(at - 4);
            older *= a;
            previous += loadFloat(row + nibble * 4 + NibbleTable);
            older += previous;
            nibble = rc::ld8(in + i + 1) >> 4;
            rc::stf32(at + 4, float(older));
        }
        rc::st32(state + 4, rc::ld32(out - 4));
        rc::st32(state + 8, rc::ld32(out - 8));
        rc::st32(state + 0x10, out);
        rc::st32(state + 0xc, in);
    }
    cpu.esp += 4;
}

// Output module: float to 16-bit (module, frames, buffer, output, flags).
void sub_72cac7(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!mixer::s_nativeKernels)
    {
        sub_72cac7_guest(app, cpu);
        return;
    }
    const reg32 RoundingBias = 0x7c6a68;
    const reg32 module = argument(cpu, 0);
    const reg32 frames = argument(cpu, 1);
    reg32 samples = argument(cpu, 2);
    reg32 out = argument(cpu, 3);
    const reg32 input = rc::ld32(module + 8);
    if (input)
    {
        const reg32 base = cpu.esp - 12 - 20;
        rc::st32(base, input);
        rc::st32(base + 4, frames);
        rc::st32(base + 8, out);
        rc::st32(base + 12, samples);
        rc::st32(base + 16, rc::ld8(module + 0x1a));
        const reg32 returnSlot = cpu.esp;
        const reg32 ebx = cpu.ebx, esi = cpu.esi, edi = cpu.edi;
        const reg32 depth = cpu.fpu.count;
        cpu.eax = rc::ld8(module + 0x1a);
        cpu.ecx = input;
        cpu.ebx = frames;
        cpu.esi = samples;
        cpu.edi = out;
        cpu.esp = base - 4;
        cpu.ip = rc::ld32(input);
        app->dynamic_call(cpu.ip, cpu);
        if (cpu.terminate)
            return;
        RC_FPU_CALL(cpu, depth, 0x72caeeu);
        cpu.ebx = ebx;
        cpu.esi = esi;
        cpu.edi = edi;
        cpu.esp = returnSlot + 4;
        if (sreg32(cpu.eax) <= 0)
            return;
    }
    else
    {
        cpu.esp += 4;
    }
    const double bias = loadFloat(RoundingBias);
    const reg32 end = samples + frames * 4;
    cpu.ecx = end;
    reg32 high = 0;
    reg32 low = 0;
    const bool fast = mixer::s_neonKernels &&
                      kernels::output(rc::host(0), samples, end, out, rc::ldf32(RoundingBias), high, low);
    for (; !fast && samples < end; samples += 4, out += 2)
    {
        double value = loadFloat(samples);
        value += bias;
        rc::stf32(samples, float(value));
        reg32 bits = rc::ld32(samples) & 0xfffff;
        if (bits > 0x7fff && bits < 0xf8000)
        {
            const bool positive = bits < 0x80000;
            high += positive;
            low += !positive;
            bits = positive ? 0x7fff : 0xffff8000;
        }
        rc::st16(out, x86::reg16(bits));
    }
    cpu.eax = frames;
    if (mixer::s_hash)
    {
        static uint32_t s_value = 2166136261u;
        static uint32_t s_blocks = 0;
        for (reg32 i = 0; i < frames; ++i)
            s_value = (s_value ^ rc::ld16(out - 2 * frames + 2 * i)) * 16777619u;
        if (++s_blocks % 128 == 0)
            pf::logInfo("audio hash: %u blocks %08x", unsigned(s_blocks), unsigned(s_value));
    }
    if (pf::perf::enabled())
    {
        pf::perf::audio(pf::perf::AudioOutputSamples, frames);
        pf::perf::audio(pf::perf::AudioSaturatedHigh, high);
        pf::perf::audio(pf::perf::AudioSaturatedLow, low);
    }
}

}

namespace mixer
{

namespace
{

using x86::reg32;

uint32_t s_random = 0x1234567u;
int s_modified = 0;     // comparisons where the kernel changed memory

uint32_t nextRandom()
{
    s_random ^= s_random << 13;
    s_random ^= s_random >> 17;
    s_random ^= s_random << 5;
    return s_random;
}

float randomFloat()
{
    const uint32_t kind = nextRandom() % 16;
    if (kind == 0)
        return 0.0f;
    if (kind == 1)
        return -0.0f;
    uint32_t bits = nextRandom();
    if (kind < 14)
        bits = (bits & 0x807fffffu) | ((120 + nextRandom() % 16) << 23);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

using Kernel = void (*)(win32::WinApplication*, x86::CPU&);

struct Scratch
{
    reg32 address;
    reg32 bytes;
    std::vector<uint8_t> snapshot;
    std::vector<uint8_t> result;
};

void fillRandom(Scratch& scratch, bool floats)
{
    for (reg32 i = 0; i + 4 <= scratch.bytes; i += 4)
        rc::st32(scratch.address + i, floats ? [] { float f = randomFloat(); reg32 v; memcpy(&v, &f, 4); return v; }() : nextRandom());
}

// Size of the differences of the last compareKernel, read as floats word by word: the largest gap in units in the last
// place, the largest relative gap, and words that differ but are not two finite floats of one sign.
struct Difference
{
    uint32_t words = 0;
    uint32_t maxUlps = 0;
    double maxRelative = 0.0;
    uint32_t other = 0;
    double maxAbsolute = 0.0;   // over all differing finite pairs, the sign changes included
};
Difference s_difference;

void measureDifference(const uint8_t* expected, const uint8_t* actual, reg32 bytes)
{
    s_difference = Difference();
    for (reg32 i = 0; i + 4 <= bytes; i += 4)
    {
        uint32_t a, b;
        memcpy(&a, expected + i, 4);
        memcpy(&b, actual + i, 4);
        if (a == b)
            continue;
        ++s_difference.words;
        float fa, fb;
        memcpy(&fa, &a, 4);
        memcpy(&fb, &b, 4);
        if (std::isfinite(fa) && std::isfinite(fb))
            s_difference.maxAbsolute = std::max(s_difference.maxAbsolute, std::fabs(double(fa) - double(fb)));
        if (!std::isfinite(fa) || !std::isfinite(fb) || ((a ^ b) & 0x80000000u))
        {
            ++s_difference.other;
            continue;
        }
        s_difference.maxUlps = std::max(s_difference.maxUlps, a > b ? a - b : b - a);
        const double scale = std::max(std::fabs(double(fa)), std::fabs(double(fb)));
        if (scale > 0.0)
            s_difference.maxRelative = std::max(s_difference.maxRelative, std::fabs(double(fa) - double(fb)) / scale);
    }
}

// Runs both kernels on the same memory; true when results agree.
bool compareKernel(win32::WinApplication* app, Kernel guest, Kernel nativeKernel, Scratch& scratch, reg32 stack,
                   std::initializer_list<reg32> arguments)
{
    uint8_t* const memory = rc::host(scratch.address);
    scratch.snapshot.assign(memory, memory + scratch.bytes);
    x86::CPU a{};
    x86::CPU b{};
    for (x86::CPU* cpu : { &a, &b })
    {
        memcpy(memory, scratch.snapshot.data(), scratch.bytes);
        reg32 slot = stack;
        rc::st32(slot, 0x1000);
        for (reg32 value : arguments)
        {
            slot += 4;
            rc::st32(slot, value);
        }
        cpu->esp = stack;
        (cpu == &a ? guest : nativeKernel)(app, *cpu);
        if (cpu == &a)
            scratch.result.assign(memory, memory + scratch.bytes);
    }
    s_modified += memcmp(scratch.result.data(), scratch.snapshot.data(), scratch.bytes) != 0;
    measureDifference(scratch.result.data(), memory, scratch.bytes);
    return memcmp(scratch.result.data(), memory, scratch.bytes) == 0 && a.esp == b.esp && a.eax == b.eax;
}

void runSelftest(win32::WinApplication& app)
{
    if (!NFSU_DEV_BOOL("audio_selftest", false))
        return;
    const reg32 Size = 0x10000;
    const reg32 base = win32::heap::allocate(Size, true);
    const reg32 stack = win32::heap::allocate(0x100, true);
    if (!base || !stack)
    {
        pf::logError("audio selftest: no guest memory");
        return;
    }
    Scratch scratch{base, Size, {}, {}};
    int failures = 0;
    int runs = 0;
    struct Totals
    {
        const char* name;
        uint32_t runs, differing, words, maxUlps, other;
        double maxRelative, maxAbsolute;
    };
    Totals totals[] = { {"mix", 0, 0, 0, 0, 0, 0.0, 0.0}, {"convert", 0, 0, 0, 0, 0, 0.0, 0.0},
                        {"resample", 0, 0, 0, 0, 0, 0.0, 0.0}, {"eaxa", 0, 0, 0, 0, 0, 0.0, 0.0} };
    auto check = [&](const char* name, bool ok)
    {
        ++runs;
        for (Totals& t : totals)
        {
            if (strcmp(t.name, name) != 0)
                continue;
            ++t.runs;
            t.differing += !ok;
            t.words += s_difference.words;
            t.maxUlps = std::max(t.maxUlps, s_difference.maxUlps);
            t.other += s_difference.other;
            t.maxRelative = std::max(t.maxRelative, s_difference.maxRelative);
            t.maxAbsolute = std::max(t.maxAbsolute, s_difference.maxAbsolute);
        }
        if (!ok && ++failures <= 20)
            pf::logError("audio selftest: %s differs: %u words, max %u ulps (relative %.3g), %u not comparable as "
                         "floats", name, unsigned(s_difference.words), unsigned(s_difference.maxUlps),
                         s_difference.maxRelative, unsigned(s_difference.other));
    };
    for (int round = 0; round < 300; ++round)
    {
        const reg32 n = 1 + nextRandom() % 700;
        float gain = randomFloat();
        reg32 gainBits;
        memcpy(&gainBits, &gain, 4);
        fillRandom(scratch, true);
        const reg32 src = base + 4 * (nextRandom() % 64);
        const reg32 dst = base + 0x4000 + 4 * (nextRandom() % 64);
        check("mix", compareKernel(&app, sub_72c360_guest, native::sub_72c360, scratch, stack, { n, gainBits, src, dst }));
        fillRandom(scratch, false);
        check("convert", compareKernel(&app, sub_72c480_guest, native::sub_72c480, scratch, stack,
                                       { n, base + 2 * (nextRandom() % 32), base + 0x4000 + 4 * (nextRandom() % 32) }));
        fillRandom(scratch, true);
        const reg32 steps = 1 + nextRandom() % 200;
        const reg32 positionAddress = base + 0x8000;
        const reg32 fractionAddress = base + 0x8004;
        rc::st32(positionAddress, nextRandom() % 8);
        rc::st32(fractionAddress, nextRandom());
        const reg32 stepFraction = nextRandom();
        const reg32 step = nextRandom() % 2;
        check("resample", compareKernel(&app, sub_733700_guest, native::sub_733700, scratch, stack,
                                        { steps, base, base + 0x4000, positionAddress, fractionAddress, step, stepFraction }));
    }
    for (int round = 0; round < 200; ++round)
    {
        fillRandom(scratch, false);
        const reg32 state = base + 0x8000;
        const reg32 frames = 1 + nextRandom() % 8;
        rc::st32(state, 0x1c * frames);
        rc::st32(state + 4, nextRandom() & 0x3f800000u);
        rc::st32(state + 8, nextRandom() & 0x3f800000u);
        rc::st32(state + 0xc, base);
        rc::st32(state + 0x10, base + 0x4000 + 0x100);
        for (reg32 f = 0; f < frames; ++f)
        {
            const reg32 header = base + f * 15;
            if (rc::ld8(header) == 0xee)
                rc::st8(header, 0x00);
            rc::st8(header, rc::ld8(header) & 0x7f);
        }
        check("eaxa", compareKernel(&app, sub_734bf4_guest, native::sub_734bf4, scratch, stack + 0x80, { state }));
    }
    pf::logInfo("audio selftest: %d kernel comparisons (%d changed memory), %d differ", runs, s_modified, failures);
    for (const Totals& t : totals)
        pf::logInfo("audio selftest: %s: %u of %u runs differ, %u words, max %u ulps (relative %.3g), largest "
                    "absolute difference %.3g, %u not comparable as floats", t.name, unsigned(t.differing),
                    unsigned(t.runs), unsigned(t.words), unsigned(t.maxUlps), t.maxRelative, t.maxAbsolute,
                    unsigned(t.other));
}

}

void configure(win32::WinApplication& app)
{
    runSelftest(app);
    const char* const mode = NFSU_DEV_STRING("audio_mixer", "unlocked");
    NFSU_DEV_SET(s_unlocked, strcmp(mode, "unlocked") == 0);
#ifdef DEV_SETTINGS
    s_nativeKernels = NFSU_AUDIO_FAST("audio_native_kernels");
    s_neonKernels = NFSU_AUDIO_FAST("audio_neon_kernels");
#endif
    NFSU_DEV_SET(s_hash, NFSU_DEV_BOOL("audio_hash", false));
    NFSU_DEV_SET(s_sndYield, s_unlocked && NFSU_DEV_BOOL("audio_snd_yield", false));
    NFSU_DEV_SET(s_stay, s_unlocked && NFSU_DEV_BOOL("audio_mixer_stay", true));
    const char* const dump = NFSU_DEV_STRING("audio_dump", "");
    if (s_unlocked && dump[0])
        NFSU_DEV_SET(s_dump, fopen(dump, "wb"));
    NFSU_DEV_SET(native::s_engineInline, s_unlocked && NFSU_DEV_BOOL("audio_engine_inline", true));
    pf::logInfo("audio mixer: thread %s between render loops (audio_mixer_stay), engine stream callback %s "
                "(audio_engine_inline)", s_stay ? "stays outside the execution lock" : "takes the execution lock",
                native::s_engineInline ? "inline" : "locked");
    if (s_sndYield)
    {
        s_handoff = new pf::Semaphore("snd_handoff", 0);
        pf::logInfo("audio mixer: hands the SND mutex to a waiting thread between voices and render steps");
    }
    NFSU_DEV_SET(native::s_inlineNotifications, NFSU_DEV_BOOL("audio_inline_notifications", true));
    NFSU_DEV_SET(native::s_auditObjectCalls, NFSU_DEV_BOOL("audio_library_audit", true));
    pf::logInfo("audio mixer: %s (audio_mixer=%s), %s x87 kernels%s, per-chunk notifications %s, object library "
                "calls %s",
                s_unlocked ? "outside the execution lock" : "under the execution lock", mode,
                s_nativeKernels ? "native" : "guest", s_nativeKernels && s_neonKernels ? " (float fast paths)" : "",
                native::s_inlineNotifications ? "inline" : "locked",
                native::s_auditObjectCalls ? "audited" : "locked");
}

bool reaudit(win32::WinApplication* app, x86::CPU& cpu)
{
    native::Audit audit;
    if (s_midLocked || !unlocked(cpu) || native::renderAudited(audit))
        return false;
    s_thread.store(0, std::memory_order_relaxed);
    setLock(app, cpu, s_lock);
    app->lockContext(cpu);
    s_midLocked = true;
    if (pf::perf::enabled())
        pf::perf::audioLocked(audit.reason, audit.address);
    return true;
}

bool libraryAudited(x86::reg32 target, const x86::CPU& cpu, const char*& reason, x86::reg32& value)
{
    const x86::reg32 SoundState = 0x720d57;
    const x86::reg32 InstanceTeardown = 0x71ff15;
    native::Audit audit;
    bool passed;
    if (target == SoundState)
        passed = native::soundAudited(rc::ld32(cpu.esp + 4), audit);
    else if (target == InstanceTeardown)
        passed = native::teardownAudited(rc::ld32(cpu.esp + 4), audit);
    else if (native::s_auditObjectCalls)
        passed = native::objectCallAudited(target, rc::ld32(cpu.esp + 4), audit);
    else
        passed = audit.fail("library call", target);
    reason = audit.reason;
    value = audit.address;
    return passed;
}

namespace
{

// Sorted: binary search, called about 2,300 times a frame.
constexpr x86::reg32 s_pure[] = {
    0x71feef, 0x71fefb, 0x71ff03, 0x71ff0b, 0x7201fe, 0x72023c, 0x72025f, 0x7202fb, 0x720340, 0x720376,
    0x7203bd, 0x7203eb, 0x720434, 0x7204f3, 0x7205f3, 0x720684, 0x72069e, 0x7206cc, 0x7206f2, 0x720702,
    0x720728, 0x720738, 0x720767, 0x720784, 0x7207a0, 0x7207aa, 0x7207b4, 0x7207bf, 0x7207d3, 0x720e48,
    0x720f32, 0x720fd1, 0x720ff8, 0x72100b, 0x72101f,
};

constexpr bool sorted(const x86::reg32* begin, const x86::reg32* end)
{
    for (const x86::reg32* p = begin + 1; p < end; ++p)
        if (!(p[-1] < *p))
            return false;
    return true;
}

static_assert(sorted(std::begin(s_pure), std::end(s_pure)), "s_pure must be sorted and unique");

}

bool libraryPure(x86::reg32 target)
{
    if (target < s_pure[0] || target > std::end(s_pure)[-1])
        return false;
    return std::binary_search(std::begin(s_pure), std::end(s_pure), target);
}

}

}
