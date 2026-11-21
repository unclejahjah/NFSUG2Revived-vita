// Direct boot (boot_race): a skip-front-end Street X race (track 4601, AI-driven), a URL race (boot_mode=url), a drag
// race (boot_mode=drag), a drift race (boot_mode=drift) or Bayview free roam (boot_mode=freeroam, driven blind). Also the virtual clock.

#include "boot.h"
#include "bench.h"
#include "drive.h"
#include "frontend.h"

#include <platform/config.h>
#include <platform/log.h>
#include <platform/render.h>
#include <platform/sync.h>
#include <platform/time.h>
#if defined(__vita__)
#include <psp2/display.h>
#endif
#include <recomp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ug2 { namespace native { namespace flush { void fence(); }}}

namespace ug2 { namespace boot {

namespace {

using x86::reg32;

const reg32 SkipFe = 0x00864FAC;               // main 0x580E00 starts the race (0x53FEB0); cleared at track unload
const reg32 IntroScene = 0x00827E90;           // set by 0x437160, which the loader 0x57FA94 skips with SkipFe; 0 no intro
const reg32 SkipFeTrack = 0x007FAA18;          // the settings below are read by 0x53FEB0
const reg32 SkipFeHumans = 0x007FAA1C;
const reg32 SkipFeOpponents = 0x00864FB0;
const reg32 SkipFeLaps = 0x007FAA20;
const reg32 SkipFeDirection = 0x00864FB4;
const reg32 SkipFeRaceMode = 0x007FAA28;       // 1 a race; 0 with track 4000 loads Bayview with no route or finish
const reg32 SkipFeTraffic = 0x00864FBC;        // traffic level, 0 none
const reg32 SkipFeDrag = 0x00864FC0;
const reg32 SkipFeDrift = 0x00864FC4;
const reg32 SkipFeDriftHud = 0x00864FCC;       // a race type with the drift HUD
const reg32 SkipFeStreetX = 0x00864FD0;
const reg32 SkipFePointToPoint = 0x00864FD4;
const reg32 SkipFeManual = 0x00864FD8;         // byte: player 0's transmission, copied to the profile at 0x540044 (1 manual)
const reg32 SkipFeRaceFlag32 = 0x007FAA44;     // copied to TheRaceParameters+0x32 (0x89E7D2), which SetupURL sets
const reg32 RaceParametersUrl = 0x0089E7E1;    // byte TheRaceParameters+0x41: URL race, set by SetupURL 0x53FA40
const reg32 RaceDriverCount = 0x0089E814;      // TheRaceParameters driver infos: count, then the array pointer
const reg32 RaceDrivers = 0x0089E810;
const reg32 DriverInfoSize = 0x940;            // +3 grid position, +4 kind (1 human, 2 AI)
const reg32 StartingPositions = 0x005264A0;    // cdecl (order): 1 humans first (0x53FEB0), 2 AI first (SetupURL)
const reg32 AiFirst = 2;
const reg32 UpgradeLevel = 0x007FAAC0;         // -1 by default; 0..3 installs that level of every performance package
const reg32 PerfTable = 0x008383F8;            // performance packages: category, level, ...; read by 0x53FEB0
const reg32 PerfCount = 0x008383FC;
const reg32 PerfEntrySize = 0x17C;
const reg32 Kiosk = 0x008650B0;                // nonzero replaces the settings with the kiosk race's
const reg32 PlayerCar = 0x007FAA48;            // car index, read by the profile defaults 0x5346F0 after GLOBALB
const reg32 AudioEnable = 0x007FAAA8;          // read by InitAudio 0x4859B0 and the static initialiser 0x77ABE0
const reg32 DrawHud = 0x007F91C4;              // gates UI drawing: 0x5378C0, 0x537930, 0x566400
const reg32 CrowdEnable = 0x00870CD0;          // g_CrowdEnable, set at display init 0x5D24F0
const reg32 PlayerControl = 0x0089008C;        // World::Update 0x609CE0 gives player 0's car this control
const reg32 Players = 0x008900AC;              // player pointers, player 0 first
const reg32 PlayerVehicle = 0x4;
const reg32 VehiclePosition = 0x60;            // x, y, z
const reg32 AiControl = 3;
const reg32 RandomSeed = 0x007EA624;           // seeded from the ticker by 0x57ECB0 at start-up
const reg32 TrackTableLoader = 0x005D3EB0;     // chunk 0x34201, in the GLOBALB load at 0x57ED99
const reg32 TrackTable = 0x00883DA0;
const reg32 TrackCount = 0x00883DA4;
const reg32 TrackEntrySize = 0x128;
const reg32 TrackRegion = 0x40;
const reg32 TrackPointToPoint = 0x85;
const reg32 TrackNumber = 0x8A;                // int16
const reg32 TrackRaceTypes = 0x94;             // int16 mask of the race types the track allows
const reg32 UrlRaceType = 0x2000;              // set only on the 47xx Speedway tracks
const size_t TrackTextSize = 32;
const reg32 CarTable = 0x008A1CCC;             // chunk 0x34600, before the track table in GLOBALB
const reg32 CarCount = 46;
const reg32 CarEntrySize = 0x890;
const reg32 CarIndex = 0x840;
const reg32 CarKind = 0x844;                   // 0 drivable, 2 traffic
const size_t CarNameSize = 32;
const reg32 TrackLoad = 0x00580EE0;            // game-flow callback: region and track load
const reg32 TrackUnloads[] = { 0x005817D0, 0x00581820 };
const reg32 RaceCoordinator = 0x008900D8;
const reg32 StateHandlers = 0x0080214C;        // per coordinator state: thiscall (message, argument), ret 8
const int StateCount = 9;
const reg32 StateEntered = 1;                  // message: the coordinator state was just set
const int LoadingState = 1;                    // ends when the track streamer 0x88EB00 has loaded its sections
const int PreRaceState = 3;
const int RacingState = 4;
const int PostRaceState = 7;
const reg32 QueryPerformanceCounterSlot = 0x0078311C;
const reg32 FrameCarry = 0x00865148;           // these three are read only by the frame timing 0x57EAD0
const reg32 FrameMaxSixtieths = 0x007FB714;
const reg32 FrameMin = 0x00865558;
const reg32 FrameTicks = 0x00865140;           // 1/1800 s each
const reg32 FramesRendered = 0x0086514C;       // incremented at the end of each frame by 0x57A050

// World clock values after its reset 0x576CF0; 0x57A0E0 advances it each frame.
struct GuestDword
{
    reg32 address;
    reg32 value;
};
const GuestDword WorldClockStart[] = {
    { 0x007FB718, 0x3F800000 },                // seconds, 1.0f
    { 0x007FB71C, 1 },
    { 0x007FB720, 1 },                         // 60 Hz ticks
    { 0x007FB724, 0 },                         // world frames
    { 0x00865150, 0 },
    { 0x00865158, 0 },
    { 0x0086518C, 4000 },                      // 1/4000 s
};
const int VirtualTicks = 60;
const int DefaultTrack = 4601;
const int FreeRoamTrack = 4000;                // "(40XX-41XX) Circuit/Sprint", region L4RA: the whole city
const int FreeRoamSeconds = 120;
const int FreeRoamWindow = 5;
const int StreetXSeries = 46;
const int MaxOpponents = 5;
const int DragTrack = 4201;                    // "(4201) City Drag #1"
const int DriftTrack = 4301;                   // "(4301) Vacant Lot Drift #1"
const int DriftOpponents = 3;
const int UrlTrack = 4711;                     // "(4711) Speedway Airport #1"
const int UrlOpponents = 5;                    // quick race mode 4 (URL) at 0x4B2C55; 3 for the others

struct Settings
{
    int track = DefaultTrack;
    std::string car;
    bool drag = false;
    bool drift = false;
    bool aiDriver = true;
    bool seed = true;
    bool crowd = false;
    bool intro = false;
    bool freeRoam = false;
    bool url = false;
    bool blindDrive = false;
    bool manual = false;
    int upgrades = 0;
};

Settings s_settings;
reg32 s_drawHud = 1;                           // the game's value while the boot race hides it
bool s_hudHidden = false;
bool s_aiDriving = false;
bool s_introRace = false;                      // SkipFe cleared for this boot race's intro
reg32 s_raceStart = 0;                         // frame of the race start
win32::MethodPtr s_trackTableLoader = nullptr;
win32::MethodPtr s_trackLoad = nullptr;
win32::MethodPtr s_trackUnloads[2] = {};
win32::MethodPtr s_stateHandlers[StateCount] = {};
win32::MethodPtr s_queryPerformanceCounter = nullptr;
int s_mainThread = 0;
reg32 s_frame = 0;
int s_frameState = -1;                         // coordinator state at the last frame end, -1 without one
int s_heldFrames = 0;                          // loading frames of the current race with the world clock held
reg32 s_loadEnd = 0;                           // frame the last held loading ended

// Writes a guest dword and logs it with its previous value.
void set(const char* name, reg32 address, reg32 value)
{
    const reg32 previous = rc::ld32(address);
    rc::st32(address, value);
    pf::logInfo("boot: %s [0x%08X] = %d (was %d)", name, unsigned(address), int(value), int(previous));
}

std::string guestText(reg32 address, size_t size)
{
    std::string text;
    for (reg32 i = 0; i < size && rc::ld8(address + i); ++i)
        text += char(rc::ld8(address + i));
    return text;
}

// Lowercase letters and digits of text.
std::string key(const std::string& text)
{
    std::string k;
    for (char c : text)
    {
        if (c >= 'A' && c <= 'Z')
            k += char(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            k += c;
    }
    return k;
}

// The mix of 0x43E6B0 that 0x57ECB0 applies, at ticker 0.
reg32 fixedSeed()
{
    const reg32 start = 0x12345678u ^ 0x1D872B41u;
    const reg32 mixed = start ^ (start >> 5);
    return mixed ^ (mixed << 27) ^ start;
}

reg32 trackEntry(int track)
{
    const reg32 table = rc::ld32(TrackTable);
    const reg32 count = rc::ld32(TrackCount);
    for (reg32 i = 0; table && i < count; ++i)
    {
        const reg32 entry = table + i * TrackEntrySize;
        if (int(int16_t(rc::ld16(entry + TrackNumber))) == track)
            return entry;
    }
    return 0;
}

// Entry of a drivable car index; 0 if none.
reg32 carEntry(int index)
{
    const reg32 table = rc::ld32(CarTable);
    for (reg32 i = 0; table && i < CarCount; ++i)
    {
        const reg32 entry = table + i * CarEntrySize;
        if (rc::ld32(entry + CarKind) == 0 && int(rc::ld32(entry + CarIndex)) == index)
            return entry;
    }
    return 0;
}

// Drivable car index named by text; -1 if none.
int findCar(const std::string& text)
{
    const std::string k = key(text);
    if (!k.empty() && k.find_first_not_of("0123456789") == std::string::npos)
    {
        const int index = atoi(k.c_str());
        return carEntry(index) ? index : -1;
    }
    const reg32 table = rc::ld32(CarTable);
    for (reg32 i = 0; table && i < CarCount; ++i)
    {
        const reg32 entry = table + i * CarEntrySize;
        if (rc::ld32(entry + CarKind) == 0 && key(guestText(entry, CarNameSize)) == k)
            return int(rc::ld32(entry + CarIndex));
    }
    return -1;
}

std::string carName(int index)
{
    const reg32 entry = carEntry(index);
    return entry ? key(guestText(entry, CarNameSize)) : "?";
}

// Sets player 1's car from boot_car, else keeps the game's.
void applyCar()
{
    if (!rc::ld32(CarTable))
        pf::logError("boot: the car table is not loaded");
    if (s_settings.car == "list")
    {
        const reg32 table = rc::ld32(CarTable);
        for (reg32 i = 0; table && i < CarCount; ++i)
        {
            const reg32 entry = table + i * CarEntrySize;
            pf::logInfo("boot: car entry %u: index %d kind %u name \"%s\"", unsigned(i),
                        int(rc::ld32(entry + CarIndex)), unsigned(rc::ld32(entry + CarKind)),
                        guestText(entry, CarNameSize).c_str());
        }
    }
    else if (!s_settings.car.empty())
    {
        const int index = findCar(s_settings.car);
        if (index >= 0)
            set("player car", PlayerCar, reg32(index));
        else
            pf::logError("boot: boot_car=%s is not a drivable car's name or index", s_settings.car.c_str());
    }
    const int index = int(rc::ld32(PlayerCar));
    pf::logInfo("boot: player car %d (%s)", index, carName(index).c_str());
}

// Runs the loader, then sets track, car and seed.
void trackTableLoaderHook(win32::WinApplication* app, x86::CPU& cpu)
{
    s_trackTableLoader(app, cpu);
    static bool s_applied = false;
    if (s_applied || !rc::ld32(TrackTable))
        return;
    s_applied = true;
    int track = s_settings.track;
    reg32 entry = trackEntry(track);
    if (!entry && track != DefaultTrack)
    {
        pf::logError("boot: track %d is not in the track table, using %d", track, DefaultTrack);
        track = DefaultTrack;
        entry = trackEntry(track);
    }
    if (entry)
        pf::logInfo("boot: track %d \"%s\", region %s, %s", track, guestText(entry, TrackTextSize).c_str(),
                    guestText(entry + TrackRegion, TrackTextSize).c_str(),
                    rc::ld8(entry + TrackPointToPoint) ? "point to point" : "closed loop");
    else
        pf::logError("boot: track %d is not in the track table", track);
    if (s_settings.url && entry && !(rc::ld16(entry + TrackRaceTypes) & UrlRaceType))
        pf::logError("boot: track %d does not allow URL races (race types 0x%04X); the 47xx Speedway tracks do", track,
                     unsigned(rc::ld16(entry + TrackRaceTypes)));
    set("track", SkipFeTrack, reg32(track));
    set("Street X", SkipFeStreetX, track / 100 == StreetXSeries ? 1 : 0);
    set("point to point", SkipFePointToPoint, entry ? rc::ld8(entry + TrackPointToPoint) : 0);
    applyCar();
    if (s_settings.seed)
        set("random seed", RandomSeed, fixedSeed());
}

// Logs the performance packages the race will install.
void logUpgrades()
{
    if (s_settings.upgrades <= 0)
        return;
    const reg32 table = rc::ld32(PerfTable);
    const reg32 count = rc::ld32(PerfCount);
    int installed[10] = {};
    for (reg32 i = 0; table && i < count; ++i)
    {
        const int category = int(rc::ld32(table + i * PerfEntrySize));
        const int level = int(rc::ld32(table + i * PerfEntrySize + 4));
        if (category >= 0 && category < 10 && level >= 1 && level <= s_settings.upgrades)
            ++installed[category];
    }
    pf::logInfo("boot: performance packages in the table: %u; installed per category 0..9 (levels 1..%d): "
                "%d %d %d %d %d %d %d %d %d %d", unsigned(count), s_settings.upgrades, installed[0], installed[1],
                installed[2], installed[3], installed[4], installed[5], installed[6], installed[7], installed[8],
                installed[9]);
}

reg32 callGuest(win32::WinApplication* app, x86::CPU& cpu, reg32 function, const reg32* args, int count)
{
    x86::CPU saved = cpu;
    reg32 esp = cpu.esp;
    for (int i = count - 1; i >= 0; --i)
        rc::st32(esp -= 4, args[i]);
    cpu.esp = esp - 4;
    app->dynamic_call(function, cpu);
    const reg32 result = cpu.eax;
    cpu = saved;
    return result;
}

// What SetupURL 0x53FA40 sets beyond 0x53FEB0's skip-front-end race.
void applyUrl(win32::WinApplication* app, x86::CPU& cpu)
{
    rc::st8(RaceParametersUrl, 1);
    pf::logInfo("boot: URL race [0x%08X] = 1", unsigned(RaceParametersUrl));
    callGuest(app, cpu, StartingPositions, &AiFirst, 1);
    pf::logInfo("boot: starting positions 0x%08X(%u): AI cars ahead of the player", unsigned(StartingPositions),
                unsigned(AiFirst));
}

// Each driver's kind and grid position.
void logDrivers()
{
    const reg32 count = rc::ld32(RaceDriverCount);
    const reg32 drivers = rc::ld32(RaceDrivers);
    std::string line;
    char item[32];
    for (reg32 i = 0; drivers && i < count && i < 16; ++i)
    {
        const reg32 d = drivers + i * DriverInfoSize;
        snprintf(item, sizeof(item), "%s%s %u", i ? ", " : "", rc::ld32(d + 4) == 1 ? "human" : "AI",
                 unsigned(rc::ld8(d + 3)));
        line += item;
    }
    pf::logInfo("boot: %u cars (kind and grid position): %s; URL flag %u", unsigned(count), line.c_str(),
                unsigned(rc::ld8(RaceParametersUrl)));
}

// Boot race: crowd, URL, and the seed again.
void trackLoadHook(win32::WinApplication* app, x86::CPU& cpu)
{
    if (rc::ld32(SkipFe))
    {
        if (s_settings.url)
            applyUrl(app, cpu);
        set("crowd", CrowdEnable, s_settings.crowd ? 1 : 0);
        logUpgrades();
        if (s_settings.seed)
            set("random seed", RandomSeed, fixedSeed());
        if (s_settings.intro)
        {
            s_introRace = true;
            set("skip front end, for the intro", SkipFe, 0);
        }
    }
    s_trackLoad(app, cpu);
}

// Gives the HUD and player 0's car back.
void endBootRace()
{
    drive::stop();
    if (s_hudHidden)
    {
        s_hudHidden = false;
        set("drawHud", DrawHud, s_drawHud);
    }
    if (s_aiDriving)
    {
        s_aiDriving = false;
        set("player control", PlayerControl, 0);
    }
}

template <int I>
void trackUnloadHook(win32::WinApplication* app, x86::CPU& cpu)
{
    native::flush::fence();
    bench::raceEnded("track unloaded");
    endBootRace();
    s_introRace = false;
    s_trackUnloads[I](app, cpu);
}

// Race length and player 0's car position, to compare runs.
void logFinish()
{
    const reg32 player = rc::ld32(Players);
    const reg32 vehicle = player ? rc::ld32(player + PlayerVehicle) : 0;
    const reg32 position = vehicle + VehiclePosition;
    pf::logInfo("boot: race finished %u frames after its start, player 0's car at (%.6f, %.6f, %.6f)",
                unsigned(rc::ld32(FramesRendered) - s_raceStart), vehicle ? rc::ldf32(position) : 0.0f,
                vehicle ? rc::ldf32(position + 4) : 0.0f, vehicle ? rc::ldf32(position + 8) : 0.0f);
}

// Graphics settings the race uses, after the game's clamp 0x5BE690.
void logGraphics()
{
    static const char* const names[] = {
        "env map", "env update", "shadow", "headlights", "lighting", "damage", "crowd", "road reflection", "fog",
        "motion blur", "light streaks", "light glow", "animated textures", "particles", "depth of field",
        "world LOD", "car LOD", "overbright", "bleach bypass", "tinting", "FSAA", "horizon fog", "rain", "0x870D14",
        "texture filtering", "resolution",
    };
    std::string line;
    char item[48];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    {
        snprintf(item, sizeof(item), "%s%s %d", i ? ", " : "", names[i], int(rc::ld32(0x00870CB8 + 4 * reg32(i))));
        line += item;
    }
    pf::logInfo("boot: graphics in the race: %s, performance level %d, vsync %d", line.c_str(),
                int(rc::ld32(0x0087099C)), int(rc::ld32(0x00870D24)));
}

// Boot race: state log, race-start seed and the AI driver.
void stateEntered(int state)
{
    if (!rc::ld32(SkipFe) && !s_introRace)
        return;
    static int s_previous = -1;
    const int previous = s_previous;
    s_previous = state;
    const reg32 coordinator = rc::ld32(RaceCoordinator);
    pf::logInfo("boot: race coordinator state %d at frame %u", coordinator ? int(rc::ld32(coordinator)) : state,
                unsigned(rc::ld32(FramesRendered)));
    static reg32 s_preRaceFrame = 0;
    if (state == PreRaceState)
        s_preRaceFrame = rc::ld32(FramesRendered);
    if (state == RacingState && s_introRace && !rc::ld32(SkipFe))
    {
        pf::logInfo("boot: intro %s, %u frames", rc::ld32(IntroScene) ? "played" : "not found by the game",
                    unsigned(rc::ld32(FramesRendered) - s_preRaceFrame));
        set("skip front end, after the intro", SkipFe, 1);
    }
    if (state == RacingState && previous == PreRaceState)
    {
        s_raceStart = rc::ld32(FramesRendered);
        if (s_settings.seed)
            set("random seed", RandomSeed, fixedSeed());
        logGraphics();
        logDrivers();
        bench::raceStarted();
    }
    if (state == RacingState && s_settings.blindDrive)
        drive::start();
    if (state == RacingState && s_settings.aiDriver && !s_aiDriving)
    {
        s_aiDriving = true;
        set("AI drives player 0", PlayerControl, AiControl);
    }
    else if (state == PostRaceState)
    {
        if (previous == RacingState)
        {
            bench::raceEnded("player finished");
            logFinish();
        }
        endBootRace();
    }
}

template <int State>
void stateHook(win32::WinApplication* app, x86::CPU& cpu)
{
    if (rc::ld32(cpu.esp + 4) == StateEntered)
        stateEntered(State);
    s_stateHandlers[State](app, cpu);
}

const win32::MethodPtr StateHooks[StateCount] = {
    stateHook<0>, stateHook<1>, stateHook<2>, stateHook<3>, stateHook<4>,
    stateHook<5>, stateHook<6>, stateHook<7>, stateHook<8>,
};

// Writes the shown frame as BMP; returns its colour hash.
uint64_t writeShown(const char* path)
{
    uint64_t hash = 0;
#if defined(__vita__)
    SceDisplayFrameBuf shown;
    shown.size = sizeof(shown);
    if (sceDisplayGetFrameBuf(&shown, SCE_DISPLAY_SETBUF_NEXTFRAME) >= 0 && shown.base)
    {
        const uint32_t w = shown.width, h = shown.height, row = w * 3;
        std::vector<uint8_t> bmp(54 + row * h);
        const uint32_t header[13] = { uint32_t(bmp.size()), 0, 54, 40, w, h, 1 | 24 << 16, 0, row * h, 2835, 2835, 0, 0 };
        bmp[0] = 'B';
        bmp[1] = 'M';
        memcpy(bmp.data() + 2, header, sizeof(header));
        const uint32_t* pixels = static_cast<const uint32_t*>(shown.base);
        hash = 1469598103934665603ull;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t c = pixels[(h - 1 - y) * shown.pitch + x];
                uint8_t* o = bmp.data() + 54 + y * row + x * 3;
                o[0] = uint8_t(c >> 16);
                o[1] = uint8_t(c >> 8);
                o[2] = uint8_t(c);
                hash = (hash ^ (c & 0xffffffu)) * 1099511628211ull;
            }
        if (FILE* f = fopen(path, "wb"))
        {
            fwrite(bmp.data(), 1, bmp.size(), f);
            fclose(f);
        }
    }
#else
    (void)path;
#endif
    return hash;
}

// debug_hold_frame: keeps the shown frame on screen and writes it to hold.bmp.
[[noreturn]] void holdForever()
{
    pf::sleepMs(500);
    if (writeShown("ux0:data/NeedForSpeedUnderground2/hold.bmp"))
        pf::logInfo("boot: held frame written to ux0:data/NeedForSpeedUnderground2/hold.bmp");
    for (;;)
    {
#if defined(__vita__)
        // Vita3K redraws its window only for a new frame buffer: set the shown one again.
        SceDisplayFrameBuf shown;
        shown.size = sizeof(shown);
        if (sceDisplayGetFrameBuf(&shown, SCE_DISPLAY_SETBUF_NEXTFRAME) >= 0)
            sceDisplaySetFrameBuf(&shown, SCE_DISPLAY_SETBUF_NEXTFRAME);
#endif
        pf::sleepMs(250);
    }
}

// debug_shot_frames=a,b,c: race frames; fN rendered frame N; lN N frames after loading.
void shotAt(int raceFrame, int frame, int afterLoad)
{
    struct Shot { int n; char kind; };
    static const std::vector<Shot> s_frames = [] {
        std::vector<Shot> frames;
        const char* text = pf::Config::getString("debug_shot_frames", "");
        while (text && *text)
        {
            const char kind = (*text == 'f' || *text == 'l') ? *text : 0;
            frames.push_back({atoi(text + (kind != 0)), kind});
            text = strchr(text, ',');
            text = text ? text + 1 : nullptr;
        }
        return frames;
    }();
    for (const Shot& s : s_frames)
    {
        if (s.n != (s.kind == 'f' ? frame : s.kind == 'l' ? afterLoad : raceFrame))
            continue;
        pf::sleepMs(500);
        char path[64];
        snprintf(path, sizeof(path), "ux0:data/NeedForSpeedUnderground2/shot_%s%d.bmp",
                 s.kind == 'f' ? "f" : s.kind == 'l' ? "l" : "", s.n);
        const uint64_t hash = writeShown(path);
        pf::logInfo("boot: shot of %s frame %d: hash %08x%08x",
                    s.kind == 'f' ? "rendered" : s.kind == 'l' ? "after-load" : "race", s.n, unsigned(hash >> 32),
                    unsigned(hash));
    }
}

// At a frame end: no world time passes while a race loads.
void frameEnded()
{
    const reg32 coordinator = rc::ld32(RaceCoordinator);
    const int state = coordinator ? int(rc::ld32(coordinator)) : -1;
    // State 1 also follows the race (seen from state 4 here), with nothing to load.
    if (state == LoadingState && (s_heldFrames > 0 || s_frameState <= 0))
    {
        ++s_heldFrames;
        for (const GuestDword& d : WorldClockStart)
            rc::st32(d.address, d.value);
    }
    else if (s_heldFrames > 0)
    {
        pf::logInfo("boot: virtual clock: world time held at its start for %d loading frames (race coordinator "
                    "state %d)", s_heldFrames, LoadingState);
        s_heldFrames = 0;
        s_loadEnd = rc::ld32(FramesRendered);
    }
    s_frameState = state;
    static const int s_hold = pf::Config::getInt("debug_hold_frame", 0);
    shotAt(state == RacingState && s_raceStart ? int(rc::ld32(FramesRendered) - s_raceStart) : -1,
           int(rc::ld32(FramesRendered)), s_loadEnd ? int(rc::ld32(FramesRendered) - s_loadEnd) : -1);
    if (s_hold > 0 && state == RacingState && s_raceStart && rc::ld32(FramesRendered) - s_raceStart == reg32(s_hold))
    {
        pf::logInfo("boot: holding the game thread at race frame %d", s_hold);
        holdForever();
    }
}

// QueryPerformanceCounter, then 0x57EAD0's inputs for VirtualTicks per frame.
void queryPerformanceCounterHook(win32::WinApplication* app, x86::CPU& cpu)
{
    s_queryPerformanceCounter(app, cpu);
    // 0x57EAD0 takes min(max(carry + dt, min), max / 60): a huge carry gives max, a zero min skips its busy-wait.
    rc::stf32(FrameCarry, 1.0e6f);
    rc::stf32(FrameMaxSixtieths, float(VirtualTicks) / 30.0f);
    rc::stf32(FrameMin, 0.0f);
    const reg32 frame = rc::ld32(FramesRendered);
    if (frame != s_frame && pf::Thread::currentId() == s_mainThread)
    {
        s_frame = frame;
        frameEnded();
    }
    static bool s_logged = false;
    if (!s_logged && rc::ld32(FrameTicks))
    {
        s_logged = true;
        pf::logInfo("boot: virtual clock: first frame took %u ticks", unsigned(rc::ld32(FrameTicks)));
    }
}

}

// Removes the game's frame limiter wait: no minimum step and no maximum.
void uncappedClockHook(win32::WinApplication* app, x86::CPU& cpu)
{
    s_queryPerformanceCounter(app, cpu);
    rc::stf32(FrameMin, 0.0f);
    rc::stf32(FrameMaxSixtieths, 1.0e6f);
}

// 1 while a particle batch is open (0x87074c).
uint8_t particleTag()
{
    return rc::ld32(0x87074c) ? 1 : 0;
}

void configure(win32::WinApplication& app)
{
    if (pf::render::particleTagNeeded())
        pf::render::g_drawTag = particleTag;
    if (pf::Config::getBool("virtual_clock", false))
    {
        const reg32 import = rc::ld32(QueryPerformanceCounterSlot);
        s_mainThread = pf::Thread::currentId();
        s_queryPerformanceCounter = app.replaceMethod(import, queryPerformanceCounterHook);
        pf::logInfo("boot: virtual clock on: %d ticks (1/%d s) per frame, set at each QueryPerformanceCounter "
                    "(0x%08X); world time held while a race loads", VirtualTicks, 1800 / VirtualTicks,
                    unsigned(import));
    }
    else if (pf::Config::getInt("frame_rate", 30) == 0)
    {
        s_queryPerformanceCounter = app.replaceMethod(rc::ld32(QueryPerformanceCounterSlot), uncappedClockHook);
        pf::logInfo("boot: frame rate unlocked (frame_rate=0): the game's frame limiter wait is off");
    }
    if (!pf::Config::getBool("boot_race", false))
    {
        pf::logInfo("boot: boot_race=off, the game starts with its front end");
        frontend::configure(app);
        return;
    }
    if (pf::Config::getBool("memory_log", false))
        frontend::hookFileOpens(app);
    const std::string mode = pf::Config::getString("boot_mode", "race");
    s_settings.freeRoam = mode == "freeroam";
    s_settings.url = mode == "url";
    s_settings.drag = mode == "drag";
    s_settings.drift = mode == "drift";
    if (!s_settings.freeRoam && !s_settings.url && !s_settings.drag && !s_settings.drift && mode != "race")
        pf::logError("boot: boot_mode=%s is not race, url, drag, drift or freeroam, racing", mode.c_str());
    s_settings.track = pf::Config::getInt("boot_track", s_settings.freeRoam ? FreeRoamTrack
                                                        : s_settings.url ? UrlTrack : s_settings.drag ? DragTrack
                                                        : s_settings.drift ? DriftTrack : DefaultTrack);
    s_settings.car = pf::Config::getString("boot_car", "");
    s_settings.aiDriver = !s_settings.freeRoam && pf::Config::getBool("boot_ai_driver", true);
    s_settings.blindDrive = drive::available && s_settings.freeRoam && pf::Config::getBool("boot_drive", true);
    s_settings.seed = pf::Config::getBool("boot_seed", true);
    s_settings.crowd = pf::Config::getBool("boot_crowd", false);
    s_settings.intro = !s_settings.freeRoam && pf::Config::getBool("boot_intro", false);
    s_settings.manual = (s_settings.freeRoam || s_settings.drag) && pf::Config::getBool("boot_manual", true);
    s_settings.upgrades = s_settings.freeRoam ? std::min(std::max(pf::Config::getInt("boot_upgrades", 2), 0), 3) : 0;
    const int laps = std::min(std::max(pf::Config::getInt("boot_laps", 2), 1), 10);
    const bool hud = pf::Config::getBool("boot_hud", false);
    const bool audio = pf::Config::getBool("boot_audio", !pf::Config::getBool("virtual_clock", false));
    const int trafficLevel = s_settings.url ? 0
                             : std::min(std::max(pf::Config::getInt("boot_traffic_level", pf::Config::getBool("boot_traffic", false) ? 1 : 0), 0), 3);
    const bool traffic = trafficLevel > 0;
    const int opponents = s_settings.freeRoam ? 0
                          : std::min(std::max(pf::Config::getInt("boot_opponents", s_settings.url ? UrlOpponents : s_settings.drift ? DriftOpponents : 0), 0), MaxOpponents);
    pf::logInfo("boot: direct boot into %s (boot_race=off for the front end): track %d, %d laps, car %s, %s, "
                "%d AI opponents, traffic %s, crowd %s, intro %s, HUD %s, audio %s, seed %s 0x%08X",
                s_settings.freeRoam ? "free roam (boot_mode=freeroam)" : s_settings.url ? "a URL race (boot_mode=url)" : s_settings.drag ? "a drag race (boot_mode=drag)"
                : s_settings.drift ? "a drift race (boot_mode=drift)" : "a race",
                s_settings.track, laps,
                s_settings.car.empty() ? "the game's" : s_settings.car.c_str(),
                s_settings.blindDrive ? "driven blind" : s_settings.freeRoam ? "the player drives" : s_settings.aiDriver ? "the AI drives" : "the player drives",
                opponents, traffic ? "on" : "off",
                s_settings.crowd ? "on" : "off", s_settings.intro ? "on" : "off", hud ? "on" : "off", audio ? "on" : "off",
                s_settings.seed ? "fixed" : "from the ticker, not", unsigned(fixedSeed()));

    if (s_settings.intro && !s_settings.crowd)
        pf::logError("boot: boot_intro needs boot_crowd=on: the game plays no intro with the crowd off (0x4368C8)");
    bench::configure(s_settings.freeRoam ? FreeRoamSeconds : 0, s_settings.freeRoam ? FreeRoamWindow : 0);
    if (s_settings.blindDrive)
    {
        drive::configure(uint32_t(pf::Config::getInt("boot_drive_seed", 1)));
        bench::setWindowNote(drive::describe);
    }
    set("skip front end", SkipFe, 1);
    set("human players", SkipFeHumans, 1);
    set("opponents", SkipFeOpponents, reg32(opponents));
    set("laps", SkipFeLaps, reg32(laps));
    set("direction", SkipFeDirection, 0);
    set("race mode", SkipFeRaceMode, s_settings.freeRoam ? 0 : 1);
    set("traffic level", SkipFeTraffic, reg32(trafficLevel));
    set("drag", SkipFeDrag, s_settings.drag ? 1 : 0);
    set("drift", SkipFeDrift, s_settings.drift ? 1 : 0);
    set("drift HUD race", SkipFeDriftHud, s_settings.drift ? 1 : 0);
    set("kiosk", Kiosk, 0);
    if (s_settings.url)
        set("race parameters +0x32", SkipFeRaceFlag32, 1);
    if (s_settings.manual)
    {
        rc::st8(SkipFeManual, 1);
        pf::logInfo("boot: player 0 transmission manual [0x%08X] = 1", unsigned(SkipFeManual));
    }
    if (s_settings.upgrades > 0)
        set("performance upgrade level", UpgradeLevel, reg32(s_settings.upgrades));
    if (!audio)
        set("audio", AudioEnable, 0);
    if (!hud)
    {
        s_drawHud = rc::ld32(DrawHud);
        s_hudHidden = true;
        set("drawHud", DrawHud, 0);
    }

    s_trackTableLoader = app.replaceMethod(TrackTableLoader, trackTableLoaderHook);
    s_trackLoad = app.replaceMethod(TrackLoad, trackLoadHook);
    s_trackUnloads[0] = app.replaceMethod(TrackUnloads[0], trackUnloadHook<0>);
    s_trackUnloads[1] = app.replaceMethod(TrackUnloads[1], trackUnloadHook<1>);
    for (int i = 0; i < StateCount; ++i)
        s_stateHandlers[i] = app.replaceMethod(rc::ld32(StateHandlers + 4 * i), StateHooks[i]);
    pf::logInfo("boot: hooks: track table loader 0x%08X, track load 0x%08X, track unloads 0x%08X 0x%08X, "
                "race coordinator states 0x%08X..0x%08X", unsigned(TrackTableLoader), unsigned(TrackLoad),
                unsigned(TrackUnloads[0]), unsigned(TrackUnloads[1]), unsigned(StateHandlers),
                unsigned(StateHandlers + 4 * (StateCount - 1)));
}

}}
