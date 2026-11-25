#ifndef UG2_HELPICONS_H_
#define UG2_HELPICONS_H_

#include <cstdint>
#include <vector>

// The front-end button icons packed as a texture pack the game's chunk loader accepts.

namespace ug2 { namespace helpicons {

std::vector<uint8_t> buildPack();
// The same atlas with every colour white, so draws can tint it.
std::vector<uint8_t> buildTintPack();

}}

#endif /* !UG2_HELPICONS_H_ */
