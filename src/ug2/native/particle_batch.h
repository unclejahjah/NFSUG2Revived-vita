#ifndef UG2_NATIVE_PARTICLE_BATCH_H_
#define UG2_NATIVE_PARTICLE_BATCH_H_

// Particle batch entry shared by sub_5c0e10 and the draw pass sub_48bdf0: one camera-facing quad at the buffer
// cursor, thinned when optimised_particles asks.

#include <native/common.h>
#include <platform/cached.h>
#include <platform/render.h>
#include <algorithm>
#include <cmath>
#include <type_traits>

namespace ug2 { namespace native {

// Batch start of sub_5c0e10 (sub_5c0e10.cpp).
bool particleBatchBegin(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 record);

namespace batch {

#if defined(__ARM_FP) && !defined(NFSU_FPU_DOUBLE)

// Fixed operand order: of two NaNs the first wins.
inline float add(float a, float b)
{
    float r;
#ifdef __aarch64__
    __asm__("fadd %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b));
#else
    __asm__("vadd.f32 %0, %1, %2" : "=t"(r) : "t"(a), "t"(b));
#endif
    return r;
}

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

#else

inline Real add(Real a, Real b) { return a + b; }
inline Real sub(Real a, Real b) { return a - b; }
inline Real mul(Real a, Real b) { return a * b; }

#endif

// _ftol2; a float below 2^31 converts directly.
inline x86::reg32 ftol2(Real value)
{
    if constexpr (std::is_same_v< Real, float >)
    {
        if ((bits(float(value)) & 0x7fffffffu) < 0x4f000000u) [[likely]]
            return x86::reg32(x86::sreg32(float(value)));
    }
    return x86::reg32(x86::truncInteger64(double(value)));
}

// The record fields sub_5c0e10 reads.
struct Quad
{
    Real x, y, z, width, height;
    Real alpha, red, green, blue;
    x86::reg32 s0, t0, s1, t1;
};

struct Thinning
{
    uint32_t keep; // particles kept, in 256ths
    uint8_t alpha[256]; // compensated alpha of a kept particle
};

inline const Thinning& thinning()
{
    return pf::cachedStatic([] {
        Thinning t;
        t.keep = uint32_t(pf::render::particleOptions().thinPercent * 256 / 100);
        for (uint32_t i = 0; i < 256; ++i)
        {
            const float a = float(i) / 255.0f;
            t.alpha[i] = t.keep < 256
                ? uint8_t(uint32_t(std::min(255.0f, 255.0f * (1.0f - powf(1.0f - a, 256.0f / float(t.keep))) + 0.5f)))
                : uint8_t(i);
        }
        return t;
    });
}

// Drops a stable share of particles, compensating alpha.
inline x86::reg32 thin(const Thinning& t, x86::reg32 colour, x86::reg32 next)
{
    const x86::reg32 node = next ? rc::ld32(next + 4) : 0;
    if (!node || rc::ld32(node) != next || (colour >> 24) <= 0x14)
        return colour;
    if ((node >> 4) * 2654435761u >> 24 >= t.keep)
        return colour & 0x00ffffffu;
    return (colour & 0x00ffffffu) | x86::reg32(t.alpha[colour >> 24]) << 24;
}

template< bool Aligned >
inline void vertex(x86::reg32 v, Real x, Real y, Real z, x86::reg32 colour, x86::reg32 s, x86::reg32 t)
{
    if (Aligned)
    {
        rc::stf32a(v, float(x));
        rc::stf32a(v + 4, float(y));
        rc::stf32a(v + 8, float(z));
    }
    else
    {
        rc::stf32(v, float(x));
        rc::stf32(v + 4, float(y));
        rc::stf32(v + 8, float(z));
    }
    rc::st32(v + 0xc, colour);
    rc::st32(v + 0x10, s);
    rc::st32(v + 0x14, t);
}

template< bool Aligned >
inline void corners(x86::reg32 v, const Real p[4][3], x86::reg32 colour, const Quad& q)
{
    vertex< Aligned >(v, p[0][0], p[0][1], p[0][2], colour, q.s0, q.t0);
    vertex< Aligned >(v + 0x18, p[1][0], p[1][1], p[1][2], colour, q.s1, q.t0);
    vertex< Aligned >(v + 0x30, p[2][0], p[2][1], p[2][2], colour, q.s1, q.t1);
    vertex< Aligned >(v + 0x48, p[3][0], p[3][1], p[3][2], colour, q.s0, q.t1);
}

// One quad into the open batch; returns the guest's ecx.
template< typename Next >
inline x86::reg32 append(const Quad& q, const Thinning& t, Next&& next)
{
    x86::reg32 colour = ftol2(q.alpha) << 8;
    colour = (colour | ftol2(q.red)) << 8;
    colour = (colour | ftol2(q.green)) << 8;
    colour |= ftol2(q.blue);
    if (t.keep < 256)
        colour = thin(t, colour, next());

    x86::reg32 ecx = 0x872b98;
    if (rc::ld8(0x872b98) != 0)
    {
        const x86::reg32 cursor = rc::ld32(0x86b7c8);
        ecx = cursor;
        if (cursor != 0 && cursor < rc::ld32(0x86b7cc))
        {
            if ((colour & 0xff000000u) <= 0x14000000u)
            {
                for (x86::reg32 v = cursor; v != cursor + 0x60; v += 0x18)
                {
                    rc::st32(v, 0);
                    rc::st32(v + 4, 0);
                    rc::st32(v + 8, 0);
                }
            }
            else
            {
                const Real c[3] = {q.x, q.y, q.z};
                Real a[3], b[3];
                for (int i = 0; i < 3; ++i)
                {
                    a[i] = mul(Real(rc::ldf32a(0x873370 + 4 * i)), q.width);
                    b[i] = mul(Real(rc::ldf32a(0x8763c0 + 4 * i)), q.height);
                }
                Real p[4][3];
                for (int i = 0; i < 3; ++i)
                {
                    const Real left = sub(c[i], a[i]);
                    p[0][i] = add(add(a[i], b[i]), c[i]);
                    p[1][i] = add(left, b[i]);
                    p[2][i] = sub(left, b[i]);
                    p[3][i] = sub(add(a[i], c[i]), b[i]);
                }
                if ((cursor & 3) == 0) [[likely]]
                    corners< true >(cursor, p, colour, q);
                else
                    corners< false >(cursor, p, colour, q);
            }
            ecx = cursor + 0x60;
            rc::st32(0x86b7c8, ecx);
        }
    }
    rc::st32(0x870750, rc::ld32(0x870750) + 1);
    return ecx;
}

}}}

#endif /* !UG2_NATIVE_PARTICLE_BATCH_H_ */
