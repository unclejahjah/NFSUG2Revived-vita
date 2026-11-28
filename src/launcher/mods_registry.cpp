// The Mods tab's registry: each mod's ini keys and the features the port supports, read through mINI.

#include "mods_registry.h"
#include "exopts.h"
#include "novisrep.h"
#define MINI_CASE_SENSITIVE
#include "mini/ini.h"
#include <platform/settings.h>
#include <platform/textfile.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace ug2 { namespace mods {

namespace
{

Source s_sources[SourceCount] =
{
    {
        exopts::Name,
        exopts::Version,
        exopts::Author,
        exopts::IniPath,
        {
            { "Hotkeys", "AnyTrackInAnyMode", "36", false },
            { "Hotkeys", "Headlights", "71", false },
            { "Hotkeys", "FreezeCamera", "19", false },
            { "Hotkeys", "UnlockAllThings", "116", false },
            { "Hotkeys", "AutoDrive", "117", false },
            { "Hotkeys", "EnableSaveLoadHotPos", "0", false },
            { "LapControllers", "Minimum", "0", true },
            { "LapControllers", "Maximum", "127", true },
            { "LapControllers", "KOEnabled", "5", false },
            { "OpponentControllers", "Minimum", "0", false },
            { "OpponentControllers", "Maximum", "5", false },
            { "OpponentControllers", "KOEnabled", "5", false },
            { "OpponentControllers", "MaximumLANPlayers", "6", false },
            { "Drift", "MaximumMultiplier", "5", true },
            { "Drift", "PlusSign", "0", true },
            { "Drift", "ShowWithoutMultiplying", "0", true },
            { "Drift", "HideNiftyDriftMessages", "0", true },
            { "Menu", "ShowOutrun", "0", true },
            { "Menu", "ShowMoreRaceOptions", "1", false },
            { "Menu", "ShowOnline", "1", false },
            { "Menu", "ShowSubs", "0", true },
            { "Menu", "ShowSpecialVinyls", "1", true },
            { "Menu", "ShowDebugCarCustomize", "0", false },
            { "Menu", "ShowDebugEventID", "0", false },
            { "Menu", "AnyTrackInAnyRaceMode", "0", true },
            { "Menu", "FreeRunTrackSelect", "1", true },
            { "Menu", "OutrunTrackSelect", "1", true },
            { "Menu", "SplashScreenTimeLimit", "30", false },
            { "Menu", "ShowcaseCamInfiniteYRotation", "0", false },
            { "Menu", "AllowLongerProfileNames", "1", false },
            { "Menu", "DisableTakeover", "0", false },
            { "Gameplay", "EnableHiddenCameraModes", "0", true },
            { "Gameplay", "EnableDebugCamera", "0", false },
            { "Gameplay", "GameSpeed", "1", false },
            { "Gameplay", "WorldAnimationSpeed", "45", false },
            { "Gameplay", "HeadlightsMode", "2", false },
            { "Gameplay", "LowBeamBrightness", "0.75", false },
            { "Gameplay", "HighBeamBrightness", "1", false },
            { "Gameplay", "RemoveRaceBarriers", "0", true },
            { "Gameplay", "RemoveLockedAreaBarriers", "0", true },
            { "Gameplay", "ShowPercentOn1LapRaces", "1", true },
            { "Gameplay", "GameRegion", "0", false },
            { "Gameplay", "StartingCash", "0", true },
            { "Gameplay", "UnlockRegionalCars", "1", true },
            { "Gameplay", "UnlockAllThings", "0", true },
            { "Gameplay", "NeonBrightness", "1", true },
            { "Gameplay", "NosTrailRepeatCount", "8", false },
            { "Weather", "AlwaysRain", "0", false },
            { "Weather", "GeneralRainAmount", "1", false },
            { "Weather", "RoadReflectionAmount", "1", false },
            { "Weather", "RainSize", "0.03", false },
            { "Weather", "RainIntensity", "0.7", false },
            { "Weather", "RainCrossing", "0.02", false },
            { "Weather", "RainSpeed", "0.03", false },
            { "Weather", "RainGravity", "0.35", false },
            { "Fixes", "DisappearingWheelsFix", "1", false },
            { "Fixes", "ExperimentalSplitScreenFix", "0", false },
            { "Fixes", "DamageFix", "1", false },
            { "Fixes", "MegalowPartsFix", "1", false },
            { "Misc", "WindowedMode", "0", false },
            { "Misc", "SkipMovies", "0", false },
            { "Misc", "EnableSound", "1", false },
            { "Misc", "EnableMusic", "1", false },
            { "Misc", "AllowMultipleInstances", "0", false },
        },
        {},
    },
    {   // https://nfsmods.xyz/mod/2773, v1.1: no ini of its own, the port keeps the switch in novisrep::IniPath
        novisrep::Name,
        novisrep::Version,
        novisrep::Author,
        novisrep::IniPath,
        {
            { novisrep::Section, novisrep::Key, "1", true },    // NFSU/NFSU2 Visual Reputation Remover 1.1 [NFSU2] RemoveVisualRep
        },
        {},
    },
};


#define EXOPTS(id, description, comment, overlap) { ExtraOptions, modreq::id, description, comment, { overlap } }

// ExtraOptions 5.1.0.1340: descriptions from its readme, comments ours
const Mod s_mods[Count] =
{
    {   // ExtraOptions 5.1.0.1340 [Gameplay] UnlockRegionalCars
        ExtraOptions,
        modreq::EuCarContent,
        "You can drive Peugeout 106 & Vauxhall Corsa in your NTSC game. Same deal with Acura RSX & Honda Civic in your "
        "PAL game. They will be available at Customize Menu and Career Car Lot. They also appear as AI opponents.",
        "1 lists the Corsa and the Peugeot 106 in the US game, 0 keeps them out",
        { "eu_content" },
    },
    // ExtraOptions 5.1.0.1340 [LapControllers] Minimum, Maximum
    EXOPTS(LapMinimum, "You can have from 0 (which makes the race unlimited) to 127 (Really long races) laps.",
           "fewest laps in Quick Race and LAN options, 0 to 127", "lap_minimum"),
    EXOPTS(LapMaximum, "You can have from 0 (which makes the race unlimited) to 127 (Really long races) laps.",
           "most laps in Quick Race and LAN options, 0 to 127", "lap_maximum"),
    // ExtraOptions 5.1.0.1340 [Drift] MaximumMultiplier, PlusSign, ShowWithoutMultiplying, HideNiftyDriftMessages
    EXOPTS(DriftMaximumMultiplier, "The default value was x5. You can boost it up to x9, or use x1 for more "
           "challenging drift races. Now with this hack, static drift scores in Heights Drift races are multiplied with "
           "Multiplier/5 (1.8 for x9) to balance them with you.", "highest drift multiplier, 1 to 9", "drift_multiplier"),
    EXOPTS(DriftPlusSign, "You can also tweak how your score will look like. Add a + before it, or show it without "
           "multiplying.", "1 shows a + before the drift points", "drift_plus_sign"),
    EXOPTS(DriftShowWithoutMultiplying, "You can also tweak how your score will look like. Add a + before it, or show "
           "it without multiplying.", "1 shows drift points as 500x3", "drift_points_axb"),
    EXOPTS(DriftHideNiftyMessages, "You can now get rid of \"Good Drift!\", \"Perfect Drift!\", etc. messages while "
           "drifting! You are always the drift king, tho.", "1 hides Good Drift! and the like", "drift_messages"),
    // ExtraOptions 5.1.0.1340 [Menu] ShowOutrun, ShowSubs, ShowSpecialVinyls, AnyTrackInAnyRaceMode
    EXOPTS(ShowOutrun, "Your 1vs1 game mode from Career and LAN is now available in Quick Race. But with some issues: "
           "Screen will stay black, it isn't even split. And you control 2 cars at the same time.",
           "1 adds Outrun to the Quick Race modes", "quick_race_outrun"),
    EXOPTS(ShowSubs, "You want to know what they talk about, but you can't understand what do they say? This is for "
           "you! Some subtitles are buggy, because of their cut strings.", "1 shows English subtitles", "subtitles"),
    EXOPTS(ShowSpecialVinyls, "Debug vinyl and sponsor vinyls; just go install them from the Vinyls menu. Some vinyls "
           "don't work for some cars.", "1 adds the special vinyl category", "special_vinyls"),
    EXOPTS(AnyTrackInAnyRaceMode, "Drift in your favourite track, or make a free roam Drag! This option will also add "
           "hidden tracks in menus. This will make the list too long. After some races, icons won't scroll. But you "
           "can select a race safely. If you select a Sprint Drift (Heights Drifts) for another game mode, your game "
           "will crash when you finish the race.", "1 lists every track in every race mode", "any_track"),
    // ExtraOptions 5.1.0.1340 [Menu] FreeRunTrackSelect, OutrunTrackSelect
    EXOPTS(FreeRunTrackSelect, "They have a very long list! Just try it.", "1 lets Free Run pick its track", "free_run_track_select"),
    EXOPTS(OutrunTrackSelect, "They have a very long list! Just try it. No more random tracks for Outrun game mode!",
           "1 lets Outrun pick its track", "outrun_track_select"),
    // ExtraOptions 5.1.0.1340 [Gameplay] EnableHiddenCameraModes, RemoveRaceBarriers, RemoveLockedAreaBarriers
    EXOPTS(EnableHiddenCameraModes, "Birds eye free roam? Yeah, it's here! Unused Driver Camera is here, too!",
           "1 adds the hidden camera modes", "camera_modes"),
    EXOPTS(RemoveRaceBarriers, "Locked area barriers or route indicators, remove if you don't want them! This hack "
           "will be disabled for Drift, StreetX and URL tracks to fix invisible walls bug.",
           "1 removes the race route barriers", "race_barriers"),
    EXOPTS(RemoveLockedAreaBarriers, "Locked area barriers or route indicators, remove if you don't want them!",
           "1 removes the locked area barriers", "locked_area_barriers"),
    // ExtraOptions 5.1.0.1340 [Gameplay] ShowPercentOn1LapRaces, StartingCash, UnlockAllThings
    EXOPTS(ShowPercentOn1LapRaces, "No more LAPS 1/1! You'll see % progress on 1-lap races now.",
           "1 shows the % progress on 1-lap races", "lap_percent"),
    EXOPTS(StartingCash, "Start with a specified amount of cash. Get rich easily from the start!",
           "cash of a new career", "starting_cash"),
    EXOPTS(UnlockAllThings, "This will unlock everything in game. Like a season pass, but it's for FREE!",
           "1 unlocks everything", "unlock_all"),
    // ExtraOptions 5.1.0.1340 [Gameplay] NeonBrightness
    EXOPTS(NeonBrightness, "Are that neons too bright for you? You can dim them.", "under-car neon brightness", "neon_brightness"),
    {   // NFSU/NFSU2 Visual Reputation Remover 1.1 [NFSU2] RemoveVisualRep
        NoVisualRep,
        modreq::VisualRepRemover,
        "The Visual Rating system in NFSU2 only interacts with the availability of DVD Covers. Therefore this patch fixes "
        "the rating at values good enough to acquire the covers for each game stage, as such, you receive SMS about cover "
        "opportunities at the beginning of each stage. Stage 0, 1, 2, 3 and 4 are fixed with 0, 1, 3, 5.5 and 8 stars "
        "respectively. Stage 5 onwards are fixed with 10 stars. Visual Rating Meter is removed from the customization menu.",
        "1 fixes the visual rating by game stage and removes its meter, 0 keeps the game's",
        { "visual_rating" },
    },
};

#undef EXOPTS

}

std::string pathIn(const std::string& folder, const char* relative)
{
    return folder + "/" + relative;
}

bool fileExists(const std::string& file)
{
    struct stat info;
    return stat(file.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

namespace
{

void readValue(const std::string& file, const Mod& mod, State* s)
{
    std::string text, value;
    const pf::TextFile kind = pf::readTextFile(file.c_str(), &text);
    s->iniFound = kind != pf::TextFile::Missing;
    s->unreadable = kind == pf::TextFile::Unreadable;
    s->keyFound = kind == pf::TextFile::Readable && ini::get(file, def(mod).section, def(mod).key, &value);
    // ExtraOptions 5.1.0.1340 every key: deviation (user decision): missing is vanilla, not the code default
    s->value = s->keyFound ? modreq::parse(def(mod), value) : 0;
    s->on = s->keyFound && s->value != 0;
}

}

const Mod& get(Id id)
{
    return s_mods[id];
}

const Source& source(SourceId id)
{
    return s_sources[id];
}

void setDownload(SourceId id, const Download& download)
{
    s_sources[id].download = download;
}

uint64_t Download::installedBytes() const
{
    uint64_t total = 0;
    for (const FileLocation& file : files)
        total += file.bytes;
    return total;
}

std::string fileName(const Source& source)
{
    const char* slash = strrchr(source.iniPath, '/');
    return slash ? slash + 1 : source.iniPath;
}

std::string heading(const Source& source)
{
    return std::string(source.mod) + " " + source.version + " by " + source.author;
}

const char* cfgOverlap(const Mod& mod)
{
    for (const char* key : mod.overlaps)
    {
        if (pf::findSetting(pf::canonicalKey(key)))
            return key;
    }
    return nullptr;
}

namespace ini
{

bool get(const std::string& file, const char* section, const char* key, std::string* value)
{
    mINI::INIStructure data;
    if (!mINI::INIFile(file).read(data) || !data.has(section) || !data.get(section).has(key))
        return false;
    *value = data.get(section).get(key);
    return true;
}


}

State state(const Mod& mod, const std::string& gameFolder)
{
    State s;
    readValue(pathIn(gameFolder, s_sources[mod.source].iniPath), mod, &s);
    s.met = def(mod).requirementsMet(gameFolder);
    s.requirements = def(mod).requirementsString(gameFolder);
    return s;
}

bool runs(const Mod& mod, const std::string& gameFolder)
{
    const State s = state(mod, gameFolder);
    return s.on && s.met;
}

}}
