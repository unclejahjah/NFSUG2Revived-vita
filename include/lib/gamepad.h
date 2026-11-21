#ifndef LIB_GAMEPAD_H_
#define LIB_GAMEPAD_H_

#include <lib/controls.h>
#include <lib/winapp.h>

namespace win32
{

enum InputType
{
    InputType_Keyboard,
    InputType_Mouse,
    InputType_Gamepad
};

// A DirectInput device.
class Input : public GenericResource
{
public:
    virtual x86::reg32 getButtonCount() const;
    virtual x86::reg32 getAxesCount() const;
    // DIDEVICEINSTANCE / DIDEVCAPS dwDevType, as DirectInput 7 and as DirectInput 8 report it.
    virtual x86::reg32 getDeviceType() const = 0;
    virtual x86::reg32 getDeviceType8() const = 0;
};

// The synthesized keyboard: the keys the pad holds (controls.cpp) and text typed from the on-screen keyboard.
class Keyboard : public Input
{
public:
    virtual x86::reg32 getButtonCount() const;
    virtual x86::reg32 getDeviceType() const;
    virtual x86::reg32 getDeviceType8() const;
    // GetDeviceState: 0x80 at the DirectInput code of each key down.
    static void getState(x86::reg8 (&keys)[256]);
};

// No mouse is attached: it reports no motion and no buttons.
class Mouse : public Input
{
public:
    virtual x86::reg32 getButtonCount() const;
    virtual x86::reg32 getAxesCount() const;
    virtual x86::reg32 getDeviceType() const;
    virtual x86::reg32 getDeviceType8() const;
};

// The Vita controller as DirectInput joystick 0; layout in controls.cpp.
class Gamepad : public Input
{
public:
    // DirectInput axis properties (DIPROP_RANGE, DIPROP_DEADZONE, DIPROP_SATURATION).
    struct AxisProperties
    {
        x86::sreg32 min;
        x86::sreg32 max;
        x86::reg32  deadZone;   // 0..10000 of the distance from the centre
        x86::reg32  saturation;
    };

    explicit Gamepad(x86::reg32 gamepadIndex);
    ~Gamepad();

    virtual x86::reg32 getButtonCount() const;
    virtual x86::reg32 getAxesCount() const;
    virtual x86::reg32 getDeviceType() const;
    virtual x86::reg32 getDeviceType8() const;
    x86::reg32 index() const { return m_index; }

    static x86::reg32 getCount();

    // Guest threads with the execution lock held.
    AxisProperties& axisProperties(controls::Axis axis) { return m_axisProperties[axis]; }

    // Joystick buttons and axes now, axes shaped with this device's properties and scaled to their ranges.
    void getState(x86::reg32* buttons, x86::sreg32 (&axes)[controls::AxisCount], int* pov);

    // Game code run at each getState on the guest thread (execution lock held) with the buttons and hat about to
    // be reported; it may change them. Set once before the game starts.
    typedef void (*PollHook)(x86::reg32* buttons, int* pov);
    static void setPollHook(PollHook hook);

    // Synthetic pad merged into every reading: inputs ORed, nonzero sticks and a touch replace. Any thread.
    static void setInjectedPad(const controls::PadSample& pad);

    // Right stick of the last pad reading, injected pad included: -1..1, y down. Any thread.
    static void getRightStick(float* x, float* y);

    // Front touch as the mouse: last position (0..1 of the pointer area) and the left button.
    struct Touch
    {
        float x;
        float y;
        bool  down;
    };
    // False until a touch. button: latched presses and releases. Any thread.
    static bool getTouch(Touch* touch, bool button = false);

    // Display 0..1 to pointer area 0..1; false outside.
    typedef bool (*TouchMap)(float* x, float* y);
    // Without one, the area is the whole display. Any thread.
    static void setTouchMap(TouchMap map);

    // Input pump: reads the pad, posts the key changes as window messages and runs the on-screen keyboard. Called
    // by the message loop, at each presented frame and at each poll. Any thread.
    static void updateKeys();

    // A text field of the game is being edited and holds text (ASCII; any thread; called again while it lasts). The
    // first call of a field opens the on-screen keyboard with that text. Confirmed text replaces the field's (End,
    // one Backspace per character, then letters, digits and extraChars, at most maxLength) and ends with Enter. A
    // cancel, or Circle while the keyboard is closed, types Escape when the field is cancellable, else empties the
    // field and types Enter. Cross while the keyboard is closed opens it again.
    static void editText(const char* title, const char* text, uint32_t maxLength, const char* extraChars,
                         bool cancellable);
    // Host code's own text entry (the options menu): the on-screen keyboard opens at the next pump with that text
    // (none when secret, a password box then) and the pad holds idle while it is up, as for a field. False while
    // a keyboard or a field is in use. takeText gives the result once, on any thread.
    enum TextResult { TextNone, TextPending, TextConfirmed, TextCancelled };
    static bool requestText(const char* title, const char* text, uint32_t maxLength, bool secret);
    static TextResult takeText(char* text, size_t size);
    // For an editor that only appends and deletes at its end: clearing a field skips the End key.
    static void setTextAppendOnly(bool appendOnly);
    // Ends the edit of the field: a keyboard still open for it closes without typing.
    static void endTextEdit();

private:
    x86::reg32      m_index;
    AxisProperties  m_axisProperties[controls::AxisCount];
};

}

#endif
