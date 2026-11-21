#ifndef UG2_GARAGE_STICK_H_
#define UG2_GARAGE_STICK_H_

// Right stick in the front end: the garage screen's car orbit events, and menu navigation that ignores the stick.
// Approach from xan1242's NFSU-XtendedInput; none of its code.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ug2 { namespace garage_stick {

const uint32_t OrbitLeftRight = 0x48, OrbitUpDown = 0x49;   // joy events the garage screen listens for
const uint32_t MenuUpAlternate = 0x2E, MenuDownAlternate = 0x2F;
const uint8_t  Centre = 0x80;
const int      DefaultDeadZonePercent = 30;
const float    MenuStickDeadZone = 0.05f;   // below the pad's own dead zone

struct OrbitBytes
{
    bool    held;
    uint8_t leftRight, upDown;
};

// Dead zone as a fraction, from the cfg percentage.
inline float deadZone(int percent)
{
    return float(std::min(std::max(percent, 0), 99)) / 100.0f;
}

// Event value the game reads as -(byte - 128) / 128.
inline uint8_t orbitByte(float deflection)
{
    return uint8_t(int(Centre) - int(std::lround(std::min(std::max(deflection, -1.0f), 1.0f) * 127.0f)));
}

// Orbit event values; centred inside the dead zone.
inline OrbitBytes orbitBytes(float x, float y, float dead)
{
    const float length = std::hypot(x, y);
    if (length <= dead)
        return { false, Centre, Centre };
    const float scale = std::min((length - dead) / (1.0f - dead), 1.0f) / length;
    return { true, orbitByte(x * scale), orbitByte(y * scale) };
}

// Pedal byte the menu up/down scanner should see.
inline uint8_t menuPedalByte(uint8_t pedal, float rightStickY)
{
    return std::fabs(rightStickY) > MenuStickDeadZone ? Centre : pedal;
}

}}

#endif /* !UG2_GARAGE_STICK_H_ */
