#include <lib/gamepad.h>
#include <lib/window.h>
#include <platform/ime.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>

namespace win32
{

namespace
{

// Typed text goes out one key message per this interval, so the game's 64-entry key queue never overflows.
const uint64_t TypingIntervalUs = 20000;
// The longest text typed outside a known field (the pilot name holds 19 characters).
const uint32_t TextLength = 20;

// A key message, or a WM_CHAR when character is set.
struct TypedKey
{
    controls::KeyCode   key;
    bool                down;
    char                character;
};

// The text field the game edits (editText).
struct Field
{
    bool        active;
    bool        shown;          // the keyboard was opened for it
    char16_t    title[pf::ime::MaxTitleLength + 1];
    char16_t    text[pf::ime::MaxTextLength + 1];
    uint32_t    length;         // characters in text
    uint32_t    maxLength;
    char        extraChars[64]; // accepted besides letters and digits
    bool        cancellable;    // Escape cancels it
};

// Characters the game's line editor takes as commands.
const char CharBackspace = 0x08;
const char CharEnter = 0x0D;
const char CharEscape = 0x1B;
// End moves the line editor's cursor behind the text.
const controls::KeyCode KeyEnd = { 0x23, 0x4F, true };

// The pump runs on the message thread, at frames and in polls; s_mutex guards everything below.
pf::LwMutex             s_mutex("input");
controls::Mapper        s_mapper;
controls::State         s_state;
uint32_t                s_keysDown;         // controls::Key bits posted down
bool                    s_imeOpen;          // on-screen keyboard requested or shown
bool                    s_imeForField;      // ... for s_field
bool                    s_imeForHost;       // ... for requestText
bool                    s_hostPending;      // requestText asked, keyboard not opened yet
char16_t                s_hostTitle[pf::ime::MaxTitleLength + 1];
char16_t                s_hostText[pf::ime::MaxTextLength + 1];
uint32_t                s_hostMaxLength;
bool                    s_hostSecret;
int                     s_hostResult;       // Gamepad::TextResult
char                    s_hostTyped[pf::ime::MaxTextLength + 1];
uint32_t                s_releaseWait;      // buttons held at the end of the keyboard and its typing, until released
uint32_t                s_padInputs;        // inputs of the last pump
Field                   s_field;
std::deque<TypedKey>    s_typing;           // key messages of confirmed text not posted yet
uint64_t                s_typedUs;          // when the last one was posted
bool                    s_typedDown[256];   // typed keys down, by DirectInput code
bool                    s_appendOnly;       // the game's editor has no cursor: no End before clearing
Gamepad::PollHook       s_pollHook;         // not guarded: set before the game starts
std::atomic<bool>       s_settingsWanted(false);
std::atomic<int>        s_sayWanted(-1);
std::atomic<float>      s_rightStick[2];    // the last pump's right stick
controls::PadSample     s_injected;         // setInjectedPad
Gamepad::Touch          s_touch;            // getTouch
bool                    s_touchSeen;
int                     s_touchId = -1;     // the finger driving the mouse, -1 for none
int                     s_ignored[controls::PadSample::MaxTouches];     // fingers left out until they lift
int                     s_ignoredCount;
bool                    s_pressPending;     // a press or release not yet read by getTouch(button)
bool                    s_releasePending;
bool                    s_buttonReader;     // getTouch(button) has been called: presses and releases are kept
Gamepad::TouchMap       s_touchMap;
std::atomic<bool>       s_padHeld(false);   // setPadHeld

// Under s_mutex: one finger drives the mouse until it lifts.
void trackTouch(const controls::PadSample& pad, bool busy)
{
    const controls::PadSample::Touch* tracked = nullptr;
    for (int i = 0; i < pad.touchCount; ++i)
        if (pad.touches[i].id == s_touchId)
            tracked = &pad.touches[i];
    const bool released = s_touchId >= 0 && (!tracked || busy);
    if (released)
    {
        tracked = nullptr;
        s_touchId = -1;
        s_releasePending = s_buttonReader;
    }
    int ignored[controls::PadSample::MaxTouches];
    int ignoredCount = 0;
    for (int i = 0; i < pad.touchCount; ++i)
    {
        const controls::PadSample::Touch& touch = pad.touches[i];
        bool skip = std::find(s_ignored, s_ignored + s_ignoredCount, touch.id) != s_ignored + s_ignoredCount;
        float x = touch.x, y = touch.y;
        const bool inside = !s_touchMap || s_touchMap(&x, &y);
        if (&touch != tracked)
        {
            skip = skip || tracked || busy || s_pressPending || s_releasePending || !inside;
            if (skip)
            {
                ignored[ignoredCount++] = touch.id;
                continue;
            }
            tracked = &touch;
            s_touchId = touch.id;
            s_pressPending = s_buttonReader;
        }
        s_touch.x = std::min(std::max(x, 0.0f), 1.0f);
        s_touch.y = std::min(std::max(y, 0.0f), 1.0f);
        s_touchSeen = true;
    }
    std::copy(ignored, ignored + ignoredCount, s_ignored);
    s_ignoredCount = ignoredCount;
    s_touch.down = tracked != nullptr;
}

void copyText(char16_t* target, const char* source, uint32_t maxLength)
{
    uint32_t length = 0;
    for (; source[length] && length < maxLength; ++length)
        target[length] = char16_t(uint8_t(source[length]));
    target[length] = 0;
}

void postKey(const controls::KeyCode& key, bool down)
{
    Window::pushKey(key.vk, key.scancode, key.extended, down);
}

// Posts the changes from s_keysDown to held: releases in reverse key order, then presses in key order.
void postKeys(uint32_t held)
{
    const uint32_t released = s_keysDown & ~held;
    const uint32_t pressed = held & ~s_keysDown;
    for (int key = controls::KeyCount - 1; key >= 0; --key)
    {
        if (released & (uint32_t(1) << key))
            postKey(controls::keyCode(controls::Key(key)), false);
    }
    for (int key = 0; key < controls::KeyCount; ++key)
    {
        if (pressed & (uint32_t(1) << key))
            postKey(controls::keyCode(controls::Key(key)), true);
    }
    s_keysDown = held;
}

void queueKey(const controls::KeyCode& key)
{
    s_typing.push_back(TypedKey{ key, true, 0 });
    s_typing.push_back(TypedKey{ key, false, 0 });
}

void queueChar(char character)
{
    s_typing.push_back(TypedKey{ controls::KeyCode{ 0, 0, false }, false, character });
}

// Empties s_field: cursor to the end, then one Backspace per character.
void queueClear()
{
    if (!s_appendOnly)
        queueKey(KeyEnd);
    for (uint32_t i = 0; i < s_field.length; ++i)
        queueChar(CharBackspace);
}

// Types confirmed text as one WM_CHAR per character (the game's line editor reads only WM_CHAR), at most maxLength
// characters, and ends it with a carriage return, which commits the field. Into a field it replaces the field's text
// and keeps only the characters the field takes.
void queueText(const char* text, uint32_t maxLength, bool field)
{
    if (field)
        queueClear();
    uint32_t typed = 0;
    for (const char* c = text; *c && typed < maxLength; ++c)
    {
        const bool alphanumeric = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9');
        const bool printable = x86::reg8(*c) >= 0x20 && x86::reg8(*c) < 0x7f;
        const bool allowed = printable && (!field || alphanumeric || strchr(s_field.extraChars, *c));
        if (!allowed)
        {
            pf::logInfo("input: '%c' cannot be typed", *c);
            continue;
        }
        queueChar(*c);
        ++typed;
    }
    queueChar(CharEnter);
}

// The game cancels a field on Escape. A field that refuses that is emptied and committed instead, which the game
// treats as no text entered.
void queueCancel()
{
    if (s_field.cancellable)
    {
        queueChar(CharEscape);
        return;
    }
    queueClear();
    queueChar(CharEnter);
}

void openIme(bool forField)
{
    if (forField)
        s_imeOpen = pf::ime::requestOpen(s_field.title, s_field.text, s_field.maxLength);
    else
        s_imeOpen = pf::ime::requestOpen(u"Text", u"", TextLength);
    s_imeForField = s_imeOpen && forField;
    s_imeForHost = false;
}

void updateIme()
{
    char text[pf::ime::MaxTextLength + 1];
    const pf::ime::Result result = pf::ime::takeResult(text, sizeof(text));
    if (result == pf::ime::ResultPending)
        return;
    const bool field = s_imeForField;
    const bool host = s_imeForHost;
    s_imeOpen = false;
    s_imeForField = false;
    s_imeForHost = false;
    if (host)
    {
        if (result == pf::ime::ResultConfirmed)
            snprintf(s_hostTyped, sizeof(s_hostTyped), "%s", text);
        s_hostResult = result == pf::ime::ResultConfirmed ? Gamepad::TextConfirmed : Gamepad::TextCancelled;
        return;
    }
    if (field && !s_field.active)
        return;     // the field closed meanwhile
    if (result == pf::ime::ResultConfirmed)
        queueText(text, field ? s_field.maxLength : TextLength, field);
    else if (result == pf::ime::ResultCancelled && field)
        queueCancel();
}

void typeNext(uint64_t now)
{
    if (s_typing.empty() || now - s_typedUs < TypingIntervalUs)
        return;
    const TypedKey typed = s_typing.front();
    s_typing.pop_front();
    if (typed.character)
    {
        Window::pushChar(x86::reg8(typed.character));
    }
    else if (typed.key.vk)   // (none: a wait, tapKey)
    {
        s_typedDown[typed.key.dik()] = typed.down;
        postKey(typed.key, typed.down);
    }
    s_typedUs = now;
}

// Under s_mutex. While the on-screen keyboard is up or its text is being typed, and until the buttons held at its end
// are released (a button that closed the keyboard does not reach the game), the pad holds no keys or buttons. The
// rear touch halves are not waited for.
// While a field is edited with the keyboard closed, Cross opens it and Circle cancels the field; neither reaches the
// game then.
void pump()
{
    const uint64_t now = pf::ticksUs();
    controls::PadSample pad = controls::readPad();
    pad.inputs |= s_injected.inputs;
    for (int i = 0; i < 4; ++i)
        if (s_injected.sticks[i] != 0.0f)
            pad.sticks[i] = s_injected.sticks[i];
    s_rightStick[0] = pad.sticks[2];
    s_rightStick[1] = pad.sticks[3];
    if (!controls::frontTouchEnabled())
        pad.touchCount = 0;
    else if (s_injected.touchCount)
    {
        memcpy(pad.touches, s_injected.touches, sizeof(pad.touches));
        pad.touchCount = s_injected.touchCount;
    }
    const uint32_t pressed = pad.inputs & ~s_padInputs;
    s_padInputs = pad.inputs;
    controls::State state = s_mapper.update(pad, now);
    if (s_imeOpen || !s_typing.empty())
        s_releaseWait = pad.inputs & ~(controls::bit(controls::RearL2) | controls::bit(controls::RearR2) |
                                       controls::bit(controls::RearL3) | controls::bit(controls::RearR3));
    else
        s_releaseWait &= pad.inputs;
    const bool busy = s_imeOpen || !s_typing.empty() || s_releaseWait != 0 || s_padHeld;
    trackTouch(pad, busy);
    if (!busy && state.openSettings)
        s_settingsWanted = true;
    if (!busy && state.sayChord >= 0)
        s_sayWanted = state.sayChord;
    if (!busy && s_hostPending)
    {
        s_hostPending = false;
        s_imeOpen = pf::ime::requestOpen(s_hostTitle, s_hostText, s_hostMaxLength, s_hostSecret);
        s_imeForField = false;
        s_imeForHost = s_imeOpen;
        if (!s_imeOpen)
            s_hostResult = Gamepad::TextCancelled;
    }
    else if (!busy && s_field.active && !s_field.shown)
    {
        openIme(true);
        s_field.shown = s_imeOpen;
    }
    else if (!busy && state.openIme)
    {
        openIme(s_field.active);
    }
    else if (!busy && s_field.active && (pressed & controls::bit(controls::Circle)))
    {
        queueCancel();
    }
    else if (!busy && s_field.active && (pressed & controls::bit(controls::Cross)))
    {
        openIme(true);
    }
    if (s_field.active)
        state.buttons &= ~(controls::buttonMask(controls::Circle) | controls::buttonMask(controls::Cross));
    if (busy)
    {
        state.buttons = 0;
        state.keys = 0;
        state.pov = -1;
        memset(state.axes, 0, sizeof(state.axes));
    }
    postKeys(s_field.active ? state.keys : 0);
    if (s_imeOpen)
        updateIme();
    typeNext(now);
    s_state = state;
}

}

x86::reg32 Input::getButtonCount() const
{
    return 0;
}

x86::reg32 Input::getAxesCount() const
{
    return 0;
}

// DIDEVTYPE_KEYBOARD, DIDEVTYPEKEYBOARD_PCENH.
x86::reg32 Keyboard::getDeviceType() const
{
    return 0x0403;
}

// DI8DEVTYPE_KEYBOARD, DI8DEVTYPEKEYBOARD_PCENH.
x86::reg32 Keyboard::getDeviceType8() const
{
    return 0x0413;
}

x86::reg32 Keyboard::getButtonCount() const
{
    return 128;
}

void Keyboard::getState(x86::reg8 (&keys)[256])
{
    pf::LwLockGuard lock(s_mutex);
    pump();
    memset(keys, 0, sizeof(keys));
    for (int key = 0; key < controls::KeyCount; ++key)
    {
        if (s_keysDown & (uint32_t(1) << key))
            keys[controls::keyCode(controls::Key(key)).dik()] = 0x80;
    }
    for (int code = 0; code < 256; ++code)
    {
        if (s_typedDown[code])
            keys[code] = 0x80;
    }
}

// DIDEVTYPE_MOUSE, DIDEVTYPEMOUSE_TRADITIONAL.
x86::reg32 Mouse::getDeviceType() const
{
    return 0x0202;
}

// DI8DEVTYPE_MOUSE, DI8DEVTYPEMOUSE_TRADITIONAL.
x86::reg32 Mouse::getDeviceType8() const
{
    return 0x0212;
}

x86::reg32 Mouse::getButtonCount() const
{
    return 3;
}

x86::reg32 Mouse::getAxesCount() const
{
    return 3;
}

Gamepad::Gamepad(x86::reg32 gamepadIndex)
    :   m_index(gamepadIndex)
{
    // DirectInput defaults: range 0..65535, no dead zone, saturation at full deflection.
    for (AxisProperties& axis : m_axisProperties)
        axis = { 0, 65535, 0, 10000 };
}

Gamepad::~Gamepad()
{
}

x86::reg32 Gamepad::getCount()
{
    return 1;
}

x86::reg32 Gamepad::getButtonCount() const
{
    return controls::joystickButtonCount();
}

x86::reg32 Gamepad::getAxesCount() const
{
    return controls::presentAxisCount();
}

x86::reg32 Gamepad::getDeviceType() const
{
    return controls::JoystickDeviceType;
}

// DI8DEVTYPE_GAMEPAD, DI8DEVTYPEGAMEPAD_STANDARD.
x86::reg32 Gamepad::getDeviceType8() const
{
    return 0x0215;
}

// Each stick is shaped as a whole: the left with lX's properties, the right (Rz, Z) with Rz's; absent axes centred.
void Gamepad::getState(x86::reg32* buttons, x86::sreg32 (&axes)[controls::AxisCount], int* pov)
{
    controls::State state;
    {
        pf::LwLockGuard lock(s_mutex);
        pump();
        state = s_state;
    }
    float shaped[controls::AxisCount] = {};
    const AxisProperties& left = m_axisProperties[controls::AxisX];
    controls::shapeStick(state.axes[controls::AxisX], state.axes[controls::AxisY], left.deadZone, left.saturation,
                         &shaped[controls::AxisX], &shaped[controls::AxisY]);
    const AxisProperties& right = m_axisProperties[controls::AxisRz];
    controls::shapeStick(state.axes[controls::AxisRz], state.axes[controls::AxisZ], right.deadZone, right.saturation,
                         &shaped[controls::AxisRz], &shaped[controls::AxisZ]);
    *pov = state.pov;
    *buttons = state.buttons;
    if (s_pollHook)
        s_pollHook(buttons, pov);
    for (int axis = 0; axis < controls::AxisCount; ++axis)
    {
        const AxisProperties& properties = m_axisProperties[axis];
        const float position = (shaped[axis] + 1.0f) * 0.5f;
        const float range = float(properties.max) - float(properties.min);
        axes[axis] = x86::sreg32(std::lround(float(properties.min) + position * range));
    }
}

void Gamepad::tapKey(const controls::KeyCode& key)
{
    pf::LwLockGuard lock(s_mutex);
    // held for four intervals (80 ms), past the game's next poll of the keyboard at any frame rate it runs at; the
    // entries between are waits (no key, no character), so no key repeat is posted
    s_typing.push_back(TypedKey{ key, true, 0 });
    for (int i = 0; i < 3; ++i)
        s_typing.push_back(TypedKey{ controls::KeyCode{ 0, 0, false }, false, 0 });
    s_typing.push_back(TypedKey{ key, false, 0 });
}

void Gamepad::setPadHeld(bool held)
{
    s_padHeld = held;
}

void Gamepad::setPollHook(PollHook hook)
{
    s_pollHook = hook;
}

void Gamepad::setInjectedPad(const controls::PadSample& pad)
{
    pf::LwLockGuard lock(s_mutex);
    s_injected = pad;
}

void Gamepad::getRightStick(float* x, float* y)
{
    *x = s_rightStick[0];
    *y = s_rightStick[1];
}

bool Gamepad::getTouch(Touch* touch, bool button)
{
    if (!controls::frontTouchEnabled())
        return false;
    pf::LwLockGuard lock(s_mutex);
    if (!button)
    {
        *touch = s_touch;
        return s_touchSeen;
    }
    s_buttonReader = true;
    pump();
    *touch = s_touch;
    if (s_pressPending)
        touch->down = true;
    else if (s_releasePending)
        touch->down = false;
    if (!s_pressPending)
        s_releasePending = false;
    s_pressPending = false;
    return s_touchSeen;
}

void Gamepad::setTouchMap(TouchMap map)
{
    pf::LwLockGuard lock(s_mutex);
    s_touchMap = map;
}

void Gamepad::updateKeys()
{
    pf::LwLockGuard lock(s_mutex);
    pump();
}

void Gamepad::editText(const char* title, const char* text, uint32_t maxLength, const char* extraChars,
                       bool cancellable)
{
    pf::LwLockGuard lock(s_mutex);
    if (!s_field.active)
    {
        s_field.active = true;
        s_field.shown = false;
        pf::logInfo("input: text field \"%s\" \"%s\" (%u characters, also \"%s\"%s)", title, text, unsigned(maxLength),
                    extraChars, cancellable ? "" : ", no cancel");
    }
    copyText(s_field.title, title, pf::ime::MaxTitleLength);
    copyText(s_field.text, text, pf::ime::MaxTextLength);
    s_field.length = uint32_t(strnlen(text, pf::ime::MaxTextLength));
    s_field.maxLength = maxLength;
    snprintf(s_field.extraChars, sizeof(s_field.extraChars), "%s", extraChars);
    s_field.cancellable = cancellable;
}

bool Gamepad::requestText(const char* title, const char* text, uint32_t maxLength, bool secret)
{
    pf::LwLockGuard lock(s_mutex);
    if (s_imeOpen || s_hostPending || s_field.active)
        return false;
    copyText(s_hostTitle, title, pf::ime::MaxTitleLength);
    copyText(s_hostText, secret ? "" : text, pf::ime::MaxTextLength);
    s_hostMaxLength = maxLength;
    s_hostSecret = secret;
    s_hostResult = TextPending;
    s_hostTyped[0] = 0;
    s_hostPending = true;
    return true;
}

bool Gamepad::takeSettingsRequest()
{
    return s_settingsWanted.exchange(false);
}

int Gamepad::takeSayChord()
{
    return s_sayWanted.exchange(-1);
}

Gamepad::TextResult Gamepad::takeText(char* text, size_t size)
{
    pf::LwLockGuard lock(s_mutex);
    const int result = s_hostResult;
    if (result == TextPending || result == TextNone)
        return TextResult(result);
    if (text && size)
        snprintf(text, size, "%s", result == TextConfirmed ? s_hostTyped : "");
    memset(s_hostTyped, 0, sizeof(s_hostTyped));
    s_hostResult = TextNone;
    return TextResult(result);
}

void Gamepad::setTextAppendOnly(bool appendOnly)
{
    pf::LwLockGuard lock(s_mutex);
    s_appendOnly = appendOnly;
}

void Gamepad::endTextEdit()
{
    pf::LwLockGuard lock(s_mutex);
    if (!s_field.active)
        return;
    s_field.active = false;
    if (s_imeForField)
        pf::ime::requestClose();
    pf::logInfo("input: text field closed");
}

}
