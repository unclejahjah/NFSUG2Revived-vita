// Garage camera orbit (sub_4a5d90): the right stick past its dead zone stands in for a middle-button mouse drag.

#include <native/common.h>
#include <lib/gamepad.h>
#include <platform/cached.h>
#include <platform/config.h>

#include <algorithm>
#include <cmath>

namespace ug2
{

void sub_4a5d90_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

const x86::reg32 MouseMiddle = 0x008763F7, MouseDx = 0x008763E4, MouseDy = 0x008763E8;

struct GarageStick
{
    bool  enabled;
    float deadZone;     // 0..1 of full deflection
    float speed;        // mouse counts per frame at full deflection
};

// Read again when the options menu changes a setting (pf::Config::generation).
const GarageStick& settings()
{
    static GarageStick s;
    static unsigned s_generation = ~0u;
    if (s_generation != pf::Config::generation())
    {
        s_generation = pf::Config::generation();
        s.enabled = pf::Config::getBool("garage_stick", true);
        s.deadZone = std::min(std::max(pf::Config::getInt("garage_stick_deadzone", 20), 0), 99) / 100.0f;
        s.speed = float(pf::Config::getInt("garage_stick_speed", 4));
    }
    return s;
}

// One camera step as a middle-button drag of (moveX, moveY).
void dragStep(win32::WinApplication* app, x86::CPU& cpu, long moveX, long moveY)
{
    const x86::reg32 middle = rc::ld8(MouseMiddle), dx = rc::ld32(MouseDx), dy = rc::ld32(MouseDy);
    // The camera reads the mouse state only inside this call, so the real values are put back after it.
    rc::st8(MouseMiddle, 1);
    rc::st32(MouseDx, x86::reg32(x86::sreg32(moveX)));
    rc::st32(MouseDy, x86::reg32(x86::sreg32(moveY)));
    sub_4a5d90_guest(app, cpu);
    rc::st8(MouseMiddle, middle);
    rc::st32(MouseDx, dx);
    rc::st32(MouseDy, dy);
}

}

void sub_4a5d90(win32::WinApplication* app, x86::CPU& cpu)
{
    const GarageStick& g = settings();
    float x = 0.0f, y = 0.0f;
    if (g.enabled)
        win32::Gamepad::getRightStick(&x, &y);
    const float length = std::hypot(x, y);
    static float s_carry[2];
    static bool s_dragging;
    if (length <= g.deadZone)
    {
        s_carry[0] = s_carry[1] = 0.0f;
        // The camera keeps its last orbit rate after release, so end on a still frame as a mouse would.
        if (s_dragging)
            dragStep(app, cpu, 0, 0);
        else
            sub_4a5d90_guest(app, cpu);
        s_dragging = false;
        return;
    }
    const float push = std::min((length - g.deadZone) / (1.0f - g.deadZone), 1.0f) * g.speed / length;
    const float wantX = x * push + s_carry[0], wantY = y * push + s_carry[1];
    const long moveX = std::lround(wantX), moveY = std::lround(wantY);
    s_carry[0] = wantX - float(moveX);
    s_carry[1] = wantY - float(moveY);
    dragStep(app, cpu, moveX, moveY);
    s_dragging = true;
}

}

}
