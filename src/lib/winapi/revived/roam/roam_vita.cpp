// Free roam with other players on the Vita: nfsu2-web's free-roam mod (runtime/roam_game.c, roam_core.c), built as
// for the PC's NFSU2FreeRoam.asi (-DROAM_HOST_WIN: the same features as the PC plugin), and installed the way the PC
// plugin is: its settings file scripts/NFSU2FreeRoam.ini in the game folder, with the PC's [FreeRoam] keys, and its
// log beside it (scripts/NFSU2FreeRoam.log). No .ini, or Enable = 0: free roam stays off, as without the plugin.
//
// The game side runs where the PC plugin runs it, from GameLoop's debug hook (src/ug2/freeroam.cpp), on the game's
// main thread. The network is the browser game's (runtime/web/roam_web.c): the free-roam mailbox, which the Revived
// link (revived_link.cpp) carries to the server's switch and its free-roam hub, so the Vita meets the players of
// nfsu2.xyz. That needs network=revived.
#include <freeroam.h>
#include <winapi/revived.h>
#include <lib/memmap.h>
#include <lib/gamepad.h>
#include <lib/winapp.h>
#include <platform/log.h>
#include <recomp.h>

#include <psp2/kernel/processmgr.h>

#include <pthread.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>

extern "C"
{
extern uint8_t* nfsu2_guest_base;
void roam_game_frame(struct CPU* c);
}
bool revived_link_ensure();

namespace
{

const char* const GameFolder = "ux0:data/NeedForSpeedUnderground2";
const char* const IniPath = "ux0:data/NeedForSpeedUnderground2/scripts/NFSU2FreeRoam.ini";
const char* const LogPath = "ux0:data/NeedForSpeedUnderground2/scripts/NFSU2FreeRoam.log";

// The PC plugin's settings (win/roam_asi/roam_asi.c OPTS): the environment name roam_game.c asks for, the .ini key.
const struct { const char *name, *key, *def; } Opts[] = {
    { "NFSU2_ROAM",             "Enable",      "1" },   // installing the .ini is the opt-in, as the .asi on PC
    { "NFSU2_ROAM_NAME",        "Name",        ""  },
    { "NFSU2_ROAM_PORT",        "Port",        ""  },
    { "NFSU2_ROAM_PEERS",       "Peers",       ""  },
    { "NFSU2_ROAM_COLLIDE",     "Collide",     ""  },
    { "NFSU2_ROAM_DISCOVERY",   "Discovery",   ""  },
    { "NFSU2_ROAM_ROOM",        "Room",        ""  },
    { "NFSU2_ROAM_CREW",        "Crew",        ""  },
    { "NFSU2_ROAM_CREW_RGB",    "CrewColours", ""  },
    // fewer than the PC's 33 and 5: with 49 players near, free roam's own work took 11.6 ms a frame on the Vita
    // (25 us alone) and the game ran at half speed; every car shown is a car load, 2 to 5 s each (2026-11-21)
    { "NFSU2_ROAM_MAX_PLAYERS", "MaxPlayers",  "8" },
    { "NFSU2_ROAM_MAX_CARS",    "MaxCars",     "2" },
    { "NFSU2_ROAM_RATE",        "Rate",        ""  },
    { "NFSU2_ROAM_DELAY",       "Delay",       ""  },
    { "NFSU2_ROAM_EXTRAP",      "Extrap",      ""  },
    { "NFSU2_ROAM_CARPOOL_KB",  "CarPoolKB",   ""  },
    { "NFSU2_ROAM_NETTHREAD",   "NetThread",   ""  },
    { "NFSU2_ROAM_LOG",         "Log",         "1" },
    // the browser game's own (settings.ini roam_labels, roam_flag), which the PC plugin has not: names over the other
    // players' cars, and our flag before our name (a code of flags.dat: gb, us, gb-eng)
    { "NFSU2_ROAM_LABELS",      "Names",       "1" },
    { "NFSU2_ROAM_FLAG",        "Flag",        ""  },
    // quick chat (settings.ini quick_chat): on or off, and the phrases of the six chords, Select + Down, Left, Right,
    // Cross, Circle, Square, by their numbers (phrases.h)
    { "NFSU2_ROAM_SAY",         "QuickChat",   "1" },
    { "NFSU2_ROAM_SAY1",        "Say1",        "0"  },   // Hey!
    { "NFSU2_ROAM_SAY2",        "Say2",        "2"  },   // Thanks!
    { "NFSU2_ROAM_SAY3",        "Say3",        "12" },   // Race?
    { "NFSU2_ROAM_SAY4",        "Say4",        "8"  },   // Follow me!
    { "NFSU2_ROAM_SAY5",        "Say5",        "14" },   // gg
    { "NFSU2_ROAM_SAY6",        "Say6",        "1"  },   // Nice car!
};
const int OptCount = int(sizeof(Opts) / sizeof(Opts[0]));
std::string s_values[OptCount];
bool s_haveIni = false;

void trim(std::string& s)
{
    // a comment after the value: ';' or '#' at the start or after a blank (as on PC: "Team#1" stays a name)
    for (size_t i = 0; i < s.size(); ++i)
        if ((s[i] == ';' || s[i] == '#') && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t'))
        {
            s.resize(i);
            break;
        }
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        --b;
    s = s.substr(a, b - a);
}

// [FreeRoam] of the .ini, as GetPrivateProfileString reads it
void loadIni()
{
    for (int i = 0; i < OptCount; ++i)
        s_values[i] = Opts[i].def;
    FILE* f = fopen(IniPath, "r");
    if (!f)
        return;
    s_haveIni = true;
    char line[512];
    bool inSection = false;
    while (fgets(line, sizeof(line), f))
    {
        std::string l = line;
        size_t a = 0;
        while (a < l.size() && (l[a] == ' ' || l[a] == '\t'))
            ++a;
        if (a >= l.size() || l[a] == ';' || l[a] == '#')
            continue;
        if (l[a] == '[')
        {
            const size_t e = l.find(']', a);
            inSection = e != std::string::npos && !strcasecmp(l.substr(a + 1, e - a - 1).c_str(), "FreeRoam");
            continue;
        }
        const size_t eq = l.find('=');
        if (!inSection || eq == std::string::npos)
            continue;
        std::string key = l.substr(a, eq - a), value = l.substr(eq + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        trim(value);
        for (int i = 0; i < OptCount; ++i)
            if (!strcasecmp(key.c_str(), Opts[i].key))
                s_values[i] = value;
    }
    fclose(f);
}

FILE* s_log = nullptr;
long s_frame = 0;
win32::WinApplication* s_app = nullptr;
x86::CPU* s_cpu = nullptr;

bool optOn(const char* v)
{
    return v && *v && strcmp(v, "0") && strcasecmp(v, "off") && strcasecmp(v, "no") && strcasecmp(v, "false");
}

}

extern "C"
{

const char* roam_opt(const char* name)
{
    for (int i = 0; i < OptCount; ++i)
        if (!strcmp(Opts[i].name, name))
            return s_values[i].empty() ? nullptr : s_values[i].c_str();
    return nullptr;   // the PC plugin's test aids come from its environment: none here
}

void roam_log(const char* fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    while (n && line[n - 1] == '\n')
        line[--n] = 0;
    // the game's thread and the network thread both log: one line at a time
    static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&s_lock);
    pf::logInfo("%s", line);
    if (!s_log && optOn(roam_opt("NFSU2_ROAM_LOG")))
        s_log = fopen(LogPath, "w");
    if (s_log)
    {
        fprintf(s_log, "%s\n", line);
        fflush(s_log);
    }
    pthread_mutex_unlock(&s_lock);
}

// Quick chat on the Vita: a chord (Gamepad::takeSayChord) says its slot's phrase.
int roam_host_say_pick(void)
{
    const int slot = win32::Gamepad::takeSayChord();
    if (slot < 0 || slot > 5)
        return -1;
    static const char* const Keys[6] = { "NFSU2_ROAM_SAY1", "NFSU2_ROAM_SAY2", "NFSU2_ROAM_SAY3",
                                         "NFSU2_ROAM_SAY4", "NFSU2_ROAM_SAY5", "NFSU2_ROAM_SAY6" };
    const char* v = roam_opt(Keys[slot]);
    const int phrase = v ? atoi(v) : -1;
    return phrase >= 0 && phrase < 255 ? phrase : -1;
}

int roam_win_committed(uint32_t a)
{
    return a >= 0x10000 && win32::MemMap::isGuestRange(a, 1);
}

// A game function with ecx, edx and n stack arguments, as frontend.cpp's callGuest: the CPU is restored afterwards.
uint32_t roam_win_call(uint32_t fn, uint32_t ecx, uint32_t edx, int n, const uint32_t* args)
{
    if (!s_app || !s_cpu)
        return 0;
    x86::CPU& cpu = *s_cpu;
    const x86::CPU saved = cpu;
    x86::reg32 esp = cpu.esp;
    for (int i = n - 1; i >= 0; --i)
        rc::st32(esp -= 4, args[i]);
    esp -= 4;   // the return address' place
    cpu.esp = esp;
    cpu.ecx = ecx;
    cpu.edx = edx;
    s_app->dynamic_call(fn, cpu);
    const uint32_t result = cpu.eax;
    cpu = saved;
    return result;
}

uint32_t roam_win_esp(void)
{
    return s_cpu ? s_cpu->esp : 0;
}

long roam_win_frame_no(void)
{
    return s_frame;
}

uint64_t roam_win_ns(void)
{
    return sceKernelGetProcessTimeWide() * 1000ull;
}

// mod packs (runtime/modpacks.c): none on the Vita, every part is the game's own
uint32_t modpacks_for_others(uint32_t key)
{
    return key;
}

// the pose log (a test aid, PoseLog): not on the Vita
struct rh_file;
rh_file* rh_fopen(const char*) { return nullptr; }
int rh_fprintf(rh_file*, const char*, ...) { return 0; }
void rh_fflush(rh_file*) {}

}

namespace ug2 { namespace freeroam
{

void frame(win32::WinApplication* app, x86::CPU& cpu)
{
    static int s_state = 0;   // 0 not looked yet, 1 on, -1 off
    if (s_state < 0)
        return;
    if (!s_state)
    {
        loadIni();
        if (!s_haveIni || !optOn(roam_opt("NFSU2_ROAM")))
        {
            s_state = -1;
            if (s_haveIni)
                pf::logInfo("roam: %s says Enable = 0: free roam with others is off", IniPath);
            return;
        }
        if (!win32::revived::enabled())
        {
            s_state = -1;
            pf::logInfo("roam: free roam with others needs network=revived (the Revived server): off");
            return;
        }
        if (!nfsu2_guest_base)
            nfsu2_guest_base = win32::g_guestBase;
        revived_link_ensure();
        // the names over the cars also need the game's own player-names option (0x8500C9, roam_game.c G_SHOW_NAMES)
        if (optOn(roam_opt("NFSU2_ROAM_LABELS")))
            rc::st8(0x008500C9, 1);
        s_state = 1;
        roam_log("[roam] free roam with others (NFSU2FreeRoam, as the PC plugin), settings %s, through the Revived "
                 "server", IniPath);
        (void)GameFolder;
    }
    s_app = app;
    s_cpu = &cpu;
    ++s_frame;
    roam_game_frame(nullptr);
    s_app = nullptr;
    s_cpu = nullptr;
}

}}
