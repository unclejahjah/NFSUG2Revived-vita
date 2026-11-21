#ifndef UG2_MODS_RUNTIME_H_
#define UG2_MODS_RUNTIME_H_

// The PC mod features built into the game, each switched by a key in its mod's own settings file (shared with the
// PC game). The game reads the files at start-up (readModSettings); a missing file means off.

#include "mods_require.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ug2 { namespace modrt {

// The features are the shared mod settings (mods_require.h).
typedef modreq::SettingId Feature;
using modreq::EuCarContent;
const int FeatureCount = modreq::SettingCount;

struct State
{
    bool iniFound = false;
    bool unreadable = false;            // present but not readable text: off
    bool on = false;                    // the key is non-zero, as Extra Options reads a number
    bool met = false;                   // Setting::requirementsMet
    std::string requirements;           // Setting::requirementsString
};
State state(Feature feature, const std::string& gameFolder);
bool runs(Feature feature, const std::string& gameFolder);
// For the game: as read at start-up, on in the ini and every requirement met.
bool active(Feature feature);

// The car ids in the game's EU-only (CORSA, PEUGOT106) and US-only (CIVIC, RSX) lists that sub_5022c0 checks.
const uint32_t RegionListIds[4] = { 0x7f7c48, 0x7f7c4c, 0x7f7c50, 0x7f7c54 };
// Writes the active features' patches into guest memory, before the game code runs. The count written.
int applyPatches(uint8_t* guestBase);

}}

#endif
