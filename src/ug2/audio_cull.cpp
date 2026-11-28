// Silent-voice culling for the EA SND mixer: a voice whose gains are zero or under audio_silence_db skips the resampler
// kernel, filters and mix; its source modules run as usual.

#include "audio_cull.h"
#include "audio_split.h"

#include <lib/heap.h>
#include <platform/config.h>
#include <platform/switches.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <recomp.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ug2
{

void sub_733388(win32::WinApplication* app, x86::CPU& cpu);
void sub_733700_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace cull
{

namespace
{

using x86::reg32;
using x86::sreg32;

const reg32 VoiceArray      = 0x8b4a00;     // 0x5C-byte voices: +0 state (2 = playing), +1 pending start, +0x40 chain
const reg32 VoiceCount      = 0x8b47f8;     // byte
const reg32 ChannelCount    = 0x8b47f9;     // byte
const reg32 AuxCount        = 0x8180b8;
const reg32 AuxBuffers      = 0x8b4804;     // per aux effect: buffer or 0
const reg32 ChannelBuffers  = 0x8b48f0;     // per channel
const reg32 AuxIdle         = 0x8b47c8;     // aux effect idle counter, cleared when a voice mixes into an aux
const reg32 VoiceSize       = 0x5c;
const reg32 PlayingVoice    = 2;
const reg32 ChannelGains    = 0x1c;         // voice: float per channel
const reg32 AuxGains        = 0x38;         // voice: pointer to a float per aux effect

enum Kind
{
    Other,
    Source,
    Resampler,
    Filter,
    Gain,
};

struct ModuleKind
{
    reg32 render;
    Kind  kind;
};

const ModuleKind s_kinds[] = {
    {0x72802d, Source}, {0x7282ef, Source}, {0x7285bd, Source},     // MPEG streams
    {0x732c84, Source}, {0x732dfb, Source}, {0x733023, Source},     // EA-XA stream, loop, sample
    {0x7330f5, Source}, {0x7331ff, Source}, {0x7332a3, Source},     // PCM stream, loop, sample
    {0x72d18d, Resampler},
    {0x72a7a1, Filter}, {0x72a8c5, Filter}, {0x72a9b3, Filter}, {0x72aa55, Filter},
    {0x72a672, Gain},
};

Kind kindOf(reg32 render)
{
    for (const ModuleKind& k : s_kinds)
    {
        if (k.render == render)
            return k.kind;
    }
    return Other;
}

const int MaxChain  = 8;
const int SetSize   = 512;      // modules of silent voices, open addressing
const int SetLimit  = 256;
const int MaxSaved  = 512;
const reg32 MaxShadow = 2048;
const reg32 MaxShadowChannels = 8;
const int MaxCulled = 128;

struct Saved
{
    reg32 address;
    reg32 voice;
    reg32 chain;                // voice +0x40 when saved
    reg32 bits;
    uint32_t gains;             // gain changes of the voice when saved
};

bool s_enabled = false;
NFSU_DEV_VARC(bool, s_stats, false, false);                   // dev_cull_stats
NFSU_DEV_VARC(bool, s_check, false, false);                   // audio_cull_check: decide and measure, skip nothing
float s_threshold = 0.0f;               // linear gain
reg32 s_fs = 0;                         // thread running the voice render, else 0
bool s_skip = false;                    // inside a resampler module of a silent voice
reg32 s_set[SetSize];
int s_setCount = 0;
Saved s_saved[MaxSaved];
int s_savedCount = 0;
win32::MethodPtr s_resampler = nullptr;
win32::MethodPtr s_filter[4];
win32::MethodPtr s_cubicOriginal = nullptr;

struct Stats
{
    uint32_t renders = 0, voices = 0, zero = 0, quiet = 0, quietUnskippable = 0, since = 0;
    uint32_t kernels = 0, mismatches = 0, mixes = 0;
    uint32_t skippedKernels = 0, skippedFilters = 0, skippedMixes = 0;
    uint32_t byKind[3][2] = {};                    // MPEG, EA-XA, PCM: voices, culled
    float maxCall = 0.0f, maxDropped = 0.0f;       // largest dropped sample of one voice and of the sum, as 16-bit units
};
Stats s_stat;
reg32 s_culled[MaxCulled];              // voices decided silent this render
int s_culledCount = 0;
float s_shadow[MaxShadowChannels][MaxShadow];   // what the silent voices would have added to the channels
reg32 s_shadowLength = 0;

inline reg32 argument(const x86::CPU& cpu, int n)
{
    return rc::ld32(cpu.esp + 4 + 4 * n);
}

reg32 slot(reg32 module)
{
    return ((module >> 3) * 2654435761u) >> 23 & (SetSize - 1);
}

void insert(reg32 module)
{
    reg32 i = slot(module);
    while (s_set[i] && s_set[i] != module)
        i = (i + 1) & (SetSize - 1);
    if (!s_set[i])
        ++s_setCount;
    s_set[i] = module;
}

bool contains(reg32 module)
{
    for (reg32 i = slot(module); s_set[i]; i = (i + 1) & (SetSize - 1))
    {
        if (s_set[i] == module)
            return true;
    }
    return false;
}

inline bool quiet(float gain, double scale)
{
    return gain == 0.0f || std::fabs(double(gain)) * scale < double(s_threshold);
}

// Chain of a voice: false when it holds a module other than those the culling understands.
bool chainOf(reg32 root, reg32* skippable, int& skippableCount, double& scale)
{
    skippableCount = 0;
    scale = 1.0;
    reg32 module = root;
    for (int depth = 0; module; ++depth)
    {
        if (depth >= MaxChain)
            return false;
        const Kind kind = kindOf(rc::ld32(module));
        if (kind == Other)
            return false;
        if (kind == Resampler || kind == Filter)
            skippable[skippableCount++] = module;
        else if (kind == Gain)
            scale *= std::fabs(double(rc::ldf32(module + 0x1c)));
        module = rc::ld32(module + 8);
    }
    return true;
}

int sourceGroup(reg32 root)
{
    reg32 module = root;
    for (int depth = 0; module && depth < MaxChain; ++depth, module = rc::ld32(module + 8))
    {
        const reg32 render = rc::ld32(module);
        if (render == 0x72802d || render == 0x7282ef || render == 0x7285bd)
            return 0;
        if (render == 0x732c84 || render == 0x732dfb || render == 0x733023)
            return 1;
        if (render == 0x7330f5 || render == 0x7331ff || render == 0x7332a3)
            return 2;
    }
    return -1;
}

void markVoices()
{
    s_setCount = 0;
    s_savedCount = 0;
    s_culledCount = 0;
    memset(s_set, 0, sizeof(s_set));
    const reg32 voices = rc::ld32(VoiceArray);
    const reg32 count = rc::ld8(VoiceCount);
    const reg32 channels = rc::ld8(ChannelCount);
    const sreg32 aux = std::min<sreg32>(sreg32(rc::ld32(AuxCount)), 64);
    const reg32 auxBuffers = rc::ld32(AuxBuffers);
    for (reg32 i = 0; i < count; ++i)
    {
        const reg32 voice = voices + i * VoiceSize;
        if (rc::ld8(voice) != PlayingVoice || rc::ld8(voice + 1) != 0)
            continue;
        const reg32 root = rc::ld32(voice + 0x40);
        reg32 skippable[MaxChain];
        int skippableCount;
        double scale;
        const bool known = root && chainOf(root, skippable, skippableCount, scale);
        bool allQuiet = true;
        bool anyMixed = false;
        bool auxMixed = false;
        reg32 mixed[16 + 8];
        int mixedCount = 0;
        for (reg32 c = 0; c < channels && allQuiet; ++c)
        {
            const reg32 at = voice + ChannelGains + 4 * c;
            const float gain = rc::ldf32(at);
            if (gain == 0.0f)
                continue;
            anyMixed = true;
            allQuiet = quiet(gain, scale) && mixedCount < 16;
            if (allQuiet)
                mixed[mixedCount++] = at;
        }
        const reg32 auxGains = rc::ld32(voice + AuxGains);
        for (sreg32 k = 0; k < aux && allQuiet && auxGains; ++k)
        {
            if (!rc::ld32(auxBuffers + 4 * k))
                continue;
            const reg32 at = auxGains + 4 * k;
            const float gain = rc::ldf32(at);
            if (gain == 0.0f)
                continue;
            anyMixed = auxMixed = true;
            allQuiet = quiet(gain, scale) && mixedCount < 16 + 8;
            if (allQuiet)
                mixed[mixedCount++] = at;
        }
        if (s_stats)
        {
            ++s_stat.voices;
            s_stat.zero += allQuiet && !anyMixed;
            s_stat.quiet += allQuiet && anyMixed;
            s_stat.quietUnskippable += allQuiet && !known;
            const int group = known ? sourceGroup(root) : -1;
            if (group >= 0)
            {
                ++s_stat.byKind[group][0];
                s_stat.byKind[group][1] += allQuiet;
            }
            if (allQuiet && known)
                s_stat.skippedMixes += mixedCount;
        }
        if (!allQuiet || !known || s_setCount + skippableCount > SetLimit || s_savedCount + mixedCount > MaxSaved)
            continue;
        for (int k = 0; k < skippableCount; ++k)
            insert(skippable[k]);
        if (s_culledCount < MaxCulled)
            s_culled[s_culledCount++] = voice;
        if (s_check)
            continue;
        for (int k = 0; k < mixedCount; ++k)
        {
            s_saved[s_savedCount++] = {mixed[k], voice, root, rc::ld32(mixed[k]),
                                       split::gainWrites((voice - voices) / VoiceSize)};
            rc::st32(mixed[k], 0);
        }
        if (auxMixed)
            rc::st32(AuxIdle, 0);
    }
}

void restoreGains()
{
    for (int i = 0; i < s_savedCount; ++i)
    {
        const Saved& s = s_saved[i];
        if (rc::ld32(s.address) == 0 && rc::ld32(s.voice + 0x40) == s.chain
            && split::gainWrites((s.voice - rc::ld32(VoiceArray)) / VoiceSize) == s.gains)
            rc::st32(s.address, s.bits);
    }
    s_savedCount = 0;
}

void logStats()
{
    const uint32_t now = pf::perf::now();
    if (!s_stat.since)
        s_stat.since = now;
    if (now - s_stat.since < 1000000)
        return;
    const Stats& t = s_stat;
    pf::logInfo("cull: %.2f s: %u voice renders, %u voices: %u zero gain, %u under threshold (%u of them not skippable), "
                "culled %.1f%%", (now - t.since) / 1e6, t.renders, t.voices, t.zero, t.quiet, t.quietUnskippable,
                t.voices ? 100.0 * (t.zero + t.quiet - t.quietUnskippable) / t.voices : 0.0);
    pf::logInfo("cull skipped: %u resampler kernels, %u filters, %u mixes | by source (voices/culled): MPEG %u/%u, "
                "EA-XA %u/%u, PCM %u/%u", t.skippedKernels, t.skippedFilters, t.skippedMixes, t.byKind[0][0],
                t.byKind[0][1], t.byKind[1][0], t.byKind[1][1], t.byKind[2][0], t.byKind[2][1]);
    if (s_check)
        pf::logInfo("cull check: %u kernel advances compared, %u differ; %u mixes dropped, largest one sample %.3f, "
                    "largest sum in a channel %.3f (16-bit units)", t.kernels, t.mismatches, t.mixes, double(t.maxCall),
                    double(t.maxDropped));
    const Stats keep = s_stat;
    s_stat = Stats();
    s_stat.since = now;
    s_stat.maxCall = keep.maxCall;
    s_stat.maxDropped = keep.maxDropped;
}

bool counted(const x86::CPU& cpu)
{
    return s_fs && cpu.efs == s_fs;
}

void resamplerHook(win32::WinApplication* app, x86::CPU& cpu)
{
    const bool previous = s_skip;
    s_skip = counted(cpu) && contains(argument(cpu, 0));
    s_resampler(app, cpu);
    s_skip = previous;
}

void skipFilter(win32::WinApplication* app, x86::CPU& cpu, bool outputIsInput)
{
    ++s_stat.skippedFilters;
    const reg32 module = argument(cpu, 0);
    const reg32 frames = argument(cpu, 1);
    const reg32 child = rc::ld32(module + 8);
    const reg32 saved = cpu.esp;
    sreg32 result = sreg32(frames);
    if (child)
    {
        reg32 esp = saved;
        const auto push = [&esp](reg32 value) { esp -= 4; rc::st32(esp, value); };
        push(rc::ld8(module + 0x1a));
        push(argument(cpu, 2));
        push(argument(cpu, 3));
        push(frames);
        push(child);
        push(0);
        cpu.esp = esp;
        app->dynamic_call(rc::ld32(child), cpu);
        result = sreg32(cpu.eax);
        if (cpu.terminate)
            return;
    }
    cpu.esp = saved + 4;
    cpu.eax = (child && result <= 0) || (child && outputIsInput) ? reg32(result) : frames;
}

template <int K>
void filterHook(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_check && counted(cpu) && contains(argument(cpu, 0)))
    {
        skipFilter(app, cpu, K < 2);
        return;
    }
    s_filter[K](app, cpu);
}

void cubicHook(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!skipKernel(cpu))
    {
        s_cubicOriginal(app, cpu);
        return;
    }
    if (s_check)
    {
        const KernelCheck check(cpu, true);
        s_cubicOriginal(app, cpu);
        check.finish();
        return;
    }
    if (!advanceCubic(cpu))
        s_cubicOriginal(app, cpu);
}

}

bool enabled()
{
    return s_enabled;
}

void configure(win32::WinApplication& app)
{
    const char* const text = pf::Config::getString("audio_silence_db", "-60");
    NFSU_DEV_SET(s_stats, NFSU_DEV_BOOL("dev_cull_stats", false));
    NFSU_DEV_SET(s_check, NFSU_DEV_BOOL("audio_cull_check", false));
    if (NFSU_DEV_BOOL("audio_selftest", false))
        selfTest(app);
    if (NFSU_DEV_BOOL("audio_snd_yield", false))
    {
        pf::logInfo("audio cull: off while audio_snd_yield is on");
        return;
    }
    char* end = nullptr;
    double db = strtod(text, &end);
    if (strcmp(text, "off") == 0)
    {
        pf::logInfo("audio cull: off (audio_silence_db=off)");
        return;
    }
    if (end == text || *end)
    {
        pf::logInfo("audio cull: audio_silence_db=%s is not a number or off, using -60", text);
        db = -60.0;
    }
    if (db > 0.0)
        db = 0.0;
    s_threshold = float(std::pow(10.0, db / 20.0));
    s_enabled = true;
    s_resampler = app.replaceMethod(0x72d18d, &resamplerHook);
    s_filter[0] = app.replaceMethod(0x72a7a1, &filterHook<0>);
    s_filter[1] = app.replaceMethod(0x72a8c5, &filterHook<1>);
    s_filter[2] = app.replaceMethod(0x72a9b3, &filterHook<2>);
    s_filter[3] = app.replaceMethod(0x72aa55, &filterHook<3>);
    s_cubicOriginal = app.replaceMethod(0x733388, &cubicHook);
    NFSU_DEV_SET(s_stats, s_stats || s_check);
    pf::logInfo("audio cull: voices under %.1f dB (gain %.6f) skip resampler, filters and mix (audio_silence_db)", db,
                double(s_threshold));
}

Voices::Voices(const x86::CPU& cpu)
    :   m_on(s_enabled)
{
    if (!m_on)
        return;
    s_fs = cpu.efs;
    markVoices();
}

Voices::~Voices()
{
    if (!m_on)
        return;
    restoreGains();
    if (s_check)
    {
        for (reg32 c = 0; c < MaxShadowChannels; ++c)
        {
            for (reg32 i = 0; i < s_shadowLength; ++i)
                s_stat.maxDropped = std::max(s_stat.maxDropped, std::fabs(s_shadow[c][i]));
            memset(s_shadow[c], 0, sizeof(float) * s_shadowLength);
        }
        s_shadowLength = 0;
    }
    s_fs = 0;
    s_skip = false;
    if (s_stats)
    {
        ++s_stat.renders;
        logStats();
    }
}

bool skipKernel(const x86::CPU& cpu)
{
    if (s_skip && counted(cpu))
    {
        ++s_stat.skippedKernels;
        return true;
    }
    return false;
}

bool checking()
{
    return s_check;
}

namespace
{

struct Advanced
{
    reg32 position, fraction;
    bool  valid;
};

Advanced predictLinear(const x86::CPU& cpu)
{
    const sreg32 n = sreg32(argument(cpu, 0));
    Advanced a{rc::ld32(argument(cpu, 3)), rc::ld32(argument(cpu, 4)), true};
    if (n <= 0)
        return a;
    const uint64_t total = uint64_t(a.fraction) + uint64_t(reg32(n)) * argument(cpu, 6);
    a.fraction = reg32(total);
    a.position += reg32(n) * argument(cpu, 5) + reg32(total >> 32);
    return a;
}

Advanced predictCubic(const x86::CPU& cpu)
{
    const sreg32 n = sreg32(argument(cpu, 0));
    const reg32 whole = argument(cpu, 5);
    Advanced a{rc::ld32(argument(cpu, 3)), rc::ld32(argument(cpu, 4)), whole < 0x8000};
    const reg32 step = (whole << 16) | (argument(cpu, 6) >> 16);
    const uint64_t total = (a.fraction >> 16) + (n > 0 ? uint64_t(reg32(n)) * step : 0);
    a.fraction = reg32(total & 0xffff) << 16;
    a.position += reg32(total >> 16);
    return a;
}

void store(const x86::CPU& cpu, const Advanced& a)
{
    rc::st32(argument(cpu, 3), a.position);
    rc::st32(argument(cpu, 4), a.fraction);
}

}

void advanceLinear(x86::CPU& cpu)
{
    store(cpu, predictLinear(cpu));
    cpu.esp += 4;
}

bool advanceCubic(x86::CPU& cpu)
{
    const Advanced a = predictCubic(cpu);
    if (!a.valid)
        return false;
    store(cpu, a);
    cpu.esp += 4;
    return true;
}

KernelCheck::KernelCheck(const x86::CPU& cpu, bool cubic)
    :   m_position(argument(cpu, 3))
    ,   m_fraction(argument(cpu, 4))
{
    const Advanced a = cubic ? predictCubic(cpu) : predictLinear(cpu);
    m_valid = a.valid;
    m_predictedPosition = a.position;
    m_predictedFraction = a.fraction;
}

void KernelCheck::finish() const
{
    if (!m_valid)
        return;
    ++s_stat.kernels;
    s_stat.mismatches += rc::ld32(m_position) != m_predictedPosition || rc::ld32(m_fraction) != m_predictedFraction;
}

void checkMix(const x86::CPU& cpu)
{
    const reg32 voice = cpu.esi;
    bool culled = false;
    for (int i = 0; i < s_culledCount && !culled; ++i)
        culled = s_culled[i] == voice;
    if (!counted(cpu) || !culled || argument(cpu, 2) != rc::ld32(0x8b48d4))
        return;
    const reg32 n = std::min<reg32>(argument(cpu, 0), MaxShadow);
    const reg32 dst = argument(cpu, 3);
    const reg32 channels = std::min<reg32>(rc::ld8(ChannelCount), MaxShadowChannels);
    for (reg32 c = 0; c < channels; ++c)
    {
        if (rc::ld32(ChannelBuffers + 4 * c) != dst)
            continue;
        float gain;
        const reg32 bits = argument(cpu, 1);
        memcpy(&gain, &bits, sizeof(gain));
        for (reg32 i = 0; i < n; ++i)
        {
            const float v = gain * rc::ldf32(argument(cpu, 2) + 4 * i);
            s_shadow[c][i] += v;
            s_stat.maxCall = std::max(s_stat.maxCall, std::fabs(v));
        }
        s_shadowLength = std::max(s_shadowLength, n);
        ++s_stat.mixes;
        return;
    }
}

void selfTest(win32::WinApplication& app)
{
    const reg32 Size = 0x10000;
    const reg32 base = win32::heap::allocate(Size, true);
    const reg32 stack = win32::heap::allocate(0x100, true);
    if (!base || !stack)
    {
        pf::logError("audio cull selftest: no guest memory");
        return;
    }
    uint32_t random = 0x2468aceu;
    const auto next = [&random]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
    uint32_t runs[2] = {0, 0}, bad[2] = {0, 0};
    for (int round = 0; round < 4000; ++round)
    {
        const bool cubic = round & 1;
        for (reg32 i = 0; i < 0x4000; i += 4)
        {
            const float f = float(int32_t(next() % 20001) - 10000) / 7.0f;
            rc::stf32(base + i, f);
        }
        const reg32 n = 1 + next() % 600;
        const reg32 whole = next() % 4;
        const reg32 stepFraction = (next() & 0xffff) << 16;
        const reg32 position = base + 0x8000, fraction = base + 0x8004;
        const reg32 start = next() % 64;
        const reg32 startFraction = cubic ? (next() & 0xffff) << 16 : next();
        reg32 result[2][2];
        for (int which = 0; which < 2; ++which)
        {
            rc::st32(position, start);
            rc::st32(fraction, startFraction);
            x86::CPU cpu{};
            cpu.esp = stack;
            const reg32 args[] = {0x1000, n, base + 32, base + 0x4000, position, fraction, whole, stepFraction};
            for (int k = 0; k < 8; ++k)
                rc::st32(stack + 4 * k, args[k]);
            if (which == 0)
                cubic ? sub_733388(&app, cpu) : sub_733700_guest(&app, cpu);
            else if (!cubic)
                advanceLinear(cpu);
            else if (!advanceCubic(cpu))
                break;
            result[which][0] = rc::ld32(position);
            result[which][1] = rc::ld32(fraction);
        }
        ++runs[cubic];
        bad[cubic] += result[0][0] != result[1][0] || result[0][1] != result[1][1];
    }
    pf::logInfo("audio cull selftest: advance of the linear kernel %u runs, %u differ; of the cubic kernel %u runs, "
                "%u differ", runs[0], bad[0], runs[1], bad[1]);
}

}}
