#ifndef UG2_NATIVE_SHADOW_H_
#define UG2_NATIVE_SHADOW_H_

// Shadow compare of a native function against its translated guest body (native_shadow=on, native_shadow_rate=N).
// Built only with the cmake option NFSU_NATIVE_SHADOW; otherwise call() runs the native body.
// Side effects beyond registers and the declared spans are compared only as D3D bridge calls (Spec::bridge).

#include <native/common.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <platform/switches.h>

#include <vector>

namespace ug2 { namespace native { namespace shadow {

enum Register : x86::reg8 { Eax = 1, Ecx = 2, Edx = 4, Ebx = 8, Esp = 16, Ebp = 32, Esi = 64, Edi = 128 };

// A guest memory range the function may write; Floats are compared within maxUlps per float.
// matmulN != 0: a 4x4 row-major product of the matrices at matmulA and matmulB, each element within matmulN * 2^-24 * S
// (S = sum of the absolute products) of the guest, exact when S is 0 or a value is not finite.
struct Span
{
    x86::reg32 address;
    x86::reg32 size;
    bool floats;
    uint32_t matmulN = 0;
    x86::reg32 matmulA = 0, matmulB = 0;
};

typedef std::vector<Span> Spans;
typedef void (*Body)(win32::WinApplication*, x86::CPU&);
typedef void (*Declare)(x86::CPU& entry, Spans& spans);

struct Spec
{
    const char* name;
    x86::reg8 registers;    // registers compared (the caller-read set of ug2_layout.h); the x87 depth always is
    uint32_t maxUlps;       // largest float difference (in ulps) accepted in a float span or st0
    bool compareSt0;        // st0 is a result
    bool bridge;            // calls the D3D bridge: the guest run records the calls, the native run is compared to them
};

#ifdef NFSU_NATIVE_SHADOW

// One sampled call is compared; the guest result stays in place, and with Spec::bridge the guest run is the only one
// that reaches the renderer. Other calls run the native body only.
void compare(const Spec& spec, win32::WinApplication* app, x86::CPU& cpu, Body native, Body guest, Declare declare);

inline bool enabled()
{
    return NFSU_DEV_BOOL_CACHED("native_shadow", false);
}

inline void call(const Spec& spec, win32::WinApplication* app, x86::CPU& cpu, Body native, Body guest, Declare declare)
{
    if (!enabled()) [[likely]]
        native(app, cpu);
    else
        compare(spec, app, cpu, native, guest, declare);
}

#else

// Built without NFSU_NATIVE_SHADOW (cmake option): the native body only.
inline void call(const Spec&, win32::WinApplication* app, x86::CPU& cpu, Body native, Body, Declare)
{
    native(app, cpu);
}

#endif

}}}

#endif /* !UG2_NATIVE_SHADOW_H_ */
