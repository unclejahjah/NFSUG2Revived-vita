#ifndef UG2_CUSTOMIZE_H_
#define UG2_CUSTOMIZE_H_

// The Customize screen as data: entry values, bind wait verdicts on PC captures, our stick watch and the release
// guard. Host tested.

#include "controlsdata.h"

#include <cstdint>
#include <string>

namespace ug2 { namespace customize {

using controlsdata::Control;
using controlsdata::Entry;

// The entry's value: icon tokens, "Touchscreen", or empty for None.
std::string entryValue(const controlsdata::Binds& binds, Entry entry);

const uint32_t SpareAction = 11;    // armed for Free Look: no race action uses it
// The action the PC capture (0x5C2100) is armed with.
uint32_t armAction(Entry entry);

enum Verdict { Ignore, Bind, Cancel };
const int ButtonSelect = 8, ButtonStart = 9;
// A button the PC capture found (0x5B8460), by PS2 index.
Verdict onButton(Entry entry, int button, Control* control);

// Front touch reaches the menu only when no bind wait holds the input.
bool touchReachesGame(bool inputHeld);

// Entries taking a stick; our watch decides their sticks.
bool watchesSticks(Entry entry);
const float StickOn = 0.7f, StickOff = 0.3f;
struct StickWatch
{
    bool centred[2] = { false, false };     // left, right: seen under StickOff since the wait began
    bool pushed[2] = { false, false };      // past StickOn last frame, for logs
};
// A stick counts once centred, then pushed past StickOn.
Verdict onSticks(StickWatch* watch, Entry entry, const float sticks[4], Control* control);

// One bind wait: our pad read each frame and the PC capture's finds; the first verdict stands.
struct Wait
{
    Entry entry = controlsdata::EntrySteerLeft;
    uint32_t lastInputs = 0;                // last frame's inputs: Start cancels on a new press only
    StickWatch watch;
    Verdict result = Ignore;
    Control control = controlsdata::CtlCross;
    std::string found;                      // what decided it, for logs
};
// Inputs held as the wait begins never count as presses.
void beginWait(Wait* wait, Entry entry, uint32_t inputs);
// Our pad read of one frame; why empty if unchanged.
Verdict waitPad(Wait* wait, uint32_t inputs, const float sticks[4], std::string* why);
// PC capture finds: button 0x5B8460, axis 0x5B8250, hat 0x5C1720.
Verdict waitButton(Wait* wait, int button, std::string* why);
Verdict waitAxis(Wait* wait, int32_t axisId, int direction, std::string* why);
Verdict waitHat(Wait* wait, int hat, uint32_t pov, std::string* why);
// Vita input names joined by '+', or "nothing".
std::string inputNames(uint32_t inputs);

// Release guard: nothing held but rear touch, sticks centred.
bool released(uint32_t inputs, const float sticks[4]);

}}

#endif /* !UG2_CUSTOMIZE_H_ */
