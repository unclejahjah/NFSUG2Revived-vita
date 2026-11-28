// Shadow compare: runs the native body and the guest body on the same state, reports differences, keeps the guest result.

#include "shadow.h"

#ifdef NFSU_NATIVE_SHADOW

#include <platform/log.h>
#include <platform/switches.h>
#include <winapi/d3d9_native.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <string>
#include <cmath>
#include <cstdlib>

namespace ug2 { namespace native { namespace shadow {

namespace {

const int DetailLimit = 5;
const uint32_t ReportEvery = 500;
const int MaxStackArguments = 8;

struct Outcome
{
    x86::CPU cpu;
    std::vector<std::vector<uint8_t>> memory;
};

struct Stats
{
    std::atomic<uint32_t> calls{0};
    std::atomic<uint32_t> compared{0};
    std::atomic<uint32_t> mismatches{0};
    std::atomic<uint32_t> details{0};
    std::atomic<uint32_t> worst{0};
    std::atomic<uint32_t> worstMatmulPpm{0};    // matmul4 spans: worst error / allowed, in millionths
    std::atomic<bool> matmul{false};
    std::atomic<uint64_t> bridgeCalls{0};
};

thread_local bool t_active = false;
thread_local win32::d3d9::bridge::Capture t_capture;

uint32_t ordered(float value)
{
    uint32_t u;
    memcpy(&u, &value, 4);
    return (u & 0x80000000u) ? 0x80000000u - (u & 0x7fffffffu) : u + 0x80000000u;
}

// Distance in ulps; 0xffffffff when one side is NaN and the other not.
uint32_t ulps(float a, float b)
{
    if (std::isnan(a) || std::isnan(b))
        return std::isnan(a) && std::isnan(b) ? 0 : 0xffffffffu;
    const uint32_t x = ordered(a), y = ordered(b);
    return x > y ? x - y : y - x;
}

typedef std::array<float, 32> MatmulInput;     // the two input matrices of a matmul4 span

// matmul4: |native - guest| <= N * 2^-24 * S; equal when S is 0 or a value is not finite.
bool matmulElementOk(float native, float guest, double sum, uint32_t n, double& ratio, double& allowed)
{
    ratio = 0;
    allowed = 0;
    if (!std::isfinite(sum) || !std::isfinite(native) || !std::isfinite(guest) || sum == 0)
        return ulps(native, guest) == 0;
    allowed = double(n) * 5.9604644775390625e-8 * sum;
    ratio = std::fabs(double(native) - double(guest)) / allowed;
    return ratio <= 1;
}

float floatAt(const uint8_t* p)
{
    float f;
    memcpy(&f, p, 4);
    return f;
}

void captureMatmul(const Spans& spans, std::vector<MatmulInput>& out)
{
    out.assign(spans.size(), MatmulInput());
    for (size_t i = 0; i < spans.size(); ++i)
        if (spans[i].matmulN)
        {
            memcpy(out[i].data(), ::native::host(spans[i].matmulA), 64);
            memcpy(out[i].data() + 16, ::native::host(spans[i].matmulB), 64);
        }
}

void capture(const Spans& spans, std::vector<std::vector<uint8_t>>& out)
{
    out.resize(spans.size());
    for (size_t i = 0; i < spans.size(); ++i)
        out[i].assign(::native::host(spans[i].address), ::native::host(spans[i].address) + spans[i].size);
}

void restore(const Spans& spans, const std::vector<std::vector<uint8_t>>& in)
{
    for (size_t i = 0; i < spans.size(); ++i)
        memcpy(::native::host(spans[i].address), in[i].data(), spans[i].size);
}

struct Registers
{
    const char* name;
    x86::reg8 bit;
};

const Registers kRegisters[] = {
    {"eax", Eax}, {"ecx", Ecx}, {"edx", Edx}, {"ebx", Ebx},
    {"esp", Esp}, {"ebp", Ebp}, {"esi", Esi}, {"edi", Edi},
};

x86::reg32 registerValue(const x86::CPU& cpu, int index)
{
    const x86::reg32 values[8] = {cpu.eax, cpu.ecx, cpu.edx, cpu.ebx, cpu.esp, cpu.ebp, cpu.esi, cpu.edi};
    return values[index];
}

double topOfStack(const x86::CPU& cpu)
{
    return cpu.fpu.count > 0 ? double(cpu.fpu.regs[cpu.fpu.count & 7]) : 0.0;
}

void describeRegisters(char* text, size_t size, const x86::CPU& cpu)
{
    snprintf(text, size, "eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x x87 %u st0 %g",
             cpu.eax, cpu.ecx, cpu.edx, cpu.ebx, cpu.esp, cpu.ebp, cpu.esi, cpu.edi, unsigned(cpu.fpu.count),
             topOfStack(cpu));
}

// Differences between two outcomes; appends a description of each to text.
uint32_t differences(const Spec& spec, const Spans& spans, const std::vector<MatmulInput>& inputs, const Outcome& a,
                     const Outcome& b, uint32_t& worst, double& worstRatio, std::string& text, bool bridge)
{
    uint32_t count = 0;
    char line[160];
    for (int i = 0; i < 8; ++i)
    {
        if (!(spec.registers & kRegisters[i].bit))
            continue;
        if (registerValue(a.cpu, i) != registerValue(b.cpu, i))
        {
            snprintf(line, sizeof(line), "  %s native %08x guest %08x\n", kRegisters[i].name, registerValue(a.cpu, i),
                     registerValue(b.cpu, i));
            text += line;
            ++count;
        }
    }
    if (bridge)
    {
        namespace br = win32::d3d9::bridge;
        const br::Capture& c = t_capture;
        if (c.differs)
        {
            snprintf(line, sizeof(line), "  bridge call %u: guest %s, native %s, word %u guest %08x native %08x\n",
                     unsigned(c.differingCall), br::methodName(c.expected), br::methodName(c.got),
                     unsigned(c.differingWord), unsigned(c.expectedWord), unsigned(c.gotWord));
            text += line;
            ++count;
        }
        else if (c.replayed != c.calls.size())
        {
            snprintf(line, sizeof(line), "  bridge calls: guest %u native %u\n", unsigned(c.calls.size()),
                     unsigned(c.replayed));
            text += line;
            ++count;
        }
    }
    if (a.cpu.fpu.count != b.cpu.fpu.count)
    {
        snprintf(line, sizeof(line), "  x87 depth native %u guest %u\n", unsigned(a.cpu.fpu.count),
                 unsigned(b.cpu.fpu.count));
        text += line;
        ++count;
    }
    else if (spec.compareSt0 && a.cpu.fpu.count > 0)
    {
        const float x = float(topOfStack(a.cpu)), y = float(topOfStack(b.cpu));
        const uint32_t u = ulps(x, y);
        worst = std::max(worst, u);
        if (u > spec.maxUlps)
        {
            snprintf(line, sizeof(line), "  st0 native %.9g guest %.9g (%u ulps)\n", x, y, unsigned(u));
            text += line;
            ++count;
        }
    }
    for (size_t s = 0; s < spans.size(); ++s)
    {
        const uint8_t* x = a.memory[s].data();
        const uint8_t* y = b.memory[s].data();
        if (spans[s].matmulN)
        {
            const float* ma = inputs[s].data();
            const float* mb = ma + 16;
            for (uint32_t o = 0; o < 64; o += 4)
            {
                const int i = int(o / 16), j = int(o / 4 % 4);
                double sum = 0;
                for (int k = 0; k < 4; ++k)
                    sum += std::fabs(double(ma[4 * i + k]) * double(mb[4 * k + j]));
                const float n = floatAt(x + o), g = floatAt(y + o);
                double ratio, allowed;
                const bool ok = matmulElementOk(n, g, sum, spans[s].matmulN, ratio, allowed);
                if (allowed > 0)
                    worstRatio = std::max(worstRatio, ratio);
                if (ok)
                    continue;
                if (count < 24)
                {
                    snprintf(line, sizeof(line), "  span %u [%08x] element (%d,%d) native %.9g guest %.9g (S %.9g, allowed %.3g)\n",
                             unsigned(s), spans[s].address + o, i, j, n, g, sum, allowed);
                    text += line;
                }
                ++count;
            }
        }
        else if (spans[s].floats)
        {
            for (uint32_t o = 0; o + 4 <= spans[s].size; o += 4)
            {
                const uint32_t u = ulps(floatAt(x + o), floatAt(y + o));
                worst = std::max(worst, u);
                if (u > spec.maxUlps && count < 24)
                {
                    snprintf(line, sizeof(line), "  span %u [%08x] native %.9g guest %.9g (%u ulps)\n", unsigned(s),
                             spans[s].address + o, floatAt(x + o), floatAt(y + o), unsigned(u));
                    text += line;
                    ++count;
                }
                else if (u > spec.maxUlps)
                    ++count;
            }
        }
        else if (memcmp(x, y, spans[s].size) != 0)
        {
            uint32_t first = 0;
            while (x[first] == y[first])
                ++first;
            snprintf(line, sizeof(line), "  span %u [%08x+%u] first difference at +%u: native %02x guest %02x\n", unsigned(s),
                     spans[s].address, unsigned(spans[s].size), unsigned(first), x[first], y[first]);
            text += line;
            ++count;
        }
    }
    return count;
}

Stats& statsOf(const Spec& spec)
{
    static std::vector<std::pair<const Spec*, Stats*>> table;
    static std::atomic_flag lock = ATOMIC_FLAG_INIT;
    while (lock.test_and_set(std::memory_order_acquire)) {}
    Stats* found = nullptr;
    for (auto& entry : table)
        if (entry.first == &spec)
            found = entry.second;
    if (!found)
    {
        found = new Stats;
        table.emplace_back(&spec, found);
    }
    lock.clear(std::memory_order_release);
    return *found;
}

}

void compare(const Spec& spec, win32::WinApplication* app, x86::CPU& cpu, Body native, Body guest, Declare declare)
{
    if (t_active)
    {
        native(app, cpu);
        return;
    }
    static const uint32_t rate = pf::cachedStatic([] {
        return uint32_t(std::max(NFSU_DEV_INT("native_shadow_rate", 8), 1));
    });
    Stats& stats = statsOf(spec);
    if (stats.calls.fetch_add(1, std::memory_order_relaxed) % rate != 0)
    {
        native(app, cpu);
        return;
    }

    t_active = true;
    Spans spans;
    declare(cpu, spans);
    const x86::CPU entry = cpu;
    char inputs[256];
    describeRegisters(inputs, sizeof(inputs), entry);
    x86::reg32 arguments[MaxStackArguments];
    for (int i = 0; i < MaxStackArguments; ++i)
        arguments[i] = rc::ld32(entry.esp + 4 + 4 * i);
    std::vector<std::vector<uint8_t>> before;
    capture(spans, before);
    std::vector<MatmulInput> matmulInputs;
    captureMatmul(spans, matmulInputs);

    Outcome nativeResult, guestResult;
    namespace br = win32::d3d9::bridge;
    if (spec.bridge)
    {
        t_capture.clear();
        br::begin(t_capture, br::Capture::Record);
    }
    guest(app, cpu);
    if (spec.bridge)
        br::end();
    guestResult.cpu = cpu;
    capture(spans, guestResult.memory);
    if (cpu.terminate)
    {
        t_active = false;
        return;
    }

    cpu = entry;
    restore(spans, before);
    if (spec.bridge)
        br::begin(t_capture, br::Capture::Replay);
    native(app, cpu);
    if (spec.bridge)
        br::end();
    nativeResult.cpu = cpu;
    capture(spans, nativeResult.memory);

    cpu = guestResult.cpu;
    restore(spans, guestResult.memory);
    t_active = false;

    uint32_t worst = 0;
    double worstRatio = 0;
    std::string text;
    const uint32_t count = differences(spec, spans, matmulInputs, nativeResult, guestResult, worst, worstRatio, text, spec.bridge);
    uint32_t known = stats.worst.load(std::memory_order_relaxed);
    while (worst > known && !stats.worst.compare_exchange_weak(known, worst, std::memory_order_relaxed)) {}
    for (const Span& span : spans)
        if (span.matmulN)
        {
            stats.matmul.store(true, std::memory_order_relaxed);
            const uint32_t ppm = uint32_t(std::min(worstRatio, 4000.0) * 1e6);
            uint32_t seen = stats.worstMatmulPpm.load(std::memory_order_relaxed);
            while (ppm > seen && !stats.worstMatmulPpm.compare_exchange_weak(seen, ppm, std::memory_order_relaxed)) {}
        }
    if (spec.bridge)
        stats.bridgeCalls.fetch_add(t_capture.calls.size(), std::memory_order_relaxed);
    const uint32_t compared = stats.compared.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count)
    {
        stats.mismatches.fetch_add(1, std::memory_order_relaxed);
        if (stats.details.fetch_add(1, std::memory_order_relaxed) < DetailLimit)
        {
            char out[256];
            describeRegisters(out, sizeof(out), nativeResult.cpu);
            char guestOut[256];
            describeRegisters(guestOut, sizeof(guestOut), guestResult.cpu);
            pf::logInfo("shadow %s: mismatch (%u differences) at call %u\n  in:  %s\n  args: %08x %08x %08x %08x %08x %08x "
                        "%08x %08x\n  native out: %s\n  guest out:  %s\n%s",
                        spec.name, unsigned(count), unsigned(stats.calls.load()), inputs, arguments[0], arguments[1],
                        arguments[2], arguments[3], arguments[4], arguments[5], arguments[6], arguments[7], out, guestOut,
                        text.c_str());
        }
    }
    if (compared % ReportEvery == 0)
    {
        pf::logInfo("shadow %s: %u compared, %u mismatches, worst %u ulps, %llu bridge calls", spec.name,
                    unsigned(compared), unsigned(stats.mismatches.load()), unsigned(stats.worst.load()),
                    (unsigned long long)stats.bridgeCalls.load());
        if (stats.matmul.load(std::memory_order_relaxed))
            pf::logInfo("shadow %s: worst matmul4 error %.4f of allowed", spec.name,
                        double(stats.worstMatmulPpm.load()) / 1e6);
    }
}

}}}

#endif
