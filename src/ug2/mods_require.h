#ifndef UG2_MODS_REQUIRE_H_
#define UG2_MODS_REQUIRE_H_

// Each supported mod setting and whether it can work in a game folder. Built into both the game and the launcher,
// so both decide the same way; how a setting decides is its own business.

#include <string>

namespace ug2 { namespace modreq {

enum SettingId { EuCarContent, SettingCount };

class Setting
{
public:
    const char* mod;                    // display name of the source mod
    const char* name;
    const char* iniPath;                // relative to the game folder
    const char* section;
    const char* key;

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

}}

#endif
