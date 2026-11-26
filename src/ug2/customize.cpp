// The Customize screen as data: see customize.h.

#include "customize.h"

#include <cmath>

namespace ug2 { namespace customize {

namespace {

namespace data = controlsdata;

const char* const Tokens[data::ControlCount] =
{
    "BIND_TRIANGLE", "BIND_CIRCLE", "BIND_CROSS", "BIND_SQUARE", "BIND_L", "BIND_R", "BIND_DPAD_UP", "BIND_DPAD_DOWN",
    "BIND_DPAD_LEFT", "BIND_DPAD_RIGHT", "BIND_LSTICK", "BIND_RSTICK", "BIND_REAR_UL", "BIND_REAR_UR", "BIND_REAR_LL",
    "BIND_REAR_LR",
};

// PS2 button index to our control, -1 where none binds.
const int ButtonControls[data::ButtonCount] =
{
    data::CtlTriangle, data::CtlCircle, data::CtlCross, data::CtlSquare, data::CtlRearL2, data::CtlRearR2, data::CtlL,
    data::CtlR, -1, -1, data::CtlRearL3, data::CtlRearR3, data::CtlDpadUp, data::CtlDpadRight, data::CtlDpadDown,
    data::CtlDpadLeft,
};

const char* const ButtonNames[data::ButtonCount] = { "Triangle", "Circle", "Cross", "Square", "L2", "R2", "L1", "R1", "Select",
    "Start", "L3", "R3", "d-pad up", "d-pad right", "d-pad down", "d-pad left" };

const char* const InputNames[win32::controls::InputCount] = { "Cross", "Circle", "Square", "Triangle", "L", "R", "Select",
    "Start", "up", "down", "left", "right", "rear L2", "rear R2", "rear L3", "rear R3" };

const char* const StickNames[2] = { "left stick", "right stick" };

bool worldEntry(Entry e)
{
    return e == data::EntryEnterShop || e == data::EntryWorldMap || e == data::EntryReadSms || e == data::EntryCareerStats;
}

float magnitude(const float sticks[4], int side)
{
    return std::sqrt(sticks[2 * side] * sticks[2 * side] + sticks[2 * side + 1] * sticks[2 * side + 1]);
}

bool takesStick(Entry entry, int side)
{
    const data::Binds none = {};
    return data::entryTakes(none, entry, side ? data::CtlRightStick : data::CtlLeftStick);
}

std::string buttonName(int button)
{
    return "button " + std::to_string(button) + " (" + (button >= 0 && button < data::ButtonCount ? ButtonNames[button] : "none") + ")";
}

std::string axisName(int32_t axisId)
{
    return axisId == data::AxisX ? "left stick X" : axisId == data::AxisY ? "left stick Y" : axisId == data::AxisZ ? "right stick Y" :
           axisId == data::AxisRz ? "right stick X" : "absent axis " + std::to_string(axisId);
}

// Already decided: later finds of the same frame change nothing.
bool decided(const Wait* wait, const std::string& what, std::string* why)
{
    if (wait->result == Ignore)
        return false;
    *why = what + ": ignored, the wait is already decided by " + wait->found;
    return true;
}

}

std::string entryValue(const data::Binds& binds, Entry entry)
{
    const unsigned mask = data::controlsOf(binds, entry);
    std::string value;
    for (int c = 0; c < data::ControlCount; ++c)
    {
        if (mask & (1u << c))
            value += std::string("$") + Tokens[c] + "$";
    }
    if (value.empty() && worldEntry(entry))
        value = "Touchscreen";
    return value;
}

uint32_t armAction(Entry entry)
{
    const uint32_t action = data::entryAction(entry);
    return action ? action : SpareAction;
}

Verdict onButton(Entry entry, int button, Control* control)
{
    if (button == ButtonStart)
        return Cancel;
    if (button < 0 || button >= data::ButtonCount || ButtonControls[button] < 0 || entry == data::EntryFreeLook)
        return Ignore;
    *control = Control(ButtonControls[button]);
    return Bind;
}

bool touchReachesGame(bool inputHeld)
{
    return !inputHeld;
}

bool watchesSticks(Entry entry)
{
    return takesStick(entry, 0) || takesStick(entry, 1);
}

Verdict onSticks(StickWatch* watch, Entry entry, const float sticks[4], Control* control)
{
    Verdict verdict = Ignore;
    for (int side = 0; side < 2; ++side)
    {
        const float m = magnitude(sticks, side);
        const bool centred = watch->centred[side] || m < StickOff;
        watch->centred[side] = centred;
        watch->pushed[side] = m > StickOn;
        if (verdict == Ignore && takesStick(entry, side) && centred && m > StickOn)
        {
            *control = side ? data::CtlRightStick : data::CtlLeftStick;
            verdict = Bind;
        }
    }
    return verdict;
}

std::string inputNames(uint32_t inputs)
{
    std::string names;
    for (int i = 0; i < win32::controls::InputCount; ++i)
    {
        if (inputs & (1u << i))
            names += (names.empty() ? "" : "+") + std::string(InputNames[i]);
    }
    return names.empty() ? "nothing" : names;
}

void beginWait(Wait* wait, Entry entry, uint32_t inputs)
{
    *wait = Wait();
    wait->entry = entry;
    wait->lastInputs = inputs;
}

Verdict waitPad(Wait* wait, uint32_t inputs, const float sticks[4], std::string* why)
{
    namespace in = win32::controls;
    const uint32_t pressed = inputs & ~wait->lastInputs, lifted = wait->lastInputs & ~inputs;
    wait->lastInputs = inputs;
    why->clear();
    if (wait->result != Ignore)
        return Ignore;
    const StickWatch before = wait->watch;
    Control control = data::CtlCross;
    Verdict verdict = Ignore;
    std::string outcome;
    if (pressed & in::bit(in::Start))
    {
        verdict = Cancel;
        wait->found = "Start (our pad read)";
        outcome = "Start pressed: cancels";
    }
    else if (onSticks(&wait->watch, wait->entry, sticks, &control) == Bind)
    {
        verdict = Bind;
        wait->control = control;
        wait->found = std::string("our stick watch (") + StickNames[control == data::CtlRightStick] + ")";
        outcome = std::string(StickNames[control == data::CtlRightStick]) + " pushed: binds";
    }
    for (int side = 0; verdict == Ignore && side < 2; ++side)
    {
        if (!wait->watch.pushed[side] || before.pushed[side])
            continue;
        outcome += std::string(outcome.empty() ? "" : "; ") + StickNames[side] + " pushed: " +
                   (!takesStick(wait->entry, side) ? "this row takes no " + std::string(StickNames[side]) :
                    "held since the wait began, centre it first");
    }
    if (verdict == Ignore && pressed)
    {
        const uint32_t select = pressed & in::bit(in::Select);
        outcome += std::string(outcome.empty() ? "" : "; ") + "pressed " + inputNames(pressed) +
                   (select == pressed ? ": Select is ignored" : ": left to the PC capture");
    }
    if (pressed || lifted || verdict != Ignore || !outcome.empty())
        *why = "inputs " + inputNames(inputs) + (lifted ? " (lifted " + inputNames(lifted) + ")" : "") +
               (outcome.empty() ? "" : ", " + outcome);
    wait->result = verdict;
    return verdict;
}

Verdict waitButton(Wait* wait, int button, std::string* why)
{
    const std::string name = buttonName(button);
    if (decided(wait, name, why))
        return Ignore;
    Control control = data::CtlCross;
    const Verdict verdict = onButton(wait->entry, button, &control);
    if (verdict == Bind)
    {
        wait->control = control;
        *why = name + ": binds";
    }
    else if (verdict == Cancel)
        *why = name + ": cancels";
    else
        *why = name + (button == ButtonSelect ? ": Select is ignored" : wait->entry == data::EntryFreeLook ?
                       ": ignored, Free Look takes only the right stick" : ": ignored, binds nothing");
    if (verdict != Ignore)
    {
        wait->result = verdict;
        wait->found = "the PC capture, " + name;
    }
    return verdict;
}

Verdict waitAxis(Wait* wait, int32_t axisId, int direction, std::string* why)
{
    const std::string name = axisName(axisId) + (direction < 0 ? " -" : " +");
    if (decided(wait, name, why))
        return Ignore;
    *why = name + ": ignored, our stick watch decides sticks (centred, then past " + std::to_string(int(StickOn * 100)) + "%)";
    return Ignore;
}

Verdict waitHat(Wait* wait, int hat, uint32_t pov, std::string* why)
{
    const std::string name = "hat " + std::to_string(hat) + " at " + std::to_string(pov & 0xffff);
    if (decided(wait, name, why))
        return Ignore;
    *why = name + ": ignored, hats are stick flicks and our stick watch decides sticks";
    return Ignore;
}

bool released(uint32_t inputs, const float sticks[4])
{
    namespace in = win32::controls;
    const uint32_t rear = in::bit(in::RearL2) | in::bit(in::RearR2) | in::bit(in::RearL3) | in::bit(in::RearR3);
    return (inputs & ~rear) == 0 && magnitude(sticks, 0) < StickOff && magnitude(sticks, 1) < StickOff;
}

}}
