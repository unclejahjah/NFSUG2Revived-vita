// Free look: while driving with the right stick unbound, its direction turns the chase camera rig about the car
// (Look Back's turn at 180); aims ahead swing, rear aims snap.

#include "freelook.h"
#include "freelook_math.h"
#include "frontend.h"

#include <lib/controls.h>
#include <lib/gamepad.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/settings.h>
#include <recomp.h>

#include <algorithm>
#include <cmath>

namespace ug2 { namespace freelook {

namespace
{

using x86::reg32;
namespace fm = freelook_math;

const reg32 RaceCoordinator = 0x008900D8;   // pointer; its first dword is 6 while the race is paused
const reg32 OptionsExit = 0x008384DC;       // byte
const reg32 ReplayState = 0x0085EF6C;       // byte; the game ignores Look Back while it is set (0x6033c7)
const reg32 FrameStep = 0x00865198;         // float: the frame's time step in seconds

// Chase camera mover (vtable 0x787f28, update 0x453570) and the camera director that creates it (0x5f1fd0).
const reg32 MoverType = 0x0C;               // 1: the player's chase camera, the one Look Back reaches
const reg32 MoverDirector = 0x20;
const reg32 MoverNoMirror = 0x30;           // nonzero: Look Back keeps the rig unmirrored
const reg32 MoverView = 0x4C;
const reg32 MoverLookBack = 0x51;           // byte: the game's Look Back mirrors the rig
const reg32 DirectorMirrors = 0x2B;         // byte; zero keeps the rig unmirrored
const reg32 DirectorMover = 0x238;          // the active mover

// The update's look-at call (0x453b44): its frame from the callee's esp.
const reg32 FrameFrom = 0x54, FrameTo = 0x44, FrameRig = 0x184;     // eye, target, rig matrix (car space to world)

const uint32_t StaleFrames = 4;

struct State
{
    reg32       camera = 0;
    uint32_t    frame = 0;          // of the camera's last update
    bool        live = false;
    bool        held = false;       // the stick is out of its dead zone
    bool        on = false;         // free look turns the camera
    float       yaw = 0;            // view yaw shown, degrees right of ahead
    float       aim = 0;            // the stick's yaw
    float       stickX = 0, stickY = 0;     // shaped right stick
    uint32_t    stepFrame = 0;      // frame of the last swing step
    uint32_t    logFrame = 0;
    bool        turned = false;     // the look-at hook has turned this camera
    const char* declined = nullptr; // the reason last logged
};

State s_state;

int deadZone()
{
    static const int percent = pf::Config::getInt("free_look_deadzone", pf::FreeLookDeadZoneDefault);
    return percent;
}
Update s_update = {};

struct Settings
{
    float swing;            // degrees a second, 0 snaps
    bool  releaseSwing;
    bool  easeBack;         // free_look_ease_back (fork): released under EaseBackDegrees round, it swings back
};

const float EaseBackDegrees = 120.0f;

// Read again when Vita Settings changes one (pf::Config::generation).
const Settings& settings()
{
    static Settings s;
    static unsigned s_generation = ~0u;
    if (s_generation != pf::Config::generation())
    {
        s_generation = pf::Config::generation();
        s.swing = float(std::min(std::max(pf::Config::getInt("free_look_swing", fm::DefaultSwingSpeed), 0), 2000));
        s.releaseSwing = pf::Config::getBool("free_look_release_swing", false);
        s.easeBack = pf::Config::getBool("free_look_ease_back", true);
    }
    return s;
}

float frameStep()
{
    const float dt = rc::ldf32(FrameStep);
    return dt > 0.0f ? std::min(dt, 0.1f) : 0.0f;
}

bool logging()
{
#ifdef DEV_TOOLS
    return true;
#else
    return false;
#endif
}

float stickDegrees(float x, float y)
{
    return float(std::atan2(double(x), double(-y)) * (180.0 / 3.14159265358979323846));
}

void decline(const char* why, reg32 esp)
{
    if (!logging() || s_state.declined == why)
        return;
    s_state.declined = why;
    pf::logInfo("freelook: look-at not turned: %s (esp %08X, expected %08X)", why, unsigned(esp),
                unsigned(s_update.lookAtEsp));
}

bool paused()
{
    const reg32 coordinator = rc::ld32(RaceCoordinator);
    return (coordinator && rc::ld32(coordinator) == 6) || rc::ld8(OptionsExit) != 0;
}

void release(const char* why)
{
    State& s = s_state;
    if (s.on && logging())
        pf::logInfo("freelook: release (%s): chase view", why);
    s.held = s.on = false;
    s.yaw = s.aim = 0;
}

void setLive(bool live, reg32 camera, const char* why)
{
    if (live != s_state.live && logging())
        pf::logInfo("freelook: %s (%s), camera %08X view %u", live ? "live" : "idle", why, unsigned(camera),
                    unsigned(camera ? rc::ld32(camera + MoverView) : 0));
    s_state.live = live;
}

void update(reg32 camera)
{
    State& s = s_state;
    const uint32_t frame = frontend::frameCounter();
    const reg32 director = rc::ld32(camera + MoverDirector);
    if (!director || rc::ld32(director + DirectorMover) != camera)
    {
        if (camera == s.camera)
        {
            release("camera replaced");
            setLive(false, camera, "camera replaced");
            s.camera = 0;
        }
        return;
    }
    if (camera != s.camera || frame - s.frame > StaleFrames)
    {
        if (camera == s.camera)
            release("no camera update for a while");
        else if (logging() && s.camera)
            pf::logInfo("freelook: camera %08X -> %08X", unsigned(s.camera), unsigned(camera));
        s.held = s.on = s.turned = false;
        s.yaw = s.aim = 0;
        s.camera = camera;
    }
    s.frame = frame;
    if (paused())
    {
        setLive(false, camera, "paused");
        return;
    }
    const fm::Race race = { rc::ld32(camera + MoverType) == 1, rc::ld8(ReplayState) != 0 };
    if (!fm::live(race))
    {
        const char* why = !race.chaseCamera ? "not the race chase camera" : race.replay ? "replay"
                          : !win32::controls::driving() ? "not driving" : "the right stick has a game action";
        release(why);
        setLive(false, camera, why);
        return;
    }
    setLive(true, camera, "driving, right stick unbound");

    float x, y, rx, ry;
    win32::Gamepad::getRightStick(&rx, &ry);
    win32::controls::deadZoneShape(rx, ry, deadZone(), &x, &y);
    s.stickX = x;
    s.stickY = y;
    const fm::Aim aim = fm::aim(x, y);
    const float dt = frame != s.stepFrame ? frameStep() : 0.0f;
    s.stepFrame = frame;
    const Settings& cfg = settings();
    const fm::View view = fm::next({ s.on, s.yaw }, aim, dt, cfg.swing, cfg.releaseSwing,
                                   cfg.easeBack ? EaseBackDegrees : 0.0f);
    if (logging())
    {
        const bool rear = std::fabs(aim.yaw) >= fm::RearDegrees, wasRear = std::fabs(s.aim) >= fm::RearDegrees;
        if (aim.held && (!s.held || rear != wasRear))
            pf::logInfo("freelook: %s: stick %.2f,%.2f at %.1f deg, aim %.1f, %s from %.1f", s.held ? "half" : "press",
                        x, y, stickDegrees(x, y), aim.yaw, rear ? "snap" : "swing", s.yaw);
        else if (aim.held && !rear && s.yaw != s.aim && view.yaw == aim.yaw && aim.yaw == s.aim)
            pf::logInfo("freelook: swing reached %.1f", aim.yaw);
        if (!aim.held && s.held)
            pf::logInfo("freelook: release (stick centred): %s from %.1f", view.on ? "swing back" : "chase view",
                        s.yaw);
        else if (!aim.held && s.on && !view.on)
            pf::logInfo("freelook: release swing done: chase view");
    }
    if (aim.held && !s.held)
        s.declined = nullptr;
    s.held = aim.held;
    s.on = view.on;
    s.yaw = view.yaw;
    s.aim = aim.held ? aim.yaw : 0.0f;
}

}

Update enter(reg32 camera, reg32 esp)
{
    const Update outer = s_update;
    s_update = { camera, fm::lookAtEsp(esp) };
    if (camera)
        update(camera);
    return outer;
}

void leave(const Update& outer)
{
    s_update = outer;
}

void beforeLookAt(win32::WinApplication*, x86::CPU& cpu)
{
    State& s = s_state;
    const reg32 camera = s_update.camera;
    if (!camera || camera != s.camera || !s.live || !s.on)
        return;
    if (cpu.esp != s_update.lookAtEsp)
    {
        decline("another look-at inside the update", cpu.esp);
        return;
    }
    const reg32 from = rc::ld32(cpu.esp + 8), to = rc::ld32(cpu.esp + 12), up = rc::ld32(cpu.esp + 16);
    if (from != cpu.esp + FrameFrom || to != cpu.esp + FrameTo)
    {
        decline("eye or target not in the update's frame", cpu.esp);
        return;
    }
    const reg32 director = rc::ld32(camera + MoverDirector);
    const bool mirrored = rc::ld8(camera + MoverLookBack) && !rc::ld32(camera + MoverNoMirror) &&
                          (!director || rc::ld8(director + DirectorMirrors));
    const float turn = fm::residual(s.yaw, mirrored);
    if (logging() && frontend::frameCounter() - s.logFrame >= 6)
    {
        const float x = s.stickX, y = s.stickY;
        s.logFrame = frontend::frameCounter();
        pf::logInfo("freelook: stick %.2f,%.2f at %.1f deg, aim %.1f, yaw %.1f, game look back %u, turn %.1f", x, y,
                    stickDegrees(x, y), s.aim, s.yaw, unsigned(mirrored), turn);
    }
    if (turn == 0.0f)
    {
        decline(mirrored ? "stick down with Look Back held: the game's own view" : "yaw 0: the chase view", cpu.esp);
        return;
    }
    if (!s.turned && logging())
        pf::logInfo("freelook: first turn of camera %08X: yaw %.1f, turn %.1f", unsigned(camera), s.yaw, turn);
    s.turned = true;
    s.declined = nullptr;

    float m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = rc::ldf32(cpu.esp + FrameRig + 4 * i);
    const fm::Vec eye = { rc::ldf32(from), rc::ldf32(from + 4), rc::ldf32(from + 8) };
    const fm::Vec look = { rc::ldf32(to), rc::ldf32(to + 4), rc::ldf32(to + 8) };
    const fm::Vec upward = { rc::ldf32(up), rc::ldf32(up + 4), rc::ldf32(up + 8) };
    const fm::Vec forward = { look.x - eye.x, look.y - eye.y, look.z - eye.z };
    float cosA, sinA;
    fm::turn(turn, fm::rightTurnSign(m, forward, upward), &cosA, &sinA);
    const fm::Vec newEye = fm::spin(m, eye, cosA, sinA), newLook = fm::spin(m, look, cosA, sinA);
    rc::stf32(from, newEye.x);
    rc::stf32(from + 4, newEye.y);
    rc::stf32(from + 8, newEye.z);
    rc::stf32(to, newLook.x);
    rc::stf32(to + 4, newLook.y);
    rc::stf32(to + 8, newLook.z);
}

}}
