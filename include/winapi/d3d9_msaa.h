#ifndef WINAPI_D3D9_MSAA_H_
#define WINAPI_D3D9_MSAA_H_

// Multisampling the D3D layer offers: NONMASKABLE with 3 quality levels, 0 and 1 taken as 2x, 2 as 4x.

#include <cstdint>

namespace win32 { namespace d3d9 {

const uint32_t MultisampleNone = 0, MultisampleNonMaskable = 1;
const uint32_t NonMaskableQualities = 3;

// Renderer samples for a present parameters request.
inline uint32_t requestedSamples(uint32_t type, uint32_t quality)
{
    if (type == MultisampleNonMaskable)
        return quality >= 2 ? 4 : 2;
    return type >= 4 ? 4 : type >= 2 ? 2 : 1;
}

}}

#endif /* !WINAPI_D3D9_MSAA_H_ */
