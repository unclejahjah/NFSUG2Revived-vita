// Native sub_5d6570: scenery instance cull, LOD pick, render-list entry and light flares (thiscall, ret 0xc).

#include <native/common.h>
#include <native/shadow.h>
#include <platform/switches.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <scenery.h>

#include <algorithm>
#include <cmath>

#if defined(__ARM_NEON) && !defined(NFSU_FPU_DOUBLE)
#include <arm_neon.h>
#endif

namespace ug2
{

void sub_5d6570_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5bc300(win32::WinApplication* app, x86::CPU& cpu);
void sub_5d64d0(win32::WinApplication* app, x86::CPU& cpu);
void sub_5d6240(win32::WinApplication* app, x86::CPU& cpu);
void sub_613540(win32::WinApplication* app, x86::CPU& cpu);
void sub_43c990(win32::WinApplication* app, x86::CPU& cpu);
void sub_43ce50(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

// Sphere width in pixels for a view, without the guest's margin.
inline float onScreenWidth(x86::reg32 pos, x86::reg32 ci, float r)
{
    const float dx = rc::ldf32(pos) - rc::ldf32(ci + 0x60);
    const float dy = rc::ldf32(pos + 4) - rc::ldf32(ci + 0x64);
    const float dz = rc::ldf32(pos + 8) - rc::ldf32(ci + 0x68);
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    return d > r ? 2.0f * rc::ldf32(ci + 0x80) * r / d : 1e9f;
}

inline uint32_t polys(x86::reg32 model)
{
    const x86::reg32 s = model ? rc::ld32(model + 0xc) : 0;
    return s ? uint32_t(std::max(0, int(int16_t(rc::ld16(s + 0x14))))) : 0;
}

// True when a model of the scenery info carries light markers.
inline bool hasLights(x86::reg32 info)
{
    for (x86::reg32 slot = 0x30; slot <= 0x38; slot += 4)
    {
        const x86::reg32 m = rc::ld32(info + slot);
        const x86::reg32 s = m ? rc::ld32(m + 0xc) : 0;
        if (s && rc::ld32(s + 0x80) != 0 && rc::ld8(s + 0x1b) != 0)
            return true;
    }
    return false;
}

// True when the reflection model saves enough geometry (world_lod_* keys).
inline bool worthLowering(const scenery::Settings& set, x86::reg32 info, x86::reg32 model, x86::reg32 low)
{
    const uint32_t from = polys(model), to = polys(low);
    if (from < set.lodMinTris || to * 100 > from * set.lodMaxPercent)
        return false;
    if (2.0f * rc::ldf32(info + 0x3c) < set.lodMinWidth)
        return false;
    return set.lodSigns || (rc::ld32(info) & 0xffffff) != 0x5f5358;
}

// Callees keep ebx..ebp and x87 depth; trivial ones inlined.
void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("draw_scenery_native"))
    {
        ug2::sub_5d6570_guest(app, cpu);
        return;
    }
    const x86::reg32 entry = cpu.esp;
    const auto leave = [&] { cpu.esp = entry + 0x10; };

    const x86::reg32 sec = cpu.ecx;
    const x86::reg32 ci = rc::ld32(entry + 0x8);
    const x86::reg32 inst = rc::ld32(sec + 0x20) + (rc::ld32(entry + 0x4) << 6);
    const int32_t ex = int32_t(rc::ld32(ci + 0x84));
    if (ex >= 0)
    {
        const x86::reg32 off = (rc::ld32(inst + 0x1c) << 7) + x86::reg32(ex >> 3);
        if (rc::ld8(rc::ld32(sec + 0x30) + off) & (1u << (ex & 7)))
            return leave();
    }
    if (rc::ld8(ci + 0x44) & uint8_t(uint8_t(rc::ld16(inst + 0x1a)) ^ 0x60))
        return leave();
    const x86::reg32 info = rc::ld32(sec + 0x18) + x86::reg32(int32_t(int16_t(rc::ld16(inst + 0x18))) * 0x44);
    const x86::reg32 frame = ((entry - 0x4) & ~x86::reg32(15)) - 0x80;

    x86::reg32 state = rc::ld32(entry + 0xc);
    if (state == 1)
    {
#if defined(__ARM_NEON) && !defined(NFSU_FPU_DOUBLE)
        const uint8x16_t b0 = vld1q_u8(rc::host(inst));
        const uint8x8_t b1 = vld1_u8(rc::host(inst + 0x10));
        x86::reg8* const box = rc::host(frame + 0x10);
        vst1_u8(box, vget_low_u8(b0));
        const uint32x2_t mid = vreinterpret_u32_u8(vget_high_u8(b0));
        vst1_lane_u32(reinterpret_cast<uint32_t*>(box + 0x8), mid, 0);
        vst1_lane_u32(reinterpret_cast<uint32_t*>(box + 0x10), mid, 1);
        vst1_u8(box + 0x14, b1);
#else
        rc::st32(frame + 0x10, rc::ld32(inst));
        rc::stf32(frame + 0x14, float(ld(inst + 0x4)));
        rc::stf32(frame + 0x18, float(ld(inst + 0x8)));
        rc::stf32(frame + 0x20, float(ld(inst + 0xc)));
        rc::stf32(frame + 0x24, float(ld(inst + 0x10)));
        rc::stf32(frame + 0x28, float(ld(inst + 0x14)));
#endif
        rc::st32(frame - 0x4, 0);
        rc::st32(frame - 0x8, frame + 0x20);
        rc::st32(frame - 0xc, frame + 0x10);
        cpu.ecx = rc::ld32(ci + 0x40);
        cpu.esp = frame - 0x10;
        ug2::sub_5bc300(app, cpu);
        state = cpu.eax;
        rc::st32(entry + 0xc, state);
        if (state == 0)
            return leave();
    }

    rc::stf32(frame - 0x4, float(ld(0x7a47cc) + ld(info + 0x3c)));
    rc::st32(frame - 0x8, inst + 0x20);
    rc::st32(frame - 0xc, ci);
    cpu.esp = frame - 0x10;
    ug2::sub_5d64d0(app, cpu);
    const int32_t px = int32_t(cpu.eax);
    if (px <= 1)
        return leave();

    const scenery::Settings& set = scenery::g_settings;
    const x86::reg32 camera = rc::ld32(ci + 0x40);
    const int other = camera != 0x832e50 && camera != 0x832ec0;
    const float minPixels = other ? set.otherMinPixels : set.mainMinPixels;
    float width = 0;
    if (minPixels > 0 || set.lodPixels > 0 || scenery::logging())
    {
        width = onScreenWidth(inst + 0x20, ci, rc::ldf32(info + 0x3c));
        SCENE_STATS(
            if (scenery::logging())
            {
                ++scenery::g_counters.tested[other];
                if (!other)
                    scenery::g_counters.scale = rc::ldf32(ci + 0x80);
            })
        if (width < minPixels && !(set.keepLights && hasLights(info)))
        {
            SCENE_STATS(
                if (scenery::logging())
                {
                    ++scenery::g_counters.smallCulled[other];
                    scenery::g_counters.smallPolys[other] += polys(rc::ld32(info + 0x34));
                })
            return leave();
        }
    }
    struct Tally
    {
        const scenery::Settings& set;
        int other;
        float width;
        x86::reg32 model = 0;
        bool drawn = false;
        ~Tally()
        {
            SCENE_STATS(
                if (!scenery::logging())
                    return;
                if (!drawn)
                {
                    ++scenery::g_counters.engineCulled[other];
                    return;
                }
                const int band = scenery::widthBand(width);
                ++scenery::g_counters.drawn[other][band];
                scenery::g_counters.polys[other][band] += polys(model);)
        }
    } tally{set, other, width};

    const x86::reg32 flags = rc::ld32(ci + 0x44);
    x86::reg32 model;
    if (flags & 0x1800)
    {
        if (px < 0x20)
            return leave();
        model = rc::ld32(info + ((rc::ld8(inst + 0x1b) & 4) ? 0x34 : 0x38));
    }
    else if (flags & 0x20)
    {
        if (px < 0x10)
            return leave();
        model = rc::ld32(info + 0x34);
    }
    else if (int32_t(rc::ld32(0x8707dc)) >= 3)
    {
        if (px < 0x17)
            return leave();
        model = rc::ld32(info + 0x34);
    }
    else
    {
        if (px < 0x11)
            return leave();
        const x86::reg32 m = rc::ld32(info + 0x30);
        Real f = ld(0x7d7d3c);
        if (m != 0)
        {
            const x86::reg32 s = rc::ld32(m + 0xc);
            if (s != 0)
            {
                if (int16_t(rc::ld16(s + 0x14)) < 40)
                    f = ld(0x7a47dc);
                else if (ld(s + 0x94) > ld(0x7d7d3c))
                {
                    Real g = ld(s + 0x98);
                    if (g < ld(0x7a47e0))
                        g = ld(0x7a47e0);
                    f = Real(px) / g;
                }
            }
        }
        if (f >= ld(0x7a47dc) && rc::ld32(0x870cf4) - 2u < 2u)
            model = rc::ld32(info + 0x30);
        else
        {
            app->allowPreemption(cpu);
            model = rc::ld32(info + 0x34);
        }
    }
    if (model == 0)
        return leave();
    if (width < set.lodPixels && !other && !(rc::ld8(inst + 0x1b) & 4))
    {
        const x86::reg32 low = rc::ld32(info + 0x38);
        if (low != 0 && low != model && worthLowering(set, info, model, low))
        {
            SCENE_STATS(
                if (scenery::logging())
                {
                    ++scenery::g_counters.lowered;
                    scenery::g_counters.loweredFrom += polys(model);
                    scenery::g_counters.loweredTo += polys(low);
                })
            model = low;
        }
    }
    tally.model = model;
    tally.drawn = true;

    if (rc::ld8(inst + 0x1b) & 1)
    {
        const x86::reg32 e = rc::ld32(ci + 0x4c);
        if (e >= rc::ld32(ci + 0x50))
            return leave();
        rc::st32(ci + 0x4c, e + 12);
        rc::st32(e, model + state);
        rc::st32(e + 0x4, 0);
        rc::st32(e + 0x8, inst);
        return leave();
    }

    // Inlined sub_401e80(1).
    x86::reg32 mat = rc::ld32(0x82da5c);
    {
        const x86::reg32 next = mat + 0x40;
        if (next < rc::ld32(0x82da60))
            rc::st32(0x82da5c, next);
        else
        {
            const x86::reg32 used = rc::ld32(0x82da70) + 0x40;
            rc::st32(0x82da6c, 1);
            rc::st32(0x82da70, used);
            mat = 0;
        }
    }
    if (mat == 0)
        return leave();
    rc::st32(frame - 0x4, mat);
    cpu.ecx = inst;
    cpu.esp = frame - 0x8;
    ug2::sub_5d6240(app, cpu);

    // Inlined sub_5d6210.
#if defined(__ARM_NEON) && !defined(NFSU_FPU_DOUBLE)
    vst1q_u8(rc::host(mat + 0x30),
             vreinterpretq_u8_u32(vsetq_lane_u32(0x3f800000u, vreinterpretq_u32_u8(vld1q_u8(rc::host(inst + 0x20))), 3)));
#else
    {
        const x86::reg32 p0 = rc::ld32(inst + 0x20);
        const Real p1 = ld(inst + 0x24), p2 = ld(inst + 0x28);
        rc::st32(mat + 0x30, p0);
        rc::stf32(mat + 0x34, float(p1));
        rc::st32(mat + 0x3c, 0x3f800000u);
        rc::stf32(mat + 0x38, float(p2));
    }
#endif

    if (rc::ld32(ci + 0x44) & 0x800)
        rc::stf32(mat + 0x28, float(-ld(mat + 0x28)));

    x86::reg32 e = rc::ld32(ci + 0x4c);
    const x86::reg32 end = rc::ld32(ci + 0x50);
    rc::st32(frame + 0xc, e);
    if (e >= end)
        return leave();
    rc::st32(ci + 0x4c, e + 12);
    rc::st32(e, state + model);

    x86::reg32 edi = mat, esi = inst;
    if (rc::ld32(ci + 0x44) & 0x4000)
    {
        const x86::reg32 s = rc::ld32(model + 0xc);
        if (s != 0 && int8_t(rc::ld8(s + 0xe)) < 0)
        {
            rc::st32(frame - 0x4, mat);
            const Real turn = ld(0x7a47e4) * ld(mat + 0x30);
            const int32_t angle = int32_t(x86::reg64(x86::truncInteger64(double(turn))));
            const x86::reg32 view = rc::ld32(ci + 0x40);
            rc::st32(frame - 0x8, x86::reg32(angle % 360));
            rc::st32(frame - 0xc, frame + 0x40);
            rc::st32(frame - 0x10, view);
            const x86::reg32 savedEbx = cpu.ebx, savedEsi = cpu.esi, savedEdi = cpu.edi, savedEbp = cpu.ebp;
            cpu.eax = ci;
            cpu.ecx = view;
            cpu.edx = frame + 0x40;
            cpu.ebx = model;
            cpu.esi = inst;
            cpu.edi = mat;
            cpu.ebp = entry - 0x4;
            cpu.esp = frame - 0x14;
            ug2::sub_613540(app, cpu);
            const x86::reg32 sp = cpu.esp;
            esi = cpu.esi;
            edi = cpu.edi;
            rc::st32(sp - 0x4, frame + 0x40);
            rc::st32(sp - 0x8, edi);
            rc::st32(sp - 0xc, edi);
            cpu.esp = sp - 0x10;
            ug2::sub_43c990(app, cpu);
            cpu.ebx = savedEbx;
            cpu.esi = savedEsi;
            cpu.edi = savedEdi;
            cpu.ebp = savedEbp;
            e = rc::ld32(frame + 0xc);
        }
    }

    rc::st32(e + 0x4, edi);
    rc::st32(e + 0x8, esi);
    // Inlined sub_48b1e0(1) and (2).
    const x86::reg32 view = rc::ld32(ci + 0x40);
    if (view != 0x832e50 && view != 0x832ec0)
        return leave();

    // Inlined sub_48d8a0.
    const auto nextMarker = [model](x86::reg32 prev) -> x86::reg32 {
        const x86::reg32 s = rc::ld32(model + 0xc);
        if (s == 0)
            return 0;
        const x86::reg32 first = rc::ld32(s + 0x80);
        const x86::reg32 count = x86::reg32(int32_t(int8_t(rc::ld8(s + 0x1b))));
        if (first == 0 || count == 0)
            return 0;
        if (prev == 0)
            return first;
        if (prev < first || prev >= ((count * 5 - 5) << 4) + first)
            return 0;
        return prev + 0x50;
    };
    x86::reg32 marker = nextMarker(0);
    while (marker != 0)
    {
        if (rc::ld32(model + 0xc) != 0)
        {
            // Inlined sub_48ab20(type, kind).
            const x86::reg32 kind = (rc::ld32(ci + 0x40) != 0x832e50 ? 1u : 0u) + 1;
            const x86::reg32 type = rc::ld32(marker + 0x4) == 3 ? 1u : 0u;
            const x86::reg32 slot = type * 4 + 0x82dae8;
            const x86::reg32 used = rc::ld32(slot);
            x86::reg32 flare = 0;
            if (used != rc::ld32(type * 4 + 0x7f3d44))
            {
                const x86::reg32 index = type * 0xc8 + used;
                rc::st8(index + 0x82d398, x86::reg8(kind));
                flare = rc::ld32(index * 4 + 0x82cc30);
                rc::st32(slot, used + 1);
            }
            if (flare != 0)
            {
                const x86::reg32 sub = rc::ld32(model + 0xc);
                const Real d0 = ld(marker + 0x40) - ld(sub + 0x70);
                const Real d1 = ld(marker + 0x44) - ld(sub + 0x74);
                const Real d2 = ld(marker + 0x48) - ld(sub + 0x78);
                rc::stf32(frame + 0x20, float(d0));
                rc::stf32(frame + 0x24, float(d1));
                rc::stf32(frame + 0x28, float(d2));
                const x86::reg32 entryPtr = rc::ld32(frame + 0xc);
                const x86::reg32 m = rc::ld32(entryPtr + 0x4);
                const Real f0 = ld(frame + 0x20), f1 = ld(frame + 0x24), f2 = ld(frame + 0x28);
                const Real x = ((f2 * ld(m + 0x20) + f1 * ld(m + 0x10)) + f0 * ld(m)) + ld(m + 0x30);
                const Real y = ((f2 * ld(m + 0x24) + f1 * ld(m + 0x14)) + f0 * ld(m + 0x4)) + ld(m + 0x34);
                const Real z = ((f2 * ld(m + 0x28) + f1 * ld(m + 0x18)) + f0 * ld(m + 0x8)) + ld(m + 0x38);
                rc::stf32(frame + 0x38, float(z));
                rc::stf32(frame + 0x20, float(x));
                rc::st32(flare + 0x10, rc::ld32(frame + 0x20));
                rc::stf32(flare + 0x14, float(y));
                rc::stf32(flare + 0x18, float(ld(frame + 0x38)));
                rc::st8(flare + 0x2c, x86::reg8(rc::ld8(marker + 0x4) + 0xa));
                if (rc::ld32(marker + 0x4) != 3)
                    rc::st8(flare + 0x2d, 2);
                else
                {
                    rc::st8(flare + 0x2d, 4);
                    const x86::reg32 m2 = rc::ld32(entryPtr + 0x4);
                    const Real y3 = y - ld(m2 + 0x34);
                    const Real x3 = ld(frame + 0x20) - ld(m2 + 0x30);
                    rc::st32(frame - 0x4, frame + 0x10);
                    rc::st32(frame - 0x8, frame + 0x10);
                    rc::stf32(frame + 0x10, float(x3));
                    rc::stf32(frame + 0x14, float(y3));
                    cpu.esp = frame - 0xc;
                    ug2::sub_43ce50(app, cpu);
                    rc::st32(flare + 0x20, rc::ld32(frame + 0x10));
                    rc::st32(flare + 0x24, rc::ld32(frame + 0x14));
                    rc::st32(flare + 0x28, 0);
                }
            }
        }
        marker = nextMarker(marker);
        if (marker == 0)
            break;
        app->allowPreemption(cpu);
    }
    leave();
}

void declare(x86::CPU& entry, shadow::Spans& spans)
{
    const x86::reg32 ci = rc::ld32(entry.esp + 0x8);
    spans.push_back(shadow::Span{ci + 0x4c, 4, false});
    spans.push_back(shadow::Span{rc::ld32(ci + 0x4c), 12, false});
    spans.push_back(shadow::Span{0x82da5c, 0x20, false});
    spans.push_back(shadow::Span{0x82dae8, 0x10, false});
    spans.push_back(shadow::Span{0x82d398, 0x190, false});
}

// eax, ecx and edx are not compared: sub_5d69b0 reloads them after the call.
const shadow::Spec spec = {"sub_5d6570", shadow::Ebx | shadow::Ebp | shadow::Esi | shadow::Edi | shadow::Esp, 0, false, false};

}

void sub_5d6570(win32::WinApplication* app, x86::CPU& cpu)
{
    shadow::call(spec, app, cpu, body, ug2::sub_5d6570_guest, declare);
}

}}
