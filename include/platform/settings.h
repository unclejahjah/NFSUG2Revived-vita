#ifndef PLATFORM_SETTINGS_H_
#define PLATFORM_SETTINGS_H_

#include <string>

// The table of every cfg key: type, range or options, default, group and description.

namespace pf
{

const int FreeLookDeadZoneDefault = 30;     // percent; 0 would let any stick drift turn the camera

enum class SettingType : unsigned char { Bool, Int, Enum, String };
enum class SettingGroup : unsigned char { General, Display, Controls, Audio, Performance, Developer, Count };

enum SettingFlags : unsigned
{
    SettingNeedsDevSettings = 1,    // read only in a DEV_SETTINGS build
    SettingNeedsDevTools    = 2,    // read only in a DEV_TOOLS build
    SettingOff              = 4,    // an Int that also takes "off", one step below min
    SettingAuto             = 8,    // absent means "auto": the code derives the value
    SettingHostOnly         = 16,   // read by the host build only
    SettingSecret           = 32,   // a String never shown: only whether it is set
    SettingNeedsShadowBuild = 64,   // read only with NFSU_NATIVE_SHADOW
    SettingInGame           = 128,  // set in the game's own menus, not shown by the configurator
};

// Bool: def 0 or 1. Int: def within min..max. Enum: def indexes text, "a|b|c". String: text is the default.
struct Setting
{
    const char*  key;
    SettingType  type;
    SettingGroup group;
    int          def;
    int          min;
    int          max;
    int          step;
    const char*  text;
    const char*  description;
    unsigned     flags;
};

const Setting* settingsTable(int* count);
const Setting* findSetting(const char* key);    // by current name only
const char*    settingGroupName(SettingGroup group);
// False when this build never reads the key.
bool           settingReadInThisBuild(const Setting& setting);

// An old key that still works: it names the same setting as its replacement.
struct SettingAlias
{
    const char* oldKey;
    const char* key;
};
const SettingAlias* settingAliases(int* count);
bool settingRetired(const char* key);           // a removed key: the loader skips its line silently
const char* canonicalKey(const char* key);      // the current name of an alias; key itself otherwise
const char* oldKeyOf(const char* key);          // the alias of a current name, or null

int         enumCount(const Setting& setting);
std::string enumName(const Setting& setting, int index);
int         enumIndex(const Setting& setting, const char* name);    // -1 when not an option; any case
// The default as the cfg writes it; auto for SettingAuto.
std::string defaultText(const Setting& setting);

}

#endif
