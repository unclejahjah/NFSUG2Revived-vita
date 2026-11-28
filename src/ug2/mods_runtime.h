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
using namespace modreq;
const int FeatureCount = modreq::SettingCount;

struct State
{
    bool iniFound = false;
    bool unreadable = false;            // present but not readable text: off
    bool keyFound = false;              // the readable file has the key; without it the key is vanilla
    double value = 0;                   // as the mod reads it, when keyFound
    bool on = false;                    // keyFound and the value is non-zero, as Extra Options reads a number
    bool met = false;                   // Setting::requirementsMet
    std::string requirements;           // Setting::requirementsString
};
State state(Feature feature, const std::string& gameFolder);
bool runs(Feature feature, const std::string& gameFolder);
// For the game: as read at start-up, on in the ini and every requirement met.
bool active(Feature feature);
// As read at start-up: the settings file is there and readable, so the mod's unconditional changes apply.
bool installed();
// As read at start-up: the file has the key (else the key leaves the game vanilla), and its value.
bool present(Feature feature);
double value(Feature feature);

// ExtraOptions 5.1.0.1340 [Gameplay] UnlockRegionalCars: EU-only (CORSA, PEUGOT106), US-only (CIVIC, RSX) list ids.
const uint32_t RegionListIds[4] = { 0x7f7c48, 0x7f7c4c, 0x7f7c50, 0x7f7c54 };
// ExtraOptions 5.1.0.1340 [Gameplay] UnlockRegionalCars: AI car list entries 26, 27 <- CORSA, PEUGOT106.
const uint32_t AiListExtra = 0x7f6dbe;
const uint8_t AiListCars[2] = { 0x07, 0x06 };

// ExtraOptions 5.1.0.1340 [LapControllers] Minimum, Maximum: the immediates the generated code reads (MOD_SWITCHES).
const uint32_t LapMinimumBytes[5] = { 0x4b3d82, 0x4b3d9b, 0x4b3e7a, 0x4b3ee4, 0x4b3e9e };
const uint32_t LapMaximumBytes[4] = { 0x4b3d86, 0x4b3d97, 0x4b3f30, 0x4b3f41 };
// ExtraOptions 5.1.0.1340 [Drift] MaximumMultiplier: level limits, threshold table, sprint drift score factor.
const uint32_t DriftLevelBytes[2] = { 0x56cc58, 0x56cc90 };
const uint32_t DriftThresholds = 0x925004;
const int32_t DriftThresholdPoints[32] = {
    350, 1400, 4200, 11200, 22400, 38080, 57120, 85680, 300, 1200, 3600, 9600, 19200, 32640, 48960, 73440,
    250, 1000, 3000, 8000, 16000, 27200, 40800, 61200, 200, 800, 2400, 6400, 12800, 21760, 32640, 48960 };
const uint32_t DriftFactor = 0x925100;
// ExtraOptions 5.1.0.1340 [Gameplay] NeonBrightness: the brightness immediate of CarRenderInfo::RenderNeon.
const uint32_t NeonBrightnessBytes = 0x60d8d0;
// ExtraOptions 5.1.0.1340 [Gameplay] RemoveRaceBarriers, RemoveLockedAreaBarriers: one letter of each model name.
const uint32_t PlayerBarriersLetter = 0x7a078b;     // "PLAYER_BARRIERS_%d"
const uint32_t BarriersLetter = 0x7a0798;           // "BARRIERS_%d"
const uint32_t CareerBarriersLetter = 0x7a073c;     // "BARRIERS_CAREER%d"
// ExtraOptions 5.1.0.1340 [Gameplay] UnlockAllThings: _UnlockAllThings.
const uint32_t UnlockAllFlag = 0x838464;

// The generated code's switches (MOD_SWITCHES in tools/recompiler/games/ug2.py) and the natives' state.
extern bool euCarsAiList;
extern bool exoLapLimits, exoDriftAxB, exoDriftPlus, exoDriftNifty, exoDriftLevels, exoShowOutrun, exoShowSubs,
    exoAnyTrack, exoFreeRunSelect, exoOutrunSelect, exoVinyls, exoCameras, exoNeon, exoLapPercent,
    exoStartingCash, exoUnlockAll, exoRaceBarriers, exoDriftFactor;
extern int32_t exoStartingCashValue;
// NFSU/NFSU2 Visual Reputation Remover 1.1 [NFSU2] RemoveVisualRep: on, and the executable's guard bytes match; read by
// the call switch at 0x557EEB and the native 0x5234C0.
extern bool visualRepRemoved;
// Writes the active features' patches into guest memory, before the game code runs. The count of changes made.
int applyPatches(uint8_t* guestBase);

}}

#endif
