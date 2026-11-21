// The Vita's settings for the game's own Options screens (options_menu.h; the screens are menu_entry.cpp): the
// settings of the browser version's Settings page that the Vita has, grouped as there, and their files.

#include "options_menu.h"
#include <platform/config.h>
#include <platform/log.h>
#ifdef __vita__
#include <psp2/appmgr.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>
#include <vector>

namespace ug2 { namespace options {

namespace
{

// ---- The settings ---------------------------------------------------------------------------------------------------

enum Store { Cfg, Ini };            // NeedForSpeedUnderground2.cfg, or scripts/NFSU2FreeRoam.ini's [FreeRoam]
enum Kind { Choice, Text, Secret };

struct Option
{
    const char* value;              // as written to the file
    const char* name;               // as shown
};

struct Item
{
    const char*     label;
    Store           store;
    const char*     key;
    Kind            kind;
    const Option*   options;        // Choice
    int             optionCount;
    const char*     def;            // the value with no line in the file
    int             maxLength;      // Text
    const char*     empty;          // Text: what an empty value means
    const char*     help;
};

#define OPTIONS(a) a, int(sizeof(a) / sizeof(a[0]))

const Option OnOff[] = { { "on", "On" }, { "off", "Off" } };
const Option IniOnOff[] = { { "1", "On" }, { "0", "Off" } };
const Option Network[] = { { "revived", "NFSU2 Revived" }, { "direct", "Local Wi-Fi" }, { "off", "Off" } };
const Option Collide[] = { { "0", "Ghost cars" }, { "1", "Solid cars" }, { "none", "Ghost everything" } };
const Option MaxPlayers[] = { { "2", "2" }, { "4", "4" }, { "6", "6" }, { "8", "8" }, { "12", "12" }, { "16", "16" },
                              { "24", "24" }, { "33", "33" } };
const Option MaxCars[] = { { "0", "0" }, { "1", "1" }, { "2", "2" }, { "3", "3" }, { "4", "4" }, { "5", "5" } };
const Option Rate[] = { { "", "Default (20)" }, { "10", "10 a second" }, { "15", "15 a second" }, { "20", "20 a second" },
                        { "25", "25 a second" }, { "30", "30 a second" } };
const Option Delay[] = { { "", "Default (100 ms)" }, { "0", "0 ms" }, { "50", "50 ms" }, { "100", "100 ms" },
                         { "150", "150 ms" }, { "200", "200 ms" }, { "300", "300 ms" } };
const Option Extrap[] = { { "", "Default (250 ms)" }, { "0", "0 ms" }, { "100", "100 ms" }, { "250", "250 ms" },
                          { "500", "500 ms" }, { "1000", "1000 ms" } };
const Option FrameRate[] = { { "30", "30 fps" }, { "0", "Unlocked" } };
const Option Preset[] = { { "vita", "Vita" }, { "max", "Max" } };
const Option LodPixels[] = { { "0", "Off" }, { "64", "64 px" }, { "128", "128 px" }, { "192", "192 px" },
                             { "256", "256 px" } };
const Option MinPixels[] = { { "0", "Off" }, { "8", "8 px" }, { "16", "16 px" }, { "32", "32 px" }, { "48", "48 px" },
                             { "64", "64 px" } };
const Option MixRate[] = { { "0", "Game's own" }, { "44100", "44100 Hz" }, { "32000", "32000 Hz" },
                           { "22050", "22050 Hz" } };
const Option Silence[] = { { "-60", "-60 dB" }, { "-50", "-50 dB" }, { "-40", "-40 dB" }, { "off", "Off" } };
const Option DeadZone[] = { { "0", "0%" }, { "10", "10%" }, { "20", "20%" }, { "30", "30%" }, { "40", "40%" },
                            { "50", "50%" } };
const Option Speed[] = { { "1", "1" }, { "2", "2" }, { "3", "3" }, { "4", "4" }, { "5", "5" }, { "6", "6" },
                         { "8", "8" }, { "10", "10" } };

const Item Multiplayer[] = {
    { "Network play", Cfg, "network", Choice, OPTIONS(Network), "revived", 0, nullptr,
      "The game's LAN and Online menus. NFSU2 Revived: through nfsu2.xyz, with the players in the browser. "
      "Local Wi-Fi: LAN races with PCs on your network, Online through the Online server below." },
    { "Revived server", Cfg, "revived_server", Text, nullptr, 0, "nfsu2.xyz", 63, "nfsu2.xyz",
      "The NFSU2 Revived server to play on." },
    { "Player name", Cfg, "revived_name", Text, nullptr, 0, "", 24, "Vita user name",
      "The name the other players see on the Revived server. A claimed name needs its passphrase." },
    { "Passphrase", Cfg, "revived_passphrase", Secret, nullptr, 0, "", 128, nullptr,
      "The passphrase of your nfsu2.xyz cloud saves, to play under the name it claimed. It stays on the Vita: "
      "the server only gets a code made from it." },
    { "Online server", Cfg, "online_server", Text, nullptr, 0, "eahub.eu", 63, "eahub.eu",
      "Local Wi-Fi only: the community server the Online menu uses in place of EA's closed servers." },
};

const Item FreeRoam[] = {
    { "Free roam", Ini, "Enable", Choice, OPTIONS(IniOnOff), "0", 0, nullptr,
      "Meet other players in the city (NFSU2 Revived's free-roam mod, scripts/NFSU2FreeRoam.ini)." },
    { "Name", Ini, "Name", Text, nullptr, 0, "", 24, "Profile name",
      "The name your friends see over your car." },
    { "Room", Ini, "Room", Text, nullptr, 0, "", 32, "Everybody",
      "Only players with the same room name see each other." },
    { "Crews", Ini, "Crew", Text, nullptr, 0, "", 120, "None",
      "Up to 4 crews, joined by commas. Players who share one see each other's names and map arrows in its colour." },
    { "Crew colours", Ini, "CrewColours", Text, nullptr, 0, "", 40, "Cyan, magenta, violet, red",
      "Each crew's colour in the order of Crews, 6 hex digits each, joined by commas (3cd2ff, ff4646)." },
    { "Ghost mode", Ini, "Collide", Choice, OPTIONS(Collide), "0", 0, nullptr,
      "Ghost cars: drive through other players. Solid cars: they can shove yours. Ghost everything: traffic too." },
    { "Group size", Ini, "MaxPlayers", Choice, OPTIONS(MaxPlayers), "8", 0, nullptr,
      "Players in your group, you included. More players cost frames on the Vita." },
    { "Players' cars shown", Ini, "MaxCars", Choice, OPTIONS(MaxCars), "2", 0, nullptr,
      "Other players' cars drawn at once. Each one is a car load of 2 to 5 seconds and costs frames." },
    { "Update rate", Ini, "Rate", Choice, OPTIONS(Rate), "", 0, nullptr,
      "How often your car's position is sent." },
    { "Smoothing delay", Ini, "Delay", Choice, OPTIONS(Delay), "", 0, nullptr,
      "Other cars are drawn this far in the past, smoothed. Less is closer to live, more is smoother." },
    { "Prediction", Ini, "Extrap", Choice, OPTIONS(Extrap), "", 0, nullptr,
      "How far ahead a car is predicted when its updates are late." },
};

const Item Display[] = {
    { "Widescreen", Cfg, "widescreen", Choice, OPTIONS(OnOff), "on", 0, nullptr,
      "On fills the 16:9 screen with the HUD at the edges; off is the original 4:3 picture with black bars." },
    { "Graphics preset", Cfg, "gfx_preset", Choice, OPTIONS(Preset), "vita", 0, nullptr,
      "Vita is what the port is tuned for; Max turns everything on and is much slower. The game's Display menu "
      "changes the individual options." },
    { "Scenery detail distance", Cfg, "world_lod_pixels", Choice, OPTIONS(LodPixels), "0", 0, nullptr,
      "Scenery narrower than this on screen uses its simpler model. More gives speed and more pop-in." },
    { "Small scenery", Cfg, "world_min_pixels", Choice, OPTIONS(MinPixels), "0", 0, nullptr,
      "Scenery smaller than this on screen is not drawn. More gives speed and more pop-in." },
    { "Optimised particles", Cfg, "optimised_particles", Choice, OPTIONS(OnOff), "on", 0, nullptr,
      "Cheaper tyre smoke and other particles, close to the original picture. Off is exactly as the original." },
};

const Item Sound[] = {
    { "Hardware music decoding", Cfg, "music_hw_decode", Choice, OPTIONS(OnOff), "on", 0, nullptr,
      "The Vita's MP3 decoder plays the music, which frees the processor. It sounds the same." },
    { "Short sound lock", Cfg, "audio_mixer_split", Choice, OPTIONS(OnOff), "on", 0, nullptr,
      "The sound mixer holds the game's sound lock for a shorter time. Worth about 1 fps in busy races." },
    { "Mix rate", Cfg, "audio_mix_rate", Choice, OPTIONS(MixRate), "0", 0, nullptr,
      "The rate the game mixes its sound at. 32000 Hz needs less processor time; 22050 Hz less again, but duller." },
    { "Quiet sounds", Cfg, "audio_silence_db", Choice, OPTIONS(Silence), "-60", 0, nullptr,
      "Sounds quieter than this are not mixed, which saves processor time. -60 dB is inaudible; off mixes all." },
};

const Item Controller[] = {
    { "Rear touch pad", Cfg, "rear_touch", Choice, OPTIONS(OnOff), "on", 0, nullptr,
      "The rear touch pad's corners: shifting above, look back and camera below." },
    { "Touch screen", Cfg, "front_touch", Choice, OPTIONS(OnOff), "on", 0, nullptr,
      "Touching the screen works as a mouse click in the menus." },
    { "Garage camera stick", Cfg, "garage_stick", Choice, OPTIONS(OnOff), "on", 0, nullptr,
      "The right stick turns the car in the garage and the customisation menus." },
    { "Camera stick dead zone", Cfg, "garage_stick_deadzone", Choice, OPTIONS(DeadZone), "20", 0, nullptr,
      "How far the right stick moves before the garage camera turns." },
    { "Camera stick speed", Cfg, "garage_stick_speed", Choice, OPTIONS(Speed), "4", 0, nullptr,
      "How fast the garage camera turns with the stick fully over." },
};

const Item Game[] = {
    { "Frame rate", Cfg, "frame_rate", Choice, OPTIONS(FrameRate), "30", 0, nullptr,
      "30 locks the game to 30 frames a second for steady pacing; unlocked runs as fast as it can." },
    { "Free-roam log", Ini, "Log", Choice, OPTIONS(IniOnOff), "1", 0, nullptr,
      "Write scripts/NFSU2FreeRoam.log." },
    { "Detailed log", Cfg, "log_info", Choice, OPTIONS(OnOff), "off", 0, nullptr,
      "Write a detailed log to NeedForSpeedUnderground2.log, useful when reporting a problem." },
};

struct Page
{
    const char* title;
    const Item* items;
    int         count;
};

// The browser's Settings sections, the Vita's settings in each (Frame rate's one row is in Game). The game's
// options list has rows for 11 settings at most.
const Page Pages[] = {
    { "Multiplayer", OPTIONS(Multiplayer) },
    { "Free Roam", OPTIONS(FreeRoam) },
    { "Display", OPTIONS(Display) },
    { "Sound", OPTIONS(Sound) },
    { "Controller", OPTIONS(Controller) },
    { "Game", OPTIONS(Game) },
};
const int PageCount = int(sizeof(Pages) / sizeof(Pages[0]));

// Written when free roam is first turned on without an .ini: the PC mod's file with the Vita's defaults.
const char IniTemplate[] =
    "; NFSU2FreeRoam.ini: free-roam multiplayer for Need for Speed Underground 2 on the PS Vita port, the same\n"
    "; settings as the PC's NFSU2FreeRoam.asi plugin (NFSU2 Revived). It belongs in the \"scripts\" folder of the\n"
    "; game folder, as on PC (the Vita has the free-roam code built in, so there is no .asi). Written by the\n"
    "; main menu's Vita Settings; it can be edited by hand too.\n"
    "\n"
    "[FreeRoam]\n"
    "; 1 = on, 0 = the plugin does nothing\n"
    "Enable = 1\n"
    "\n"
    "; the name your friends see (printable ASCII, up to 24 characters); empty = your game profile's name\n"
    "Name =\n"
    "\n"
    "; only players with the same room name see each other (empty = everybody)\n"
    "Room =\n"
    "\n"
    "; your crews, up to 4, joined by commas (empty = none)\n"
    "Crew =\n"
    "\n"
    "; each crew's colour in the order of Crew, 6 hex digits without '#' (empty = cyan, magenta, violet, red)\n"
    "CrewColours =\n"
    "\n"
    "; 1 = friends' cars are solid; 0 = ghost cars you drive through; none = traffic is a ghost too\n"
    "Collide = 0\n"
    "\n"
    "; group size, you included (2..33), and friends' cars shown at once (0..5)\n"
    "MaxPlayers = 8\n"
    "MaxCars = 2\n"
    "\n"
    "; write scripts\\NFSU2FreeRoam.log (1) or not (0)\n"
    "Log = 1\n";

// ---- Settings files ---------------------------------------------------------------------------------------------------

typedef std::vector<std::string> Lines;

bool readLines(const std::string& path, Lines* lines)
{
    lines->clear();
    FILE* f = fopen(path.c_str(), "r");
    if (!f)
        return false;
    std::string line;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), f))
    {
        line += buffer;
        if (!line.empty() && line.back() == '\n')
        {
            line.pop_back();
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines->push_back(line);
            line.clear();
        }
    }
    if (!line.empty())
        lines->push_back(line);
    fclose(f);
    return true;
}

void splitLines(const char* text, Lines* lines)
{
    lines->clear();
    std::string line;
    for (const char* p = text; *p; ++p)
    {
        if (*p == '\n')
        {
            lines->push_back(line);
            line.clear();
        }
        else
            line += *p;
    }
    if (!line.empty())
        lines->push_back(line);
}

// Written beside the file and moved over it; the Vita refuses to rename over a file, so that one goes first.
bool writeLines(const std::string& path, const Lines& lines)
{
    const std::string part = path + ".part";
    FILE* f = fopen(part.c_str(), "w");
    if (!f)
        return false;
    bool ok = true;
    for (const std::string& line : lines)
        ok = fprintf(f, "%s\n", line.c_str()) >= 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok)
    {
        unlink(part.c_str());
        return false;
    }
    if (rename(part.c_str(), path.c_str()) == 0)
        return true;
    struct stat status;
    return stat(path.c_str(), &status) == 0 && unlink(path.c_str()) == 0 && rename(part.c_str(), path.c_str()) == 0;
}

std::string trimmed(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}

// The key a "key = value" line sets (case-insensitive), with the value; commented: the line starts with '#' or ';'.
bool lineSets(const std::string& line, const char* key, bool commented, std::string* value)
{
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    const bool comment = i < line.size() && (line[i] == '#' || line[i] == ';');
    if (comment != commented)
        return false;
    if (comment)
    {
        ++i;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            ++i;
    }
    const size_t n = strlen(key);
    if (line.size() < i + n || strncasecmp(line.c_str() + i, key, n) != 0)
        return false;
    i += n;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    if (i >= line.size() || line[i] != '=')
        return false;
    if (value)
        *value = trimmed(line.substr(i + 1));
    return true;
}

// The .ini's value after an inline comment is cut, as the free-roam port reads it (roam_vita.cpp).
std::string iniValue(const std::string& raw)
{
    std::string s = raw;
    for (size_t i = 0; i < s.size(); ++i)
        if ((s[i] == ';' || s[i] == '#') && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t'))
        {
            s.resize(i);
            break;
        }
    return trimmed(s);
}

// [FreeRoam]: the first line after its header and the line after its last key; false without the section.
bool iniSection(const Lines& lines, size_t* begin, size_t* end)
{
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const std::string t = trimmed(lines[i]);
        if (t.size() < 2 || t[0] != '[')
            continue;
        if (strncasecmp(t.c_str(), "[FreeRoam]", 10) != 0)
            continue;
        *begin = i + 1;
        size_t last = i + 1;
        for (size_t j = i + 1; j < lines.size(); ++j)
        {
            const std::string u = trimmed(lines[j]);
            if (!u.empty() && u[0] == '[')
                break;
            if (!u.empty() && u[0] != ';' && u[0] != '#')
                last = j + 1;
        }
        *end = last;
        return true;
    }
    return false;
}

bool cfgGet(const Lines& lines, const char* key, std::string* value)
{
    bool found = false;
    for (const std::string& line : lines)
        found = lineSets(line, key, false, value) || found;     // the last line wins, as Config reads it
    return found;
}

void cfgSet(Lines& lines, const char* key, const std::string& value)
{
    const std::string line = std::string(key) + "=" + value;
    for (size_t i = lines.size(); i-- > 0;)
        if (lineSets(lines[i], key, false, nullptr))
        {
            lines[i] = line;
            return;
        }
    // Under the commented default the settings file was written with, so the explanation stays above it.
    for (size_t i = 0; i < lines.size(); ++i)
        if (lineSets(lines[i], key, true, nullptr))
        {
            lines.insert(lines.begin() + long(i) + 1, line);
            return;
        }
    lines.push_back(line);
}

bool iniGet(const Lines& lines, const char* key, std::string* value)
{
    size_t begin, end;
    if (!iniSection(lines, &begin, &end))
        return false;
    bool found = false;
    for (size_t i = begin; i < end; ++i)
    {
        std::string raw;
        if (lineSets(lines[i], key, false, &raw))
        {
            *value = iniValue(raw);
            found = true;
        }
    }
    return found;
}

void iniSet(Lines& lines, const char* key, const std::string& value)
{
    const std::string line = std::string(key) + " = " + value;
    size_t begin, end;
    if (!iniSection(lines, &begin, &end))
    {
        lines.push_back("[FreeRoam]");
        lines.push_back(line);
        return;
    }
    for (size_t i = begin; i < end; ++i)
        if (lineSets(lines[i], key, false, nullptr))
        {
            lines[i] = line;
            return;
        }
    lines.insert(lines.begin() + long(end), line);
}

int boolValue(const std::string& v)
{
    const char* const On[] = { "on", "1", "true", "yes" };
    const char* const Off[] = { "off", "0", "false", "no" };
    for (const char* s : On)
        if (!strcasecmp(v.c_str(), s))
            return 1;
    for (const char* s : Off)
        if (!strcasecmp(v.c_str(), s))
            return 0;
    return -1;
}

// The option a value is, -1 for none (a value written by hand).
int optionIndex(const Item& item, const std::string& value)
{
    for (int i = 0; i < item.optionCount; ++i)
        if (!strcasecmp(item.options[i].value, value.c_str()))
            return i;
    // Booleans in any spelling the settings file takes; network=on is revived.
    const int b = boolValue(value);
    for (int i = 0; b >= 0 && i < item.optionCount; ++i)
        if (boolValue(item.options[i].value) == b)
            return i;
    if (!strcmp(item.key, "network") && b == 1)
        return 0;
    return -1;
}

// ---- The settings now --------------------------------------------------------------------------------------------------

std::string s_cfgPath, s_iniPath;
bool s_enabled = false, s_loaded = false;
// Per page and item: the value in the file (or the default) and the value now.
std::vector<std::vector<std::string>> s_saved, s_values;

const Item& item(int g, int i)
{
    return Pages[g].items[i];
}

}

void configure(const char* settingsPath, const char* gameFolder)
{
    s_enabled = pf::Config::getBool("options_menu", true);
    s_cfgPath = settingsPath;
    s_iniPath = std::string(gameFolder) + "/scripts/NFSU2FreeRoam.ini";
    pf::logInfo("options menu: %s", s_enabled ? "the main menu's Vita Settings" : "off (options_menu=off)");
}

bool enabled()
{
    return s_enabled;
}

void load()
{
    Lines cfg, ini;
    readLines(s_cfgPath, &cfg);
    const bool iniExists = readLines(s_iniPath, &ini);
    s_saved.assign(PageCount, {});
    for (int p = 0; p < PageCount; ++p)
        for (int i = 0; i < Pages[p].count; ++i)
        {
            const Item& it = Pages[p].items[i];
            std::string v = it.def;
            if (it.store == Cfg)
                cfgGet(cfg, it.key, &v);
            else if (!strcmp(it.key, "Enable"))
            {
                if (iniExists)      // Enable = 1 when the .ini has none, as roam_vita.cpp reads it
                {
                    v = "1";
                    iniGet(ini, it.key, &v);
                }
                else
                    v = "0";        // no .ini: free roam is off
            }
            else
                iniGet(ini, it.key, &v);
            s_saved[p].push_back(v);
        }
    s_values = s_saved;
    s_loaded = true;
}

bool dirty()
{
    return s_loaded && s_values != s_saved;
}

bool save()
{
    if (!dirty())
        return true;
    Lines cfg, ini;
    readLines(s_cfgPath, &cfg);
    const bool haveIni = readLines(s_iniPath, &ini);
    bool cfgChanged = false, iniChanged = false;
    for (int p = 0; p < PageCount; ++p)
        for (int i = 0; i < Pages[p].count; ++i)
        {
            if (s_values[p][i] == s_saved[p][i])
                continue;
            const Item& it = Pages[p].items[i];
            if (it.store == Cfg)
            {
                cfgSet(cfg, it.key, s_values[p][i]);
                cfgChanged = true;
            }
            else
            {
                if (!haveIni && !iniChanged)
                    splitLines(IniTemplate, &ini);
                iniSet(ini, it.key, s_values[p][i]);
                iniChanged = true;
            }
        }
    bool ok = true;
    if (cfgChanged && !writeLines(s_cfgPath, cfg))
    {
        pf::logError("options menu: %s could not be written", s_cfgPath.c_str());
        ok = false;
    }
    if (iniChanged)
    {
        const std::string folder = s_iniPath.substr(0, s_iniPath.rfind('/'));
        mkdir(folder.c_str(), 0777);
        if (!writeLines(s_iniPath, ini))
        {
            pf::logError("options menu: %s could not be written", s_iniPath.c_str());
            ok = false;
        }
    }
    if (ok)
    {
        pf::logInfo("options menu: settings saved");
        s_saved = s_values;
    }
    return ok;
}

void restart()
{
    pf::logInfo("options menu: restarting the game");
#ifdef __vita__
    const int result = sceAppMgrLoadExec("app0:eboot.bin", nullptr, nullptr);
    pf::logError("options menu: sceAppMgrLoadExec failed: 0x%08x", result);
#endif
}

int groupCount()
{
    return PageCount;
}

const char* groupTitle(int g)
{
    return Pages[g].title;
}

int itemCount(int g)
{
    return Pages[g].count;
}

const char* itemLabel(int g, int i)
{
    return item(g, i).label;
}

bool itemIsText(int g, int i)
{
    return item(g, i).kind != Choice;
}

bool itemIsSecret(int g, int i)
{
    return item(g, i).kind == Secret;
}

int itemMaxLength(int g, int i)
{
    return item(g, i).maxLength;
}

std::string itemValue(int g, int i)
{
    if (!s_loaded)
        load();
    const Item& it = item(g, i);
    const std::string& v = s_values[g][i];
    if (it.kind == Secret)
        return v.empty() ? "Not set" : "Set";
    if (it.kind == Text)
        return v.empty() ? std::string(it.empty) : v;
    const int o = optionIndex(it, v);
    return o >= 0 ? it.options[o].name : v;
}

std::string itemText(int g, int i)
{
    if (!s_loaded)
        load();
    return item(g, i).kind == Text ? s_values[g][i] : std::string();
}

void cycle(int g, int i, int delta)
{
    if (!s_loaded)
        load();
    const Item& it = item(g, i);
    if (it.kind != Choice)
        return;
    std::string& v = s_values[g][i];
    int o = optionIndex(it, v);
    o = o < 0 ? 0 : (o + delta + it.optionCount) % it.optionCount;
    v = it.options[o].value;
}

void setText(int g, int i, const std::string& text)
{
    if (!s_loaded)
        load();
    const Item& it = item(g, i);
    if (it.kind == Choice || (it.kind == Secret && text.empty()))
        return;
    std::string v = trimmed(text);
    if (int(v.size()) > it.maxLength)
        v.resize(size_t(it.maxLength));
    s_values[g][i] = v;
}

}}
