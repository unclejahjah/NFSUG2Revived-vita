#ifndef UG2_CONTROLSROWS_H_
#define UG2_CONTROLSROWS_H_

// The Controls and Joypad Configuration rows as data: Control Type cycling, the stick shape rows' steps and limits,
// and the shape's registry.txt keys. No game access, so the host tests cover it.

#include "controlstore.h"
#include <lib/controls.h>

namespace ug2 { namespace controlsrows {

// Controls_Type 0..3 (Type 1..4), 4 Custom; wraps both ways.
int nextType(int type, int direction);

enum StickPart { PartDeadZone, PartOuterLimit, PartSensitivity, PartCount };
struct StickLimits
{
    int low, high, step;    // percent
};
StickLimits stickLimits(StickPart part);
// The part's value in whole percent.
int stickPercent(const win32::controls::StickShape& shape, StickPart part);
// One step left (-1) or right (+1), held at the limits.
win32::controls::StickShape stepStick(const win32::controls::StickShape& shape, StickPart part, int direction);
// The old cfg text of a retired stick key, or null.
typedef const char* (*OldCfgValue)(const char* key);
// At start-up: both shapes from registry.txt, else the old cfg key, else the default; nothing is written.
void loadShapes(controlstore::Backend& backend, OldCfgValue oldValue);
// Writes the part's own Controls_*Stick* key, and any value taken from the old cfg that is still unstored.
void saveShape(controlstore::Backend& backend, bool right, StickPart part);

}}

#endif /* !UG2_CONTROLSROWS_H_ */
