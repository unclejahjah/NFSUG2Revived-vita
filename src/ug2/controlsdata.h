#ifndef UG2_CONTROLSDATA_H_
#define UG2_CONTROLSDATA_H_

// Control Types as data: the game's PS2 pad record (race map, hats and axis setup), its stick roles and bind rules.
// No game access, so the host tests cover it.

#include <lib/controls.h>
#include <cstdint>

namespace ug2 { namespace controlsdata {

// The game's race actions.
enum Action : uint32_t
{
    ActionNone = 0, Accelerate = 1, Brake = 2, SteerLeft = 3, SteerRight = 4, Handbrake = 5, Nitrous = 6, ShiftUp = 7, ShiftDown = 8, LookBack = 9,
    ChangeCamera = 10, Pause = 12, ResetCar = 14, EnterShop = 15, WorldMap = 16, ReadSms = 17, CareerStats = 18,
    HydraulicsBounce = 19, Hydraulics = 22, Headlights = 23,
};

// Index of the adapter's buttons in a race map.
enum Button
{
    Triangle, Circle, Cross, Square, L2, R2, L1, R1, Select, Start, L3, R3, DpadUp, DpadRight, DpadDown, DpadLeft,
    ButtonCount
};

const int MapEntries = 32;      // 16 buttons, then four hats of four directions (down, up, left, right)
const int TypeCustom = 5;
// Race map hat entries: hat 1 holds the left stick's Shift flicks, hat 2 the right stick's (down, up).
// Hats 0 and 3 never move.
const int HatBase = 16, HatLeftStick = HatBase + 4, HatRightStick = HatBase + 8;
// Hat 3 never moves: its last entry holds FreeLookOffMark when free look is removed from the right stick.
const int FreeLookOff = HatBase + 15;
const uint32_t FreeLookOffMark = 13;
// DIJOYSTATE axis ids (byte offsets) as the axis setup holds them: left stick X and Y, right Rz and Z.
const int32_t AxisX = 0, AxisY = 4, AxisZ = 8, AxisRz = 20;

// Race map of Control Type 1..4; other types give 1.
void raceMap(int type, uint32_t map[MapEntries]);

// What a stick does, read from the record.
enum StickRole { RoleNone, RoleSteer, RoleThrottle, RoleShift, RoleLook };

// The Vita controls a bind can use, and the bindable actions.
enum Control
{
    CtlTriangle, CtlCircle, CtlCross, CtlSquare, CtlL, CtlR, CtlDpadUp, CtlDpadDown, CtlDpadLeft, CtlDpadRight,
    CtlLeftStick, CtlRightStick, CtlRearL2, CtlRearR2, CtlRearL3, CtlRearR3, ControlCount
};   // the four rear touch quadrants press the PS2 buttons L2, R2, L3, R3

enum Entry
{
    EntrySteerLeft, EntrySteerRight, EntryFreeLook, EntryAccelerate, EntryBrake, EntryHandbrake, EntryNitrous, EntryShiftUp,
    EntryShiftDown, EntryLookBack, EntryChangeCamera, EntryEnterShop, EntryWorldMap, EntryReadSms, EntryCareerStats,
    EntryHydraulics, EntryHydraulicsBounce, EntryHeadlights, EntryCount
};

// One control scheme as plain data: the action of each face button, L, R, d-pad arrow and rear quadrant (0 for none),
// and each stick's role. Control Types are instances.
struct Scheme
{
    uint32_t face[4];       // Triangle, Circle, Cross, Square
    uint32_t l, r;
    uint32_t dpad[4];       // up, right, down, left
    uint32_t rear[4];       // upper left L2, upper right R2, lower left L3, lower right R3
    StickRole left, right;
};

// Control Type 1..4; other types give 1.
Scheme presetScheme(int type);

// The game's PS2 pad record of one slot after the GUID: race map, four axis entries (steer, throttle, brake, merged
// pedal) and the merged byte. Presets and Custom are values of it; docs/pad-binding.md.
struct Binds
{
    uint32_t map[MapEntries];
    int32_t  axisId[4];
    int32_t  axisSign[4];
    uint8_t  flags[4];
    uint8_t  merged;
};

Binds presetBinds(int type);
Binds bindsOf(const Scheme& scheme);
Scheme schemeOf(const Binds& binds);
// Side 0 left, 1 right; a free right stick gives RoleLook unless free look is removed (RoleNone).
StickRole stickRole(const Binds& binds, int side);
// What the input layer routes for this record.
win32::controls::StickRoles inputRoles(const Binds& binds);
// Bit per Control the entry is bound to.
unsigned controlsOf(const Binds& binds, Entry entry);
// False, binds unchanged, when the control cannot take it.
bool bindControl(Binds* binds, Entry entry, Control control);
bool entryTakes(const Binds& binds, Entry entry, Control control);
// The entry's race action; 0 for stick-only entries.
uint32_t entryAction(Entry entry);
// Clears the entry, a stick pair both; false if unchanged.
bool unbindEntry(Binds* binds, Entry entry);
bool isStick(Control control);
// One control per action: stick pairs win, then lowest button.
void onePerAction(Binds* binds);

const char* controlTypeName(int type);          // "1".."4", "Custom"
const char* actionName(uint32_t action);        // English, for logs
const char* roleName(StickRole role);           // English, for logs

}}

#endif /* !UG2_CONTROLSDATA_H_ */
