// The Headlights action, always on: see headlights.h.

#include "headlights.h"
#include <cstring>

namespace ug2 { namespace headlights {

bool on = true;

namespace
{

int s_mode = 2;
bool s_held = false;

void apply(uint8_t* guestBase)
{
    const float brightness = s_mode == 0 ? 0.0f : s_mode == 1 ? LowBeam : HighBeam;
    memcpy(guestBase + Brightness, &brightness, 4);
    guestBase[FlareName] = s_mode == 0 ? 0 : 'H';
}

}

void reset()
{
    s_mode = 2;
    s_held = false;
}

int mode()
{
    return s_mode;
}

void frame(uint8_t* guestBase, bool pressed, uint32_t gameFlow)
{
    const bool press = pressed && !s_held;
    s_held = pressed;
    if (!press || gameFlow != uint32_t(Gameplay))
        return;
    s_mode = (s_mode + 1) % 3;
    apply(guestBase);
}

}}
