// The EALayer3 decoder object's granule method (0x72C270) on the hardware MP3 decoder: granule pairs become MPEG-1
// frames. Stream reading, PCM inserts, start delay and end trim stay the game's.

#include "music_decode.h"
#include "ealayer3.h"

#include <platform/config.h>
#include <platform/switches.h>
#include <platform/cores.h>
#include <platform/log.h>
#include <platform/mp3dec.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <recomp.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ug2 { namespace music
{

namespace
{

using x86::reg32;
using ealayer3::Granule;
using ealayer3::GranuleSamples;

const reg32 DecodeGranule   = 0x72c270;     // vtable +0x18, thiscall: granule at [+0x24] to PCM at [+0x1B8]
const reg32 StartStream     = 0x72beb3;     // vtable +8, thiscall (first granule, 0)
const reg32 Destroy         = 0x72c2a6;     // vtable +0, thiscall (flags)
const reg32 ParseHeader     = 0x72be11;     // vtable +4, thiscall (header byte)
const reg32 ReadPointer     = 0x24;
const reg32 FrameStart      = 0x28;
const reg32 GranuleBytes    = 0x14;
const reg32 Output          = 0x1b8;        // float, 576 per channel, channel after channel
const int   MaxStreams      = 16;
const uint32_t StealAfter   = 8000;         // granules decoded since a stream's last one
const uint32_t CheckGranules = 48;          // left channel kept for the delay search (music_hw_check)
const int   MaxLag          = 1152;
const uint32_t CheckLogEvery = 500;
const int   WorkerPriority  = 70;           // just above the mixer (72)

struct Check
{
    uint32_t granules = 0, sizeMismatches = 0, over2 = 0, apart = 0, clipped = 0, reported = 0;
    uint32_t inserts = 0, insertsFound = 0;
    FILE* dump = nullptr;
    uint32_t dumped = 0;
    uint32_t insertOffset = 0, insertCount = 0;  // PCM insert after the last granule, checked at the next call
    int16_t insertPcm[GranuleSamples * 2];
    float maxDiff = 0;
    double sumSquares = 0;
    uint32_t kept = 0;
    bool searched = false;
    float game[CheckGranules * GranuleSamples];
    float hardware[CheckGranules * GranuleSamples];
};

struct Stream
{
    reg32 object = 0;
    pf::mp3::Decoder* decoder = nullptr;
    bool hardware = false;
    int header = -1;                        // last header byte given to the game's parser
    reg32 pendingAt = 0;                    // granule 1 whose samples are in pending
    bool paired = false;                    // the last granule's frame had both granules
    uint32_t lone = 0;                      // frames with an empty partner granule
    uint32_t lastUse = 0;
    int16_t pending[GranuleSamples * 2];
    Check* check = nullptr;
};

Stream s_streams[MaxStreams];
win32::MethodPtr s_decode = nullptr;
win32::MethodPtr s_start = nullptr;
win32::MethodPtr s_destroy = nullptr;
NFSU_DEV_VARC(bool, s_check, false, false);                       // music_hw_check: both decoders, compared
NFSU_DEV_VARC(const char*, s_dumpPath, nullptr, "");           // music_hw_check_dump: first granules of each stream, both decoders
int s_dumps = 0;
const uint32_t DumpGranules = 300;
uint32_t s_clock = 0;                       // granules decoded
bool s_failLogged = false;

struct Timing
{
    uint32_t frames = 0, us = 0, maxUs = 0, gameGranules = 0, since = 0;
    uint32_t ahead = 0, misses = 0, waits = 0, waitUs = 0;
};
Timing s_timing;

enum JobState
{
    JobIdle,
    JobQueued,
    JobDone,
};

// The next frame of a stream, decoded by the worker while the mixer plays the current one.
struct Job
{
    std::atomic<int> state{JobIdle};
    pf::mp3::Decoder* decoder = nullptr;
    reg32 at = 0;                           // its first granule
    uint32_t size = 0;
    int samples = 0;
    uint32_t us = 0;
    uint8_t frame[ealayer3::MaxFrameBytes];
    int16_t pcm[ealayer3::FrameSamples * 2];
};

Job s_jobs[MaxStreams];                     // one per entry of s_streams
pf::Semaphore* s_work = nullptr;
pf::Semaphore* s_done = nullptr;
NFSU_DEV_VARC(bool, s_ahead, true, true);                        // music_hw_ahead (dev): decode the next frame on the worker

Stream* find(reg32 object)
{
    for (Stream& s : s_streams)
    {
        if (s.object == object)
            return &s;
    }
    return nullptr;
}

Job& job(const Stream& s)
{
    return s_jobs[&s - s_streams];
}

// Waits for the stream's queued frame.
void settle(Stream& s)
{
    Job& j = job(s);
    if (j.state.load(std::memory_order_acquire) != JobQueued)
        return;
    const uint32_t start = pf::perf::now();
    while (j.state.load(std::memory_order_acquire) == JobQueued)
        s_done->waitForUs(2000);
    ++s_timing.waits;
    s_timing.waitUs += pf::perf::now() - start;
}

int worker(void*)
{
    if (pf::perf::enabled())
        pf::perf::trackThread("music_decode");
    for (;;)
    {
        s_work->wait();
        for (Job& j : s_jobs)
        {
            if (j.state.load(std::memory_order_acquire) != JobQueued)
                continue;
            const uint32_t start = pf::perf::now();
            j.samples = pf::mp3::decode(j.decoder, j.frame, j.size, j.pcm);
            j.us = pf::perf::now() - start;
            j.state.store(JobDone, std::memory_order_release);
            s_done->post();
        }
    }
    return 0;
}

pf::mp3::Decoder* acquire()
{
    if (pf::mp3::Decoder* decoder = pf::mp3::create())
        return decoder;
    Stream* idle = nullptr;
    for (Stream& s : s_streams)
    {
        if (s.decoder && s_clock - s.lastUse > StealAfter && (!idle || s.lastUse < idle->lastUse))
            idle = &s;
    }
    if (!idle)
        return nullptr;
    settle(*idle);
    s_jobs[idle - s_streams].state.store(JobIdle, std::memory_order_relaxed);
    pf::mp3::Decoder* decoder = idle->decoder;
    idle->decoder = nullptr;
    return decoder;
}

void logCheck(const Stream& s)
{
    const Check& c = *s.check;
    if (!c.granules)
        return;
    const double samples = double(c.granules) * GranuleSamples * 2;
    pf::logInfo("music check 0x%08x: %u granules, max difference %.2f, rms %.4f, %u samples over 2 LSB in %u "
                "granules, %u past 16 bits in the game's (left out), %u granule size mismatches, %u of %u PCM inserts "
                "in place, %u lone granules",
                unsigned(s.object), unsigned(c.granules), double(c.maxDiff), std::sqrt(c.sumSquares / samples),
                unsigned(c.over2), unsigned(c.apart), unsigned(c.clipped), unsigned(c.sizeMismatches),
                unsigned(c.insertsFound), unsigned(c.inserts), unsigned(s.lone));
}

void release(Stream& s)
{
    if (s.check)
    {
        if (s.check->dump)
            fclose(s.check->dump);
        logCheck(s);
        delete s.check;
    }
    settle(s);
    job(s).state.store(JobIdle, std::memory_order_relaxed);
    if (s.decoder)
        pf::mp3::destroy(s.decoder);
    s = Stream();
}

void failed(Stream& s, const char* what)
{
    if (!s_failLogged)
    {
        s_failLogged = true;
        pf::logError("music: hardware decoder %s (0x%08X); the game's decoder takes over", what,
                     unsigned(pf::mp3::lastError()));
    }
    s.hardware = false;
}

void store(const int16_t* pcm, float* out)
{
    for (uint32_t i = 0; i < GranuleSamples; ++i)
    {
        out[i] = float(pcm[2 * i]);
        out[GranuleSamples + i] = float(pcm[2 * i + 1]);
    }
}

bool sameShape(const Granule& a, const Granule& b)
{
    return a.version == b.version && a.rateIndex == b.rateIndex && a.channels == b.channels;
}

void countDecode(uint32_t us)
{
    Timing& t = s_timing;
    ++t.frames;
    t.us += us;
    t.maxUs = std::max(t.maxUs, us);
}

// Decodes the frame, taking the worker's result when bytes match.
bool decodeFrame(Stream& s, reg32 at, const Granule* first, const Granule* second, int16_t* pcm)
{
    settle(s);
    if (!s.decoder)
    {
        s.decoder = acquire();
        if (!s.decoder || !pf::mp3::reset(s.decoder))
            return false;
    }
    uint8_t frame[ealayer3::MaxFrameBytes];
    const uint32_t size = ealayer3::frame(first, second, frame);
    if (!size)
        return false;
    Job& j = job(s);
    if (j.state.load(std::memory_order_acquire) == JobDone)
    {
        j.state.store(JobIdle, std::memory_order_relaxed);
        if (j.at == at && j.size == size && memcmp(j.frame, frame, size) == 0)
        {
            ++s_timing.ahead;
            countDecode(j.us);
            memcpy(pcm, j.pcm, sizeof(j.pcm));
            return j.samples == int(ealayer3::FrameSamples);
        }
        ++s_timing.misses;
        if (!pf::mp3::reset(s.decoder))
            return false;
    }
    const uint32_t start = pf::perf::now();
    const int samples = pf::mp3::decode(s.decoder, frame, size, pcm);
    countDecode(pf::perf::now() - start);
    return samples == int(ealayer3::FrameSamples);
}

uint32_t le32(const uint8_t* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

// Next frame's first granule, in this block or the next.
const uint8_t* following(const Granule& second)
{
    Granule g;
    if (const uint8_t* p = ealayer3::next(second))
    {
        if (ealayer3::parse(p, g) && g.index == 0 && sameShape(g, second))
            return p;
    }
    const uint8_t* end = second.data + second.bytes;
    for (const uint8_t* p = end; p <= end + 4; ++p)
    {
        // In the game's stream buffer a block's tag reads 0x100, not SCDl.
        if (memcmp(p, "SCDl", 4) != 0 && le32(p) != 0x100)
            continue;
        const uint32_t size = le32(p + 4), samples = le32(p + 8);
        const uint8_t* first = p + 8 + 4 + 4 * second.channels;
        if (size >= 32 && size <= 0x10000 && samples <= 0x10000 && first[0] == 0 && first[1] != 0
            && ealayer3::parse(first + 1, g) && g.index == 0 && sameShape(g, second))
            return first + 1;
    }
    return nullptr;
}

// Queues the frame after this pair for the worker.
void queueNext(Stream& s, reg32 at, const Granule& first, const Granule& second)
{
    Job& j = job(s);
    if (!s_ahead || !s.decoder || j.state.load(std::memory_order_acquire) != JobIdle)
        return;
    const uint8_t* data = following(second);
    Granule a, b;
    if (!data || !ealayer3::parse(data, a) || a.index != 0 || !sameShape(first, a))
    {
        static int s_logged = 0;
        if (s.check && s_logged++ < 6)
        {
            const uint8_t* e = second.data + second.bytes;
            pf::logInfo("music check: no next frame after 0x%08x (%s): %02x %02x %02x %02x %02x %02x %02x %02x | "
                        "%02x %02x %02x %02x %02x %02x %02x %02x", unsigned(at + reg32(e - first.data)),
                        data ? "bad granule" : "no block", e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], e[9],
                        e[10], e[11], e[12], e[13], e[14], e[15]);
        }
        return;
    }
    const uint8_t* bData = ealayer3::next(a);
    const bool pair = bData && ealayer3::parse(bData, b) && b.index == 1 && sameShape(a, b);
    j.size = ealayer3::frame(&a, pair ? &b : nullptr, j.frame);
    if (!j.size)
        return;
    j.at = at + reg32(data - first.data);
    j.decoder = s.decoder;
    j.state.store(JobQueued, std::memory_order_release);
    s_work->post();
}

// Hardware-decoded samples of granule g into out.
bool hardwareGranule(Stream& s, reg32 at, const Granule& g, float* out)
{
    int16_t pcm[ealayer3::FrameSamples * 2];
    if (g.index == 0)
    {
        const uint8_t* nextData = ealayer3::next(g);
        Granule second;
        const bool pair = nextData && ealayer3::parse(nextData, second) && second.index == 1 && sameShape(g, second);
        if (!decodeFrame(s, at, &g, pair ? &second : nullptr, pcm))
            return false;
        store(pcm, out);
        memcpy(s.pending, pcm + GranuleSamples * 2, sizeof(s.pending));
        if (pair)
            queueNext(s, at, g, second);
        s.pendingAt = pair ? at + reg32(nextData - g.data) : 0;
        s.paired = pair;
        s.lone += !pair;
        return true;
    }
    if (s.pendingAt == at)
    {
        store(s.pending, out);
        s.pendingAt = 0;
        s.paired = true;
        return true;
    }
    if (!decodeFrame(s, 0, nullptr, &g, pcm))
        return false;
    store(pcm + GranuleSamples * 2, out);
    s.pendingAt = 0;
    s.paired = false;
    ++s.lone;
    return true;
}

void parseHeader(win32::WinApplication* app, x86::CPU& cpu, reg32 object, uint8_t header)
{
    const x86::CPU saved = cpu;
    cpu.esp -= 4;
    rc::st32(cpu.esp, header);
    cpu.esp -= 4;
    cpu.ecx = object;
    app->dynamic_call(ParseHeader, cpu);
    if (cpu.terminate)
        return;
    const x86::FPU fpu = cpu.fpu;
    cpu = saved;
    cpu.fpu = fpu;
}

void searchDelay(Stream& s)
{
    Check& c = *s.check;
    c.searched = true;
    const int n = int(CheckGranules * GranuleSamples);
    double best = 1e30, atZero = 0;
    int bestLag = 0;
    for (int lag = -MaxLag; lag <= MaxLag; ++lag)
    {
        double sum = 0;
        for (int i = MaxLag; i < n - MaxLag; i += 2)
            sum += std::fabs(double(c.game[i]) - double(c.hardware[i + lag]));
        if (sum < best)
        {
            best = sum;
            bestLag = lag;
        }
        if (lag == 0)
            atZero = sum;
    }
    const double count = double(n - 2 * MaxLag) / 2;
    pf::logInfo("music check 0x%08x: delay search over %u granules: hardware output %d samples from the game's "
                "(mean difference %.3f there, %.3f at 0)", unsigned(s.object), unsigned(CheckGranules), -bestLag,
                best / count, atZero / count);
}

// Copies the PCM insert at p for checkInsert.
void keepInsert(Check& c, const uint8_t* p)
{
    ++c.inserts;
    const uint32_t offset = (uint32_t(p[1]) << 8) | p[2], count = (uint32_t(p[3]) << 8) | p[4];
    if (offset > GranuleSamples || count > offset)
        return;
    c.insertOffset = offset;
    c.insertCount = count;
    for (uint32_t i = 0; i < 2 * count; ++i)
        c.insertPcm[i] = int16_t((p[5 + 2 * i] << 8) | p[6 + 2 * i]);
}

// Counts the kept insert when the game spliced it into out.
void checkInsert(Check& c, const float* out)
{
    bool same = true;
    for (uint32_t i = 0; i < c.insertCount; ++i)
    {
        for (uint32_t ch = 0; ch < 2; ++ch)
            same &= out[ch * GranuleSamples + GranuleSamples - c.insertOffset + i] == float(c.insertPcm[2 * i + ch]);
    }
    c.insertsFound += same;
    c.insertCount = 0;
}

void compare(Stream& s, const float* game, const float* hardware, uint32_t gameBytes, const Granule& g, bool paired)
{
    Check& c = *s.check;
    const uint32_t bytes = g.bytes;
    const uint32_t over2 = c.over2;
    ++c.granules;
    c.sizeMismatches += gameBytes != bytes;
    float peak = 0;
    for (uint32_t i = 0; i < GranuleSamples * 2; ++i)
    {
        const float d = std::fabs(game[i] - hardware[i]);
        if (std::fabs(game[i]) > 32767.0f)
        {
            ++c.clipped;
            continue;
        }
        c.maxDiff = std::max(c.maxDiff, d);
        c.sumSquares += double(d) * d;
        c.over2 += d > 2.0f;
        peak = std::max(peak, std::fabs(game[i]));
    }
    c.apart += c.over2 > over2;
    if (!c.searched && (c.kept || peak > 1000.0f))
    {
        memcpy(c.game + c.kept * GranuleSamples, game, GranuleSamples * sizeof(float));
        memcpy(c.hardware + c.kept * GranuleSamples, hardware, GranuleSamples * sizeof(float));
        if (++c.kept == CheckGranules)
            searchDelay(s);
    }
    if (!c.dump && c.over2 > over2 && s_dumpPath && *s_dumpPath && s_dumps < 4)
    {
        char path[256];
        snprintf(path, sizeof(path), "%s%02d.f32", s_dumpPath, s_dumps++);
        c.dump = fopen(path, "wb");
        pf::logInfo("music check 0x%08x: first difference over 2 LSB at granule %u, dumping to %s", unsigned(s.object),
                    unsigned(c.granules), path);
    }
    if (c.dump && c.dumped < DumpGranules)
    {
        ++c.dumped;
        const uint32_t head[4] = {c.granules, g.index, paired, bytes};
        fwrite(head, sizeof(head), 1, c.dump);
        fwrite(game, sizeof(float), GranuleSamples * 2, c.dump);
        fwrite(hardware, sizeof(float), GranuleSamples * 2, c.dump);
        fwrite(g.data, 1, 1536, c.dump);
        fflush(c.dump);
    }
    if (c.granules - c.reported >= CheckLogEvery)
    {
        c.reported = c.granules;
        logCheck(s);
    }
}

void logTiming()
{
    Timing& t = s_timing;
    const uint32_t now = pf::perf::now();
    if (int32_t(now - t.since) < 5000000)
        return;
    if (t.frames || t.gameGranules)
        pf::logInfo("music hw 5s: %u frames (%u decoded ahead, %u predictions missed), %u us average, %u us max; "
                    "mixer waited %u times, %u us; %u granules on the game's decoder", unsigned(t.frames),
                    unsigned(t.ahead), unsigned(t.misses), unsigned(t.frames ? t.us / t.frames : 0),
                    unsigned(t.maxUs), unsigned(t.waits), unsigned(t.waitUs), unsigned(t.gameGranules));
    t = Timing();
    t.since = now;
}

void decodeHook(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 object = cpu.ecx;
    Stream* s = find(object);
    if (pf::perf::enabled())
        logTiming();
    if (!s || !s->hardware)
    {
        ++s_timing.gameGranules;
        s_decode(app, cpu);
        return;
    }
    ++s_clock;
    s->lastUse = s_clock;
    const reg32 at = rc::ld32(object + ReadPointer);
    Granule g;
    if (!ealayer3::parse(rc::host(at), g) || g.version != 3 || g.channels != 2)
    {
        failed(*s, "got an unexpected granule");
        s_decode(app, cpu);
        return;
    }
    float* out = reinterpret_cast<float*>(rc::host(rc::ld32(object + Output)));
    if (s->check)
    {
        if (s->check->insertCount)
            checkInsert(*s->check, out);
        if (g.data[g.bytes] == 0xee)
            keepInsert(*s->check, g.data + g.bytes);
        s_decode(app, cpu);
        if (cpu.terminate)
            return;
        float game[GranuleSamples * 2];
        memcpy(game, out, sizeof(game));
        if (!hardwareGranule(*s, at, g, out))
        {
            failed(*s, "failed");
            memcpy(out, game, sizeof(game));
            return;
        }
        compare(*s, game, out, rc::ld32(object + GranuleBytes), g, s->paired);
        return;
    }
    rc::st32(object + FrameStart, at);
    if (g.data[0] != s->header)
    {
        s->header = g.data[0];
        parseHeader(app, cpu, object, g.data[0]);
        if (cpu.terminate)
            return;
    }
    if (!hardwareGranule(*s, at, g, out))
    {
        failed(*s, "failed");
        s_decode(app, cpu);
        return;
    }
    rc::st32(object + GranuleBytes, g.bytes);
    cpu.eax = 0;
    cpu.esp += 4;
}

// Least recently used entry when all are taken.
Stream& add(reg32 object)
{
    Stream* slot = &s_streams[0];
    for (Stream& s : s_streams)
    {
        if (!s.object)
        {
            slot = &s;
            break;
        }
        if (s.lastUse < slot->lastUse)
            slot = &s;
    }
    if (slot->object)
        release(*slot);
    slot->object = object;
    return *slot;
}

// Checks the PCM insert a stream ended with.
void lastInsert(Stream* s)
{
    if (s && s->check && s->check->insertCount)
        checkInsert(*s->check, reinterpret_cast<const float*>(rc::host(rc::ld32(s->object + Output))));
}

void startHook(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 object = cpu.ecx;
    const reg32 first = rc::ld32(cpu.esp + 4);
    lastInsert(find(object));
    s_start(app, cpu);
    if (cpu.terminate)
        return;
    Granule g;
    const bool eligible = ealayer3::parse(rc::host(first), g) && g.version == 3 && g.channels == 2;
    Stream* s = find(object);
    if (!eligible)
    {
        if (s)
            release(*s);
        pf::logInfo("music: stream 0x%08x starts, header 0x%02x, game decoder (not MPEG-1 stereo)", unsigned(object),
                    unsigned(rc::ld8(first)));
        return;
    }
    if (!s)
        s = &add(object);
    if (s->check)
    {
        if (s->check->dump)
            fclose(s->check->dump);
        logCheck(*s);
        delete s->check;
        s->check = nullptr;
    }
    settle(*s);
    job(*s).state.store(JobIdle, std::memory_order_relaxed);
    if (!s->decoder)
        s->decoder = acquire();
    s->hardware = s->decoder && pf::mp3::reset(s->decoder);
    s->header = -1;
    s->pendingAt = 0;
    s->lastUse = s_clock;
    if (s->hardware && s_check)
    {
        s->check = new Check();
    }
    pf::logInfo("music: stream 0x%08x starts, header 0x%02x, %s", unsigned(object), unsigned(rc::ld8(first)),
                s->hardware ? "hardware decoder" : "game decoder (no hardware decoder free)");
}

void destroyHook(win32::WinApplication* app, x86::CPU& cpu)
{
    if (Stream* s = find(cpu.ecx))
    {
        lastInsert(s);
        release(*s);
    }
    s_destroy(app, cpu);
}

// Decodes two empty frames: the decoder works, 1152 samples each.
bool selfTest()
{
    pf::mp3::Decoder* decoder = pf::mp3::create();
    if (!decoder)
        return false;
    Granule silent;
    silent.version = 3;
    silent.rateIndex = 2;
    silent.mode = 1;
    silent.channels = 2;
    uint8_t frame[ealayer3::MaxFrameBytes];
    int16_t pcm[ealayer3::FrameSamples * 2];
    const uint32_t size = ealayer3::frame(&silent, &silent, frame);
    const uint32_t start = pf::perf::now();
    const int first = pf::mp3::decode(decoder, frame, size, pcm);
    const int second = pf::mp3::decode(decoder, frame, size, pcm);
    const uint32_t us = pf::perf::now() - start;
    pf::mp3::destroy(decoder);
    pf::logInfo("music: hardware decoder test: %d and %d samples from two %u-byte frames in %u us", first, second,
                unsigned(size), unsigned(us));
    return first == int(ealayer3::FrameSamples) && second == int(ealayer3::FrameSamples);
}

}

void configure(win32::WinApplication& app)
{
    if (!pf::Config::getBool("music_hw_decode", true))
        return;
    if (!pf::mp3::init() || !selfTest())
    {
        pf::logError("music: hardware decoder unavailable (0x%08X); music_hw_decode is off",
                     unsigned(pf::mp3::lastError()));
        return;
    }
    NFSU_DEV_SET(s_check, NFSU_DEV_BOOL("music_hw_check", false));
    NFSU_DEV_SET(s_ahead, NFSU_DEV_BOOL("music_hw_ahead", true));
    s_work = new pf::Semaphore("music_work", 0);
    s_done = new pf::Semaphore("music_done", 0);
    pf::Thread* thread = new pf::Thread("music_decode", &worker, nullptr, WorkerPriority, 32 * 1024,
                                        pf::cores::mask(pf::cores::Helper));
    thread->start();
    NFSU_DEV_SET(s_dumpPath, NFSU_DEV_STRING("music_hw_check_dump", ""));
    s_decode = app.replaceMethod(DecodeGranule, &decodeHook);
    s_start = app.replaceMethod(StartStream, &startHook);
    s_destroy = app.replaceMethod(Destroy, &destroyHook);
    pf::logInfo("music: EALayer3 streams on the hardware MP3 decoder (music_hw_decode=on)%s",
                s_check ? ", compared with the game's decoder (music_hw_check)" : "");
}

}}
