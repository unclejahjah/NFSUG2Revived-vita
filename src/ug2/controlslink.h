#ifndef UG2_CONTROLSLINK_H_
#define UG2_CONTROLSLINK_H_

// The controls store joined to the game: registry.txt as backend, the pad slot as the record, the profile's control
// load (0x5B9570) and save (0x5B9420), and the stick shape values.

#include "controlsrows.h"
#include "controlstore.h"
#include <lib/winapp.h>

namespace ug2 { namespace controlslink {

controlstore::Backend& backend();
// False when no pad slot exists.
bool readSlot(controlstore::Record* record);
void writeSlot(const controlstore::Record& record);
// The pad's input slot, 0 for none.
x86::reg32 padSlot();
// After a registry.txt change: the slot follows it.
void storeChanged(const char* why);

// Boot decision; before guest code, after registry.txt loads.
void configure();
// The record in play, one control per action.
controlstore::Record activeRecord();
// Control Type 1..4 for the game's defaults; Custom gives 1.
int defaultsType();
// Each frame: the pad slot holds the active record.
void frame();
// After the guest loaded a profile's controls.
void afterProfileLoad();
// After the guest wrote the profile's controls at out.
void patchProfileSave(x86::reg32 out);
// Writes the stick row's value into registry.txt.
void saveShape(bool right, controlsrows::StickPart part);

}}

#endif /* !UG2_CONTROLSLINK_H_ */
