#ifndef UG2_NATIVE_ANIM_H_
#define UG2_NATIVE_ANIM_H_

// Animation time to track index (sub_6e4900), shared by the keyframe natives.

#include <native/common.h>

namespace ug2 { namespace native { namespace anim {

// sub_6e4900: time -/+ 0.5 through float to integer.
inline x86::reg32 timeIndex(x86::CPU& cpu, float time, float* adjusted = nullptr)
{
    Real value = Real(time);
    const double a = value;
    const double b = Real(rc::ldf32(0x7d7d3c));
    cpu.fpu.compare(a, b);
    const Real half = Real(rc::ldf32(0x7c1a3c));
    if (cpu.fpu.status.word & 0x100)
        value += half;
    else
        value -= half;
    value = Real(float(value));
    if (adjusted)
        *adjusted = float(value);
    return x86::reg32(x86::fistInteger<x86::sreg32>(x86::Float(x86::roundIntegral(double(value), cpu.fpu.control.rc))));
}

}}}

#endif /* !UG2_NATIVE_ANIM_H_ */
