#ifndef UG2_HEADLIGHTS_H_
#define UG2_HEADLIGHTS_H_

// The Headlights action, always on: a press while driving cycles off, low beam and high beam as Extra Options 5.1.0.1340's
// Headlights hotkey does. Not saved.

#include <cstdint>

namespace ug2 { namespace headlights {

extern bool on;     // the generated code's value switch at 0x61594C: always true

const uint32_t Brightness = 0x615950;           // the immediate of 0x61594C
const uint32_t FlareName = 0x7a3658;            // first byte of the flare texture name
const float LowBeam = 0.75f, HighBeam = 1.0f;   // Extra Options' shipped LowBeamBrightness, HighBeamBrightness
const uint32_t GameFlow = 0x8654a4;             // 6 while racing or driving
const int Gameplay = 6;

// Back to high beam, the game's own state; no guest write.
void reset();
// 0 off, 1 low beam, 2 high beam (the game's own).
int mode();
// One frame: a new press while driving cycles the mode. pressed: the action is held now.
void frame(uint8_t* guestBase, bool pressed, uint32_t gameFlow);

}}

#endif
