#ifndef UG2_NATIVE_MATRIX4_H_
#define UG2_NATIVE_MATRIX4_H_

// Float 4x4 matrix multiply in the order of the guest's x87 chain (sub_4013f0), shared with natives that inline it.

#include <native/common.h>

#include <utility>

#if defined(__ARM_NEON) && !defined(NFSU_FPU_DOUBLE)
#include <arm_neon.h>
#endif

namespace ug2 { namespace native { namespace matrix4 {

// Order of the k terms of each output element in the x87 chain.
inline constexpr uint8_t Order[16][4] = {
    {0, 3, 2, 1}, {3, 1, 2, 0}, {2, 0, 3, 1}, {3, 2, 0, 1},
    {2, 3, 1, 0}, {2, 0, 1, 3}, {3, 2, 1, 0}, {3, 2, 0, 1},
    {2, 3, 1, 0}, {2, 0, 1, 3}, {3, 2, 1, 0}, {3, 2, 0, 1},
    {2, 3, 1, 0}, {2, 0, 1, 3}, {3, 2, 1, 0}, {3, 2, 0, 1},
};

template <int E>
inline float element(const float* a, const float* b)
{
    constexpr int row = E >> 2, column = E & 3;
    const float p0 = a[4 * row + Order[E][0]] * b[4 * Order[E][0] + column];
    const float p1 = a[4 * row + Order[E][1]] * b[4 * Order[E][1] + column];
    const float p2 = a[4 * row + Order[E][2]] * b[4 * Order[E][2] + column];
    const float p3 = a[4 * row + Order[E][3]] * b[4 * Order[E][3] + column];
    float sum = p0 + p1;
    sum = sum + p2;
    sum = sum + p3;
    return sum;
}

template <int... E>
inline void elements(const float* a, const float* b, float* out, std::integer_sequence<int, E...>)
{
    ((out[E] = element<E>(a, b)), ...);
}

// Reads all inputs first: out may overlap A or B.
inline void multiplyCopied(const uint8_t* pa, const uint8_t* pb, uint8_t* out)
{
    float a[16], b[16], product[16];
    memcpy(a, pa, sizeof(a));
    memcpy(b, pb, sizeof(b));
    elements(a, b, product, std::make_integer_sequence<int, 16>());
    memcpy(out, product, sizeof(product));
}

#if defined(__ARM_NEON) && !defined(NFSU_FPU_DOUBLE)

// Scalar VFP on NEON lanes: exact whatever the FPSCR.
template <int E>
inline float element(float32x4_t a, const float32x4_t* b)
{
    constexpr int column = E & 3;
    const float p0 = vgetq_lane_f32(a, Order[E][0]) * vgetq_lane_f32(b[Order[E][0]], column);
    const float p1 = vgetq_lane_f32(a, Order[E][1]) * vgetq_lane_f32(b[Order[E][1]], column);
    const float p2 = vgetq_lane_f32(a, Order[E][2]) * vgetq_lane_f32(b[Order[E][2]], column);
    const float p3 = vgetq_lane_f32(a, Order[E][3]) * vgetq_lane_f32(b[Order[E][3]], column);
    float sum = p0 + p1;
    sum = sum + p2;
    sum = sum + p3;
    return sum;
}

// Kept out of line, off the fast path.
__attribute__((noinline)) inline void multiplyOverlapping(const uint8_t* pa, const uint8_t* pb, uint8_t* out)
{
    multiplyCopied(pa, pb, out);
}

template <int R>
inline void row(const uint8_t* pa, const float32x4_t* b, uint8_t* out)
{
    float32x4_t a = vreinterpretq_f32_u8(vld1q_u8(pa + 16 * R));
    // Keeps clang from splitting the vector load into scalar loads.
    __asm__("" : "+w"(a));
    const float32x4_t r = {element<4 * R>(a, b), element<4 * R + 1>(a, b), element<4 * R + 2>(a, b),
                           element<4 * R + 3>(a, b)};
    vst1q_u8(out + 16 * R, vreinterpretq_u8_f32(r));
}

// Rows in order: unsafe only when out starts inside A.
inline void multiply(const uint8_t* pa, const uint8_t* pb, uint8_t* out)
{
    if (uintptr_t(out) - uintptr_t(pa) - 1 < 63)
    {
        multiplyOverlapping(pa, pb, out);
        return;
    }
    float32x4_t b[4];
    for (int i = 0; i < 4; ++i)
        b[i] = vreinterpretq_f32_u8(vld1q_u8(pb + 16 * i));
    // Keeps clang from splitting the vector loads into scalar loads.
    __asm__("" : "+w"(b[0]), "+w"(b[1]), "+w"(b[2]), "+w"(b[3]));
    row<0>(pa, b, out);
    row<1>(pa, b, out);
    row<2>(pa, b, out);
    row<3>(pa, b, out);
}

#else

inline void multiply(const uint8_t* pa, const uint8_t* pb, uint8_t* out)
{
    multiplyCopied(pa, pb, out);
}

#endif

}}}

#endif /* !UG2_NATIVE_MATRIX4_H_ */
