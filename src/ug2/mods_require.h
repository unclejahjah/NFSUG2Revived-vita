#ifndef UG2_MODS_REQUIRE_H_
#define UG2_MODS_REQUIRE_H_

// Each supported mod setting: its key, value type and defaults, and whether it can work in a game folder. Built into
// both the game and the launcher, so both read and decide the same way.

#include <string>
#include <vector>

namespace ug2 { namespace modreq {

enum SettingId
{
    EuCarContent,
    LapMinimum, LapMaximum,
    DriftMaximumMultiplier, DriftPlusSign, DriftShowWithoutMultiplying, DriftHideNiftyMessages,
    ShowOutrun, ShowSubs, ShowSpecialVinyls, AnyTrackInAnyRaceMode, FreeRunTrackSelect, OutrunTrackSelect,
    EnableHiddenCameraModes, RemoveRaceBarriers, RemoveLockedAreaBarriers, ShowPercentOn1LapRaces,
    StartingCash, UnlockAllThings, NeonBrightness,
    VisualRepRemover,
    SettingCount
};

// Bool: a number read as != 0. Int and Enum: whole numbers, Enum with a label per value. Float: a decimal.
enum class Kind { Bool, Int, Float, Enum };

class Setting
{
public:
    const char* mod;                    // display name of the source mod
    const char* name;
    const char* iniPath;                // relative to the game folder
    const char* section;
    const char* key;
    Kind kind = Kind::Bool;
    const char* codeDefault = "1";      // what the mod uses when the key is missing
    const char* shipped = "1";          // the value in the mod's own settings file
    // The value with which the mod's patches give the game's own bytes: the Mods tab's default and a new file's
    // value. Empty when no value does: the key's Vanilla choice, which leaves it out of the file.
    const char* vanilla = "0";
    double min = 0, max = 1, step = 1;  // the Mods tab's range
    std::vector<const char*> labels;    // Enum: one per value from min

    Setting(const char* mod, const char* name, const char* iniPath, const char* section, const char* key)
        : mod(mod), name(name), iniPath(iniPath), section(section), key(key) {}
    virtual ~Setting() {}
    virtual bool requirementsMet(const std::string& gameFolder) const = 0;
    // One short sentence saying what the setting needs.
    virtual std::string requirementsString(const std::string& gameFolder) const = 0;
};

const Setting& setting(SettingId id);
// SettingCount when no setting uses that key.
SettingId find(const char* iniPath, const char* section, const char* key);

// The key's value as the mod reads it from text (no comment): an Int, Enum or Bool through std::stoi (base 16 after
// 0x), the code default when that fails; a Float through atof.
double parse(const Setting& s, const std::string& text);
double codeDefault(const Setting& s);

}}

#endif
