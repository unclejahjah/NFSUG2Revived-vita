#ifndef UG2_NATIVE_FAST_H_
#define UG2_NATIVE_FAST_H_

// Fast paths shared by the Medium natives: a cheap config switch, and float accesses that become vldr/vstr when
// the caller has checked the addresses are 4-aligned (they fault on unaligned ones).

#include <native/common.h>
#include <platform/config.h>

namespace ug2 { namespace native { namespace fast {

#ifdef DEV_SETTINGS
[[gnu::noinline, gnu::cold]] inline bool lookup(signed char& state, const char* key)
{
    if (state == 0)
        state = pf::Config::getBool(key, true) ? 1 : -1;
    return state > 0;
}

// True when the key is on; state caches the answer.
inline bool enabled(signed char& state, const char* key)
{
    if (state > 0) [[likely]]
        return true;
    return lookup(state, key);
}
#else
inline bool enabled(signed char&, const char*) { return true; }
#endif

typedef float __attribute__((may_alias)) AlignedFloat;
typedef float __attribute__((may_alias, aligned(1))) LooseFloat;

template< bool A >
inline Real lf(x86::reg32 address)
{
    const x86::reg8* p = win32::g_guestBase + address;
    return Real(A ? *reinterpret_cast< const AlignedFloat* >(p) : *reinterpret_cast< const LooseFloat* >(p));
}

template< bool A >
inline void sf(x86::reg32 address, float value)
{
    x86::reg8* p = win32::g_guestBase + address;
    if (A)
        *reinterpret_cast< AlignedFloat* >(p) = value;
    else
        *reinterpret_cast< LooseFloat* >(p) = value;
}

inline bool aligned(x86::reg32 addresses) { return (addresses & 3) == 0; }

// Point (3 floats at p) times the 4x4 matrix at m, in the guest's order of additions.
template< bool A >
inline void transform(x86::reg32 m, x86::reg32 p, float& ox, float& oy, float& oz)
{
    const Real px = lf< A >(p), py = lf< A >(p + 4), pz = lf< A >(p + 8);
    Real x = lf< A >(m + 0x20) * pz;
    x += lf< A >(m + 0x10) * py;
    x += lf< A >(m) * px;
    x += lf< A >(m + 0x30);
    Real y = lf< A >(m + 0x24) * pz;
    y += lf< A >(m + 4) * px;
    y += lf< A >(m + 0x14) * py;
    y += lf< A >(m + 0x34);
    Real z = lf< A >(m + 0x28) * pz;
    z += lf< A >(m + 8) * px;
    z += lf< A >(m + 0x18) * py;
    z += lf< A >(m + 0x38);
    ox = float(x);
    oy = float(y);
    oz = float(z);
}

}}}

#endif /* !UG2_NATIVE_FAST_H_ */
