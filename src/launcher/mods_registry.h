#ifndef LAUNCHER_MODS_REGISTRY_H_
#define LAUNCHER_MODS_REGISTRY_H_

// The Mods tab's registry: every key of each PC mod's ini, and the features the port supports, each switched by a key
// in the mod's own settings file shared with the PC game; the cfg never controls the same feature.

#include "mods_require.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ug2 { namespace mods {

// The Extra Options features in modreq::SettingId order.
enum Id
{
    EuCarContent,
    LapMinimum, LapMaximum,
    DriftMaximumMultiplier, DriftPlusSign, DriftShowWithoutMultiplying, DriftHideNiftyMessages,
    ShowOutrun, ShowSubs, ShowSpecialVinyls, AnyTrackInAnyRaceMode, FreeRunTrackSelect, OutrunTrackSelect,
    EnableHiddenCameraModes, RemoveRaceBarriers, RemoveLockedAreaBarriers, ShowPercentOn1LapRaces,
    StartingCash, UnlockAllThings, NeonBrightness,
    VisualRepRemover,
    Count
};

// One key of a mod's PC ini. A supported key has a feature in the registry below; flip the flag when one is added.
struct IniKey
{
    const char* section;
    const char* key;
    const char* def;            // the mod's default
    bool supported;
};

// One file the mod needs: where it is in the archive and where it goes in the game folder.
struct FileLocation
{
    std::string from;           // path in the archive, '/' separated
    std::string to;             // relative to the game folder, as in the PC game's layout
    uint64_t bytes;
};

// Where a mod's files can be fetched from; launcher only, the game never downloads.
struct Download
{
    std::string url;            // direct HTTPS link to the archive (zip, 7z or rar)
    uint64_t archiveBytes = 0;
    std::string archiveSha256;  // lower-case hex
    std::vector<FileLocation> files;

    uint64_t installedBytes() const;
    // Archive plus installed bytes: the archive stays until installed.
    uint64_t spaceNeeded() const { return archiveBytes + installedBytes(); }
};

// A PC mod, a group of the Mods tab: every key of its ini, in the ini's order.
struct Source
{
    const char* mod;
    const char* version;
    const char* author;
    const char* iniPath;        // relative to the game folder
    std::vector<IniKey> keys;
    Download download;          // empty url: the files cannot be downloaded

    // Empty when the mod has no download.
    const std::string& downloadUrl() const { return download.url; }
    const std::vector<FileLocation>& fileLocations() const { return download.files; }
    bool downloadable() const { return !download.url.empty() && !download.files.empty(); }
};

enum SourceId { ExtraOptions, NoVisualRep, SourceCount };

// A supported key: its name, section, key and requirements are the shared setting's (mods_require.h), or for a
// launcher-only entry its own.
struct Mod
{
    SourceId source;
    modreq::SettingId setting;  // SettingCount with own
    const char* description;    // quoted from the mod's own readme
    const char* comment;        // our wording, for a line we write
    std::vector<const char*> overlaps;  // cfg keys that would control the same feature; none may exist
    const modreq::Setting* own = nullptr;   // a setting the game never sees
    const char* note = nullptr;             // our line after the description
};

const Mod& get(Id id);
inline const modreq::Setting& def(const Mod& mod) { return mod.own ? *mod.own : modreq::setting(mod.setting); }
const Source& source(SourceId id);
// Replaces a source's download; the host tests register fake ones.
void setDownload(SourceId id, const Download& download);
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

struct State
{
    bool iniFound = false;
    bool unreadable = false;            // the ini exists but cannot be read (pf::readTextFile); the mod is off
    bool keyFound = false;              // the readable ini has the key
    double value = 0;                   // as the mod reads it, when keyFound; a missing key is vanilla
    bool on = false;                    // keyFound and the value non-zero
    bool met = false;                   // modreq::Setting::requirementsMet
    std::string requirements;           // modreq::Setting::requirementsString
};
State state(const Mod& mod, const std::string& gameFolder);

// On in the ini and every requirement met: what the game does.
bool runs(const Mod& mod, const std::string& gameFolder);

}}

#endif
