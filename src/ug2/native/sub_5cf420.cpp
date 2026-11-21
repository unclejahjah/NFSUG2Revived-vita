// Native sub_5cf420: draw-queue flush over the 16 x 23 list heads. Guest frame kept, registers live in the CPU; children run through entry points, CommitChanges and EndPass through the bridge.

#include <native/common.h>
#include <native/flush_context.h>
#include <native/unload_cancel.h>
#include <platform/switches.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <algorithm>
#include <atomic>
#include <offload.h>
#include <winapi/d3d9_probe.h>
#include <scenery.h>
#include <winapi/d3d9_native.h>

#include <initializer_list>
#include <vector>

#if defined(__ARM_NEON) && !defined(NFSU_FPU_DOUBLE)
#include <arm_neon.h>
#endif

namespace ug2
{

void sub_5cf420_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5bcc30(win32::WinApplication* app, x86::CPU& cpu);
void sub_5bdde0(win32::WinApplication* app, x86::CPU& cpu);
void sub_48b1e0(win32::WinApplication* app, x86::CPU& cpu);
void sub_5bdaa0(win32::WinApplication* app, x86::CPU& cpu);
void sub_5bd140(win32::WinApplication* app, x86::CPU& cpu);
void sub_5bd4a0(win32::WinApplication* app, x86::CPU& cpu);
void sub_5bdcd0(win32::WinApplication* app, x86::CPU& cpu);
void sub_60fe00(win32::WinApplication* app, x86::CPU& cpu);
void sub_4901d0(win32::WinApplication* app, x86::CPU& cpu);
void sub_60fde0(win32::WinApplication* app, x86::CPU& cpu);
void sub_43cdf0(win32::WinApplication* app, x86::CPU& cpu);
void sub_43f920(win32::WinApplication* app, x86::CPU& cpu);
void sub_5cea20(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_5bc6f0(win32::WinApplication* app, x86::CPU& cpu);
void sub_5bc7b0(win32::WinApplication* app, x86::CPU& cpu);
void sub_5bc890(win32::WinApplication* app, x86::CPU& cpu);
void sub_5bcad0(win32::WinApplication* app, x86::CPU& cpu);
void sub_5bcb10(win32::WinApplication* app, x86::CPU& cpu);
bool textureRetryWanted(x86::reg32 tex);
void textureRetry(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 tex);

namespace
{

namespace br = win32::d3d9::bridge;
using x86::reg32;
using x86::sreg32;
typedef void (*Child)(win32::WinApplication*, x86::CPU&);

const reg32 CurrentGroup = 0x870910;
const reg32 DevicePointer = 0x870974;
const reg32 CurrentView = 0x870848;
const reg32 LastStream = 0x87092c;
const reg32 LastIndices = 0x870930;
const reg32 EntryPool = 0x87091c;
const reg32 BucketPool = 0x870918;
const reg32 StripPool = 0x870954;
const reg32 Ring = 0x8a07a8;

// The guest's callee-saved registers, kept in the CPU, and its frame base (esp after the prologue).
struct State
{
    reg32& ebx;
    reg32& ebp;
    reg32& esi;
    reg32& edi;
    reg32 frame;
};

// flush_direct=off: CommitChanges and EndPass through the vtable.
bool s_direct = true;

template< reg32 A > inline void increment(const x86::CPU& cpu) { flush::st32<A>(cpu, flush::ld32<A>(cpu) + 1); }

inline reg32 third(reg32 value) { return reg32((uint64_t(0xaaaaaaabu) * value) >> 32) >> 1; }

inline void toCpu(x86::CPU& cpu, const State&, reg32 esp)
{
    cpu.esp = esp;
}

inline void fromCpu(const x86::CPU&, State&) {}

// Leaf child: the guest assumes its esp.
inline bool leaf(win32::WinApplication* app, x86::CPU& cpu, const State& s, Child child, reg32 esp)
{
    toCpu(cpu, s, esp);
    child(app, cpu);
    return !cpu.terminate;
}

// Child after which the guest reloads esp.
inline bool call(win32::WinApplication* app, x86::CPU& cpu, State& s, Child child, reg32 esp, reg32 cleanup)
{
    toCpu(cpu, s, esp);
    child(app, cpu);
    if (cpu.terminate)
        return false;
    fromCpu(cpu, s);
    s.frame = cpu.esp + cleanup;
    return true;
}

// stdcall COM method through the guest vtable.
bool com(win32::WinApplication* app, x86::CPU& cpu, State& s, reg32 object, reg32 slot, std::initializer_list<reg32> args)
{
    reg32 at = s.frame;
    for (const reg32* a = args.end(); a != args.begin();)
        rc::st32(at -= 4, *--a);
    rc::st32(at -= 4, object);
    toCpu(cpu, s, at - 4);
    br::callGuest(app, cpu, rc::ld32(rc::ld32(object) + 4 * slot));
    if (cpu.terminate)
        return false;
    fromCpu(cpu, s);
    s.frame = cpu.esp;
    return true;
}

// Effect CommitChanges (vtable slot 67).
inline bool commitChanges(win32::WinApplication* app, x86::CPU& cpu, State& s, reg32 effect)
{
    if (__builtin_expect(s_direct, 1))
    {
        if (!cpu.og)
            app->allowPreemption(cpu);
        if (__builtin_expect(br::effectCommitChanges(app, cpu, effect), 1))
            return true;
    }
    return com(app, cpu, s, effect, 67, {});
}

// Effect EndPass (vtable slot 68).
inline bool endPass(win32::WinApplication* app, x86::CPU& cpu, State& s, reg32 effect)
{
    if (__builtin_expect(s_direct, 1))
    {
        if (!cpu.og)
            app->allowPreemption(cpu);
        if (__builtin_expect(br::effectEndPass(app, cpu, effect), 1))
            return true;
    }
    return com(app, cpu, s, effect, 68, {});
}

const reg32 EmptyGroupFunction = 0x4022c0;

// Begin functions a context runs.
bool knownBegin(reg32 address)
{
    return address == 0x5bc6f0 || address == 0x5bc7b0 || address == 0x5bc890 || address == 0x5bcad0 ||
           address == 0x5bcb10 || address == EmptyGroupFunction;
}

// Runs a begin function by address.
void beginByAddress(win32::WinApplication* app, x86::CPU& cpu, reg32 address)
{
    switch (address)
    {
    case 0x5bc6f0: sub_5bc6f0(app, cpu); break;
    case 0x5bc7b0: sub_5bc7b0(app, cpu); break;
    case 0x5bc890: sub_5bc890(app, cpu); break;
    case 0x5bcad0: sub_5bcad0(app, cpu); break;
    case 0x5bcb10: sub_5bcb10(app, cpu); break;
    default: cpu.esp += 4; break;
    }
}

// thiscall group function (vtable +4 begin, +8 end).
bool group(win32::WinApplication* app, x86::CPU& cpu, State& s, reg32 g, reg32 offset)
{
    toCpu(cpu, s, s.frame - 4);
    cpu.ecx = g;
    if (cpu.og)
    {
        if (offset == 4)
            beginByAddress(app, cpu, rc::ld32(rc::ld32(g) + 4));
        return !cpu.terminate;
    }
    br::callGuest(app, cpu, rc::ld32(rc::ld32(g) + offset));
    if (cpu.terminate)
        return false;
    fromCpu(cpu, s);
    s.frame = cpu.esp;
    return true;
}

// rep movs as the translated code runs it.
void repMove(x86::CPU& cpu, reg32& si, reg32& di, reg32 count, reg32 size)
{
    if (rc::repMove(cpu.flags.df, cpu.ees + di, si, count, size))
    {
        si += count * size;
        di += count * size;
        return;
    }
    const reg32 step = cpu.flags.df ? 0u - size : size;
    for (reg32 n = count; n != 0; --n, si += step, di += step)
        if (size == 4)
            rc::st32(cpu.ees + di, rc::ld32(si));
        else
            rc::st8(cpu.ees + di, rc::ld8(si));
}

// Lazy vertex buffer: create, lock, copy the vertices, unlock.
bool createVertexBuffer(win32::WinApplication* app, x86::CPU& cpu, State& s)
{
    const reg32 mesh = s.ebx;
    const reg32 size = rc::ld32(mesh + 0x30);
    rc::st32(mesh + 0x40, 1);
    if (!com(app, cpu, s, rc::ld32(DevicePointer), 26, {size, 8, 0, 1, s.esi, 0}))
        return false;
    s.esi = rc::ld32(s.esi);
    if (!com(app, cpu, s, s.esi, 11, {0, 0, s.frame + 0x28, 0}))
        return false;
    const reg32 bytes = rc::ld32(s.ebx + 0x30);
    s.esi = rc::ld32(s.ebx + 0x28);
    s.edi = rc::ld32(s.frame + 0x28);
    repMove(cpu, s.esi, s.edi, bytes >> 2, 4);
    repMove(cpu, s.esi, s.edi, bytes & 3, 1);
    if (!com(app, cpu, s, rc::ld32(s.ebx + 0x2c), 12, {}))
        return false;
    s.edi = rc::ld32(s.frame + 0x14);
    return true;
}

// Type 0x14 group: ring texture, fade float and its alpha byte.
bool fadeParameters(win32::WinApplication* app, x86::CPU& cpu, State& s)
{
    const reg32 E = s.frame;
    reg32 i;
    if (cpu.og)
        i = flush::ld32<0x8a1c94>(cpu);
    else
    {
        if (!leaf(app, cpu, s, sub_60fe00, E - 4))
            return false;
        i = cpu.eax;
    }
    i = flush::ld32(cpu, Ring + 4 * i) == rc::ld32(rc::ld32(E + 0x1c) + 0x24) ? i - 2 : i - 1;
    i &= 7;
    rc::st32(E - 4, 0);
    rc::st32(E - 8, 0);
    rc::st32(E - 0xc, flush::ld32(cpu, Ring + 4 * i));
    if (!leaf(app, cpu, s, sub_4901d0, E - 0x10))
        return false;
    const reg32 info = cpu.eax;
    reg32 current = flush::ld32<CurrentGroup>(cpu);
    const reg32 effect = rc::ld32(current + 0x20);
    s.esi = rc::ld32(effect);
    br::effectSetTexture(app, cpu, effect, rc::ld32(rc::ld32(current + 0x1c) + 0x894), rc::ld32(rc::ld32(info) + 0x18));

    const reg32 depth = cpu.fpu.count;
    if (cpu.og)
        rc::stf32(E + 0x1c, float(Real(flush::ldf32<0x8a1c9c>(cpu)) / Real(rc::ldf32(0x802954))));
    else
    {
        if (!leaf(app, cpu, s, sub_60fde0, E - 4))
            return false;
        rc::stf32(E + 0x1c, float(cpu.fpu.regs[(depth + 1) & 7]));
        cpu.fpu.count = depth;
    }
    current = flush::ld32<CurrentGroup>(cpu);
    s.esi = rc::ld32(E + 0x1c);
    br::effectSetFloat(app, cpu, rc::ld32(current + 0x20), rc::ld32(rc::ld32(current + 0x1c) + 0x844), rc::ldf32(E + 0x1c));

    const Real product = Real(rc::ldf32(E + 0x1c)) * Real(rc::ldf32(0x784544));
    s.esi = rc::ld32(flush::ld32<CurrentGroup>(cpu) + 0x20);
    s.edi = rc::ld32(s.esi);
    cpu.fpu.regs[(depth + 1) & 7] = product;
    const reg32 alpha = reg32(x86::truncInteger64(product)) << 24;
    br::effectSetInt(app, cpu, s.esi, rc::ld32(rc::ld32(flush::ld32<CurrentGroup>(cpu) + 0x1c) + 0x86c), sreg32(alpha));
    s.edi = rc::ld32(E + 0x14);
    return true;
}

enum UnloadMode : uint8_t { UnloadCancel, UnloadFence, UnloadOff };

// offload_unload_mode; release builds cancel.
UnloadMode unloadMode()
{
#ifdef DEV_SETTINGS
    static const char* const names[] = {"cancel", "fence", "off"};
    static const UnloadMode mode = UnloadMode(pf::Config::getChoice("offload_unload_mode", names, 3, 0));
    return mode;
#else
    return UnloadCancel;
#endif
}

std::atomic<uint32_t> s_stale{0};

// A step found unloaded pointers; true: draw anyway (mode off).
bool stepDead(const char* what, const reg32* pointers, const unload::Hit& hit)
{
#ifdef DEV_TOOLS
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1) < 48)
        pf::logError("offload: a queued %s points into unloaded memory: pointer %u 0x%08x (first 0x%08x) in 0x%08x+0x%x, unload kind %u",
                     what, unsigned(hit.index), unsigned(pointers[hit.index]), unsigned(pointers[0]), unsigned(hit.base),
                     unsigned(hit.size), unsigned(hit.tag));
#else
    (void)what;
    (void)pointers;
    (void)hit;
#endif
    if (unloadMode() == UnloadOff)
    {
        ++s_stale;
        return true;
    }
    unload::end(true);
    return false;
}

// Worker draw step over these pointers; false: cancelled.
inline bool stepBegin(const x86::CPU& cpu, const char* what, const reg32* pointers, reg32 n)
{
    unload::Hit hit;
    if (__builtin_expect(!unload::begin(flush::context(cpu)->serial, pointers, n, &hit), 1))
        return true;
    return stepDead(what, pointers, hit);
}

// Pointers read inside a step; false: cancelled.
inline bool stepMore(const x86::CPU& cpu, const char* what, const reg32* pointers, reg32 n)
{
    unload::Hit hit;
    if (__builtin_expect(!unload::dead(flush::context(cpu)->serial, pointers, n, &hit), 1))
        return true;
    return stepDead(what, pointers, hit);
}

inline void stepEnd(const x86::CPU& cpu)
{
    if (cpu.og)
        unload::end();
}

// Worker: whether a queued entry's data is still loaded; opens its step.
bool entryLive(const x86::CPU& cpu, reg32 entry)
{
    const reg32 p[6] = {rc::ld32(entry + 0x10), rc::ld32(entry + 8), rc::ld32(entry + 0x1c), rc::ld32(entry + 0x20),
                        rc::ld32(entry + 0x24), rc::ld32(entry + 0x28)};
    if (!stepBegin(cpu, "entry (solid, strip, light, material, matrix, skin)", p, 6))
        return false;
    if (__builtin_expect(unload::count() == 0, 1) || rc::ld32(entry + 0xc) != 0)
        return true;
    const reg32 mesh = rc::ld32(p[0]);
    if (mesh < 0x10000)
        return true;
    const reg32 q[3] = {mesh, rc::ld32(mesh + 0x28), rc::ld32(mesh + 0x18)};
    return stepMore(cpu, "entry's mesh (mesh, vertices, indices)", q, 3);
}

// Worker: whether a bucket's texture is still loaded; opens its step.
bool bucketLive(const x86::CPU& cpu, reg32 tex)
{
    if (!stepBegin(cpu, "bucket (texture)", &tex, 1))
        return false;
    if (__builtin_expect(unload::count() == 0, 1))
        return true;
    const reg32 platform = rc::ld32(tex);
    return stepMore(cpu, "bucket's texture platform record", &platform, 1);
}

// Indexed entry (edi strip, ebp entry): skinning, buffers, a DrawIndexedPrimitive per pass.
bool drawIndexed(win32::WinApplication* app, x86::CPU& cpu, State& s)
{
    rc::st32(s.frame + 0x14, s.edi);
    const reg32 skin = rc::ld32(s.ebp + 0x28);
    if (skin != 0 && (rc::ld8(s.ebp + 0x14) & 1))
    {
        rc::st32(s.frame - 4, s.edi);
        rc::st32(s.frame - 8, skin);
        if (!call(app, cpu, s, sub_5bdcd0, s.frame - 0xc, 8))
            return false;
    }
    s.ebx = rc::ld32(rc::ld32(s.ebp + 0x10));
    if (__builtin_expect(s.ebx == 0 && cpu.og, 0))
    {
        // An unload overtook this flush: the guest would create a buffer at address 0.
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            pf::logError("offload: a queued draw's model was unloaded (entry 0x%08x); draw skipped", unsigned(s.ebp));
        return true;
    }
    s.esi = s.ebx + 0x2c;
    if (rc::ld32(s.ebx + 0x2c) == 0)
    {
        offload::created();
        if (!createVertexBuffer(app, cpu, s))
            return false;
    }
    const reg32 mesh = s.ebx;
    if (offload::counting())
        offload::streamStep(rc::ld32(mesh + 0x2c) == flush::ld32<LastStream>(cpu), rc::ld32(mesh + 0x20) == flush::ld32<LastIndices>(cpu),
                            rc::ld32(mesh + 0x2c), rc::ld32(mesh + 0x20));
    if (rc::ld32(mesh + 0x2c) != flush::ld32<LastStream>(cpu))
    {
        const reg32 buffer = rc::ld32(mesh + 0x2c);
        s.esi = rc::ld32(flush::ld32<CurrentGroup>(cpu) + 8);
        br::setStreamSource(app, cpu, rc::ld32(DevicePointer), 0, buffer, 0, s.esi);
        flush::st32<LastStream>(cpu, rc::ld32(mesh + 0x2c));
        increment<0x872ee8>(cpu);
    }
    if (rc::ld32(mesh + 0x20) != flush::ld32<LastIndices>(cpu))
    {
        br::setIndices(app, cpu, rc::ld32(DevicePointer), rc::ld32(mesh + 0x20));
        flush::st32<LastIndices>(cpu, rc::ld32(mesh + 0x20));
        increment<0x872eec>(cpu);
    }
    if (rc::ld32(s.edi + 0xc) == 0 || rc::ld32(mesh + 0x34) >= 0xffffu)
        return true;
    rc::st32(s.frame + 0x18, rc::ld32(rc::ld32(s.ebp + 0x18) + 0x14));
    const reg32 current = flush::ld32<CurrentGroup>(cpu);
    const reg32 type = rc::ld32(current + 4);
    if (type == 0x14)
    {
        if (!fadeParameters(app, cpu, s))
            return false;
    }
    else if ((flush::ld8<0x8707cd>(cpu) == 0 || flush::viewKind(cpu) != 0) && rc::ld8(current + 0x18) != 0 &&
             (type == 0 || type == 1 || type == 5 || type == 4))
        rc::st32(s.frame + 0x18, 1);
    if (!commitChanges(app, cpu, s, rc::ld32(rc::ld32(s.ebp + 0x18) + 0x20)))
        return false;
    s.esi = 0;
    if (sreg32(rc::ld32(s.frame + 0x18)) > 0)
    {
        offload::issued(rc::ld32(s.frame + 0x18));
        do
        {
            br::effectPass(app, cpu, rc::ld32(rc::ld32(s.ebp + 0x18) + 0x20), s.esi);
            const reg32 device = rc::ld32(DevicePointer);
            rc::st32(s.frame + 0x1c, rc::ld32(device));
            const reg32 count = third(rc::ld32(s.edi + 0xc));
            s.edi = rc::ld32(s.frame + 0x14);
            const reg32 start = reg32(sreg32(rc::ld32(s.edi + 0x34) - rc::ld32(s.ebx + 0x18)) >> 1);
            br::drawIndexedPrimitive(app, cpu, device, 4, 0, 0, rc::ld32(s.ebx + 0x34), start, count);
            if (!endPass(app, cpu, s, rc::ld32(rc::ld32(s.ebp + 0x18) + 0x20)))
                return false;
            ++s.esi;
        } while (sreg32(s.esi) < sreg32(rc::ld32(s.frame + 0x18)));
    }
    rc::st32(s.ebx + 0x40, 1);
    increment<0x872ee0>(cpu);
    flush::st32<0x872ef0>(cpu, flush::ld32<0x872ef0>(cpu) + third(rc::ld32(s.ebx + 0x34)));
    return true;
}

// Immediate entry (edi strip): a DrawPrimitiveUP strip per pass, then the strip back to its pool.
bool drawImmediate(win32::WinApplication* app, x86::CPU& cpu, State& s)
{
    br::setRenderState(app, cpu, rc::ld32(DevicePointer), 14, 0);
    flush::st32<LastStream>(cpu, 0);
    const reg32 entryGroup = rc::ld32(s.ebp + 0x18);
    s.ebx = rc::ld32(entryGroup + 0x14);
    if (!commitChanges(app, cpu, s, rc::ld32(entryGroup + 0x20)))
        return false;
    s.esi = 0;
    if (sreg32(s.ebx) > 0)
    {
        offload::issued(s.ebx);
        do
        {
            br::effectPass(app, cpu, rc::ld32(rc::ld32(s.ebp + 0x18) + 0x20), s.esi);
            const reg32 vertices = s.edi + 4;
            br::drawPrimitiveUP(app, cpu, rc::ld32(DevicePointer), 5, rc::ld32(s.edi) - 2,
                                vertices ? rc::host(vertices) : nullptr, 0x18);
            if (!endPass(app, cpu, s, rc::ld32(rc::ld32(s.ebp + 0x18) + 0x20)))
                return false;
            ++s.esi;
        } while (sreg32(s.esi) < sreg32(s.ebx));
    }
    br::setRenderState(app, cpu, rc::ld32(DevicePointer), 14, 1);
    increment<0x872ee4>(cpu);
    if (cpu.og)
    {
        static_cast<flush::Context*>(cpu.og)->strips.push(s.edi);
        return true;
    }
    rc::st32(s.frame - 4, s.edi);
    cpu.ecx = rc::ld32(StripPool);
    return leaf(app, cpu, s, sub_43cdf0, s.frame - 8);
}

// One entry: unlinked, its setup children, its draw, back to the entry pool.
bool drawEntryBody(win32::WinApplication* app, x86::CPU& cpu, State& s, reg32 list)
{
    s.ebp = rc::ld32(list);
    const reg32 next = rc::ld32(s.ebp), previous = rc::ld32(s.ebp + 4);
    rc::st32(previous, next);
    rc::st32(next + 4, previous);
    rc::st32(s.frame + 0x1c, rc::ld32(s.esi + 0x10));
    rc::st32(s.frame - 4, flush::viewIndex(cpu));
    rc::st32(s.frame - 8, rc::ld32(s.ebp + 0x24));
    if (!call(app, cpu, s, sub_5bdde0, s.frame - 0xc, 8))
        return false;
    if (rc::ld32(rc::ld32(rc::ld32(s.ebp + 0x18) + 0x1c) + 0x59c) != 0)
    {
        if (cpu.og)
            rc::st32(s.frame - 8, 0x832de0 + 9 * 0x70);
        else
        {
            rc::st32(s.frame - 4, 9);
            if (!leaf(app, cpu, s, sub_48b1e0, s.frame - 8))
                return false;
            rc::st32(s.frame - 8, cpu.eax);
        }
        rc::st32(s.frame - 0xc, rc::ld32(s.ebp + 0x24));
        if (!call(app, cpu, s, sub_5bdaa0, s.frame - 0x10, 0xc))
            return false;
    }
    if (const reg32 light = rc::ld32(s.ebp + 0x1c))
    {
        rc::st32(s.frame - 4, rc::ld32(s.ebp + 0x24));
        rc::st32(s.frame - 8, light);
        if (!call(app, cpu, s, sub_5bd140, s.frame - 0xc, 8))
            return false;
    }
    if (const reg32 material = rc::ld32(s.ebp + 0x20))
    {
        rc::st32(s.frame - 4, rc::ld32(s.ebp + 0x14));
        rc::st32(s.frame - 8, material);
        if (!call(app, cpu, s, sub_5bd4a0, s.frame - 0xc, 8))
            return false;
    }
    const reg32 immediate = rc::ld32(s.ebp + 0xc);
    s.edi = rc::ld32(s.ebp + 8);
    if (!(immediate != 0 ? drawImmediate(app, cpu, s) : drawIndexed(app, cpu, s)))
        return false;
    if (cpu.og)
    {
        static_cast<flush::Context*>(cpu.og)->entries.push(s.ebp);
        return true;
    }
    rc::st32(s.frame - 4, s.ebp);
    rc::st32(s.frame - 8, rc::ld32(EntryPool));
    return leaf(app, cpu, s, sub_43f920, s.frame - 0xc);
}

bool drawEntry(win32::WinApplication* app, x86::CPU& cpu, State& s, reg32 list)
{
    const reg32 entry = rc::ld32(list);
    if (cpu.og && !entryLive(cpu, entry))
    {
        flush::Context& context = *static_cast<flush::Context*>(cpu.og);
        const reg32 next = rc::ld32(entry), previous = rc::ld32(entry + 4);
        rc::st32(previous, next);
        rc::st32(next + 4, previous);
        if (rc::ld32(entry + 0xc) != 0)
            context.strips.push(rc::ld32(entry + 8));
        context.entries.push(entry);
        return true;
    }
    offload::entryBegin(entry);
    const bool ok = drawEntryBody(app, cpu, s, list);
    offload::entryEnd();
    stepEnd(cpu);
    return ok;
}

// A non-empty list (ebx head, ebp group slot): group begin, buckets and entries, group end.
bool drawList(win32::WinApplication* app, x86::CPU& cpu, State& s)
{
    s.edi = rc::ld32(s.ebp);
    flush::st32<CurrentGroup>(cpu, s.edi);
    rc::st32(s.frame + 0x2c, s.edi);
    if (!group(app, cpu, s, s.edi, 4))
        return false;
    br::setVertexDeclaration(app, cpu, rc::ld32(DevicePointer), rc::ld32(s.edi + 0x24));
    br::effectBegin(app, cpu, rc::ld32(s.edi + 0x20), reinterpret_cast<win32::UINT*>(rc::host(s.frame + 0x30)), 0);
    offload::listBegin();
    s.esi = rc::ld32(s.ebx);
    increment<0x872f04>(cpu);
    rc::st32(s.frame + 0x10, s.esi);
    if (s.esi != s.ebx)
    {
        for (;;)
        {
            const reg32 bucket = s.esi;
            const reg32 next = rc::ld32(bucket), previous = rc::ld32(bucket + 4);
            rc::st32(previous, next);
            rc::st32(next + 4, previous);
            increment<0x872ed8>(cpu);
            offload::bucketBegin(bucket);
            const reg32 tex = rc::ld32(bucket + 0x10);
            const bool live = !cpu.og || bucketLive(cpu, tex);
            if (live)
            {
                rc::st32(s.frame - 4, tex);
                const bool ok = call(app, cpu, s, sub_5bcc30, s.frame - 8, 4);
                stepEnd(cpu);
                if (!ok)
                    return false;
            }
            reg32 list = s.esi + 0x18;
            if (live && rc::ld32(list) != list)
            {
                do
                {
                    if (!drawEntry(app, cpu, s, list))
                        return false;
                    s.esi = rc::ld32(s.frame + 0x10);
                    list = s.esi + 0x18;
                } while (rc::ld32(list) != list);
                s.edi = rc::ld32(s.frame + 0x2c);
                s.ebx = rc::ld32(s.frame + 0x20);
            }
            offload::bucketEnd();
            if (cpu.og)
            {
                flush::Context& context = *static_cast<flush::Context*>(cpu.og);
                const reg32 entries = s.esi + 0x18;
                for (reg32 n = rc::ld32(entries); n != entries; n = rc::ld32(entries))
                {
                    const reg32 next = rc::ld32(n), previous = rc::ld32(n + 4);
                    rc::st32(previous, next);
                    rc::st32(next + 4, previous);
                    context.entries.push(n);
                }
                context.buckets.push(s.esi);
            }
            else
            {
                rc::st32(rc::ld32(rc::ld32(s.esi + 0x10)) + 0x1c, 0);
                cpu.ecx = s.esi;
                if (!leaf(app, cpu, s, sub_5cea20, s.frame - 4))
                    return false;
                rc::st32(s.frame - 4, s.esi);
                rc::st32(s.frame - 8, rc::ld32(BucketPool));
                if (!leaf(app, cpu, s, sub_43f920, s.frame - 0xc))
                    return false;
            }
            const reg32 first = rc::ld32(s.ebx);
            rc::st32(s.frame + 0x10, first);
            if (first == s.ebx)
                break;
            s.esi = rc::ld32(s.frame + 0x10);
        }
        s.ebp = rc::ld32(s.frame + 0x24);
    }
    if (!group(app, cpu, s, s.edi, 8))
        return false;
    s.edi = rc::ld32(s.edi + 0x20);
    br::effectEnd(app, cpu, s.edi);
    flush::st32<CurrentGroup>(cpu, 0);
    offload::listEnd();
    return true;
}

const reg32 Heads = 0x870d30;
const reg32 Slots = 0x86eb28;
const reg32 Row = 23;
const reg32 HeadCount = 16 * Row;

inline bool listEmpty(reg32 k) { return rc::ld32(Heads + 8 * k) == Heads + 8 * k; }

// First non-empty list head at or after k, HeadCount when none.
inline reg32 nextList(reg32 k)
{
#if defined(__ARM_NEON) && !defined(NFSU_FPU_DOUBLE)
    while (k < HeadCount)
    {
        const reg32 start = k < HeadCount - 16 ? k : HeadCount - 16;
        const reg32 base = Heads + 8 * start;
        const uint32_t* p = reinterpret_cast<const uint32_t*>(rc::host(base));
        const uint32x4_t lanes = {0, 8, 16, 24};
        const uint32x4_t a = vaddq_u32(vdupq_n_u32(base), lanes);
        const uint32x4_t d0 = veorq_u32(vld2q_u32(p).val[0], a);
        const uint32x4_t d1 = veorq_u32(vld2q_u32(p + 8).val[0], vaddq_u32(a, vdupq_n_u32(32)));
        const uint32x4_t d2 = veorq_u32(vld2q_u32(p + 16).val[0], vaddq_u32(a, vdupq_n_u32(64)));
        const uint32x4_t d3 = veorq_u32(vld2q_u32(p + 24).val[0], vaddq_u32(a, vdupq_n_u32(96)));
        const uint32x4_t d = vorrq_u32(vorrq_u32(d0, d1), vorrq_u32(d2, d3));
        if (vget_lane_u64(vreinterpret_u64_u32(vorr_u32(vget_low_u32(d), vget_high_u32(d))), 0) != 0)
            for (; k < start + 16; ++k)
                if (!listEmpty(k))
                    return k;
        k = start + 16;
    }
    return HeadCount;
#else
    for (; k < HeadCount; ++k)
        if (!listEmpty(k))
            return k;
    return HeadCount;
#endif
}

// The guest's 16 x 23 loop step after s.ebx, s.ebp; false at its end.
inline bool guestStep(State& s)
{
    s.ebp += 4;
    s.ebx += 8;
    rc::st32(s.frame + 0x20, s.ebx);
    rc::st32(s.frame + 0x24, s.ebp);
    if (sreg32(s.ebp) < sreg32(0x86eb84))
        return true;
    if (sreg32(s.ebx) >= sreg32(0x8718b0))
        return false;
    s.ebp = Slots;
    rc::st32(s.frame + 0x24, s.ebp);
    rc::st32(s.frame + 0x20, s.ebx);
    return true;
}

#ifdef NFSU_OFFLOAD_CORE

// Keys no call asks for: the texture, matrix, stream and index caches miss next.
void resetCaches(bool begin)
{
    offload::boundary(begin);
    rc::st32(0x870940, 0);
    rc::st32(0x870944, 0);
    rc::st32(0x876850, 0);
    rc::st32(0x87684c, 0);
    rc::st32(0x800534, 0xffffffffu);
    rc::st32(LastStream, 0);
    rc::st32(LastIndices, 0);
}

// The flush records into its own segment (offload_flush), the guest caches reset on both sides.
struct Segment
{
    Segment()
    {
        const uint64_t start = offload::instructions();
        m_open = NFSU_SWITCH_CACHED("offload_flush", 1) && br::segmentBegin();
        if (m_open)
            resetCaches(true);
        m_begin = offload::instructions() - start;
    }
    ~Segment()
    {
        if (m_open)
        {
            const uint64_t start = offload::instructions();
            br::segmentEnd();
            resetCaches(false);
            offload::logParts(m_begin, offload::instructions() - start);
        }
    }

private:
    bool m_open;
    uint64_t m_begin = 0;
};

// Natives on, no shadow compare.
bool contextAllowed()
{
    static const bool allowed = NFSU_SWITCH_CACHED("offload_context", 1) && NFSU_SWITCH_CACHED("offload_flush", 1) &&
        NFSU_NATIVE("flush_native") && NFSU_NATIVE("texture_step_native") && NFSU_NATIVE("world_upload_native") &&
        NFSU_NATIVE("material_native") && NFSU_NATIVE("light_upload_native") && NFSU_NATIVE("headlight_pass_native") &&
        NFSU_NATIVE("sub_5bdcd0_native") && NFSU_NATIVE("sub_4901d0_native") && NFSU_NATIVE("sub_5bc6f0_native") &&
        NFSU_NATIVE("sub_5bc7b0_native") && NFSU_NATIVE("sub_5bc890_native") && NFSU_NATIVE("sub_5bcad0_native") &&
        NFSU_NATIVE("sub_5bcb10_native") && NFSU_RENDER_FAST_CACHED("flush_direct")
#ifdef NFSU_NATIVE_SHADOW
        && !shadow::enabled()
#endif
        ;
    return allowed;
}

// Marker work; false: run the flush directly.
bool prepare(win32::WinApplication* app, const x86::CPU& cpu, flush::Context& context, reg32& arena, reg32& draws)
{
    arena = 0;
    draws = 0;
    context.listCount = 0;
    context.beginSnapshot();
#ifdef DEV_TOOLS
    context.solids.clear();
#endif
    if (rc::ld32(rc::ld32(CurrentView) + 4) >= flush::ViewCount)
        return false;
    for (reg32 k = nextList(0); k < HeadCount; k = nextList(k + 1))
    {
        const reg32 group = rc::ld32(Slots + 4 * (k % Row));
        if (group == 0 || rc::ld8(group + 0x18) == 0 || !knownBegin(rc::ld32(rc::ld32(group) + 4)) ||
            rc::ld32(rc::ld32(group) + 8) != EmptyGroupFunction)
            return false;
        context.lists[context.listCount++] = uint16_t(k);
    }
    reg32 lastLight = 0, stale[8], lastGroup = 0, stripDraws = 0, meshDraws = 0;
    const reg32 staleCount = br::segmentStaleBuffers(stale, 8);
    x86::CPU scratch = cpu;
    State s = {scratch.ebx, scratch.ebp, scratch.esi, scratch.edi, scratch.esp - 0x34};
    for (reg32 i = 0; i < context.listCount; ++i)
    {
        const reg32 head = Heads + 8 * context.lists[i];
        const reg32 listGroup = rc::ld32(Slots + 4 * (context.lists[i] % Row));
        const reg32 kind = rc::ld32(listGroup + 4);
        const bool wantsLight = kind == 4 || kind == 5;
        lastGroup = 0;
        const bool onePass = (rc::ld8(0x8707cd) == 0 || rc::ld32(rc::ld32(CurrentView)) != 0) && rc::ld8(listGroup + 0x18) != 0 &&
                             (kind == 0 || kind == 1 || kind == 5 || kind == 4);
        for (reg32 bucket = rc::ld32(head); bucket != head; bucket = rc::ld32(bucket))
        {
            const reg32 tex = rc::ld32(bucket + 0x10);
            if (tex == 0)
                return false;
            if (textureRetryWanted(tex))
            {
                textureRetry(app, scratch, tex);
                if (scratch.terminate)
                    return false;
            }
            const reg32 entries = bucket + 0x18;
            for (reg32 entry = rc::ld32(entries); entry != entries; entry = rc::ld32(entry))
            {
                if (wantsLight)
                {
                    const reg32 light = rc::ld32(entry + 0x1c);
                    if (light != 0 && light != lastLight)
                    {
                        if (!context.addLight(light))
                            return false;
                        lastLight = light;
                    }
                }
                const reg32 entryGroup = rc::ld32(entry + 0x18);
                if (entryGroup != lastGroup)
                {
                    const sreg32 entryPasses = sreg32(rc::ld32(entryGroup + 0x14));
                    lastGroup = entryGroup;
                    stripDraws = br::effectDrawnPasses(rc::ld32(entryGroup + 0x20), entryPasses > 0 ? reg32(entryPasses) : 0);
                    meshDraws = onePass ? br::effectDrawnPasses(rc::ld32(entryGroup + 0x20), 1) : stripDraws;
                }
                if (rc::ld32(entry + 0xc) != 0)
                {
                    draws += stripDraws;
                    const reg32 vertices = rc::ld32(rc::ld32(entry + 8));
                    const reg32 passes = rc::ld32(rc::ld32(entry + 0x18) + 0x14);
                    if (vertices < 2 || vertices > 0x10000)
                        return false;
                    if (sreg32(passes) > 0)
                        arena += passes * ((vertices * 0x18 + 15) & ~15u);
                    continue;
                }
                const reg32 mesh = rc::ld32(rc::ld32(entry + 0x10));
#ifdef DEV_TOOLS
                if (context.solids.empty() || context.solids.back() != rc::ld32(entry + 0x10))
                    context.solids.push_back(rc::ld32(entry + 0x10));
#endif
                draws += meshDraws;
                for (reg32 i = 0; i < staleCount; ++i)
                    if (stale[i] == rc::ld32(mesh + 0x20) || stale[i] == rc::ld32(mesh + 0x2c))
                        br::segmentPrepareBuffer(stale[i]);
                if (rc::ld32(mesh + 0x2c) != 0)
                    continue;
                offload::created();
                s.ebx = mesh;
                s.esi = mesh + 0x2c;
                s.frame = scratch.esp - 0x34;
                if (!createVertexBuffer(app, scratch, s) || scratch.terminate || rc::ld32(mesh + 0x2c) == 0)
                    return false;
            }
        }
    }
    return true;
}

// Lists leave the guest heads, bucket pointers clear.
void detach(flush::Context& context)
{
    for (reg32 i = 0; i < context.listCount; ++i)
    {
        const reg32 head = Heads + 8 * context.lists[i];
        for (reg32 bucket = rc::ld32(head); bucket != head; bucket = rc::ld32(bucket))
            rc::st32(rc::ld32(rc::ld32(bucket + 0x10)) + 0x1c, 0);
        flush::detachList(head, context.heads + 8 * context.lists[i]);
    }
}

// The detached lists on the context's CPU.
bool drawLists(win32::WinApplication* app, x86::CPU& cpu, flush::Context& context)
{
    State s = {cpu.ebx, cpu.ebp, cpu.esi, cpu.edi, cpu.esp - 0x34};
    increment<0x872edc>(cpu);
    for (reg32 i = 0; i < context.listCount; ++i)
    {
        const reg32 k = context.lists[i];
        s.ebx = context.heads + 8 * k;
        s.ebp = Slots + 4 * (k % Row);
        rc::st32(s.frame + 0x20, s.ebx);
        rc::st32(s.frame + 0x24, s.ebp);
        if (!drawList(app, cpu, s))
            return false;
    }
    return true;
}

const uint32_t MaxJobs = 32;

std::vector<flush::Context*> s_jobs;        // every job
std::vector<flush::Context*> s_pending;     // jobs not closed yet, in marker order
std::atomic<uint32_t> s_serial{0};          // jobs queued so far
uint32_t s_peak = 0;
bool s_fencing = false;
uint64_t s_fenceWaitUs = 0;

// Unload notices no pending job was queued before are dropped.
void retireNotices()
{
    if (unload::count() != 0)
        unload::retire(s_pending.empty() ? ~0u : s_pending.front()->serial);
}

// A job that is closed; null when all are pending.
flush::Context* freeJob()
{
    for (flush::Context* job : s_jobs)
        if (!job->pending)
            return job;
    if (s_jobs.size() == MaxJobs)
        return nullptr;
    s_jobs.push_back(new flush::Context);
    return s_jobs.back();
}

// The flush on the job's CPU, then the segment is the resolver's.
void runJob(flush::Context& job)
{
#ifdef DEV_TOOLS
    win32::d3d9::probe::segmentRun(true);
#endif
    job.ok = drawLists(job.app, job.cpu, job);
#ifdef DEV_TOOLS
    win32::d3d9::probe::segmentRun(false);
    if (win32::d3d9::probe::on())
        win32::d3d9::probe::count(win32::d3d9::probe::PoolFrees, job.entries.count + job.buckets.count + job.strips.count);
#endif
    br::segmentFinish(job);
}

// Game thread: what a done job leaves for core 0.
void closeJob(flush::Context& job)
{
    flush::queue::await(&job);
    if (!job.ok)
        pf::fatal("offload: a flush on its own context failed");
    job.writeBack();
    job.giveBack();
    br::segmentEnd(job);
    job.pending = false;
    job.state.store(flush::JobIdle, std::memory_order_relaxed);
}

// Closes the first pending jobs while the test holds; the rest of the list keeps its order.
template< typename F > void closeWhile(F&& test)
{
    size_t n = 0;
    while (n < s_pending.size() && test(*s_pending[n]))
        closeJob(*s_pending[n++]);
    s_pending.erase(s_pending.begin(), s_pending.begin() + n);
}

void closeDoneJobs()
{
    closeWhile([](flush::Context& job) { return flush::queue::isDone(job); });
}

// Jobs of frames before this one are joined.
void joinOlder(uint32_t frame)
{
    closeWhile([frame](flush::Context& job) { return job.frame < frame; });
    closeDoneJobs();
}

void onFrameOpened(uint32_t frame)
{
    joinOlder(frame - 1);
}

// Core 0 does what the segment's draws may read: effect textures kept as they are at the marker.
void snapshotEffects(flush::Context& context)
{
    for (reg32 i = 0; i < context.listCount; ++i)
        br::segmentSnapshotEffect(context, rc::ld32(rc::ld32(Slots + 4 * (context.lists[i] % Row)) + 0x20));
}

// One flush on a context; false: nothing done.
bool runContext(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!contextAllowed())
        return false;
    constexpr bool late = flush::CoreMode != flush::Mode::Inline;
    static const bool poison = NFSU_SWITCH_CACHED("offload_poison", 0);
    static const bool sequential = poison || NFSU_SWITCH_CACHED("offload_sequential", 0);
    if (late)
    {
        static const bool hooked = (br::setFrameHook(&onFrameOpened), true);
        (void)hooked;
        if (const uint32_t frame = br::openFrameIndex())
            joinOlder(frame);
    }
    retireNotices();
    flush::Context* job = freeJob();
    if (!job)
        return false;
    flush::Context& context = *job;
    uint64_t part[offload::Parts] = {};
    uint64_t mark = offload::instructions();
    const auto lap = [&](uint32_t i)
    {
        const uint64_t now = offload::instructions();
        part[i] = now - mark;
        mark = now;
    };
    reg32 arena, draws;
    const bool prepared = prepare(app, cpu, context, arena, draws);
    lap(0);
    if (!prepared || !br::segmentBegin(context, arena, draws, late ? &flush::queue::await : nullptr, &context))
    {
        offload::contextRun(false);
        return false;
    }
    lap(1);
    context.pending = true;
    context.serial = s_serial.fetch_add(1) + 1;
    context.app = app;
    context.frame = br::openFrameIndex();
    context.ok = true;
    context.run = &runJob;
    offload::contextRun(true);
    resetCaches(true);
    context.open(cpu);
    detach(context);
    if (late)
        snapshotEffects(context);
    uint8_t heads[HeadCount * 8];
    if (poison)
    {
        memcpy(heads, rc::host(Heads), sizeof(heads));
        context.poison();
        for (reg32 i = 0; i < HeadCount * 2; ++i)
            rc::st32(Heads + 4 * i, 0xa5a5a5a5u);
    }
    lap(2);
    if (late)
    {
        offload::parts(part);
        s_pending.push_back(&context);
        s_peak = std::max<uint32_t>(s_peak, uint32_t(s_pending.size()));
        flush::queue::submit(context);
        if (sequential)
        {
            flush::queue::await(&context);
            if (poison)
            {
                memcpy(rc::host(Heads), heads, sizeof(heads));
                context.unpoison();
            }
            closeWhile([&context](flush::Context& j) { return &j == &context; });
        }
        resetCaches(false);
        rc::st32(0x87094c, 0);
        cpu.esp += 4;
        return true;
    }
    runJob(context);
    lap(3);
    if (poison)
    {
        memcpy(rc::host(Heads), heads, sizeof(heads));
        context.unpoison();
    }
    if (!context.ok)
    {
        cpu.terminate = true;
        return true;
    }
    context.writeBack();
    context.giveBack();
    lap(4);
    br::segmentEnd(context);
    context.pending = false;
    resetCaches(false);
    rc::st32(0x87094c, 0);
    cpu.esp += 4;
    lap(5);
    offload::parts(part);
    return true;
}

#endif

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("flush_native"))
    {
        sub_5cf420_guest(app, cpu);
        return;
    }
    s_direct = NFSU_RENDER_FAST_CACHED("flush_direct");
    State s = {cpu.ebx, cpu.ebp, cpu.esi, cpu.edi, cpu.esp - 0x34};
    rc::st32(s.frame + 0xc, s.ebx);
    rc::st32(s.frame + 8, s.ebp);
    rc::st32(s.frame + 4, s.esi);
    rc::st32(s.frame, s.edi);
    increment<0x872edc>(cpu);
    reg32 k = 0;
    bool guest = false;
    for (;;)
    {
        if (!guest)
        {
            k = nextList(k);
            if (k == HeadCount)
                break;
            s.ebx = Heads + 8 * k;
            s.ebp = Slots + 4 * (k % Row);
            rc::st32(s.frame + 0x20, s.ebx);
            rc::st32(s.frame + 0x24, s.ebp);
        }
        else if (!guestStep(s))
            break;
        else if (rc::ld32(s.ebx) == s.ebx)
            continue;
        const reg32 ebx = s.ebx, ebp = s.ebp;
        if (!drawList(app, cpu, s))
            return;
        if (!guest)
        {
            guest = s.ebx != ebx || s.ebp != ebp;
            ++k;
        }
    }
    cpu.edi = rc::ld32(s.frame);
    cpu.esi = rc::ld32(s.frame + 4);
    cpu.ebp = rc::ld32(s.frame + 8);
    rc::st32(0x87094c, 0);
    cpu.ebx = rc::ld32(s.frame + 0xc);
    cpu.esp = s.frame + 0x38;
}

}

// Not routed through shadow::call: the flush unlinks and frees, so a second run cannot be compared.
void sub_5cf420(win32::WinApplication* app, x86::CPU& cpu)
{
    offload::flushBegin();
#ifdef NFSU_OFFLOAD_CORE
    if (runContext(app, cpu))
    {
        offload::flushEnd();
        return;
    }
    const Segment segment;
#endif
    scenery::timed(scenery::PhaseFlush, [&] { body(app, cpu); });
    offload::flushEnd();
}

namespace flush
{

#ifdef NFSU_OFFLOAD_CORE
void closeDone()
{
    closeDoneJobs();
}

void fence()
{
    const uint32_t start = pf::perf::now();
    closeWhile([](Context&) { return true; });
    s_fenceWaitUs += pf::perf::now() - start;
}

// Waits out the worker's draw step in progress; microseconds waited.
uint32_t awaitStep()
{
    const uint32_t at = unload::step();
    if (!(at & 1))
        return 0;
    const uint32_t start = pf::perf::now();
    for (uint32_t spins = 0; unload::step() == at; ++spins)
    {
        const uint32_t waited = pf::perf::now() - start;
        if (waited > 200000)
        {
            pf::logError("offload: an unload gave up waiting for the flush worker's draw");
            break;
        }
        if (spins > 2000)
            pf::sleepUs(50);
    }
    return pf::perf::now() - start;
}

std::atomic<uint32_t> s_waits{0}, s_waitedUs{0}, s_longestUs{0}, s_fences{0};
std::atomic<bool> s_waitDue{false};

void unloadSettle()
{
    if (!s_waitDue.exchange(false))
        return;
    if (const uint32_t waited = awaitStep())
    {
        ++s_waits;
        s_waitedUs += waited;
        if (waited > s_longestUs.load())
            s_longestUs.store(waited);
    }
}

void unloadNotice(const char* what, reg32 from, reg32 size, uint32_t kind, bool settle)
{
    const uint32_t serial = s_serial.load();
    if (serial == 0)
        return;
    const UnloadMode mode = unloadMode();
    const bool game = pf::Thread::currentId() == queue::gameThread();
    if (game)
    {
        retireNotices();
        uint32_t running = 0;
        for (const Context* job : s_pending)
            running += !queue::isDone(*job);
#ifdef DEV_TOOLS
        static uint32_t calls = 0, logged = 0, hitsLogged = 0;
        uint32_t hits = 0;
        for (const Context* job : s_pending)
            if (!queue::isDone(*job) && size != 0)
                for (const reg32 solid : job->solids)
                    hits += solid - from < size;
        if (hits != 0 && hitsLogged < 64 && ++hitsLogged)
            pf::logError("offload: %s unload of 0x%08x+0x%x at frame %u frees %u solids that unfinished flushes still draw",
                         what, unsigned(from), unsigned(size), unsigned(rc::ld32(0x86514c)), unsigned(hits));
        ++calls;
        if ((running != 0 || calls <= 4 || (calls & (calls - 1)) == 0) && logged < 400 && ++logged)
        {
            const unload::Stats t = unload::stats();
            pf::logInfo("offload: %s unload %u at frame %u: %u flushes pending, %u not finished | notices %u (%u joined, %u full, "
                        "%u retired, %u live), draws checked %u, cancelled %u, stale %u, waits %u (%u us, longest %u), fences %u",
                        what, unsigned(calls), unsigned(rc::ld32(0x86514c)), unsigned(s_pending.size()), unsigned(running),
                        unsigned(t.notices), unsigned(t.joined), unsigned(t.full), unsigned(t.retired), unsigned(unload::count()),
                        unsigned(t.steps), unsigned(t.skipped), unsigned(s_stale.load()), unsigned(s_waits.load()),
                        unsigned(s_waitedUs.load()), unsigned(s_longestUs.load()), unsigned(s_fences.load()));
        }
#endif
        if (running == 0)
            return;
        if (mode == UnloadFence)
        {
            ++s_fences;
            fence();
            return;
        }
    }
    else
    {
        static std::atomic<bool> told{false};
        if (!told.exchange(true))
            pf::logError("offload: %s unload off the game thread", what);
    }
    if (size == 0 || !unload::publish(from, size, serial, kind))
    {
        ++s_fences;
        if (game)
            fence();
        else
            pf::logError("offload: %s unload off the game thread could not be announced", what);
        return;
    }
    if (mode == UnloadOff)
        return;
    s_waitDue.store(true);
    if (settle)
        unloadSettle();
}

void unloadPoison(uint32_t kind, reg32 from, reg32 size)
{
#ifdef DEV_TOOLS
    static const uint32_t kinds = uint32_t(pf::Config::getInt("offload_unload_poison", 0));
    if (!(kinds & kind) || s_serial.load() == 0)
        return;
    for (reg32 at = (from + 3) & ~3u; at + 4 <= from + size; at += 4)
        rc::st32(at, 0xdeadbeefu);
#else
    (void)kind;
    (void)from;
    (void)size;
#endif
}

Pending pendingStats()
{
    const Pending p = { uint32_t(s_pending.size()), s_peak, queue::totals().waitedUs[1], s_fenceWaitUs };
    s_peak = 0;
    return p;
}
#else
void closeDone() {}
void fence() {}
void unloadNotice(const char*, reg32, reg32, uint32_t, bool) {}
void unloadSettle() {}
void unloadPoison(uint32_t, reg32, reg32) {}
Pending pendingStats() { return Pending{}; }
#endif

}

}}
