// Control Types as data: see controlsdata.h.

#include "controlsdata.h"

#include <cstring>

namespace ug2 { namespace controlsdata {

namespace
{

// The Control Types: any action on any control (0 for none); the rear is L2, R2 (upper) and L3, R3 (lower).
const Scheme Presets[4] =
{
    //  Triangle      Circle   Cross       Square       L1         R1            D-pad up   right    down          left           L2  R2  L3          R3                  left stick  right stick
    { { ShiftUp,      Nitrous, Handbrake,  ShiftDown }, Brake,     Accelerate, { EnterShop, ReadSms, ChangeCamera, WorldMap  }, { 0,  0,  Hydraulics, HydraulicsBounce }, RoleSteer,  RoleLook },
    { { ChangeCamera, 0,       Accelerate, Brake     }, Handbrake, Nitrous,    { EnterShop, ShiftUp, Headlights,   ShiftDown }, { 0,  0,  Hydraulics, HydraulicsBounce }, RoleSteer,  RoleLook },
    { { ChangeCamera, 0,       0,          LookBack  }, Handbrake, Nitrous,    { EnterShop, ShiftUp, Headlights,   ShiftDown }, { 0,  0,  Hydraulics, HydraulicsBounce }, RoleSteer,  RoleThrottle },
    { { ChangeCamera, Nitrous, Handbrake,  LookBack  }, Brake,     Accelerate, { EnterShop, ReadSms, Headlights,   WorldMap  }, { 0,  0,  Hydraulics, HydraulicsBounce }, RoleSteer,  RoleShift },
};

void freeStick(Binds* b, int side);

void setRole(Binds* b, int side, StickRole role)
{
    freeStick(b, side);
    if (side)
        b->map[FreeLookOff] = role == RoleNone ? FreeLookOffMark : 0;
    if (role == RoleSteer)
    {
        b->axisId[0] = side ? AxisRz : AxisX;
        b->axisSign[0] = 1;
        b->flags[0] = 1;
    }
    else if (role == RoleThrottle)
    {
        b->axisId[3] = side ? AxisZ : AxisY;
        b->axisSign[3] = 1;
        b->flags[1] = b->flags[2] = 0;
        b->flags[3] = 1;
        b->merged = 1;
    }
    else if (role == RoleShift)
    {
        const int hat = side ? HatRightStick : HatLeftStick;
        b->map[hat] = ShiftDown;
        b->map[hat + 1] = ShiftUp;
    }
}

const int ControlIndex[ControlCount] = { Triangle, Circle, Cross, Square, L1, R1, DpadUp, DpadDown, DpadLeft, DpadRight, -1, -1,
                                         L2, R2, L3, R3 };

const uint32_t EntryActions[EntryCount] =
{
    SteerLeft, SteerRight, 0, Accelerate, Brake, Handbrake, Nitrous, ShiftUp, ShiftDown, LookBack, ChangeCamera, EnterShop,
    WorldMap, ReadSms, CareerStats, Hydraulics, HydraulicsBounce, Headlights,
};

// Pairs a stick carries together: one leaving frees both.
uint32_t partner(uint32_t action, StickRole* role)
{
    switch (action)
    {
    case Accelerate: *role = RoleThrottle; return Brake;
    case Brake:      *role = RoleThrottle; return Accelerate;
    case ShiftUp:    *role = RoleShift; return ShiftDown;
    case ShiftDown:  *role = RoleShift; return ShiftUp;
    case SteerLeft:  *role = RoleSteer; return SteerRight;
    case SteerRight: *role = RoleSteer; return SteerLeft;
    default:         return 0;
    }
}

void freeStick(Binds* b, int side)
{
    const StickRole role = stickRole(*b, side);
    if (role == RoleSteer)
        b->flags[0] = 0;
    else if (role == RoleThrottle)
        b->flags[3] = 0;
    else if (role == RoleShift)
    {
        const int hat = side ? HatRightStick : HatLeftStick;
        b->map[hat] = b->map[hat + 1] = 0;
    }
}

void clearAction(Binds* b, uint32_t action)
{
    StickRole pair = RoleNone;
    const uint32_t mate = partner(action, &pair);
    uint32_t other = 0;
    for (int side = 0; side < 2; ++side)
    {
        if (mate && stickRole(*b, side) == pair)
        {
            freeStick(b, side);
            other = mate;
        }
    }
    for (int i = 0; i < ButtonCount; ++i)
    {
        if (b->map[i] == action || (other && b->map[i] == other))
            b->map[i] = 0;
    }
}

int sideOf(Control c)
{
    return c == CtlLeftStick ? 0 : 1;
}

}

Scheme presetScheme(int type)
{
    return Presets[type >= 1 && type <= 4 ? type - 1 : 0];
}

Binds bindsOf(const Scheme& p)
{
    Binds b;
    memset(&b, 0, sizeof(b));
    for (int i = 0; i < 4; ++i)
        b.map[Triangle + i] = p.face[i];
    b.map[L1] = p.l;
    b.map[R1] = p.r;
    b.map[Select] = ResetCar;
    b.map[Start] = Pause;
    b.map[DpadUp] = p.dpad[0];
    b.map[DpadRight] = p.dpad[1];
    b.map[DpadDown] = p.dpad[2];
    b.map[DpadLeft] = p.dpad[3];
    b.map[L2] = p.rear[0];
    b.map[R2] = p.rear[1];
    b.map[L3] = p.rear[2];
    b.map[R3] = p.rear[3];
    const int32_t ids[4] = { AxisX, AxisY, AxisY, AxisZ }, signs[4] = { 1, -1, -1, 1 };
    memcpy(b.axisId, ids, sizeof(ids));
    memcpy(b.axisSign, signs, sizeof(signs));
    b.merged = 1;
    setRole(&b, 0, p.left);
    setRole(&b, 1, p.right);
    return b;
}

Binds presetBinds(int type)
{
    return bindsOf(presetScheme(type));
}

StickRole stickRole(const Binds& b, int side)
{
    if (b.flags[0] && b.axisId[0] == (side ? AxisRz : AxisX))
        return RoleSteer;
    if (b.flags[3] && b.merged && b.axisId[3] == (side ? AxisZ : AxisY))
        return RoleThrottle;
    const int hat = side ? HatRightStick : HatLeftStick;
    if (b.map[hat] == ShiftDown || b.map[hat + 1] == ShiftUp)     // the hat's left and right entries are the game's own defaults
        return RoleShift;
    return side && b.map[FreeLookOff] != FreeLookOffMark ? RoleLook : RoleNone;
}

win32::controls::StickRoles inputRoles(const Binds& b)
{
    win32::controls::StickRoles roles;
    for (int side = 0; side < 2; ++side)
    {
        const StickRole role = stickRole(b, side);
        roles.axes[side] = role == RoleSteer || role == RoleThrottle;
        roles.flick[side] = role == RoleShift;
        roles.throttle[side] = role == RoleThrottle;
    }
    roles.freeLook = stickRole(b, 1) == RoleLook;
    return roles;
}

Scheme schemeOf(const Binds& b)
{
    Scheme p;
    for (int i = 0; i < 4; ++i)
        p.face[i] = b.map[Triangle + i];
    p.l = b.map[L1];
    p.r = b.map[R1];
    p.dpad[0] = b.map[DpadUp];
    p.dpad[1] = b.map[DpadRight];
    p.dpad[2] = b.map[DpadDown];
    p.dpad[3] = b.map[DpadLeft];
    p.rear[0] = b.map[L2];
    p.rear[1] = b.map[R2];
    p.rear[2] = b.map[L3];
    p.rear[3] = b.map[R3];
    p.left = stickRole(b, 0);
    p.right = stickRole(b, 1);
    return p;
}

void raceMap(int type, uint32_t map[MapEntries])
{
    memcpy(map, presetBinds(type).map, MapEntries * sizeof(uint32_t));
}

bool isStick(Control control)
{
    return control == CtlLeftStick || control == CtlRightStick;
}

uint32_t entryAction(Entry entry)
{
    return EntryActions[entry];
}

unsigned controlsOf(const Binds& b, Entry entry)
{
    unsigned mask = 0;
    const uint32_t action = EntryActions[entry];
    for (int side = 0; side < 2; ++side)
    {
        const StickRole role = stickRole(b, side);
        const bool match = entry == EntrySteerLeft || entry == EntrySteerRight ? role == RoleSteer :
                           entry == EntryFreeLook ? side == 1 && role == RoleLook :
                           entry == EntryAccelerate || entry == EntryBrake ? role == RoleThrottle :
                           entry == EntryShiftUp || entry == EntryShiftDown ? role == RoleShift : false;
        if (match)
            mask |= 1u << (CtlLeftStick + side);
    }
    if (!action)
        return mask;
    for (int c = 0; c < ControlCount; ++c)
    {
        if (ControlIndex[c] >= 0 && b.map[ControlIndex[c]] == action)
            mask |= 1u << c;
    }
    return mask;
}

bool entryTakes(const Binds&, Entry entry, Control control)
{
    StickRole role;
    const bool pairs = partner(EntryActions[entry], &role) != 0;
    if (entry == EntryFreeLook)
        return control == CtlRightStick;
    return isStick(control) ? pairs : true;
}

bool bindControl(Binds* b, Entry entry, Control control)
{
    if (!entryTakes(*b, entry, control))
        return false;
    const uint32_t action = EntryActions[entry];
    if (entry == EntryFreeLook)
    {
        freeStick(b, 1);
        b->map[FreeLookOff] = 0;
    }
    else if (isStick(control))
    {
        StickRole role = RoleNone;
        const uint32_t mate = partner(action, &role);
        clearAction(b, action);
        clearAction(b, mate);
        setRole(b, sideOf(control), role);
    }
    else
    {
        clearAction(b, action);
        b->map[ControlIndex[control]] = action;
    }
    return true;
}

bool unbindEntry(Binds* b, Entry entry)
{
    if (entry == EntryFreeLook)
    {
        if (stickRole(*b, 1) != RoleLook)
            return false;
        b->map[FreeLookOff] = FreeLookOffMark;
        return true;
    }
    const Binds before = *b;
    clearAction(b, EntryActions[entry]);
    return memcmp(&before, b, sizeof(before)) != 0;
}

void onePerAction(Binds* b)
{
    StickRole left = stickRole(*b, 0), right = stickRole(*b, 1);
    if (left == RoleShift && right == RoleShift)
    {
        freeStick(b, 1);
        right = stickRole(*b, 1);
    }
    for (int hat = 0; hat < 4; ++hat)
    {
        for (int d = 0; d < 4; ++d)
        {
            const StickRole role = hat == 1 ? left : hat == 2 ? right : RoleNone;
            if (role != RoleShift || d >= 2)
                b->map[HatBase + 4 * hat + d] = 0;
        }
    }
    if (right == RoleNone)
        b->map[FreeLookOff] = FreeLookOffMark;
    for (StickRole role : { left, right })
    {
        if (role != RoleSteer && role != RoleThrottle && role != RoleShift)
            continue;
        const uint32_t pair[2] = { role == RoleSteer ? SteerLeft : role == RoleThrottle ? Accelerate : ShiftUp,
                                   role == RoleSteer ? SteerRight : role == RoleThrottle ? Brake : ShiftDown };
        for (int i = 0; i < ButtonCount; ++i)
        {
            if (b->map[i] == pair[0] || b->map[i] == pair[1])
                b->map[i] = 0;
        }
    }
    for (int i = 0; i < ButtonCount; ++i)
    {
        for (int j = i + 1; b->map[i] && j < ButtonCount; ++j)
        {
            if (b->map[j] == b->map[i])
                b->map[j] = 0;
        }
    }
}

const char* controlTypeName(int type)
{
    static const char* const Names[] = { "1", "2", "3", "4", "Custom" };
    return Names[type >= 1 && type <= 5 ? type - 1 : 0];
}

const char* actionName(uint32_t action)
{
    switch (action)
    {
    case ActionNone:        return "-";
    case Accelerate:        return "Accelerate";
    case Brake:             return "Brake";
    case SteerLeft:         return "SteerLeft";
    case SteerRight:        return "SteerRight";
    case Handbrake:         return "Handbrake";
    case Nitrous:           return "Nitrous";
    case ShiftUp:           return "ShiftUp";
    case ShiftDown:         return "ShiftDown";
    case LookBack:          return "LookBack";
    case ChangeCamera:      return "Camera";
    case Pause:             return "Pause";
    case ResetCar:          return "Reset";
    case EnterShop:         return "EnterShop";
    case WorldMap:          return "WorldMap";
    case ReadSms:           return "SMS";
    case CareerStats:       return "Career";
    case HydraulicsBounce:  return "HydBounce";
    case Hydraulics:        return "Hydraulics";
    case Headlights:        return "Headlights";
    default:                return "?";
    }
}

const char* roleName(StickRole role)
{
    static const char* const Names[] = { "none", "Steer", "Accelerate/Brake", "Shift", "unbound (free look)" };
    return Names[role];
}

}}
