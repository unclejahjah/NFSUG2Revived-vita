#ifndef UG2_FREELOOK_MATH_H_
#define UG2_FREELOOK_MATH_H_

// Free look maths: stick direction to view yaw, the front-half swing, and turning the chase camera rig like Look Back.

#include <lib/controls.h>
#include <cmath>
#include <cstdint>

namespace ug2 { namespace freelook_math {

const float RearDegrees = 90.0f;        // aims this far round or more snap; nearer ahead they swing
const int   DefaultSwingSpeed = 180;    // degrees a second

struct Vec
{
    float x, y, z;
};

// Stick direction as a view yaw.
struct Aim
{
    bool  held;     // outside the dead zone
    float yaw;      // degrees clockwise from ahead: 90 right, 180 behind, -90 left
};

// Degrees into -180..180; 180 stays 180.
inline float wrap(float degrees)
{
    float r = std::fmod(degrees, 360.0f);
    if (r > 180.0f)
        r -= 360.0f;
    else if (r <= -180.0f)
        r += 360.0f;
    return r;
}

// Shaped stick (dead zone already applied): x right, y down.
inline Aim aim(float x, float y)
{
    if (!(std::hypot(x, y) > 0.0f))
        return { false, 0.0f };
    const float yaw = float(std::atan2(double(x), double(-y)) * (180.0 / 3.14159265358979323846));
    return { true, yaw };
}

// The race state free look needs besides the controls.
struct Race
{
    bool chaseCamera;   // the director's active player chase mover
    bool replay;
};

// Live only while driving with the right stick unbound.
inline bool live(const Race& race)
{
    return race.chaseCamera && !race.replay && win32::controls::driving() &&
           win32::controls::rightStickUnbound();
}

// The yaw free look shows.
struct View
{
    bool  on;       // free look turns the camera
    float yaw;      // degrees right of ahead
};

// Moves current toward target the short way, at most step.
inline float approach(float current, float target, float step)
{
    const float d = wrap(target - current);
    if (std::fabs(d) <= step)
        return target;
    return wrap(current + (d > 0 ? step : -step));
}

// Next view: rear aims snap, front aims swing at speed.
// Released, the view swings back to ahead when releaseSwing, or (easeBackBelow > 0) when it was turned less than
// easeBackBelow degrees; otherwise it cuts straight back.
inline View next(const View& view, const Aim& aim, float dt, float speed, bool releaseSwing, float easeBackBelow = 0.0f)
{
    const float step = speed > 0 ? speed * dt : 360.0f;
    if (aim.held)
    {
        if (std::fabs(aim.yaw) >= RearDegrees)
            return { true, aim.yaw };
        return { true, approach(view.on ? view.yaw : 0.0f, aim.yaw, step) };
    }
    const bool swingBack = releaseSwing || (easeBackBelow > 0.0f && std::fabs(view.yaw) < easeBackBelow);
    if (!swingBack || !view.on)
        return { false, 0.0f };
    const float yaw = approach(view.yaw, 0.0f, step);
    return { yaw != 0.0f, yaw };
}

// Turn giving yaw, after the game's own Look Back mirror (or not).
inline float residual(float yaw, bool mirrored)
{
    return mirrored ? wrap(yaw - 180.0f) : wrap(yaw);
}

// esp at the update's look-at call (0x453b44), from the update's entry esp.
inline uint32_t lookAtEsp(uint32_t entryEsp)
{
    const uint32_t frame = ((entryEsp - 4) & ~uint32_t(0xF)) - 0x1A4 - 0xC;
    return frame - 0x10 - 4;
}

inline float dot(const Vec& a, const Vec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec cross(const Vec& a, const Vec& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline Vec row(const float* m, int r) { return { m[4 * r], m[4 * r + 1], m[4 * r + 2] }; }

// Turns p about m's car-space z axis, x toward y.
inline Vec spin(const float* m, const Vec& p, float cosA, float sinA)
{
    const Vec r0 = row(m, 0), r1 = row(m, 1), r2 = row(m, 2), o = row(m, 3);
    const Vec d = { p.x - o.x, p.y - o.y, p.z - o.z };
    const float lx = dot(d, r0) / dot(r0, r0), ly = dot(d, r1) / dot(r1, r1), lz = dot(d, r2) / dot(r2, r2);
    const float nx = lx * cosA - ly * sinA, ny = lx * sinA + ly * cosA;
    return { o.x + nx * r0.x + ny * r1.x + lz * r2.x, o.y + nx * r0.y + ny * r1.y + lz * r2.y,
             o.z + nx * r0.z + ny * r1.z + lz * r2.z };
}

// +1 when an x-toward-y turn moves the view right.
inline float rightTurnSign(const float* m, const Vec& forward, const Vec& up)
{
    const Vec r0 = row(m, 0), r1 = row(m, 1);
    const float fx = dot(forward, r0) / dot(r0, r0), fy = dot(forward, r1) / dot(r1, r1);
    const Vec turn = { -fy * r0.x + fx * r1.x, -fy * r0.y + fx * r1.y, -fy * r0.z + fx * r1.z };
    return dot(turn, cross(forward, up)) >= 0.0f ? 1.0f : -1.0f;
}

// Turn of yaw degrees right; exact at 0 and 180.
inline void turn(float yaw, float sign, float* cosA, float* sinA)
{
    if (yaw == 0.0f)
    {
        *cosA = 1.0f;
        *sinA = 0.0f;
        return;
    }
    if (std::fabs(yaw) == 180.0f)
    {
        *cosA = -1.0f;
        *sinA = 0.0f;
        return;
    }
    const double a = double(yaw) * (3.14159265358979323846 / 180.0);
    *cosA = float(std::cos(a));
    *sinA = float(std::sin(a)) * sign;
}

}}

#endif /* !UG2_FREELOOK_MATH_H_ */
