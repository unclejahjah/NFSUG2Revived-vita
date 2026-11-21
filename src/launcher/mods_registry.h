#ifndef LAUNCHER_MODS_REGISTRY_H_
#define LAUNCHER_MODS_REGISTRY_H_

// The Mods tab's registry: every key of each PC mod's ini, and the features the port supports, each switched by a key
// in the mod's own settings file shared with the PC game; the cfg never controls the same feature.

#include "mods_require.h"
#include <string>
#include <vector>

namespace ug2 { namespace mods {

enum Id { EuCarContent, Count };

// One key of a mod's PC ini. A supported key has a feature in the registry below; flip the flag when one is added.
struct IniKey
{
    const char* section;
    const char* key;
    const char* def;            // the mod's default
    bool supported;
};

// A PC mod, a group of the Mods tab: every key of its ini, in the ini's order.
struct Source
{
    const char* mod;
    const char* version;
    const char* author;
    const char* iniPath;        // relative to the game folder
    std::vector<IniKey> keys;
};

enum SourceId { ExtraOptions, SourceCount };

// A supported key: its name, section, key and requirements are the shared setting's (mods_require.h).
struct Mod
{
    SourceId source;
    modreq::SettingId setting;
    const char* description;    // quoted from the mod's own readme
    const char* comment;        // our wording, for a line we write
    std::vector<const char*> overlaps;  // cfg keys that would control the same feature; none may exist
};

const Mod& get(Id id);
inline const modreq::Setting& def(const Mod& mod) { return modreq::setting(mod.setting); }
const Source& source(SourceId id);
std::string heading(const Source& source);   // "{mod} {version} by {author}"
std::string fileName(const Source& source);  // the ini's name, e.g. NFSU2ExtraOptionsSettings.ini
// The first cfg key controlling the same feature, or null.
const char* cfgOverlap(const Mod& mod);

// The mod's ini through mINI, patched for Extra Options' "Key = 1   // comment" lines.
namespace ini
{
// The value of key in section, without its comment.
bool get(const std::string& file, const char* section, const char* key, std::string* value);
}

std::string pathIn(const std::string& folder, const char* relative);
bool fileExists(const std::string& file);
bool isOn(const std::string& value);    // an Extra Options number, as it reads one

struct State
{
    bool iniFound = false;
    bool unreadable = false;            // the ini exists but cannot be read (pf::readTextFile); the mod is off
    bool on = false;                    // the ini's key is 1
    bool met = false;                   // modreq::Setting::requirementsMet
    std::string requirements;           // modreq::Setting::requirementsString
};
State state(const Mod& mod, const std::string& gameFolder);

// On in the ini and every requirement met: what the game does.
bool runs(const Mod& mod, const std::string& gameFolder);

}}

#endif
