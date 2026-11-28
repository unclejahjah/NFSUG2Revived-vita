#ifndef LIB_CONTROLS_H_
#define LIB_CONTROLS_H_

#include <x86.h>
#include <winapi/types.h>
#include <cstdint>
#include <vector>

namespace win32
{

// Vita controls as the game sees them: one DirectInput 8 game controller presented as
// the EMS USB2 PlayStation 2 adapter, whose buttons follow the game's built-in table for that product, plus key
// messages for text fields. The mapping tables are in controls.cpp.
namespace controls
{

// Vita inputs.
enum Input
{
    Cross, Circle, Square, Triangle, L, R, Select, Start,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    RearL2, RearR2, RearL3, RearR3,     // rear touch quadrants: upper left, upper right, lower left, lower right
    InputCount
};

inline uint32_t bit(Input input) { return uint32_t(1) << input; }

// Keyboard keys the pad sends. Presses are posted in this order and releases in the reverse one (Shift before
// Delete goes down, after it comes up).
enum Key
{
    KeyUp, KeyDown, KeyLeft, KeyRight,
    KeyEscape, KeyEnter, KeyLeftShift, KeyDelete,
    KeyF1, KeyF2, KeyF3,
    KeyCount
};

// A key as WM_KEYDOWN/WM_KEYUP carry it: virtual key, PC/AT set-1 scancode and the extended flag. The DirectInput
// key code (DIK) is the scancode with bit 7 set for an extended key.
struct KeyCode
{
    x86::reg32 vk;
    x86::reg8  scancode;
    bool       extended;

    x86::reg8 dik() const { return x86::reg8(scancode | (extended ? 0x80 : 0)); }
};

const KeyCode& keyCode(Key key);
// Key of a character typed from the on-screen keyboard: letters, digits and space; shift is set for capitals.
// False for other characters.
bool characterKey(char c, KeyCode* key, bool* shift);
const KeyCode& shiftKey();

// DIJOYSTATE axis slots (lX, lY, lZ, lRx, lRy, lRz, two sliders): X, Y left stick, Z right stick Y (the merged
// pedal), Rz right stick X.
enum Axis
{
    AxisX, AxisY, AxisZ, AxisRx, AxisRy, AxisRz, AxisSlider0, AxisSlider1,
    AxisCount
};

// DirectInput button bits an input reports (0 for none).
uint32_t buttonMask(Input input);

bool axisPresent(Axis axis);
x86::reg32 presentAxisCount();
x86::reg32 joystickButtonCount();

// Object names DirectInput reports for the joystick's buttons and axes (GetObjectInfo, EnumObjects). Null entries,
// and every object before setObjectNames, get the default names. The strings must stay valid.
void setObjectNames(const char* const* buttons, x86::reg32 buttonCount, const char* const* axes);
const char* buttonName(x86::reg32 button);  // null for the default
const char* axisName(Axis axis);            // null for the default

// Product name, DirectInput 7 device type and the product GUID (the EMS adapter's VID/PID GUID).
extern const char* const JoystickName;
extern const x86::reg32  JoystickDeviceType;
extern const GUID        JoystickProduct;

// One reading of the pad and touch panels.
struct PadSample
{
    uint32_t    inputs;             // bit per Input
    float       sticks[4];          // left x, left y, right x, right y: -1..1, y down
    struct Touch
    {
        float x, y;                 // 0..1 of the panel
        int   id;                   // the same while the finger stays down
    };
    static const int MaxTouches = 6;
    Touch       touches[MaxTouches];
    int         touchCount = 0;     // front panel touches
};

PadSample readPad();

#ifndef __vita__
// Host builds: readPad returns the source's sample, or without one the last setPadState sample (keyboard frontends).
typedef PadSample (*PadSource)(void* context);
void setPadSource(PadSource source, void* context);
void setPadState(const PadSample& sample);
#endif

// A recorded pad for replays: lines "<step> <inputs> [lx ly rx ry]" in step order, each in force from its step (the
// caller's frame or poll count) on. Inputs are lower-case Input names joined by '+', or '-'; '#' starts a comment.
class PadScript
{
public:
    // False, logged and empty for a missing file or a bad line.
    bool load(const char* path);
    PadSample sampleAt(uint64_t step) const;

private:
    struct Entry
    {
        uint64_t    step;
        PadSample   sample;
    };
    std::vector<Entry> m_entries;
};

// Stick roles from the pad record, applied while driving: a stick without an axis role sends centred axes. Steer and Throttle shape only their own axis.
struct StickRoles
{
    bool axes[2];       // left, right: the stick drives game axes (X and Y left, Rz and Z right)
    bool flick[2];      // the stick's up and down flicks move its hat: HatLeftStick for the left, HatRightStick for the right
    bool freeLook;      // the right stick has no action
    bool throttle[2];   // with axes: the role is Throttle (vertical axis), not Steer (horizontal axis)
};
StickRoles stickRoles();
void setStickRoles(const StickRoles& roles);
// Free-look hook: the record leaves the right stick unbound.
bool rightStickUnbound();

// Hats of the joystick (the race map's hat entries 16 to 31, four per hat: down, up, left, right). Hat 0 stays centred.
const int HatLeftStick = 1;     // left stick flicks
const int HatRightStick = 2;    // right stick flicks
const int HatCount = 3;

// Fractions 0..1: dead zone, travel that counts as full, and the linear share of linear * t + (1 - linear) * t^3.
struct StickShape
{
    float deadZone;
    float saturation;
    float sensitivity;
};
// Garage and free look: 30% dead zone, full travel, linear.
const StickShape FixedStickShape = { 0.30f, 1.0f, 1.0f };
// Driving shape: from left_stick_* and right_stick_* in the cfg.
StickShape stickShape(bool right);
void setStickShape(bool right, const StickShape& shape);
// Cone half-angle in degrees (cfg stick_cone_angle, 0 to 45); 0 reads the axis alone.
int stickConeAngle();
void setStickConeAngle(int degrees);
// The cfg shape for a driving role, else FixedStickShape.
StickShape roleShape(const StickRoles& roles, bool right);
// A raw stick shaped with FixedStickShape.
void fixedShape(float x, float y, float* outX, float* outY);
// A raw stick with a dead zone of percent (clamped 0 to 70), full travel, linear.
void deadZoneShape(float x, float y, int percent, float* outX, float* outY);

// Set by the pad hook while driving; roles apply then.
void setDriving(bool driving);
bool driving();

extern const int HatUp;
extern const int HatDown;
extern const int HatLeft;
extern const int HatRight;
// Hat angle in hundredths of a degree; -1 centred.
int hatAngle(bool up, bool down, bool left, bool right);

// Set by the pad hook while driving in free roam.
void setFreeRoam(bool freeRoam);
bool freeRoam();
// The Vita inputs (bit(Input)) the mapper saw last.
uint32_t heldInputs();

// Free-roam touch zones, fixed on the screen: a short tap requests the zone's world action, whatever the buttons
// do or the HUD shows. The pad hook sends it as the game's joy event.
enum HudZone { HudMap, HudMessages, HudShop, HudCareer, HudZoneCount };
struct ZonePoint
{
    float x, y;                 // display pixels
};
// A zone's outline, in either winding.
struct ZonePolygon
{
    static const int MaxPoints = 8;
    int       count;
    ZonePoint points[MaxPoints];
};
// The front panel in display pixels.
const float PanelWidth = 960.0f, PanelHeight = 544.0f;
// Inside or on an edge (within half a pixel).
bool insidePolygon(const ZonePolygon& polygon, float x, float y);
// HudZoneCount polygons by HudZone, kept by pointer; null for none.
void setHudZones(const ZonePolygon* zones);
// Bit per HudZone (1 << zone) tapped since the last call.
uint32_t takeWorldRequests();

// What the game gets from one PadSample.
struct State
{
    uint32_t    buttons;            // DirectInput button bits
    float       axes[AxisCount];    // -1..1, absent axes 0, sticks already shaped
    uint32_t    keys;               // bit per Key held
    int         pov[HatCount];      // hats in hundredths of a degree, -1 centred
    bool        openIme;            // the on-screen keyboard chord was pressed
    bool        openSettings;       // the Vita Settings chord (Select + Up) was pressed
    int         sayChord;           // quick chat: the slot 0-6 picked (quickChatSlots: 6 is add friend), -1 none
};

// Turns pad samples into joystick and key state: chords with Select, the Select tap (respot) and buttons held
// across chords. One instance; calls must not overlap.
class Mapper
{
public:
    Mapper();
    State update(const PadSample& pad, uint64_t nowUs);

private:
    uint32_t    m_previous;         // inputs of the last sample
    uint32_t    m_chorded;          // inputs pressed while Select was held, mapped by the chord table until released
    bool        m_selectUsed;       // a chord was used during this Select press
    uint64_t    m_selectPulseEndUs; // the Select tap's button is reported until then
    uint64_t    m_chatSinceUs;      // quick chat: when its button (quick_chat_button) went down
    bool        m_chatUsed;         // a phrase was picked during this press
    uint32_t    m_chatSwallow;      // phrase buttons pressed with it, kept from the game until released
    uint64_t    m_chatPulseEndUs;   // a short tap of the chat button reaches the game until then
    float       m_steer;            // steering=digital: the left stick's ramped X position
    uint64_t    m_steerUs;          // time of the last ramp step
    // horn_button and drift_button on a D-pad direction: held past HoldUs it is ours (the horn sounds, drift
    // switches once); a shorter press reaches the game afterwards as a tap, so its own action still works.
    struct Hold
    {
        uint64_t sinceUs;           // when the button went down in free roam, 0 when up
        bool     fired;             // held long enough: ours for the rest of this press
        uint64_t pulseEndUs;        // the short press reaches the game until then
    };
    Hold        m_hold[5];          // horn, drift, a race lobby's Up, Down and Left (setLobbyKeys)
    bool        m_lobbyDownFired;   // quick chat's button held on into the lobby's Down: fired once this press
    bool        m_flickArmed[2];    // per stick: back under the re-arm level since the last flick
    uint64_t    m_shiftEndUs[2][2]; // per stick: up and down flick hats end
    struct Tap
    {
        int      id;                // -1 for a free entry
        uint64_t startUs;
        float    startX, startY;
        float    farthest;          // greatest distance moved from the start
        bool     seen;
    };
    Tap         m_taps[PadSample::MaxTouches];

    void updateTaps(const PadSample& pad, uint64_t nowUs);
    bool held(Hold& hold, Input input, uint32_t inputs, uint32_t pressed, uint64_t nowUs, uint32_t* hidden,
              uint32_t* pulse);
};

// Free roam's quick chat (quick_chat_button): the button held to pick a phrase (an Input, or -1 for Select, whose
// chords pick then), and the buttons that pick phrase slots 1-6 while it is held; slots[6], the seventh, adds the
// player whose nose touches ours as a friend.
int quickChatButton();
// Free roam proper (driving, quick chat on) says so each frame, with pf::ticksUs(): only then is the button taken.
void setQuickChatLive(uint64_t nowUs);
void quickChatSlots(Input slots[7]);
// Since when the quick chat button has been held in free roam (pf::ticksUs), 0 when it is not held now.
uint64_t quickChatHeldSince();
// Free roam's horn (horn_button: dpad_left by default, held; dpad_right, rear_l3, rear_r3 or off): held now, while free
// roam's world runs (it is taken from the game only then, and not while quick chat's button is held).
bool hornHeld();
// Free roam's show-off (Select + L): the presses since the last call.
int takeShowoffPresses();
// Free roam's world runs (driving, not paused) says so each frame, with pf::ticksUs(): only then is the horn taken.
void setHornLive(uint64_t nowUs);
// horn_touch_button: the red horn button on the screen (roam_game.c hb_draw) shown and touchable (the default)
bool hornTouchOn();
// Free roam's drift mode (drift_button: dpad_right by default, held; dpad_left, rear_r3, rear_l3 or off): the presses since
// the last call, taken only while free roam's world says so (setDriftLive, each frame, with pf::ticksUs()).
int takeDriftPresses();
void setDriftLive(uint64_t nowUs);
// Free roam's race lobby panel (roam_game.c lb_panel) offers its keys each frame, with pf::ticksUs(): bit 0, D-pad Up
// held opens a new lobby (a tap still reaches the game, after it: Engage); down, D-pad Down held switches Friends only (over quick chat when it is on
// Down: held on for LobbyDownUs with no phrase picked).  takeLobbyPresses: bit 0 Left fired, bit 1 Down, since the last
// call.  lobbyDownHeldUs: how long Down has been held towards it now (0 none), for the quick chat box's bar.
// left: D-pad Left held joins the lobby at this start (bit 2 of takeLobbyPresses; a tap is still the World Map).
void setLobbyKeys(uint64_t nowUs, bool up, bool down, bool left);
int takeLobbyPresses();
uint64_t lobbyDownHeldUs();
// The lobby panel offers D-pad Down (Friends only) this frame.
bool lobbyDownOffered();
extern const uint64_t LobbyDownUs;

// Radial dead zone and response curve of one stick.
void shapeStick(float x, float y, const StickShape& shape, float* outX, float* outY);
// One axis shaped on its own: sign(v) * response(|v|).
float shapeAxis(float v, const StickShape& shape);
// Role axis input: stick distance within the cone angle, else the axis; 0 is axis only.
float coneInput(float x, float y, bool vertical, int angle);

}

}

#endif
