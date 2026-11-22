// The right stick looks round the car while driving (camera_stick). Without the stick nothing changes: the game's
// player camera (CubicCameraMover::Update 0x453570) runs exactly as it does, and the stick only adds a layer on top.
//
// - Chase views (POV 0, 1, 5): the camera turns round the car to the stick's direction (up is the normal view,
//   down looks behind). Released when it was turned less than 120 degrees, it eases back behind the car; at 120 or
//   more it cuts straight back, so the road ahead is in view at once. The turn is made to the eye the game passes to
//   its look-at (0x5ba9a0) inside the update, so the game's own wall pass (0x444530) and traffic pass (0x44b040)
//   that follow apply to the turned eye: a wall beside the car pulls the camera in.
// - Bumper and hood: no turning; the stick's lower half is the game's Look Back (mover +0x51), as the button.
//
// The mover's fields (+0x4C POV in use, +0x51 Look Back) and the look-at's arguments are from the v1.2 exe; the
// orbit and its two ways back follow a request by Jah (the same design as elmnt22's vita-nfsu2 port, none of its
// code).

#include <lib/gamepad.h>
#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/time.h>
#include <recomp.h>
#include <cmath>

namespace ug2
{

void sub_453570_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5ba9a0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

using x86::reg32;

const float Pi = 3.14159265f;
const float DeadZone = 0.3f;                    // stick length below which it is released
const float CutBackAngle = 120.0f * Pi / 180.0f;
// 1/s at camera_stick_speed 10 (the first version's, too fast to Jah): how fast the camera follows the stick's angle,
// and the eased way back; speed n of 10 scales both by n / 10
const float FollowRateMax = 14.0f;
const float ReturnRateMax = 6.0f;
float s_followRate = FollowRateMax * 0.4f;
float s_returnRate = ReturnRateMax * 0.4f;
const reg32 PovOffset = 0x4C, LookBackOffset = 0x51;

struct Orbit
{
    float yaw = 0.0f;           // radians, 0 = the game's camera
    float target = 0.0f;        // the stick's angle while held
    bool held = false;
    bool lookBack = false;      // we set the mover's Look Back
    uint64_t lastUs = 0;
};

Orbit s_orbit;
bool s_inUpdate = false;        // inside 0x453570, for its look-at call

bool enabled()
{
    static unsigned s_generation = ~0u;
    static bool s_on = true;
    if (pf::Config::generation() != s_generation)
    {
        s_generation = pf::Config::generation();
        s_on = pf::Config::getBool("camera_stick", true);
        int speed = pf::Config::getInt("camera_stick_speed", 4);
        speed = speed < 1 ? 1 : speed > 10 ? 10 : speed;
        s_followRate = FollowRateMax * float(speed) / 10.0f;
        s_returnRate = ReturnRateMax * float(speed) / 10.0f;
    }
    return s_on;
}

float wrap(float a)
{
    while (a > Pi)
        a -= 2.0f * Pi;
    while (a < -Pi)
        a += 2.0f * Pi;
    return a;
}

bool chaseView(reg32 mover)
{
    const reg32 pov = rc::ld32(mover + PovOffset);
    return pov == 0 || pov == 1 || pov == 5;
}

// One frame of the stick, before the game's update.
void step(reg32 mover)
{
    const uint64_t now = pf::ticksUs();
    float dt = s_orbit.lastUs ? float(now - s_orbit.lastUs) / 1e6f : 1.0f / 30.0f;
    if (dt <= 0.0f || dt > 0.25f)
        dt = 1.0f / 30.0f;
    s_orbit.lastUs = now;
    float x = 0.0f, y = 0.0f;                   // right, down
    win32::Gamepad::getRightStick(&x, &y);
    const bool out = std::hypot(x, y) > DeadZone;

    if (!chaseView(mover))
    {
        // Bumper and hood: the lower half is Look Back; ours ends when the stick leaves it.
        s_orbit.yaw = 0.0f;
        s_orbit.held = false;
        const bool back = out && y > 0.0f;
        if (back)
            rc::st8(mover + LookBackOffset, 1);
        else if (s_orbit.lookBack)
            rc::st8(mover + LookBackOffset, 0);
        s_orbit.lookBack = back;
        return;
    }
    if (s_orbit.lookBack)
    {
        rc::st8(mover + LookBackOffset, 0);
        s_orbit.lookBack = false;
    }
    if (out)
    {
        // Up 0, right -90 degrees (the eye moves to the car's left and looks right), down 180.
        s_orbit.target = std::atan2(-x, -y);
        s_orbit.yaw = wrap(s_orbit.yaw + wrap(s_orbit.target - s_orbit.yaw) * (1.0f - std::exp(-s_followRate * dt)));
        s_orbit.held = true;
        return;
    }
    if (s_orbit.held && std::fabs(s_orbit.target) >= CutBackAngle)
        s_orbit.yaw = 0.0f;                     // released behind the car: cut back to the normal view
    s_orbit.held = false;
    s_orbit.yaw -= s_orbit.yaw * (1.0f - std::exp(-s_returnRate * dt));
    if (std::fabs(s_orbit.yaw) < 1e-3f)
        s_orbit.yaw = 0.0f;
}

}

namespace native
{

// CubicCameraMover::Update (thiscall): the stick's frame, then the game's update.
void sub_453570(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 mover = cpu.ecx;
    if (!enabled())
    {
        if (s_orbit.lookBack)
            rc::st8(mover + LookBackOffset, 0);
        s_orbit = Orbit();
        sub_453570_guest(app, cpu);
        return;
    }
    step(mover);
    s_inUpdate = s_orbit.yaw != 0.0f && chaseView(mover) && !rc::ld8(mover + LookBackOffset);
    sub_453570_guest(app, cpu);
    s_inUpdate = false;
}

// Look-at (cdecl; matrix out, eye, target, up): in the turned chase view the eye goes round the target.
void sub_5ba9a0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (s_inUpdate)
    {
        s_inUpdate = false;                     // the update's one look-at
        const reg32 eye = rc::ld32(cpu.esp + 8), target = rc::ld32(cpu.esp + 12), up = rc::ld32(cpu.esp + 16);
        const float tx = rc::ldf32(target), ty = rc::ldf32(target + 4);
        const float dx = rc::ldf32(eye) - tx, dy = rc::ldf32(eye + 4) - ty;
        const float c = std::cos(s_orbit.yaw), s = std::sin(s_orbit.yaw);
        rc::store<float>(eye, tx + dx * c - dy * s);
        rc::store<float>(eye + 4, ty + dx * s + dy * c);
        rc::store<float>(up, 0.0f);             // level while turned
        rc::store<float>(up + 4, 0.0f);
        rc::store<float>(up + 8, 1.0f);
    }
    sub_5ba9a0_guest(app, cpu);
}

}

}
