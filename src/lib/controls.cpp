#include <lib/controls.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <platform/log.h>
#ifdef __vita__
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#else
#include <mutex>
#include <cstring>
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace win32 { namespace controls
{

// The Vita pad is presented as the EMS USB2 PlayStation 2 adapter (VID 0B43, PID 0003): the game has a built-in
// binding table for that product, so no binding file is needed.
const char* const JoystickName       = "Vita Gamepad";
const x86::reg32  JoystickDeviceType = 0x0204;      // DIDEVTYPE_JOYSTICK, DIDEVTYPEJOYSTICK_GAMEPAD
const GUID        JoystickProduct    = { 0x00030B43, 0x0000, 0x0000, { 0, 0, 'P', 'I', 'D', 'V', 'I', 'D' } };

// ---- Layout ----

// Joystick buttons by DirectInput index, as the EMS driver reports them on Windows: 0 Triangle, 1 Circle, 2 Cross,
// 3 Square, 4 L2, 5 R2, 6 L1, 7 R1, 8 Select, 9 Start, 10 L3, 11 R3, 12-15 d-pad up, right, down, left (no hat).
// Select is a tap (button 8): see the Mapper.
struct ButtonMapping
{
    Input input;
    int   button;
};

static const ButtonMapping s_buttons[] =
{
    { Triangle,  0 },
    { Circle,    1 },
    { Cross,     2 },
    { Square,    3 },
    { RearL2,    4 },
    { RearR2,    5 },
    { L,         6 },
    { R,         7 },
    { Start,     9 },
    { RearL3,    10 },
    { RearR3,    11 },
    { DpadUp,    12 },
    { DpadRight, 13 },
    { DpadDown,  14 },
    { DpadLeft,  15 },
};

// Button that a short press of Select (no chord) reports: respot.
static const int s_selectTapButton = 8;
static const int s_buttonCount = 16;

// Keys held while an input is held in a text field: the d-pad moves its cursor (WM_KEYDOWN Left / Right).
struct KeyMapping
{
    Input    input;
    uint32_t keys;
};

static constexpr uint32_t keys(Key key) { return uint32_t(1) << key; }

static const KeyMapping s_keys[] =
{
    { DpadLeft,  keys(KeyLeft)   },
    { DpadRight, keys(KeyRight)  },
};

// Select + input. An input pressed while Select is held uses this table until it is released.
struct Chord
{
    Input    input;
    uint32_t keys;
    int      button;    // -1 for none
    bool     ime;       // opens the on-screen keyboard
    bool     settings;  // opens Vita Settings (src/ug2/menu_entry.cpp)
    int      say;       // free roam's quick chat slot (Gamepad::takeSayChord), -1 for none
};

static const Chord s_chords[] =
{
    { Triangle,  0, -1, true,  false, -1 }, // text entry
    { DpadUp,    0, -1, false, true,  -1 }, // Vita Settings
    // free roam's quick chat (scripts/NFSU2FreeRoam.ini Say1 to Say6)
    { DpadDown,  0, -1, false, false, 0 },
    { DpadLeft,  0, -1, false, false, 1 },
    { DpadRight, 0, -1, false, false, 2 },
    { Cross,     0, -1, false, false, 3 },
    { Circle,    0, -1, false, false, 4 },
    { Square,    0, -1, false, false, 5 },
    { R,         0, -1, false, false, 6 },  // add the player nose to nose as a friend
};

// How long a Select tap holds its button, enough for one game frame at 10 fps.
static const uint64_t s_tapUs = 100000;

// Left and right stick: dead zone and saturation as fractions of full deflection, and the response curve
// y = linear * x + (1 - linear) * x^3 between them (finer aim near the centre). The dead zone (stick_deadzone,
// percent) and the curve (stick_response) are read again when Vita Settings changes them.
static const float s_stickSaturation = 0.94f;

struct StickShape
{
    unsigned generation = ~0u;
    float deadZone = 0.15f;
    float linear = 0.6f;
};

// steering=digital: how far the left stick goes before it counts, and the seconds to full lock and to the centre.
static const float s_digitalThreshold = 0.5f;
static const float s_digitalTurnS = 0.25f;
static const float s_digitalReturnS = 0.12f;

static bool digitalSteering()
{
    static std::atomic<unsigned> s_generation{~0u};
    static std::atomic<bool> s_digital{false};
    const unsigned generation = pf::Config::generation();
    if (s_generation.load(std::memory_order_acquire) != generation)
    {
        static const char* const Modes[] = { "analog", "digital" };
        s_digital.store(pf::Config::getChoice("steering", Modes, 2, 0) == 1, std::memory_order_relaxed);
        s_generation.store(generation, std::memory_order_release);
    }
    return s_digital.load(std::memory_order_relaxed);
}

// Free roam proper says so every frame (setQuickChatLive); the button is the game's again 0.25 s after the last.
static std::atomic<uint64_t> s_chatLiveUs{0};

void setQuickChatLive(uint64_t nowUs)
{
    s_chatLiveUs.store(nowUs, std::memory_order_relaxed);
}

static std::atomic<uint64_t> s_cameraLiveUs{0};

void setCameraStickLive(uint64_t nowUs)
{
    s_cameraLiveUs.store(nowUs, std::memory_order_relaxed);
}

static std::atomic<uint64_t> s_chatHeldSince{0};

uint64_t quickChatHeldSince()
{
    return s_chatHeldSince.load(std::memory_order_relaxed);
}

static bool quickChatLive(uint64_t nowUs)
{
    const uint64_t at = s_chatLiveUs.load(std::memory_order_relaxed);
    return at && nowUs - at < 250000;
}

// quick_chat_button: dpad_down (the default), dpad_up, triangle or select
int quickChatButton()
{
    static std::atomic<unsigned> s_generation{~0u};
    static std::atomic<int> s_button{DpadDown};
    const unsigned generation = pf::Config::generation();
    if (s_generation.load(std::memory_order_acquire) != generation)
    {
        static const char* const Names[] = { "dpad_down", "dpad_up", "triangle", "select" };
        static const int Buttons[] = { DpadDown, DpadUp, Triangle, -1 };
        s_button.store(Buttons[pf::Config::getChoice("quick_chat_button", Names, 4, 0)], std::memory_order_relaxed);
        s_generation.store(generation, std::memory_order_release);
    }
    return s_button.load(std::memory_order_relaxed);
}

// horn_button: rear_l3 (the default: the rear touch pad's lower left, none of the game's controls until bound),
// rear_r3, dpad_left, dpad_right or off
static int hornButton()
{
    static std::atomic<unsigned> s_generation{~0u};
    static std::atomic<int> s_button{RearL3};
    const unsigned generation = pf::Config::generation();
    if (s_generation.load(std::memory_order_acquire) != generation)
    {
        static const char* const Names[] = { "rear_l3", "rear_r3", "dpad_left", "dpad_right", "off" };
        static const int Buttons[] = { RearL3, RearR3, DpadLeft, DpadRight, -1 };
        s_button.store(Buttons[pf::Config::getChoice("horn_button", Names, 5, 0)], std::memory_order_relaxed);
        s_generation.store(generation, std::memory_order_release);
    }
    return s_button.load(std::memory_order_relaxed);
}

static std::atomic<bool> s_hornHeld{false};
static std::atomic<uint64_t> s_hornLiveUs{0};

void setHornLive(uint64_t nowUs)
{
    s_hornLiveUs.store(nowUs, std::memory_order_relaxed);
}

bool hornHeld()
{
    return s_hornHeld.load(std::memory_order_relaxed);
}

void quickChatSlots(Input slots[7])
{
    const int button = quickChatButton();
    if (button < 0)
    {
        // Select's chords: Up is Vita Settings, Triangle the keyboard
        static const Input Chords[7] = { DpadDown, DpadLeft, DpadRight, Cross, Circle, Square, R };
        memcpy(slots, Chords, sizeof(Chords));
        return;
    }
    static const Input Order[] = { DpadUp, DpadLeft, DpadRight, Cross, Circle, Square, DpadDown, Triangle };
    static const Input Friend[] = { Triangle, DpadDown };   // the seventh: Triangle, or Down where Triangle is the button
    int n = 0;
    for (Input input : Order)
        if (int(input) != button && n < 6)
            slots[n++] = input;
    slots[6] = button == Triangle ? Friend[1] : Friend[0];
}

static StickShape stickShape()
{
    static std::atomic<unsigned> s_generation{~0u};
    static std::atomic<float> s_deadZone{0.15f}, s_linear{0.6f};
    const unsigned generation = pf::Config::generation();
    if (s_generation.load(std::memory_order_acquire) != generation)
    {
        static const char* const Responses[] = { "standard", "linear", "precise" };
        static const float Linear[] = { 0.6f, 1.0f, 0.3f };
        const int percent = std::clamp(pf::Config::getInt("stick_deadzone", 15), 0, 40);
        const int response = pf::Config::getChoice("stick_response", Responses, 3, 0);
        s_deadZone.store(float(percent) / 100.0f, std::memory_order_relaxed);
        s_linear.store(Linear[response >= 0 && response < 3 ? response : 0], std::memory_order_relaxed);
        s_generation.store(generation, std::memory_order_release);
    }
    StickShape shape;
    shape.generation = generation;
    shape.deadZone = s_deadZone.load(std::memory_order_relaxed);
    shape.linear = s_linear.load(std::memory_order_relaxed);
    return shape;
}

// ---- Keys ----

// Order of enum Key.
static const KeyCode s_keyCodes[] =
{
    { 0x26, 0x48, true  },  // up
    { 0x28, 0x50, true  },  // down
    { 0x25, 0x4B, true  },  // left
    { 0x27, 0x4D, true  },  // right
    { 0x1B, 0x01, false },  // escape
    { 0x0D, 0x1C, false },  // enter
    { 0x10, 0x2A, false },  // left shift
    { 0x2E, 0x53, true  },  // delete
    { 0x70, 0x3B, false },  // F1
    { 0x71, 0x3C, false },  // F2
    { 0x72, 0x3D, false },  // F3
};
static_assert(std::size(s_keyCodes) == KeyCount, "s_keyCodes must list every Key");
static_assert(KeyCount <= 32, "keys are a 32-bit set");
static_assert(InputCount <= 32, "inputs are a 32-bit set");

// Set-1 scancodes of A..Z and 0..9.
static const x86::reg8 s_letterScancodes[26] =
{
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
};
static const x86::reg8 s_digitScancodes[10] = { 0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A };

const KeyCode& keyCode(Key key)
{
    return s_keyCodes[key];
}

const KeyCode& shiftKey()
{
    return s_keyCodes[KeyLeftShift];
}

bool characterKey(char c, KeyCode* key, bool* shift)
{
    *shift = c >= 'A' && c <= 'Z';
    if (c >= 'a' && c <= 'z')
        *key = KeyCode{ x86::reg32('A' + (c - 'a')), s_letterScancodes[c - 'a'], false };
    else if (*shift)
        *key = KeyCode{ x86::reg32(c), s_letterScancodes[c - 'A'], false };
    else if (c >= '0' && c <= '9')
        *key = KeyCode{ x86::reg32(c), s_digitScancodes[c - '0'], false };
    else if (c == ' ')
        *key = KeyCode{ 0x20, 0x39, false };
    else
        return false;
    return true;
}

// ---- Joystick ----

uint32_t buttonMask(Input input)
{
    uint32_t mask = 0;
    for (const ButtonMapping& mapping : s_buttons)
    {
        if (mapping.input == input)
            mask |= uint32_t(1) << mapping.button;
    }
    return mask;
}

// The adapter's four axes: X, Y (left stick), Z (right stick Y) and Rz (right stick X).
bool axisPresent(Axis axis)
{
    return axis == AxisX || axis == AxisY || axis == AxisZ || axis == AxisRz;
}

x86::reg32 presentAxisCount()
{
    return 4;
}

x86::reg32 joystickButtonCount()
{
    return s_buttonCount;
}

static const char* s_buttonNames[s_buttonCount] = {};
static const char* s_axisNames[AxisCount] = {};

void setObjectNames(const char* const* buttons, x86::reg32 buttonCount, const char* const* axes)
{
    for (x86::reg32 i = 0; i < x86::reg32(s_buttonCount); ++i)
        s_buttonNames[i] = buttons && i < buttonCount ? buttons[i] : nullptr;
    for (int i = 0; i < AxisCount; ++i)
        s_axisNames[i] = axes ? axes[i] : nullptr;
}

const char* buttonName(x86::reg32 button)
{
    return button < x86::reg32(s_buttonCount) ? s_buttonNames[button] : nullptr;
}

const char* axisName(Axis axis)
{
    return axis >= 0 && axis < AxisCount ? s_axisNames[axis] : nullptr;
}

// ---- Pad ----

#ifdef __vita__
struct PadButton
{
    uint32_t mask;
    Input    input;
};

static const PadButton s_padButtons[] =
{
    { SCE_CTRL_CROSS,    Cross     },
    { SCE_CTRL_CIRCLE,   Circle    },
    { SCE_CTRL_SQUARE,   Square    },
    { SCE_CTRL_TRIANGLE, Triangle  },
    { SCE_CTRL_LTRIGGER, L         },
    { SCE_CTRL_RTRIGGER, R         },
    { SCE_CTRL_SELECT,   Select    },
    { SCE_CTRL_START,    Start     },
    { SCE_CTRL_UP,       DpadUp    },
    { SCE_CTRL_DOWN,     DpadDown  },
    { SCE_CTRL_LEFT,     DpadLeft  },
    { SCE_CTRL_RIGHT,    DpadRight },
};

static bool s_touchReady = false;
static bool s_rearTouch = true;
static bool s_frontTouch = false;
static SceTouchPanelInfo s_panels[2];   // front, back

// rear_touch=off and front_touch (off by default) choose the panels read.
static void initTouch()
{
    s_touchReady = true;
    s_rearTouch = pf::Config::getBool("rear_touch", true);
    s_frontTouch = frontTouchEnabled();
    for (int port = 0; port < 2; ++port)
    {
        if (!(port ? s_rearTouch : s_frontTouch))
            continue;
        sceTouchSetSamplingState(port, SCE_TOUCH_SAMPLING_STATE_START);
        if (sceTouchGetPanelInfo(port, &s_panels[port]) < 0)
            s_panels[port] = SceTouchPanelInfo{};
        if (s_panels[port].maxAaX <= s_panels[port].minAaX || s_panels[port].maxAaY <= s_panels[port].minAaY)
        {
            s_panels[port].minAaX = 0;
            s_panels[port].minAaY = 0;
            s_panels[port].maxAaX = 1919;
            s_panels[port].maxAaY = 1087;
        }
    }
}

static float stickValue(unsigned char raw)
{
    return (float(raw) - 127.5f) / 127.5f;
}

PadSample readPad()
{
    if (!s_touchReady)
        initTouch();
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);

    PadSample sample;
    sample.inputs = 0;
    for (const PadButton& button : s_padButtons)
    {
        if (pad.buttons & button.mask)
            sample.inputs |= bit(button.input);
    }
    sample.sticks[0] = stickValue(pad.lx);
    sample.sticks[1] = stickValue(pad.ly);
    sample.sticks[2] = stickValue(pad.rx);
    sample.sticks[3] = stickValue(pad.ry);

    SceTouchData touch;
    if (s_rearTouch && sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1) >= 0)
    {
        const SceTouchPanelInfo& panel = s_panels[SCE_TOUCH_PORT_BACK];
        const int centreX = (panel.minAaX + panel.maxAaX) / 2, centreY = (panel.minAaY + panel.maxAaY) / 2;
        static const Input Quadrants[2][2] = { { RearL2, RearR2 }, { RearL3, RearR3 } };
        for (SceUInt32 i = 0; i < touch.reportNum && i < SCE_TOUCH_MAX_REPORT; ++i)
            sample.inputs |= bit(Quadrants[touch.report[i].y >= centreY][touch.report[i].x >= centreX]);
    }
    if (s_frontTouch && sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) >= 0)
    {
        const SceTouchPanelInfo& panel = s_panels[SCE_TOUCH_PORT_FRONT];
        for (SceUInt32 i = 0; i < touch.reportNum && i < SCE_TOUCH_MAX_REPORT && sample.touchCount < PadSample::MaxTouches; ++i)
        {
            const float x = float(touch.report[i].x - panel.minAaX) / float(panel.maxAaX - panel.minAaX);
            const float y = float(touch.report[i].y - panel.minAaY) / float(panel.maxAaY - panel.minAaY);
            sample.touches[sample.touchCount++] = { std::min(std::max(x, 0.0f), 1.0f), std::min(std::max(y, 0.0f), 1.0f),
                                                    int(touch.report[i].id) };
        }
    }
    return sample;
}
#else
namespace
{

std::mutex  s_padMutex;
PadSource   s_padSource = nullptr;
void*       s_padContext = nullptr;
PadSample   s_padState = { 0, { 0.0f, 0.0f, 0.0f, 0.0f } };

}

void setPadSource(PadSource source, void* context)
{
    std::lock_guard<std::mutex> guard(s_padMutex);
    s_padSource = source;
    s_padContext = context;
}

void setPadState(const PadSample& sample)
{
    std::lock_guard<std::mutex> guard(s_padMutex);
    s_padState = sample;
}

PadSample readPad()
{
    PadSource source;
    void* context;
    PadSample state;
    {
        std::lock_guard<std::mutex> guard(s_padMutex);
        source = s_padSource;
        context = s_padContext;
        state = s_padState;
    }
    return source ? source(context) : state;
}
#endif

bool frontTouchEnabled()
{
    return pf::cachedStatic([] { return pf::Config::getBool("front_touch", true); });
}

// ---- Pad script ----

static const char* const s_inputNames[InputCount] =
{
    "cross", "circle", "square", "triangle", "l", "r", "select", "start",
    "up", "down", "left", "right", "l2", "r2", "l3", "r3",
};

static bool parseInputs(const char* text, uint32_t* inputs)
{
    *inputs = 0;
    if (strcmp(text, "-") == 0)
        return true;
    while (*text)
    {
        const char* end = strchr(text, '+');
        const size_t length = end ? size_t(end - text) : strlen(text);
        int input = 0;
        while (input < InputCount && (strlen(s_inputNames[input]) != length ||
                                      strncmp(s_inputNames[input], text, length) != 0))
            ++input;
        if (input == InputCount)
            return false;
        *inputs |= bit(Input(input));
        text += length + (end ? 1 : 0);
    }
    return true;
}

bool PadScript::load(const char* path)
{
    m_entries.clear();
    FILE* file = fopen(path, "r");
    if (!file)
    {
        pf::logError("pad script %s: cannot open", path);
        return false;
    }
    char line[256];
    int number = 0;
    bool ok = true;
    while (ok && fgets(line, sizeof(line), file))
    {
        ++number;
        char* hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        unsigned long long step;
        char inputs[160];
        Entry entry = { 0, { 0, { 0.0f, 0.0f, 0.0f, 0.0f } } };
        float* sticks = entry.sample.sticks;
        PadSample::Touch& touch = entry.sample.touches[0];
        const int fields = sscanf(line, "%llu %159s %f %f %f %f %f %f", &step, inputs, &sticks[0], &sticks[1],
                                  &sticks[2], &sticks[3], &touch.x, &touch.y);
        touch.id = 0;
        entry.sample.touchCount = fields >= 8 && touch.x >= 0.0f ? 1 : 0;
        if (fields <= 0)
            continue;
        entry.step = step;
        ok = fields >= 2 && parseInputs(inputs, &entry.sample.inputs) &&
             (m_entries.empty() || m_entries.back().step <= entry.step);
        if (ok)
            m_entries.push_back(entry);
        else
            pf::logError("pad script %s:%d: expected \"<step> <inputs|-> [lx ly rx ry [tx ty]]\" in step order", path,
                         number);
    }
    fclose(file);
    if (!ok)
        m_entries.clear();
    return ok;
}

PadSample PadScript::sampleAt(uint64_t step) const
{
    PadSample sample = { 0, { 0.0f, 0.0f, 0.0f, 0.0f } };
    for (const Entry& entry : m_entries)
    {
        if (entry.step > step)
            break;
        sample = entry.sample;
    }
    return sample;
}

// ---- Mapper ----

static const Chord* findChord(Input input)
{
    for (const Chord& chord : s_chords)
    {
        if (chord.input == input)
            return &chord;
    }
    return nullptr;
}

Mapper::Mapper()
    :   m_previous(0)
    ,   m_chorded(0)
    ,   m_selectUsed(false)
    ,   m_selectPulseEndUs(0)
    ,   m_chatSinceUs(0)
    ,   m_chatUsed(false)
    ,   m_chatSwallow(0)
    ,   m_chatPulseEndUs(0)
    ,   m_steer(0.0f)
    ,   m_steerUs(0)
{
}

State Mapper::update(const PadSample& pad, uint64_t nowUs)
{
    State state;
    memset(&state, 0, sizeof(state));
    state.sayChord = -1;
    const uint32_t inputs = pad.inputs;
    const uint32_t pressed = inputs & ~m_previous;
    const uint32_t released = m_previous & ~inputs;
    const bool select = (inputs & bit(Select)) != 0;

    if (pressed & bit(Select))
        m_selectUsed = false;
    m_chorded &= inputs;
    const int chatButton = quickChatLive(nowUs) ? quickChatButton() : -2;   // -2: not in free roam now
    for (int input = 0; input < InputCount; ++input)
    {
        if (!(pressed & bit(Input(input))) || input == Select || !select)
            continue;
        const Chord* chord = findChord(Input(input));
        if (!chord || (chord->say >= 0 && chatButton != -1))  // (quick chat on another button, or not now: no chord)
            continue;
        m_chorded |= bit(Input(input));
        m_selectUsed = true;
        if (chord->ime)
            state.openIme = true;
        if (chord->settings)
            state.openSettings = true;
        if (chord->say >= 0)
            state.sayChord = chord->say;
    }
    if ((released & bit(Select)) && !m_selectUsed)
        m_selectPulseEndUs = nowUs + s_tapUs;
    m_previous = inputs;

    // Quick chat on a button of its own: held, it and the phrase buttons pressed with it stay from the game (a phrase
    // button until released); a short tap that picked nothing reaches the game afterwards, as a Select tap does.
    uint32_t chatHidden = 0, chatPulse = 0;
    m_chatSwallow &= inputs;
    if (chatButton >= 0)
    {
        const uint32_t key = bit(Input(chatButton));
        if (pressed & key)
        {
            m_chatSinceUs = nowUs;
            m_chatUsed = false;
        }
        if (inputs & key)
        {
            chatHidden = key;
            Input slots[7];
            quickChatSlots(slots);
            for (int i = 0; i < 7; ++i)
                if (pressed & bit(slots[i]))
                {
                    state.sayChord = i;
                    m_chatUsed = true;
                    m_chatSwallow |= bit(slots[i]);
                }
        }
        if ((released & key) && !m_chatUsed && nowUs - m_chatSinceUs < 400000)
            m_chatPulseEndUs = nowUs + s_tapUs;
        if (nowUs < m_chatPulseEndUs && !(inputs & key))
            chatPulse = key;
    }
    // (what the HUD shows: the choices while the button is held; Select's chords alike)
    const bool chatHeld = (chatButton >= 0 && (inputs & bit(Input(chatButton)))) || (chatButton == -1 && select);
    if (!chatHeld)
        s_chatHeldSince.store(0, std::memory_order_relaxed);
    else if (!s_chatHeldSince.load(std::memory_order_relaxed))
        s_chatHeldSince.store(nowUs ? nowUs : 1, std::memory_order_relaxed);
    // the horn: its button held in free roam proper (not while quick chat's is: Left and Right pick phrases then)
    const uint64_t hornAt = s_hornLiveUs.load(std::memory_order_relaxed);
    const int hornBtn = hornAt && nowUs - hornAt < 250000 && !chatHeld ? hornButton() : -1;
    const bool horn = hornBtn >= 0 && hornBtn != chatButton && (inputs & bit(Input(hornBtn))) && !(m_chorded & bit(Input(hornBtn)));
    s_hornHeld.store(horn, std::memory_order_relaxed);
    const uint32_t hornHidden = horn ? bit(Input(hornBtn)) : 0;
    const uint32_t normal = (inputs & ~m_chorded & ~chatHidden & ~m_chatSwallow & ~hornHidden) | chatPulse;
    // steering=digital: the left stick is left, centre or right, and its X axis ramps to full lock and back as a
    // keyboard's steering does (the D-pad is not involved).
    const bool digital = digitalSteering();
    float steerX = pad.sticks[0];
    if (digital)
    {
        const float target = pad.sticks[0] <= -s_digitalThreshold ? -1.0f
                           : pad.sticks[0] >= s_digitalThreshold ? 1.0f : 0.0f;
        const float seconds = m_steerUs ? std::fmin(0.1f, float(nowUs - m_steerUs) / 1e6f) : 0.0f;
        // Full lock in s_digitalTurnS; back to the centre, or across it, faster.
        const bool returning = target == 0.0f || target * m_steer < 0.0f;
        const float step = seconds / (returning ? s_digitalReturnS : s_digitalTurnS);
        m_steer = target > m_steer ? std::fmin(target, m_steer + step) : std::fmax(target, m_steer - step);
        steerX = m_steer;
    }
    else
        m_steer = 0.0f;
    m_steerUs = nowUs;
    for (const ButtonMapping& mapping : s_buttons)
    {
        if (normal & bit(mapping.input))
            state.buttons |= uint32_t(1) << mapping.button;
    }
    for (const KeyMapping& mapping : s_keys)
    {
        if (normal & bit(mapping.input))
            state.keys |= mapping.keys;
    }
    for (const Chord& chord : s_chords)
    {
        if (!(m_chorded & bit(chord.input)))
            continue;
        state.keys |= chord.keys;
        if (chord.button >= 0)
            state.buttons |= uint32_t(1) << chord.button;
    }
    if (nowUs < m_selectPulseEndUs)
        state.buttons |= uint32_t(1) << s_selectTapButton;

    state.axes[AxisX] = steerX;
    state.axes[AxisY] = pad.sticks[1];
    const uint64_t cameraAt = s_cameraLiveUs.load(std::memory_order_relaxed);
    const bool camera = cameraAt && nowUs - cameraAt < 250000;    // the stick camera has the right stick
    state.axes[AxisZ] = camera ? 0.0f : pad.sticks[3];
    state.axes[AxisRz] = camera ? 0.0f : pad.sticks[2];
    state.pov = -1;
    return state;
}

// ---- Stick shaping ----

void shapeStick(float x, float y, uint32_t deadZone, uint32_t saturation, float* outX, float* outY)
{
    const StickShape shape = stickShape();
    const float dead = std::fmax(shape.deadZone, float(deadZone) / 10000.0f);
    const float full = std::fmin(s_stickSaturation, float(saturation) / 10000.0f);
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= dead)
    {
        *outX = 0.0f;
        *outY = 0.0f;
        return;
    }
    const float t = full > dead ? std::fmin(1.0f, (magnitude - dead) / (full - dead)) : 1.0f;
    const float scale = (shape.linear * t + (1.0f - shape.linear) * t * t * t) / magnitude;
    *outX = std::fmax(-1.0f, std::fmin(1.0f, x * scale));
    *outY = std::fmax(-1.0f, std::fmin(1.0f, y * scale));
}

}}
