// The Vita's settings for the game's own Options screens (options_menu.h; the screens are menu_entry.cpp): the
// settings of the browser version's Settings page that the Vita has, grouped as there, and their files.

#include "options_menu.h"
#include "exopts.h"
#include <platform/cfgfile.h>
#include <platform/cloud_saves.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/settings.h>
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

// src/lib/winapi/revived/revived_link.cpp: Friends and crews' "Add friend by name"
void revived_friend_ask(const std::string& name);
std::string revived_friend_status(unsigned* version);

namespace ug2 { namespace options {

namespace
{

// ---- The settings ---------------------------------------------------------------------------------------------------

enum Store { Cfg, Ini, Extra };     // NeedForSpeedUnderground2.cfg, scripts/NFSU2FreeRoam.ini's [FreeRoam], or Extra Options' [Gameplay]
enum Kind { Choice, Text, Secret, Action };     // Action: a row that does something (the cloud saves')

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
const Option Network[] = { { "revived", "On" }, { "direct", "Local Wi-Fi only" }, { "off", "Off" } };
const Option Movies[] = { { "native", "Vita decoder" }, { "vp6", "Vita, else VP6" }, { "vp6only", "VP6 only" },
                          { "game", "The game's own" } };
const Option Core3Tasks[] = { { "helpers", "Helper threads" }, { "jobs", "Job workers" } };
const Option TextureCache[] = { { "0", "0 MB" }, { "12", "12 MB" }, { "24", "24 MB" }, { "32", "32 MB" },
                                { "48", "48 MB" }, { "64", "64 MB" } };
const Option TextureHeap[] = { { "0", "No cap" }, { "128", "128 MB" }, { "192", "192 MB" }, { "256", "256 MB" },
                               { "384", "384 MB" }, { "512", "512 MB" } };
const Option BlockMs[] = { { "10", "10 ms" }, { "20", "20 ms" }, { "30", "30 ms" }, { "40", "40 ms" } };
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
const Option StickDeadZone[] = { { "0", "0%" }, { "5", "5%" }, { "10", "10%" }, { "15", "15%" }, { "20", "20%" },
                                 { "25", "25%" }, { "30", "30%" } };
const Option CameraSpeed[] = { { "1", "1" }, { "2", "2" }, { "3", "3" }, { "4", "4" }, { "5", "5" },
                               { "6", "6" }, { "7", "7" }, { "8", "8" }, { "9", "9" }, { "10", "10" } };
const Option StickResponse[] = { { "standard", "Standard" }, { "linear", "Linear" }, { "precise", "Precise" } };
const Option Steering[] = { { "analog", "Analog" }, { "digital", "Digital" } };
const Option SkipIntro[] = { { "off", "Off" }, { "logos", "Logos" }, { "opening", "Logos and opening" } };
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

// The cloud saves (nfsu2.xyz keeps them under the passphrase of Multiplayer's Passphrase). The value of the row last
// used is how it went.
const Item Saves[] = {
    { "Sync with the cloud", Cfg, "sync", Action, nullptr, 0, "", 0, nullptr,
      "Sends the saves here when they are newer and takes the cloud's when those are, as the browser does. When both "
      "changed it asks you to pick Upload or Download." },
    { "Upload saves", Cfg, "upload", Action, nullptr, 0, "", 0, nullptr,
      "The saves here replace the cloud's. The server keeps its copy of an earlier day." },
    { "Download saves", Cfg, "download", Action, nullptr, 0, "", 0, nullptr,
      "The cloud's saves replace the ones here. The saves here are kept first in save_backups, the last five." },
};

const Item Mods[] = {
    { "NFSU2 Revived", Cfg, "network", Choice, OPTIONS(Network), "revived", 0, nullptr,
      "The game's LAN and Online menus through nfsu2.xyz, with the players in the browser. Local Wi-Fi only: LAN "
      "races with PCs on your network, Online through the Online server. Off: the game stays offline." },
    { "Free roam", Ini, "Enable", Choice, OPTIONS(IniOnOff), "0", 0, nullptr,
      "Meet other players in the city (NFSU2 Revived's free-roam mod, scripts/NFSU2FreeRoam.ini)." },
    { "EU Car Content", Extra, "UnlockRegionalCars", Choice, OPTIONS(IniOnOff), "0", 0, nullptr,
      "Drive the Peugeot 106 and Vauxhall Corsa in the US game (NFSU2 Extra Options). Needs their car folders." },
};

const Item Multiplayer[] = {
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

const Option Worlds[] = { { "", "Main world" }, { "World 1", "World 1" }, { "World 2", "World 2" },
                          { "World 3", "World 3" }, { "World 4", "World 4 (no ghost cars)" } };

// Free roam's worlds: a world is a room, and only the players in the same one see each other. World 1 to 4 are
// rooms by a fixed name, so everyone who picks one meets there; the main world has no name. Both rows set Room.

// The flag before your name over your car (nfsu2.xyz's list, src/lib/winapi/revived/roam/flags.dat): the common
// ones here, any other by its code in the row under it.
const Option Flags[] = { { "", "None" }, { "gb", "United Kingdom" }, { "gb-eng", "England" }, { "gb-sct", "Scotland" },
                         { "gb-wls", "Wales" }, { "gb-nir", "Northern Ireland" }, { "ie", "Ireland" },
                         { "us", "United States" }, { "ca", "Canada" }, { "au", "Australia" }, { "nz", "New Zealand" },
                         { "de", "Germany" }, { "fr", "France" }, { "es", "Spain" }, { "it", "Italy" },
                         { "nl", "Netherlands" }, { "be", "Belgium" }, { "pt", "Portugal" }, { "pl", "Poland" },
                         { "cz", "Czechia" }, { "ro", "Romania" }, { "se", "Sweden" }, { "no", "Norway" },
                         { "dk", "Denmark" }, { "fi", "Finland" }, { "ru", "Russia" }, { "ua", "Ukraine" },
                         { "tr", "Turkey" }, { "br", "Brazil" }, { "mx", "Mexico" }, { "ar", "Argentina" },
                         { "jp", "Japan" }, { "kr", "South Korea" }, { "cn", "China" }, { "in", "India" },
                         { "ph", "Philippines" }, { "id", "Indonesia" }, { "za", "South Africa" } };

const Option ChatButtons[] = { { "dpad_down", "Hold D-pad Down" }, { "dpad_up", "Hold D-pad Up" },
                               { "triangle", "Hold Triangle" }, { "select", "Select chords" } };

// Quick chat's phrases (nfsu2.xyz's, src/lib/winapi/revived/roam/phrases.h) by their numbers: the common ones.
const Option Phrases[] = { { "0", "Hey!" }, { "1", "Nice car!" }, { "2", "Thanks!" }, { "3", "Bye!" }, { "4", "Yes" }, { "5", "No" }, { "6", "Sorry!" }, { "7", "lol" }, { "8", "Follow me!" }, { "9", "Wait up!" }, { "10", "Let's go!" }, { "11", "Nice!" }, { "12", "Race?" }, { "13", "Join my race!" }, { "14", "gg" }, { "15", "One more?" }, { "16", "Yo!" }, { "17", "Hi all!" }, { "18", "Welcome!" }, { "19", "Welcome back!" }, { "20", "Long time no see!" }, { "21", "Good luck!" }, { "22", "Have fun!" }, { "23", "Gotta go!" }, { "24", "See you around!" }, { "25", "Good night!" }, { "26", "brb" }, { "27", "Back!" }, { "28", "Sure!" }, { "29", "Maybe" }, { "30", "Okay" }, { "31", "Nope" }, { "33", "On my way!" }, { "41", "ty" }, { "42", "np" }, { "44", "My bad!" } };

const Item FriendsPage[] = {
    { "Add friend by name", Ini, "AddFriend", Text, nullptr, 0, "", 24, "Type a name",
      "Send a friend request to a player by their claimed nfsu2.xyz name. Or meet them in free roam: touch front "
      "bumpers for 3 seconds. Needs your passphrase (Multiplayer)." },
    { "Quick chat", Ini, "QuickChat", Choice, OPTIONS(IniOnOff), "1", 0, nullptr,
      "Say a set phrase to the players near you: hold the quick chat button and press a phrase button. Theirs show "
      "after their names, in every player's own language on nfsu2.xyz." },
    { "Quick chat button", Cfg, "quick_chat_button", Choice, OPTIONS(ChatButtons), "dpad_down", 0, nullptr,
      "Hold it, then: Up phrase 1, Left 2, Right 3, Cross 4, Circle 5, Square 6 (Down takes Up's place when Up is "
      "the button). Select: Select + Down, Left, Right, Cross, Circle, Square. A short tap still does its usual job." },
    { "Phrase 1", Ini, "Say1", Choice, OPTIONS(Phrases), "0", 0, nullptr,
      "The quick chat phrase in slot 1 (see Quick chat button for its button)." },
    { "Phrase 2", Ini, "Say2", Choice, OPTIONS(Phrases), "2", 0, nullptr,
      "The quick chat phrase in slot 2 (see Quick chat button for its button)." },
    { "Phrase 3", Ini, "Say3", Choice, OPTIONS(Phrases), "12", 0, nullptr,
      "The quick chat phrase in slot 3 (see Quick chat button for its button)." },
    { "Phrase 4", Ini, "Say4", Choice, OPTIONS(Phrases), "8", 0, nullptr,
      "The quick chat phrase in slot 4 (see Quick chat button for its button)." },
    { "Phrase 5", Ini, "Say5", Choice, OPTIONS(Phrases), "14", 0, nullptr,
      "The quick chat phrase in slot 5 (see Quick chat button for its button)." },
    { "Phrase 6", Ini, "Say6", Choice, OPTIONS(Phrases), "1", 0, nullptr,
      "The quick chat phrase in slot 6 (see Quick chat button for its button)." },
    { "Crews", Ini, "Crew", Text, nullptr, 0, "", 120, "None",
      "Up to 4 crews, joined by commas. Players who share one see each other's names and map arrows in its colour." },
    { "Crew colours", Ini, "CrewColours", Text, nullptr, 0, "", 40, "Cyan, magenta, violet, red",
      "Each crew's colour in the order of Crews, 6 hex digits each, joined by commas (3cd2ff, ff4646)." },
};

const Item FreeRoam[] = {
    { "Player names", Ini, "Names", Choice, OPTIONS(IniOnOff), "1", 0, nullptr,
      "Show each player's name (and flag) over their car. Turns on the game's own player names too." },
    { "Your flag", Ini, "Flag", Choice, OPTIONS(Flags), "", 0, nullptr,
      "The flag the other players see before your name. Players on nfsu2.xyz pick from the same list." },
    { "Other flag", Ini, "Flag", Text, nullptr, 0, "", 8, "None",
      "Any other flag by its two-letter country code (de, jp, br), or gb-eng style for a nation." },
    { "Show flags", Ini, "ShowFlags", Choice, OPTIONS(IniOnOff), "1", 0, nullptr,
      "Show the flags before the names over the cars on your screen. The others still see yours." },
    { "World", Ini, "Room", Choice, OPTIONS(Worlds), "", 0, nullptr,
      "Free roam shows only the players in the same world. World 4 has ghost cars off for everyone in it." },
    { "Own world", Ini, "Room", Text, nullptr, 0, "", 32, "Main world",
      "A world of your own by its name: only the players who type the same name see each other." },
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
    { "Scenery detail distance", Cfg, "dev_world_lod_pixels", Choice, OPTIONS(LodPixels), "0", 0, nullptr,
      "Scenery narrower than this on screen uses its simpler model. More gives speed and more pop-in." },
    { "Small scenery", Cfg, "dev_world_min_pixels", Choice, OPTIONS(MinPixels), "0", 0, nullptr,
      "Scenery smaller than this on screen is not drawn. More gives speed and more pop-in." },
    { "Movies", Cfg, "movies", Choice, OPTIONS(Movies), "native", 0, nullptr,
      "How the game's movies play. The Vita decoder plays the converted .mp4 files; VP6 plays the originals." },
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
    { "Sound block", Cfg, "audio_block_ms", Choice, OPTIONS(BlockMs), "20", 0, nullptr,
      "Length of one block of sound output. Shorter lowers the delay and costs more processor time." },
    { "Quiet sounds", Cfg, "audio_silence_db", Choice, OPTIONS(Silence), "-60", 0, nullptr,
      "Sounds quieter than this are not mixed, which saves processor time. -60 dB is inaudible; off mixes all." },
};

const Item Controller[] = {
    { "Rear touch pad", Cfg, "rear_touch", Choice, OPTIONS(OnOff), "on", 0, nullptr,
      "The rear touch pad works as L2, R2, L3 and R3, unbound until you set them in the game's Controls screen." },
    { "Touch screen", Cfg, "front_touch", Choice, OPTIONS(OnOff), "on", 0, nullptr,
      "Touching the screen works as a mouse click in the menus." },
    { "Garage camera stick", Cfg, "garage_stick", Choice, OPTIONS(OnOff), "on", 0, nullptr,
      "The right stick turns the car in the garage and the customisation menus." },
    { "Camera stick dead zone", Cfg, "garage_stick_deadzone", Choice, OPTIONS(DeadZone), "30", 0, nullptr,
      "How far the right stick moves before the garage camera turns." },
    { "Stick dead zone", Cfg, "stick_deadzone", Choice, OPTIONS(StickDeadZone), "15", 0, nullptr,
      "How far either stick moves before it counts, steering included. Less reacts sooner; more ignores a worn stick." },
    { "Right stick camera", Cfg, "camera_stick", Choice, OPTIONS(OnOff), "on", 0, nullptr,
      "While driving, the right stick turns the chase camera round the car and it comes back when released. In the "
      "hood and bumper views pushing it down looks back. Untouched, the camera is the game's own." },
    { "Camera stick speed", Cfg, "camera_stick_speed", Choice, OPTIONS(CameraSpeed), "4", 0, nullptr,
      "How fast the right stick camera swings round to where the stick points, and back when released. "
      "1 is the slowest, 10 the fastest." },
    { "Steering", Cfg, "steering", Choice, OPTIONS(Steering), "analog", 0, nullptr,
      "Analog steers as far as the left stick is pushed. Digital: pushed past halfway, the stick turns smoothly to "
      "full lock and back when released, as on a keyboard." },
    { "Stick response", Cfg, "stick_response", Choice, OPTIONS(StickResponse), "standard", 0, nullptr,
      "Standard is finer near the centre for small steering corrections; linear follows the stick exactly; "
      "precise is finer near the centre still." },
};

const Item Performance[] = {
    { "Third CPU core", Cfg, "use_core3", Choice, OPTIONS(OnOff), "on", 0, nullptr,
      "Use the Vita's third processor core when it can be unlocked." },
    { "Third core work", Cfg, "core3_tasks", Choice, OPTIONS(Core3Tasks), "helpers", 0, nullptr,
      "What the third core does: helper and file threads with the command-list worker, or job workers." },
    { "Texture cache", Cfg, "texture_cache_mb", Choice, OPTIONS(TextureCache), "12", 0, nullptr,
      "Memory for kept texture levels. More avoids reloading them." },
    { "Texture heap", Cfg, "texture_heap_mb", Choice, OPTIONS(TextureHeap), "0", 0, nullptr,
      "Largest size of the texture heap. No cap lets the game decide." },
};

const Item Game[] = {
    { "Frame rate", Cfg, "frame_rate", Choice, OPTIONS(FrameRate), "30", 0, nullptr,
      "30 locks the game to 30 frames a second for steady pacing; unlocked runs as fast as it can." },
    { "Fast travel", Cfg, "fast_travel", Choice, OPTIONS(OnOff), "on", 0, nullptr,
      "On the World Map, pick a place and answer No to the GPS question to be put there. Yes still sets the route." },
    { "Skip intro movies", Cfg, "skip_intro_movies", Choice, OPTIONS(SkipIntro), "off", 0, nullptr,
      "Logos removes the EA and THX logos and the notice at start-up; logos and opening removes the opening movie too." },
    { "Free-roam log", Ini, "Log", Choice, OPTIONS(IniOnOff), "1", 0, nullptr,
      "Write scripts/NFSU2FreeRoam.log." },
    { "Start-up screen", Cfg, "show_configurator_on_boot", Choice, OPTIONS(OnOff), "on", 0, nullptr,
      "The screen before the game starts, where START opens the settings manager." },
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
    { "Performance", OPTIONS(Performance) },
    { "Friends and crews", OPTIONS(FriendsPage) },
    { "Saves", OPTIONS(Saves) },
    { "Mods", OPTIONS(Mods) },
};
const int PageCount = int(sizeof(Pages) / sizeof(Pages[0]));

// Written when free roam is first turned on without an .ini: the PC mod's file with the Vita's defaults.
const char FreeRoamSection[] = "FreeRoam";

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
    "; names over the other players' cars (1) or not (0), and your flag before your name: a country code\n"
    "; (gb, us, de, gb-eng; empty = none)\n"
    "Names = 1\n"
    "Flag =\n"
    "; the flags before the names drawn on your screen (1) or not (0)\n"
    "ShowFlags = 1\n"
    "\n"
    "; quick chat (1 on, 0 off) and the phrase numbers of slots 1-6 (the cfg's quick_chat_button picks them)\n"
    "QuickChat = 1\n"
    "Say1 = 0\n"
    "Say2 = 2\n"
    "Say3 = 12\n"
    "Say4 = 8\n"
    "Say5 = 14\n"
    "Say6 = 1\n"
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

// A section: the first line after its header and the line after its last key; false without the section.
bool iniSection(const Lines& lines, const char* section, size_t* begin, size_t* end)
{
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const std::string t = trimmed(lines[i]);
        if (t.size() < 2 || t[0] != '[')
            continue;
        const size_t n = strlen(section);
        if (t.size() < n + 2 || strncasecmp(t.c_str() + 1, section, n) != 0 || t[n + 1] != ']')
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

bool iniGet(const Lines& lines, const char* section, const char* key, std::string* value)
{
    size_t begin, end;
    if (!iniSection(lines, section, &begin, &end))
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

void iniSet(Lines& lines, const char* section, const char* key, const std::string& value)
{
    const std::string line = std::string(key) + " = " + value;
    size_t begin, end;
    if (!iniSection(lines, section, &begin, &end))
    {
        lines.push_back(std::string("[") + section + "]");
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

std::string s_cfgPath, s_iniPath, s_extraPath;
const Item* s_lastAction = nullptr;       // the cloud saves row used last
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
    s_extraPath = std::string(gameFolder) + "/" + exopts::IniPath;
    pf::cloud::configure(gameFolder);
    pf::logInfo("options menu: %s", s_enabled ? "the main menu's Vita Settings" : "off (options_menu=off)");
}

bool enabled()
{
    return s_enabled;
}

void load()
{
    std::string cfg;
    pf::cfgfile::readFile(s_cfgPath.c_str(), &cfg);
    Lines ini, extra;
    const bool iniExists = readLines(s_iniPath, &ini);
    const bool extraExists = readLines(s_extraPath, &extra);
    s_saved.assign(PageCount, {});
    for (int p = 0; p < PageCount; ++p)
        for (int i = 0; i < Pages[p].count; ++i)
        {
            const Item& it = Pages[p].items[i];
            std::string v = it.def;
            if (it.store == Cfg)
            {
                // The default and the old key name are the settings table's (src/platform/settings.cpp).
                if (const pf::Setting* setting = pf::findSetting(it.key))
                    v = pf::defaultText(*setting);
                pf::cfgfile::find(cfg, it.key, &v);
            }
            else if (!strcmp(it.key, "Enable"))
            {
                if (iniExists)      // Enable = 1 when the .ini has none, as roam_vita.cpp reads it
                {
                    v = "1";
                    iniGet(ini, FreeRoamSection, it.key, &v);
                }
                else
                    v = "0";        // no .ini: free roam is off
            }
            else if (it.store == Extra)
            {
                // As the mods tab reads it: no file is off, a file without the key is on.
                v = extraExists ? "1" : "0";
                if (extraExists)
                    iniGet(extra, exopts::RegionSection, it.key, &v);
            }
            else
                iniGet(ini, FreeRoamSection, it.key, &v);
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
    std::string cfg;
    pf::cfgfile::readFile(s_cfgPath.c_str(), &cfg);
    std::vector<pf::cfgfile::Change> changes;
    Lines ini;
    const bool haveIni = readLines(s_iniPath, &ini);
    bool iniChanged = false, extraChanged = false;
    Lines extra;
    const bool haveExtra = readLines(s_extraPath, &extra);
    for (int p = 0; p < PageCount; ++p)
        for (int i = 0; i < Pages[p].count; ++i)
        {
            if (s_values[p][i] == s_saved[p][i])
                continue;
            const Item& it = Pages[p].items[i];
            if (it.store == Cfg)
            {
                if (!strcmp(it.key, "revived_name") && haveIni)
                {
                    // One name: free roam's own Name follows it (empty: the game profile's name, as before).
                    iniSet(ini, FreeRoamSection, "Name", s_values[p][i]);
                    iniChanged = true;
                }
                if (!strncmp(it.key, "revived_", 8))
                    pf::Config::set(it.key, s_values[p][i].c_str());    // the cloud saves read them from there
                pf::cfgfile::Change change;
                change.key = it.key;
                change.value = s_values[p][i];
                const pf::Setting* setting = pf::findSetting(it.key);
                change.atDefault = setting && change.value == pf::defaultText(*setting);
                changes.push_back(change);
            }
            else if (it.store == Extra)
            {
                // No file and off stays no file; the mod's own settings file is written when a mod is turned on.
                if (!haveExtra && s_values[p][i] == "0")
                    continue;
                iniSet(extra, exopts::RegionSection, it.key, s_values[p][i]);
                extraChanged = true;
            }
            else
            {
                if (!haveIni && !iniChanged)
                    splitLines(IniTemplate, &ini);
                iniSet(ini, FreeRoamSection, it.key, s_values[p][i]);
                iniChanged = true;
            }
        }
    bool ok = true;
    if (!changes.empty() && !pf::cfgfile::writeFile(s_cfgPath.c_str(), pf::cfgfile::apply(cfg, changes)))
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
    if (extraChanged && !writeLines(s_extraPath, extra))
    {
        pf::logError("options menu: %s could not be written", s_extraPath.c_str());
        ok = false;
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
    // The game itself (app0:game.bin), past the start-up screen; eboot.bin when the game was copied over it.
    int result = sceAppMgrLoadExec("app0:game.bin", nullptr, nullptr);
    if (result < 0)
        result = sceAppMgrLoadExec("app0:eboot.bin", nullptr, nullptr);
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
    const Kind kind = item(g, i).kind;
    return kind == Text || kind == Secret;
}

bool itemIsAction(int g, int i)
{
    return item(g, i).kind == Action;
}

void runAction(int g, int i)
{
    const Item& it = item(g, i);
    if (it.kind != Action)
        return;
    const pf::cloud::Action action = !strcmp(it.key, "upload") ? pf::cloud::Action::Upload
                                   : !strcmp(it.key, "download") ? pf::cloud::Action::Download : pf::cloud::Action::Sync;
    if (pf::cloud::start(action))
        s_lastAction = &it;
}

unsigned actionVersion()
{
    unsigned friends = 0;
    revived_friend_status(&friends);
    return pf::cloud::version() + friends;
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
    if (it.kind == Action)
        return s_lastAction == &it ? pf::cloud::status() : std::string("Press Cross");
    if (it.kind == Secret)
        return v.empty() ? "Not set" : "Set";
    if (it.kind == Text && it.store == Ini && !strcmp(it.key, "AddFriend"))
    {
        const std::string status = revived_friend_status(nullptr);
        return status.empty() ? std::string(it.empty) : status;
    }
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

// Rows of two pages that set one key (World and Own world) show the same value.
void shareValue(int g, int i)
{
    const Item& it = item(g, i);
    for (int p = 0; p < PageCount; ++p)
        for (int k = 0; k < Pages[p].count; ++k)
        {
            const Item& other = Pages[p].items[k];
            if ((p != g || k != i) && other.store == it.store && !strcmp(other.key, it.key))
                s_values[p][k] = s_values[g][i];
        }
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
    shareValue(g, i);
}

void setText(int g, int i, const std::string& text)
{
    if (!s_loaded)
        load();
    const Item& it = item(g, i);
    if ((it.kind != Text && it.kind != Secret) || (it.kind == Secret && text.empty()))
        return;
    std::string v = trimmed(text);
    if (int(v.size()) > it.maxLength)
        v.resize(size_t(it.maxLength));
    if (it.store == Ini && !strcmp(it.key, "AddFriend"))
    {
        // not a setting: the request goes at once, and the row shows its answer (itemValue)
        revived_friend_ask(v);
        return;
    }
    s_values[g][i] = v;
    shareValue(g, i);
}

bool applyLive(int g, int i)
{
    // The settings read where they are used, rather than once at the start.
    static const char* const Live[] = { "log_info", "garage_stick", "garage_stick_deadzone", "stick_deadzone",
                                        "stick_response", "steering", "fast_travel",
                                        "camera_stick", "camera_stick_speed", "quick_chat_button" };
    const Item& it = item(g, i);
    if (it.store != Cfg)
        return false;
    for (const char* key : Live)
    {
        if (strcmp(it.key, key) != 0)
            continue;
        pf::Config::set(it.key, s_values[g][i].c_str());
        if (!strcmp(it.key, "log_info"))
            pf::logSetInfo(pf::Config::getBool("log_info", false));
        return true;
    }
    return false;
}

}}
