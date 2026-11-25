// The Controls rows as data: see controlsrows.h.

#include "controlsrows.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace ug2 { namespace controlsrows {

namespace {

const int TypeCount = 5;

float* field(win32::controls::StickShape* s, StickPart part)
{
    return part == PartDeadZone ? &s->deadZone : part == PartOuterLimit ? &s->saturation : &s->sensitivity;
}

std::string oldKey(bool right, int part)
{
    const char* const parts[PartCount] = { "deadzone", "saturation", "sensitivity" };
    return std::string(right ? "right_stick_" : "left_stick_") + parts[part];
}

}

int nextType(int type, int direction)
{
    const int t = type < 0 || type >= TypeCount ? 0 : type;
    return ((t + (direction < 0 ? -1 : direction > 0 ? 1 : 0)) % TypeCount + TypeCount) % TypeCount;
}

StickLimits stickLimits(StickPart part)
{
    switch (part)
    {
    case PartDeadZone:   return { 0, 50, 1 };
    case PartOuterLimit: return { 50, 100, 1 };
    default:             return { 0, 100, 5 };
    }
}

int stickPercent(const win32::controls::StickShape& shape, StickPart part)
{
    win32::controls::StickShape s = shape;
    return int(std::lround(*field(&s, part) * 100.0f));
}

win32::controls::StickShape stepStick(const win32::controls::StickShape& shape, StickPart part, int direction)
{
    const StickLimits l = stickLimits(part);
    int v = stickPercent(shape, part) + (direction < 0 ? -l.step : direction > 0 ? l.step : 0);
    v = v < l.low ? l.low : v > l.high ? l.high : v;
    win32::controls::StickShape s = shape;
    *field(&s, part) = float(v) / 100.0f;
    return s;
}

namespace {

bool s_fromCfg[2][PartCount];       // value taken from the old cfg, not yet in registry.txt

}

void loadShapes(controlstore::Backend& backend, OldCfgValue oldValue)
{
    for (int side = 0; side < 2; ++side)
    {
        win32::controls::StickShape shape = win32::controls::stickShape(side != 0);
        for (int part = 0; part < PartCount; ++part)
        {
            s_fromCfg[side][part] = false;
            int percent = controlstore::stickDefault(part);
            if (!controlstore::stickStored(backend, side != 0, part, &percent))
            {
                const char* old = oldValue ? oldValue(oldKey(side != 0, part).c_str()) : nullptr;
                char* end = nullptr;
                const long v = old ? strtol(old, &end, 10) : 0;
                if (old && end != old && !*end)
                {
                    percent = controlstore::stickClamp(part, int(v));
                    s_fromCfg[side][part] = true;
                }
            }
            *field(&shape, StickPart(part)) = float(percent) / 100.0f;
        }
        win32::controls::setStickShape(side != 0, shape);
    }
}

void saveShape(controlstore::Backend& backend, bool right, StickPart part)
{
    controlstore::setStick(backend, right, part, stickPercent(win32::controls::stickShape(right), part));
    s_fromCfg[right ? 1 : 0][part] = false;
    for (int side = 0; side < 2; ++side)
    {
        const win32::controls::StickShape o = win32::controls::stickShape(side != 0);
        for (int p = 0; p < PartCount; ++p)
        {
            if (s_fromCfg[side][p])
                controlstore::setStick(backend, side != 0, p, stickPercent(o, StickPart(p)));
            s_fromCfg[side][p] = false;
        }
    }
}

}}
