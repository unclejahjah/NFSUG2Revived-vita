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

// DIJOYSTATE axis slots (lX, lY, lZ, lRx, lRy, lRz, two sliders): the joystick has X, Y (left stick), Z (the
// adapter's combined pedal, held centred: the buttons drive throttle and brake) and Rz (right stick Y).
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

// front_touch (off by default): the front panel is the mouse.
bool frontTouchEnabled();

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

// What the game gets from one PadSample.
struct State
{
    uint32_t    buttons;            // DirectInput button bits
    float       axes[AxisCount];    // -1..1, absent axes 0; not shaped yet (shapeStick)
    uint32_t    keys;               // bit per Key held
    int         pov;                // hat 0 in hundredths of a degree, -1 centred
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
};

// Free roam's quick chat (quick_chat_button): the button held to pick a phrase (an Input, or -1 for Select, whose
// chords pick then), and the buttons that pick phrase slots 1-6 while it is held; slots[6], the seventh, adds the
// player whose nose touches ours as a friend.
int quickChatButton();
// Free roam proper (driving, quick chat on) says so each frame, with pf::ticksUs(): only then is the button taken.
void setQuickChatLive(uint64_t nowUs);
void quickChatSlots(Input slots[7]);

// Radial dead zone and response curve of one stick (x, y in -1..1). deadZone and saturation are DirectInput
// property values (0..10000 of full deflection); the larger dead zone and the smaller saturation of these and the
// stick's own apply.
void shapeStick(float x, float y, uint32_t deadZone, uint32_t saturation, float* outX, float* outY);

}

}

#endif
