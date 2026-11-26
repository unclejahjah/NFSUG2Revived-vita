#ifndef UG2_CONTROLSPACK_H_
#define UG2_CONTROLSPACK_H_

#include <cstddef>
#include <cstdint>
#include <vector>

// Packs one DXT3 picture (256x256 unless given) as a texture pack the game's chunk loader accepts.

namespace ug2 { namespace controlspack {

// The game's hash of an upper case texture name.
uint32_t nameHash(const char* name);

// The pack is named after the texture; the picture is width x height texels (multiples of 4, powers of two).
std::vector<uint8_t> buildPack(const char* textureName, const uint8_t* dxt3, size_t bytes, int width = 256, int height = 256);

}}

#endif /* !UG2_CONTROLSPACK_H_ */
