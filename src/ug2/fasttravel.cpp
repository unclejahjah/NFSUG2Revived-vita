// Fast travel from the World Map, as CrabJournal's NFSU2FastTravel PC mod (GPL-3.0) does it on the same v1.2 exe:
// pick a place on the map, Accept, and answer No to "Are you sure you want to engage the GPS system?" to be put
// there. Yes sets the GPS route as before and Back (Circle) only closes the question. fast_travel=off leaves the
// game as it was.
//
// The PC mod changes the No message of that question (the immediate at 0x4B113E in the map screen's GPS prompt,
// 0x4B10C0) and hooks the map screen's NotificationMessage (vtable 0x797F20, slot 2 = 0x4E08E0). The translation
// keeps the immediate, so here the question's call to the dialog function 0x558790 has its No argument changed.

#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>

namespace ug2
{

void sub_558790_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4e08e0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

using x86::reg32;

const reg32 GpsQuestionText = 0x63477036;   // "Are you sure you want to engage the GPS system?"
const reg32 GpsYes          = 0x51123592;
const reg32 GpsNo           = 0xB4623F67;   // the game sends the same message for No and Back
const reg32 FastTravelNo    = 0x00000001;   // the PC mod's own No message
const reg32 MapBack         = 0x911AB364;   // the map screen's Back: closes the map
const reg32 WorldMapVtable  = 0x00797F20;   // GameWorldMapScreen
const reg32 CurrentWorld    = 0x00890080;   // World*

bool enabled()
{
    static unsigned s_generation = ~0u;
    static bool s_on = true;
    if (pf::Config::generation() != s_generation)
    {
        s_generation = pf::Config::generation();
        s_on = pf::Config::getBool("fast_travel", true);
    }
    return s_on;
}

// A guest pointer worth following.
bool valid(reg32 p)
{
    return p >= 0x00400000u && p < 0xF0000000u;
}

// Puts the player's car at the zone the map has selected; false when any link of the chain is missing.
bool travel(reg32 screen)
{
    const reg32 element = rc::ld32(screen + 0x4C);            // WorldMapElement*
    const reg32 entry = valid(element) ? rc::ld32(element + 0x14) : 0;
    if (!valid(entry))
        return false;
    const reg32 zone = entry + 0x0C;                          // UnlockedTriggerZone
    const reg32 zoneObject = rc::ld32(zone);
    const reg32 world = rc::ld32(CurrentWorld);
    const reg32 car = valid(world) ? rc::ld32(world + 0x1C) : 0; // cars[0]
    const reg32 mover = valid(car) ? rc::ld32(car + 0x34) : 0;   // PhysicsMover*
    const reg32 body = valid(mover) ? rc::ld32(mover + 0x20) : 0; // RigidBody*
    if (!valid(zoneObject) || !valid(body))
        return false;
    float x = rc::ldf32(zoneObject + 4), y = rc::ldf32(zoneObject + 8);
    if (rc::ld32(zone + 0x0C))                                // a photo shoot: beside it, so it does not start
    {
        x += 8.0f;
        y += 8.0f;
    }
    rc::store<float>(body + 0x20, x);                                // position x, y; height and heading kept
    rc::store<float>(body + 0x24, y);
    for (reg32 i = 0; i < 3; ++i)
        rc::store<float>(body + 0x70 + i * 4, 0.0f);                 // velocity
    pf::logInfo("fast travel: car moved to %.1f, %.1f%s", double(x), double(y),
                rc::ld32(zone + 0x0C) ? " (beside the photo shoot)" : "");
    return true;
}

}

namespace native
{

// The dialog function (cdecl; package, dialog, ..., yes, no, back, buttons, text): the GPS question's No becomes
// the fast-travel message.
void sub_558790(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 esp = cpu.esp;
    if (rc::ld32(esp + 0x24) == GpsQuestionText && rc::ld32(esp + 0x14) == GpsYes && rc::ld32(esp + 0x18) == GpsNo &&
        enabled())
        rc::st32(esp + 0x18, FastTravelNo);
    sub_558790_guest(app, cpu);
}

// GameWorldMapScreen::NotificationMessage (thiscall; hash, object, p3, p4): No moves the car, then the map closes.
void sub_4e08e0(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 screen = cpu.ecx;
    if (rc::ld32(cpu.esp + 4) == FastTravelNo && valid(screen) && rc::ld32(screen) == WorldMapVtable)
    {
        if (!travel(screen))
            pf::logError("fast travel: no place or no car, nothing moved");
        rc::st32(cpu.esp + 4, MapBack);
    }
    sub_4e08e0_guest(app, cpu);
}

}

}
