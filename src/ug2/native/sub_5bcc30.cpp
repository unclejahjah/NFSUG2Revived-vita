// Native replacement of sub_5bcc30: per-mesh texture bind, alpha and blend state, texture scroll upload (cdecl: texture info).
// texture_retry: a record whose texture creation failed is created again and drawn black meanwhile.

#include "textures.h"
#include "offload.h"

#include <native/common.h>
#include <native/flush_context.h>
#include <native/shadow.h>
#include <platform/switches.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <winapi/d3d9_native.h>

#include <type_traits>

#if defined(__ARM_NEON) && !defined(NFSU_FPU_DOUBLE)
#include <arm_neon.h>
#endif

namespace ug2
{

void sub_5bcc30_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_5ce170(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

// Low truncInteger64 word; in-range floats skip the double conversion.
x86::sreg32 low32(Real value)
{
    if constexpr (std::is_same<Real, float>::value)
    {
        if (__builtin_expect((bits(value) & 0x7fffffffu) < 0x4f000000u, 1))
            return x86::sreg32(value);
    }
    return x86::sreg32(x86::truncInteger64(value));
}

// TextureInfo::GetScroll (sub_48b450); the result stays a Real.
Real scroll(float time, float speed, x86::sreg32 type, float step)
{
    if (type == 0)
        return ld(0x7d7d3c);
    Real x = Real(time);
    if (type == 2)
    {
        x /= Real(step);
        x = Real(low32(x));
    }
    x *= Real(speed);
    return x - Real(low32(x));
}

alignas(16) const x86::reg32 identity[16] = {0x3f800000u, 0, 0, 0, 0, 0x3f800000u, 0, 0,
                                             0, 0, 0x3f800000u, 0, 0, 0, 0, 0x3f800000u};

bool retrying()
{
    return NFSU_DEV_BOOL_CACHED("texture_retry", true);
}

// Creates the record's missing texture again, once a frame.
[[gnu::noinline]] x86::reg32 retry(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 tex)
{
    static x86::reg32 s_next = 0;
    const x86::reg32 frame = flush::ld32<0x86514c>(cpu);
    if (x86::sreg32(frame - s_next) < 0)
        return 0;
    const x86::reg32 esp = cpu.esp;
    cpu.ecx = tex;
    cpu.esp = esp - 4;
    sub_5ce170(app, cpu);
    cpu.esp = esp;
    const x86::reg32 texture = rc::ld32(rc::ld32(tex) + 0x18);
    s_next = frame + (texture ? 1 : 30);
    textures::retried(texture != 0);
    return texture;
}

// The cache missed: tex or the current group changed.
[[gnu::noinline]] void miss(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 tex, x86::reg32 group)
{
    namespace br = win32::d3d9::bridge;
    // Read before the stores below: tex may point at them.
    const x86::reg32 plat = rc::ld32(tex);
    flush::st32<0x872f10>(cpu, flush::ld32<0x872f10>(cpu) + 1);
    flush::st32<0x870940>(cpu, tex);
    flush::st32<0x870944>(cpu, group);
    x86::reg32 texture = rc::ld32(plat + 0x18);
    if (texture == 0 && group != 0 && rc::ld32(tex + 0x70) != 0 && retrying())
    {
        if (!cpu.og)
            texture = retry(app, cpu, tex);
        if (cpu.terminate)
            return;
        if (texture == 0)
        {
            // The original binds nothing and draws with the previous bucket's texture: black instead, not cached.
            br::effectSetTexture(app, cpu, rc::ld32(group + 0x20), rc::ld32(rc::ld32(group + 0x1c) + 0x13c), 0);
            flush::st32<0x870940>(cpu, 0);
            cpu.esp += 4;
            return;
        }
    }
    // group == 0 binds through the device, then faults reading [4] in the original; not reproduced.
    if (texture == 0 || group == 0)
    {
        cpu.esp += 4;
        return;
    }
    const x86::reg32 effect = rc::ld32(group + 0x20);
    const x86::reg32 desc = rc::ld32(group + 0x1c);
    const x86::reg32 device = rc::ld32(0x870974);
    br::CallGroup calls(app, cpu);
    calls.setEffectTexture(effect, rc::ld32(desc + 0x13c), texture);

    const bool alphaTest = rc::ld8(tex + 0x55) == 1;
    flush::st32<0x86ead8>(cpu, alphaTest ? 1 : 0);
    flush::st32<0x86eadc>(cpu, alphaTest ? 0x5f : 0);
    const x86::sreg8 mode = x86::sreg8(rc::ld8(tex + 0x56));
    x86::reg32 source = 5, destination = 6;
    if (mode == 2 || mode == 6)
        destination = 2;
    if (mode == 5 || mode == 6)
        source = 7;
    if (mode == 5)
        destination = 8;
    flush::st32<0x86eae0>(cpu, mode != 0 ? 1 : 0);
    flush::st32<0x86eae4>(cpu, source);
    flush::st32<0x86eae8>(cpu, destination);

    const bool opaque = mode == 0;
    if (rc::ld32(group + 4) == 5)
        calls.setRenderState(device, 0x16, opaque ? 1 : 2);
    calls.setRenderState(device, 0xe, opaque ? 1 : 0);
    calls.queue();
    br::effectSetIntArray(app, cpu, effect, rc::ld32(desc + 0x74),
                          static_cast<const x86::sreg32*>(flush::hostPointer<0x86ead8>(cpu)), 5);

    const x86::reg32 uploads = rc::ld32(desc + 0x8bc);
    if (uploads != 0)
    {
        float v0 = 0.0f, v1 = 0.0f;
        if (flush::ld32<0x870ce8>(cpu) != 0)
        {
            const float time = float(Real(x86::sreg32(flush::ld32<0x86518c>(cpu))) * ld(0x784268));
            const x86::reg32 kind = rc::ld8(tex + 0x52);
            if (kind == 3)
            {
                v0 = float(Real(x86::sreg16(rc::ld16(tex + 0x5e))) * ld(0x7a3664));
                v1 = float(Real(x86::sreg16(rc::ld16(tex + 0x60))) * ld(0x7a3664));
            }
            else
            {
                const Real divisor0 = Real(flush::ldf32<0x870924>(cpu)), divisor1 = Real(flush::ldf32<0x870928>(cpu));
                if (divisor0 == 0.0 && divisor1 == 0.0)
                {
                    const x86::sreg32 type = x86::sreg8(kind);
                    const float stride = float(Real(x86::sreg16(rc::ld16(tex + 0x58))) * ld(0x7a0700));
                    const float speed0 = float(Real(x86::sreg16(rc::ld16(tex + 0x5a))) * ld(0x78d964));
                    v0 = float(scroll(time, speed0, type, stride));
                    const float speed1 = float(Real(x86::sreg16(rc::ld16(tex + 0x5c))) * ld(0x78d964));
                    v1 = float(scroll(time, speed1, type, stride));
                }
                else
                {
                    const float speed0 = float(divisor0 / Real(x86::sreg16(rc::ld16(tex + 0x44))));
                    const float speed1 = float(divisor1 / Real(x86::sreg16(rc::ld16(tex + 0x46))));
                    const x86::sreg32 type = x86::sreg32(flush::ld32<0x800504>(cpu));
                    const float stride = flush::ldf32<0x800508>(cpu);
                    v0 = float(scroll(time, speed0, type, stride));
                    v1 = float(scroll(time, speed1, type, stride));
                }
            }
        }
        flush::st32<0x870948>(cpu, 1);
        x86::reg32 matrix[16];
        x86::reg32 v[4];
#if defined(__ARM_NEON) && !defined(NFSU_FPU_DOUBLE)
        const uint32x2_t scrolls = vset_lane_u32(bits(v1), vdup_n_u32(bits(v0)), 1);
        vst1q_u32(matrix, vld1q_u32(identity));
        vst1q_u32(matrix + 4, vld1q_u32(identity + 4));
        vst1q_u32(matrix + 8, vcombine_u32(scrolls, vld1_u32(identity + 10)));
        vst1q_u32(matrix + 12, vld1q_u32(identity + 12));
        vst1q_u32(v, vcombine_u32(scrolls, vdup_n_u32(0)));
#else
        memcpy(matrix, identity, sizeof(matrix));
        matrix[8] = v[0] = bits(v0);
        matrix[9] = v[1] = bits(v1);
        v[2] = v[3] = 0;
#endif
        calls.setMatrix(effect, rc::ld32(desc + 0x8e4), reinterpret_cast<const float*>(matrix));
        calls.setVector(effect, uploads, reinterpret_cast<const float*>(v));
    }
    if (rc::ld8(tex + 0x52) == 0)
        flush::st32<0x870948>(cpu, 0);
    cpu.esp += 4;
}

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("texture_step_native"))
    {
        sub_5bcc30_guest(app, cpu);
        return;
    }
    const x86::reg32 tex = argument(cpu, 0);
    const x86::reg32 group = flush::ld32<0x870910>(cpu);
    const bool hit = flush::ld32<0x870940>(cpu) == tex && flush::ld32<0x870944>(cpu) == group;
    offload::textureStep(hit, tex, group);
    if (hit)
    {
        cpu.esp += 4;
        return;
    }
    miss(app, cpu, tex, group);
}

void declare(x86::CPU&, shadow::Spans& spans)
{
    spans.push_back({0x872f10, 4, false});
    spans.push_back({0x870940, 12, false});
    spans.push_back({0x86ead8, 20, false});
}

// eax, ecx, edx are dead after all seven call sites.
const shadow::Spec spec = {"sub_5bcc30", shadow::Ebx | shadow::Esp | shadow::Ebp | shadow::Esi | shadow::Edi, 0, false, true};

}

bool textureRetryWanted(x86::reg32 tex)
{
    return rc::ld32(rc::ld32(tex) + 0x18) == 0 && rc::ld32(tex + 0x70) != 0 && retrying();
}

void textureRetry(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 tex)
{
    retry(app, cpu, tex);
}

void sub_5bcc30(win32::WinApplication* app, x86::CPU& cpu)
{
    shadow::call(spec, app, cpu, body, sub_5bcc30_guest, declare);
}

}}
