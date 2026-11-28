// The mod features built into the game: their settings read through mINI at start-up, the file guard and patches.

#include "mods_runtime.h"
#include "mods_hook.h"
#include "exopts.h"
#include "novisrep.h"
#define MINI_CASE_SENSITIVE
#include "mini/ini.h"
#include <platform/log.h>
#include <platform/textfile.h>
#include <cstring>
#include <set>

namespace ug2 { namespace modrt {

namespace
{

bool s_read = false;
bool s_installed = false;
bool s_active[FeatureCount] = {};
bool s_present[FeatureCount] = {};
double s_value[FeatureCount] = {};

uint8_t byteOf(Feature f)
{
    return uint8_t(int32_t(s_value[f]));
}

void put32(uint8_t* guestBase, uint32_t address, uint32_t value)
{
    memcpy(guestBase + address, &value, 4);
}

void putFloat(uint8_t* guestBase, uint32_t address, float value)
{
    memcpy(guestBase + address, &value, 4);
}

}

State state(Feature feature, const std::string& gameFolder)
{
    const modreq::Setting& f = modreq::setting(feature);
    const std::string file = gameFolder + "/" + f.iniPath;
    State s;
    std::string text;
    const pf::TextFile kind = pf::readTextFile(file.c_str(), &text);
    s.iniFound = kind != pf::TextFile::Missing;
    s.unreadable = kind == pf::TextFile::Unreadable;
    s.value = modreq::codeDefault(f);
    if (kind == pf::TextFile::Readable)
    {
        mINI::INIStructure data;
        // ExtraOptions 5.1.0.1340 every key: deviation (user decision): missing is vanilla, not the code default
        s.keyFound = mINI::INIFile(file).read(data) && data.has(f.section) && data.get(f.section).has(f.key);
        if (s.keyFound)
            s.value = modreq::parse(f, data.get(f.section).get(f.key));
        s.on = s.keyFound && s.value != 0;
    }
    s.met = f.requirementsMet(gameFolder);
    s.requirements = f.requirementsString(gameFolder);
    return s;
}

bool runs(Feature feature, const std::string& gameFolder)
{
    const State s = state(feature, gameFolder);
    return s.on && s.met;
}

bool active(Feature feature)
{
    return s_read && s_active[feature];
}

bool installed()
{
    return s_read && s_installed;
}

bool present(Feature feature)
{
    return installed() && s_present[feature];
}

double value(Feature feature)
{
    return s_value[feature];
}

bool euCarsAiList = false;
bool exoLapLimits = false, exoDriftAxB = false, exoDriftPlus = false, exoDriftNifty = false, exoDriftLevels = false,
     exoShowOutrun = false, exoShowSubs = false, exoAnyTrack = false, exoFreeRunSelect = false,
     exoOutrunSelect = false, exoVinyls = false, exoCameras = false, exoNeon = false,
     exoLapPercent = false, exoStartingCash = false, exoUnlockAll = false,
     exoRaceBarriers = false, exoDriftFactor = false;
int32_t exoStartingCashValue = 0;
bool visualRepRemoved = false;

int applyPatches(uint8_t* guestBase)
{
    bool* const flags[] = { &euCarsAiList, &exoLapLimits, &exoDriftAxB, &exoDriftPlus, &exoDriftNifty,
                            &exoDriftLevels, &exoShowOutrun, &exoShowSubs, &exoAnyTrack, &exoFreeRunSelect,
                            &exoOutrunSelect, &exoVinyls, &exoCameras, &exoNeon, &exoLapPercent,
                            &exoStartingCash, &exoUnlockAll, &exoRaceBarriers,
                            &exoDriftFactor };
    for (bool* flag : flags)
        *flag = false;
    exoStartingCashValue = 0;
    visualRepRemoved = false;
    int count = 0;
    if (active(VisualRepRemover))
    {
        // NFSU/NFSU2 Visual Reputation Remover 1.1 [NFSU2] RemoveVisualRep: the mod's guard, 0xE8 at 0x557EEB, 0x55 at 0x5234C0
        if (guestBase[novisrep::CallSite] == 0xe8 && guestBase[novisrep::RatingFunction] == 0x55)
        {
            visualRepRemoved = true;
            pf::logInfo("mods: %s %s [%s] %s: 0x%x NOP run of %u bytes, rating function 0x%x replaced", novisrep::Name,
                        novisrep::Version, novisrep::Section, novisrep::Key, novisrep::NopStart, novisrep::NopLength,
                        novisrep::RatingFunction);
            count += 2;
        }
        else
            pf::logError("mods: %s %s stays off: unsupported game executable", novisrep::Name, novisrep::Version);
    }
    if (active(EuCarContent))
    {
        // ExtraOptions 5.1.0.1340 [Gameplay] UnlockRegionalCars: region list ids <- 0xFF, no id matches a car
        // ExtraOptions 5.1.0.1340 [Gameplay] UnlockRegionalCars: its 0x4DFA83/0x4E2374 RegionLockHook calls need no
        // code, as 0x5022C0 then returns 1 too and both callers read only al
        for (uint32_t id : RegionListIds)
            guestBase[id] = 0xff;
        // ExtraOptions 5.1.0.1340 [Gameplay] UnlockRegionalCars: AI car list gains CORSA and PEUGOT106
        guestBase[AiListExtra] = AiListCars[0];
        guestBase[AiListExtra + 1] = AiListCars[1];
        // ExtraOptions 5.1.0.1340 [Gameplay] UnlockRegionalCars: PickRandomCarsOfSomeSort list length 0x1A -> 0x1C
        euCarsAiList = true;
        pf::logInfo("mods: %s %s [%s] %s: region lists 0x%x-0x%x, AI car list 0x%x-0x%x, AI list length 28",
                    exopts::Name, exopts::Version, exopts::RegionSection, exopts::RegionKey, RegionListIds[0],
                    RegionListIds[3] + 3, AiListExtra, AiListExtra + 1);
        count += 6;
    }
    if (!installed())
        return count;
    // ExtraOptions 5.1.0.1340 every key: deviation (user decision): a missing key turns all its changes off

    // ExtraOptions 5.1.0.1340 [LapControllers] Minimum, Maximum: BYTE values % 128; a key not written keeps the
    // game's bytes, which the value switches then read
    const uint8_t lapMin = byteOf(LapMinimum) % 128, lapMax = byteOf(LapMaximum) % 128;
    if (present(LapMinimum))
        for (uint32_t a : LapMinimumBytes)
            guestBase[a] = lapMin;
    if (present(LapMaximum))
        for (uint32_t a : LapMaximumBytes)
            guestBase[a] = lapMax;
    exoLapLimits = present(LapMinimum) || present(LapMaximum);
    count += (present(LapMinimum) ? 5 : 0) + (present(LapMaximum) ? 4 : 0);

    // ExtraOptions 5.1.0.1340 [Drift] MaximumMultiplier: BYTE % 10, 0 -> 1; thresholds, levels and score factor
    uint8_t driftMax = byteOf(DriftMaximumMultiplier) % 10;
    if (driftMax == 0)
        driftMax = 1;
    if (present(DriftMaximumMultiplier))
    {
        for (int i = 0; i < 32; ++i)
            put32(guestBase, DriftThresholds + 4 * i, uint32_t(DriftThresholdPoints[i]));
        for (uint32_t a : DriftLevelBytes)
            guestBase[a] = driftMax;
        putFloat(guestBase, DriftFactor, float(driftMax) / 5.0f);
        exoDriftLevels = true;
        // ExtraOptions 5.1.0.1340 [Drift] MaximumMultiplier: the cave multiplies by 1.0 at x5, which changes no score
        exoDriftFactor = driftMax != 5;
        count += 4;
    }
    // ExtraOptions 5.1.0.1340 [Drift] ShowWithoutMultiplying, PlusSign, HideNiftyDriftMessages: != 0
    exoDriftAxB = active(DriftShowWithoutMultiplying);
    exoDriftPlus = active(DriftPlusSign);
    exoDriftNifty = active(DriftHideNiftyMessages);

    // ExtraOptions 5.1.0.1340 [Menu] ShowOutrun, ShowSubs, ShowSpecialVinyls, AnyTrackInAnyRaceMode: != 0
    exoShowOutrun = active(ShowOutrun);
    exoShowSubs = active(ShowSubs);
    exoVinyls = active(ShowSpecialVinyls);
    exoAnyTrack = active(AnyTrackInAnyRaceMode);
    // ExtraOptions 5.1.0.1340 [Menu] FreeRunTrackSelect, OutrunTrackSelect: != 0
    exoFreeRunSelect = active(FreeRunTrackSelect);
    exoOutrunSelect = active(OutrunTrackSelect);

    // ExtraOptions 5.1.0.1340 [Gameplay] EnableHiddenCameraModes: 5 camera modes, GetPOVTypeFromPlayerCamera
    exoCameras = active(EnableHiddenCameraModes);
    // ExtraOptions 5.1.0.1340 [Gameplay] RemoveRaceBarriers: 'I' -> 'M' in both names, and the per-track cave
    if (active(RemoveRaceBarriers))
    {
        guestBase[PlayerBarriersLetter] = 'M';
        guestBase[BarriersLetter] = 'M';
        exoRaceBarriers = true;
        count += 2;
    }
    // ExtraOptions 5.1.0.1340 [Gameplay] RemoveLockedAreaBarriers: 'I' -> 'M'
    if (active(RemoveLockedAreaBarriers))
    {
        guestBase[CareerBarriersLetter] = 'M';
        ++count;
    }
    // ExtraOptions 5.1.0.1340 [Gameplay] ShowPercentOn1LapRaces: != 0
    exoLapPercent = active(ShowPercentOn1LapRaces);
    // ExtraOptions 5.1.0.1340 [Gameplay] StartingCash: the int the cave stores
    if (present(StartingCash))
    {
        exoStartingCashValue = int32_t(value(StartingCash));
        exoStartingCash = true;
    }
    // ExtraOptions 5.1.0.1340 [Gameplay] UnlockAllThings: _UnlockAllThings <- 1 and GetPartUnlockFilter's NOPs
    if (active(UnlockAllThings))
    {
        guestBase[UnlockAllFlag] = 1;
        exoUnlockAll = true;
        ++count;
    }
    // ExtraOptions 5.1.0.1340 [Gameplay] NeonBrightness: float into RenderNeon's immediate
    if (present(NeonBrightness))
    {
        putFloat(guestBase, NeonBrightnessBytes, float(value(NeonBrightness)));
        exoNeon = true;
        ++count;
    }

    pf::logInfo("mods: %s %s: %d changes", exopts::Name, exopts::Version, count);
    return count;
}

}

std::vector<std::string> readModSettings(const std::string& gameFolder)
{
    std::vector<std::string> unreadable;
    std::set<std::string> reported;
    modrt::s_installed = false;
    for (int i = 0; i < modrt::FeatureCount; ++i)
    {
        const modreq::Setting& f = modreq::setting(modreq::SettingId(i));
        const modrt::State s = modrt::state(modrt::Feature(i), gameFolder);
        modrt::s_active[i] = s.on && s.met;
        modrt::s_value[i] = s.value;
        modrt::s_present[i] = s.keyFound;
        modrt::s_installed |= s.iniFound && !s.unreadable;
        const std::string file = gameFolder + "/" + f.iniPath;
        if (s.unreadable)
        {
            pf::logError("mods: %s %s off: %s is unreadable", f.mod, f.name, file.c_str());
            if (reported.insert(file).second)
                unreadable.push_back(modUnreadable(f.mod, file));
        }
        else if (s.on && !s.met)
            pf::logError("mods: %s %s stays off, the settings file says on: %s", f.mod, f.name, s.requirements.c_str());
        else if (!s.keyFound)
            pf::logInfo("mods: %s %s vanilla (%s)", f.mod, f.name,
                        s.iniFound ? "not in the settings file" : "no settings file");
        else if (f.kind == modreq::Kind::Bool)
            pf::logInfo("mods: %s %s %s", f.mod, f.name, modrt::s_active[i] ? "on" : "off");
        else
            pf::logInfo("mods: %s %s %g", f.mod, f.name, s.value);
    }
    modrt::s_read = true;
    return unreadable;
}

}
