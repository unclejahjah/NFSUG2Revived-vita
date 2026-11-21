// Native replacement of sub_5bd4a0: draw queue material setup (cdecl: material, packet flags).

#include <native/common.h>
#include <native/flush_context.h>
#include <native/shadow.h>
#include <platform/switches.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <winapi/d3d9_native.h>

#if defined(__ARM_NEON) && !defined(NFSU_FPU_DOUBLE)
#include <arm_neon.h>
#endif

namespace ug2
{

void sub_5bd4a0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

// Material floats +0x30..+0x9f, read once; NEON only loads them, arithmetic stays scalar VFP.
struct Material
{
#if defined(__ARM_NEON) && !defined(NFSU_FPU_DOUBLE)
    float32x4_t q[7];

    explicit Material(x86::reg32 material)
    {
        const uint8_t* p = rc::host(material + 0x30);
        for (int k = 0; k < 7; ++k)
            q[k] = vreinterpretq_f32_u8(vld1q_u8(p + 16 * k));
    }
    Real operator()(x86::reg32 offset) const { return Real(q[(offset - 0x30) >> 4][((offset - 0x30) >> 2) & 3]); }
#else
    float f[28];

    explicit Material(x86::reg32 material) { memcpy(f, rc::host(material + 0x30), sizeof(f)); }
    Real operator()(x86::reg32 offset) const { return Real(f[(offset - 0x30) >> 2]); }
#endif
};

#if defined(__ARM_FP) && !defined(NFSU_FPU_DOUBLE)

// Fixed operand order: of two NaNs the first wins.
inline float mul(float a, float b)
{
    float r;
#ifdef __aarch64__
    __asm__("fmul %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b));
#else
    __asm__("vmul.f32 %0, %1, %2" : "=t"(r) : "t"(a), "t"(b));
#endif
    return r;
}

inline float sub(float a, float b)
{
    float r;
#ifdef __aarch64__
    __asm__("fsub %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b));
#else
    __asm__("vsub.f32 %0, %1, %2" : "=t"(r) : "t"(a), "t"(b));
#endif
    return r;
}

// a * b - c; ARMv7 vnmls, as the guest build.
inline float mulSub(float c, float a, float b)
{
#ifdef __aarch64__
    return sub(mul(a, b), c);
#else
    __asm__("vnmls.f32 %0, %1, %2" : "+t"(c) : "t"(a), "t"(b));
    return c;
#endif
}

#else

inline Real mul(Real a, Real b) { return a * b; }
inline Real sub(Real a, Real b) { return a - b; }
inline Real mulSub(Real c, Real a, Real b) { return a * b - c; }

#endif

// Stores a, b, c, d to dst as one vector.
inline void store4(float* dst, float a, float b, float c, float d)
{
#if defined(__ARM_NEON) && !defined(NFSU_FPU_DOUBLE)
    const float32x4_t v = {a, b, c, d};
    vst1q_f32(dst, v);
#else
    dst[0] = a;
    dst[1] = b;
    dst[2] = c;
    dst[3] = d;
#endif
}

struct Colours
{
    float d0[4], d1[4], d2[4], d3[4], d4[4], d5[4];
};

[[gnu::noinline]] void fixedFunction(win32::WinApplication* app, x86::CPU& cpu, const Colours& c, Real half,
                                     Real power)
{
    const x86::reg32 one = 0x3f800000u;
    win32::DWORD d3dMaterial[17];
    for (int j = 0; j < 3; ++j)
    {
        d3dMaterial[j] = bits(float(Real(c.d1[j]) * half + Real(c.d0[j])));
        d3dMaterial[8 + j] = bits(float(Real(c.d3[j]) * half + Real(c.d2[j])));
    }
    for (int j = 3; j < 8; ++j)
        d3dMaterial[j] = one;
    d3dMaterial[11] = one;
    d3dMaterial[12] = d3dMaterial[13] = d3dMaterial[14] = 0;
    d3dMaterial[15] = one;
    const Real minimum = ld(0x784358);
    d3dMaterial[16] = bits(float(power));
    cpu.fpu.compare(power, minimum);
    if (power < minimum)
        d3dMaterial[16] = 0x3dcccccdu;
    const x86::reg32 device = rc::ld32(0x870974);
    win32::d3d9::bridge::setMaterial(app, cpu, device, d3dMaterial);

    const Real scale = ld(0x784544);
    const auto ftol2 = [](Real value) { return x86::reg32(x86::truncInteger64(value)); };
    const Real e0 = Real(float(Real(c.d5[0]) * half + Real(c.d4[0])));
    const Real e1 = Real(float(Real(c.d5[1]) * half + Real(c.d4[1])));
    const Real e2 = Real(float(Real(c.d5[2]) * half + Real(c.d4[2])));
    x86::reg32 factor = ftol2(Real(c.d1[3]) * ld(0x784540) + Real(c.d0[3]) * scale) << 8;
    factor = (factor | ftol2(e2 * scale)) << 8;
    factor = (factor | ftol2(e1 * scale)) << 8;
    factor |= ftol2(e0 * scale);
    cpu.eax = win32::d3d9::bridge::setRenderState(app, cpu, rc::ld32(0x870974), 0x3c, factor);
}

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("material_native"))
    {
        sub_5bd4a0_guest(app, cpu);
        return;
    }
    const x86::reg32 group = flush::ld32<0x870910>(cpu);
    const x86::reg32 kind = rc::ld32(group + 4);
    if (kind != 5 && kind != 4)
    {
        cpu.eax = kind;
        cpu.esp += 4;
        return;
    }
    const x86::reg32 material = argument(cpu, 0);
    const x86::reg32 flags = argument(cpu, 1);
    const bool lit = (flags & 0x100) && flush::ld32<0x870cb8>(cpu) != 0;
    const bool all = kind == 5;
    const Material m(material);
    const Real half = ld(0x784260);
    Colours c;
    if (!lit || (flags & 0x80))
    {
        const float a0 = float(mul(m(0x34), m(0x30)));
        const float a1 = float(mul(m(0x38), m(0x30)));
        const float a2 = float(mul(m(0x3c), m(0x30)));
        const float a3 = float(m(0x50));
        store4(c.d0, a0, a1, a2, a3);
        store4(c.d1, float(mulSub(Real(a0), m(0x44), m(0x40))), float(mulSub(Real(a1), m(0x48), m(0x40))),
               float(mulSub(Real(a2), m(0x4c), m(0x40))), float(sub(m(0x54), Real(a3))));
        if (all)
        {
            const float b0 = float(mul(m(0x60), m(0x5c)));
            const float b1 = float(mul(m(0x64), m(0x5c)));
            const float b2 = float(mul(m(0x68), m(0x5c)));
            store4(c.d2, b0, b1, b2, 0);
            store4(c.d3, float(sub(mul(m(0x70), m(0x6c)), Real(b0))), float(sub(mul(m(0x74), m(0x6c)), Real(b1))),
                   float(sub(mul(m(0x78), m(0x6c)), Real(b2))), 0);
        }
    }
    else
    {
        memset(c.d0, 0, sizeof(c.d0));
        memset(c.d1, 0, sizeof(c.d1));
        memset(c.d2, 0, sizeof(c.d2));
        memset(c.d3, 0, sizeof(c.d3));
    }
    if (all)
    {
        if (lit)
        {
            const float e0 = float(mul(mul(m(0x84), m(0x80)), half));
            const float e1 = float(mul(mul(m(0x88), m(0x80)), half));
            const float e2 = float(mul(mul(m(0x8c), m(0x80)), half));
            store4(c.d4, e0, e1, e2, 0);
            store4(c.d5, float(sub(mul(mul(m(0x94), m(0x90)), half), Real(e0))),
                   float(sub(mul(mul(m(0x98), m(0x90)), half), Real(e1))),
                   float(sub(mul(mul(m(0x9c), m(0x90)), half), Real(e2))), 0);
        }
        else
        {
            memset(c.d4, 0, sizeof(c.d4));
            memset(c.d5, 0, sizeof(c.d5));
        }
        if (rc::ld8(group + 0x18) == 0)
        {
            fixedFunction(app, cpu, c, half, mul(Real(flush::ldf32<0x80051c>(cpu)), m(0x58)));
            cpu.esp += 4;
            return;
        }
    }
    const x86::reg32 effect = rc::ld32(group + 0x20);
    const x86::reg32 table = rc::ld32(group + 0x1c);
    win32::d3d9::bridge::CallGroup calls(app, cpu);
    calls.setVector(effect, rc::ld32(table + 0x164), c.d0);
    x86::reg32 result = calls.setVector(effect, rc::ld32(table + 0x18c), c.d1);
    if (all)
    {
        calls.setVector(effect, rc::ld32(table + 0x7cc), c.d2);
        calls.setVector(effect, rc::ld32(table + 0x81c), c.d3);
        calls.setVector(effect, rc::ld32(table + 0x2a4), c.d4);
        calls.setVector(effect, rc::ld32(table + 0x27c), c.d5);
        calls.setFloat(effect, rc::ld32(table + 0x7f4), float(mul(Real(flush::ldf32<0x800b94>(cpu)), m(0x58))));
        result = calls.setFloat(effect, rc::ld32(table + 0x2cc), float(m(0x7c)));
    }
    cpu.eax = result;
    cpu.esp += 4;
}

void declare(x86::CPU&, shadow::Spans&) {}

// eax, ecx dead after the only call site, 0x5cf54e.
const shadow::Spec spec = {"sub_5bd4a0", shadow::Ebx | shadow::Esp | shadow::Ebp | shadow::Esi, 0, false, true};

}

void sub_5bd4a0(win32::WinApplication* app, x86::CPU& cpu)
{
    shadow::call(spec, app, cpu, body, sub_5bd4a0_guest, declare);
}

}}
