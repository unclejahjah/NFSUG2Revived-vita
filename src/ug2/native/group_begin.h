#ifndef UG2_NATIVE_GROUP_BEGIN_H_
#define UG2_NATIVE_GROUP_BEGIN_H_

// Steps shared by the draw-group begin natives: effect texture binds, the fog colour, the projective texture matrix.

#include <native/common.h>
#include <native/flush_context.h>
#include <native/texture_lookup.h>
#include <winapi/d3d9_native.h>

namespace ug2 { namespace native { namespace group {

namespace br = win32::d3d9::bridge;

const x86::reg32 Current = 0x870910;
const x86::reg32 Device = 0x870974;
const x86::reg32 CurrentView = 0x870848;

inline void setTexture(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32 texture)
{
    const x86::reg32 current = flush::ld32<Current>(cpu);
    br::effectSetTexture(app, cpu, rc::ld32(current + 0x20), rc::ld32(rc::ld32(current + 0x1c) + handle), texture);
}

// Texture of the info found under the name at 0x7a3658.
inline x86::reg32 nameTexture()
{
    const x86::reg32 info = texture::find(texture::nameHash(0x7a3658), 1, 0);
    return rc::ld32(rc::ld32(info) + 0x18);
}

inline void fogColour(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 device = rc::ld32(Device);
    const Real scale = ld(0x784544);
    const auto channel = [&](x86::reg32 address) { return x86::reg32(x86::truncInteger64(ld(address) * scale)); };
    x86::reg32 colour = channel(0x800520) << 8;
    colour |= channel(0x800524);
    colour <<= 8;
    colour |= channel(0x800528);
    br::setRenderState(app, cpu, device, 0x3c, colour);
}

// View matrix times the 0.5 / -0.5 bias, last column copied over the third.
inline void projectiveMatrix(win32::WinApplication* app, x86::CPU& cpu)
{
    float m[16] = {0.5f, 0, 0, 0, 0, -0.5f, 0, 0, 0, 0, 1, 0, 0.5f, 0.5f, 0, 1};
    const x86::reg32 index = flush::viewIndex(cpu);
    if (x86::reg32(0x832de0 + index * 0x70) == 0)
        return;
    const float* viewMatrix = flush::viewMatrices(cpu, index) + 16;
    br::matrixMultiply(viewMatrix, m, m);
    m[2] = m[3];
    m[6] = m[7];
    m[10] = m[11];
    m[14] = m[15];
    const x86::reg32 current = flush::ld32<Current>(cpu);
    br::effectSetMatrix(app, cpu, rc::ld32(current + 0x20), rc::ld32(rc::ld32(current + 0x1c) + 0x704), m);
}

}}}

#endif /* !UG2_NATIVE_GROUP_BEGIN_H_ */
