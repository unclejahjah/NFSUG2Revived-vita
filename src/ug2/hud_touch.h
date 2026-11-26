// Free-roam touch zones: fixed Vita screen polygons where the HUD's minimap and top-left ring sit. They work
// whether or not the HUD is drawn.
#ifndef UG2_HUD_TOUCH_H_
#define UG2_HUD_TOUCH_H_

#include <lib/controls.h>

namespace ug2 { namespace hudtouch {

// By HudZone, display pixels clockwise; from Vita screenshot 2026-11-24-060708.
inline const win32::controls::ZonePolygon* zones()
{
    static const win32::controls::ZonePolygon Zones[win32::controls::HudZoneCount] = {
        { 4, { { 16, 331 }, { 207, 331 }, { 207, 529 }, { 16, 529 } } },
        { 6, { { 96, 133 }, { 109, 120 }, { 199, 120 }, { 199, 170 }, { 109, 170 }, { 96, 157 } } },
        { 6, { { 35, 40 }, { 199, 40 }, { 199, 112 }, { 105, 112 }, { 90, 127 }, { 35, 127 } } },
        { 6, { { 52, 177 }, { 65, 164 }, { 89, 164 }, { 102, 177 }, { 102, 267 }, { 52, 267 } } },
    };
    return Zones;
}

}}

#endif
