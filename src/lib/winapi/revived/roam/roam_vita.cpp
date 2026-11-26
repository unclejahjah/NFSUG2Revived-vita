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
#include <lib/guestmodule.h>
#include <lib/heap.h>
#include <lib/file.h>
#include <lib/controls.h>
extern "C"
{
#include "horn.h"     // (C headers of nfsu2-web's, without their own extern "C")
#include "carsfx.h"
}
#include <lib/gamepad.h>
#include <platform/time.h>
#include <lib/winapp.h>
#include <platform/log.h>
#include <recomp.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/kernel/threadmgr.h>

#include <pthread.h>

#include <cstdarg>
#include <platform/sync.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>

extern "C"
{
extern uint8_t* nfsu2_guest_base;
void roam_game_frame(struct CPU* c);
void roam_game_hud(struct CPU* c);
const char* roam_game_phrase(int i);
// multiplayer races from free roam (roam_game.c mp_*, mp/mprace.c): the trip through the game's LAN race, a step a
// frame, menus included; its input is the trip's while it drives the menus
struct CPU;
void mprace_frame(struct CPU* c);
int mprace_owns_input(void);
void mprace_player_cancel(void);
void mp_vita_port_frame(void);
void showoff_frame(struct CPU* c);   // showoff.c: the hood, the trunk and the doors (Select + L)
void drift_frame(struct CPU* c);     // drift.c: drift mode (drift_button) and drift battles
void roam_vita_tap(unsigned dik);
int roam_vita_can_host(void);
int roam_host_hud_line(char* tx, int n);
int roam_host_chat_menu(const char** keys, const char** texts, int max);
int roam_game_chat_panel_drawn(void);
void roam_host_net_thread(void);
uint64_t roam_host_run_us(void);
void revived_link_control_send(const char* json);   // revived_link.cpp
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
    // the flags before the names drawn on this screen (1) or not (0): ours and everyone else's
    { "NFSU2_ROAM_SHOW_FLAGS",  "ShowFlags",   "1" },
    // quick chat (settings.ini quick_chat): on or off, and the phrases of its six slots by their numbers (phrases.h);
    // the cfg's quick_chat_button says which buttons they are (controls::quickChatSlots)
    // multiplayer races from free roam (roam_game.c mp_*): lobbies at the race markers, joined with the race's button
    // outruns between players (roam_game.c du_*): stay near another player and press Engage to challenge them
    { "NFSU2_ROAM_DUELS",       "Outruns",     "1" },
    { "NFSU2_ROAM_MPRACES",     "Races",       "1" },
    // the game's own ring on the ground at each race spot with no marker of the game's (won, or not open in the
    // career): the race type's ring, another while a lobby is open there
    { "NFSU2_ROAM_RACE_RINGS",  "RaceRings",   "1" },
    // ...and the game's ring effect played at one with a lobby open (an effect group's name; empty: the race's own)
    { "NFSU2_ROAM_RING_LOBBY",  "RingLobby",   "XEE_WORLD_CHALLENGE" },
    { "NFSU2_ROAM_SAY",         "QuickChat",   "1" },
    // horns between players: yours (dual, single, truck, train, klaxon, cucaracha, clown, trombone, sputter; off: none,
    // the others' still heard), and the other players' engines, nitrous and hydraulics heard from their cars
    { "NFSU2_ROAM_HORN",        "Horn",        "dual" },
    { "NFSU2_ROAM_CARSFX",      "CarSounds",   "1" },
    // the show-off (showoff.c: Select + L opens the hood, the trunk, the doors; friends' open on their cars too)
    { "NFSU2_ROAM_SHOWOFF",     "ShowOff",     "1" },
    // the lines nfsu2.xyz shows over the game (a player joined free roam, left, went into a race): the HUD's top line
    { "NFSU2_ROAM_NOTICES",     "Messages",    "1" },
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

// one key of [FreeRoam], read from the file now: for the link's thread, which may connect before free roam's first
// frame reads the .ini (the LAN menu), and never touches s_values
std::string iniValue(const char* want)
{
    std::string out;
    FILE* f = fopen(IniPath, "r");
    if (!f)
        return out;
    char line[1024];
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
        if (strcasecmp(key.c_str(), want))
            continue;
        trim(value);
        out = value;
    }
    fclose(f);
    return out;
}

FILE* s_log = nullptr;
long s_frame = 0;
win32::WinApplication* s_app = nullptr;
x86::CPU* s_cpu = nullptr;
// the trip's CPU (mp/cpu.h: 9 registers, never run) and whether its buttons are held now
uint32_t s_tripRegs[16];
bool s_padHeld = false;

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

// The network thread's lines wait here until it has let go of roam_plat_lock (roam_host_net_idle): a line written
// with the lock held stalled the game thread behind the memory card for 0.4 to 0.8 s while the city streamed
// (2026-11-26: LEAVE/JOIN lines, the virtual LAN's stats, then "lock 733 ms").
static SceUID s_netThread = -1;
static pthread_mutex_t s_qLock = PTHREAD_MUTEX_INITIALIZER;
static char s_q[16384];
static size_t s_qn;

static void roam_write_line(const char* line)
{
    // the game's thread and the network thread both log: one line at a time
    static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t s_flushed;
    pthread_mutex_lock(&s_lock);
    pf::logInfo("%s", line);
    if (!s_log && optOn(roam_opt("NFSU2_ROAM_LOG")))
        s_log = fopen(LogPath, "w");
    if (s_log)
    {
        fprintf(s_log, "%s\n", line);
        // (to the card at most once a second, not with every line: each flush waits on the card)
        const uint64_t now = pf::ticksUs();
        if (now - s_flushed >= 1000000)
        {
            fflush(s_log);
            s_flushed = now;
        }
    }
    pthread_mutex_unlock(&s_lock);
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
    if (s_netThread >= 0 && sceKernelGetThreadId() == s_netThread)
    {
        pthread_mutex_lock(&s_qLock);
        const bool fits = s_qn + n + 1 <= sizeof(s_q);
        if (fits)
        {
            memcpy(s_q + s_qn, line, n + 1);
            s_qn += n + 1;
        }
        pthread_mutex_unlock(&s_qLock);
        if (fits)
            return;
    }
    roam_write_line(line);
}

// roam_web.c's network thread, roam_plat_lock let go: its lines written now
extern "C" void roam_host_net_idle(void)
{
    static char batch[sizeof(s_q)];
    pthread_mutex_lock(&s_qLock);
    const size_t n = s_qn;
    memcpy(batch, s_q, n);
    s_qn = 0;
    pthread_mutex_unlock(&s_qLock);
    for (size_t at = 0; at < n; at += strlen(batch + at) + 1)
        roam_write_line(batch + at);
}

// Free roam's friends, as nfsu2.xyz's page does them (web/src/app/friends.ts FriendPrompt): two cars nose to nose for
// 3 s (roam_game.c bump_*) ask the server what that player is to us ("can"), the answer shows after its name over its
// car, and the friend button (the quick chat button + Triangle) sends the request ("add"), or accepts one. The server
// keeps the lists, for claimed names only (revived_passphrase).
struct FriendPrompt
{
    uint32_t id = 0;            // the player's session id (its name tag)
    std::string name;           // the name over its car
    std::string claimed;        // the server's name for it (its "can" answer)
    std::string st;             // "wait", or the server's word: new, asked, asks, friends, claim, theirs, far, full
    uint64_t until = 0;         // shown until (us); 0 while the noses touch
};
pthread_mutex_t s_friendLock = PTHREAD_MUTEX_INITIALIZER;
FriendPrompt s_friend;
const uint64_t FriendStayUs = 4000000;   // after the cars part, or after an answer

uint64_t nowUs()
{
    return sceKernelGetProcessTimeWide();
}

// a string or number field of the game's small JSON line ({"id":12,"name":"Bravo","on":1})
std::string jsonField(const char* json, const char* key)
{
    const std::string k = std::string("\"") + key + "\":";
    const char* p = strstr(json, k.c_str());
    if (!p)
        return "";
    p += k.size();
    std::string v;
    if (*p == '"')
    {
        for (++p; *p && *p != '"'; ++p)
        {
            if (*p == '\\' && p[1])
                ++p;
            v += *p;
        }
        return v;
    }
    while (*p && *p != ',' && *p != '}')
        v += *p++;
    return v;
}

std::string jsonText(const std::string& s)
{
    std::string o = "\"";
    for (char ch : s)
    {
        if (ch == '"' || ch == '\\')
            o += '\\';
        o += uint8_t(ch) < 0x20 ? ' ' : ch;
    }
    return o + "\"";
}

const char* friendKeys()
{
    switch (win32::controls::quickChatButton())
    {
    case win32::controls::DpadUp:   return "hold Up + Triangle";
    case win32::controls::Triangle: return "hold Triangle + Down";
    case -1:                        return "Select + R";
    default:                        return "hold Down + Triangle";
    }
}

void friendAdd()
{
    std::string msg;
    pthread_mutex_lock(&s_friendLock);
    const bool can = s_friend.id && (s_friend.st == "new" || s_friend.st == "asks")
                     && (!s_friend.until || nowUs() < s_friend.until);
    if (can)
    {
        msg = "{\"t\":\"friend\",\"op\":\"add\",\"name\":"
              + jsonText(s_friend.claimed.empty() ? s_friend.name : s_friend.claimed) + "}";
        s_friend.st = "wait";
    }
    pthread_mutex_unlock(&s_friendLock);
    if (can)
    {
        revived_link_control_send(msg.c_str());
        roam_log("[roam] friends: asked to add %s", jsonField(msg.c_str(), "name").c_str());
    }
}

bool sameName(const std::string& a, const std::string& b)
{
    return !a.empty() && !strcasecmp(a.c_str(), b.c_str());
}

// Quick chat on the Vita: while driving in free roam the chat button is taken (controls::setQuickChatLive), and a
// phrase button pressed with it (Gamepad::takeSayChord) says its slot's phrase.
void roam_host_say_live(int live)
{
    if (live)
        win32::controls::setQuickChatLive(pf::ticksUs());
}

// The phrase picked last and when (the HUD's line says it for a moment: roam_host_hud_line)
static int s_saidPhrase = -1;
static uint64_t s_saidUs = 0;

static const char* inputName(win32::controls::Input input)
{
    using namespace win32::controls;
    switch (input)
    {
    case Cross: return "X";
    case Circle: return "O";
    case Square: return "Square";
    case Triangle: return "Triangle";
    case R: return "R";
    case DpadUp: return "Up";
    case DpadDown: return "Down";
    case DpadLeft: return "Left";
    case DpadRight: return "Right";
    default: return "?";
    }
}

static const char* const SayKeys[6] = { "NFSU2_ROAM_SAY1", "NFSU2_ROAM_SAY2", "NFSU2_ROAM_SAY3",
                                        "NFSU2_ROAM_SAY4", "NFSU2_ROAM_SAY5", "NFSU2_ROAM_SAY6" };

static int slotPhrase(int slot)
{
    const char* v = roam_opt(SayKeys[slot]);
    const int phrase = v && *v ? atoi(v) : -1;
    return phrase >= 0 && phrase < 255 ? phrase : -1;
}

// The line at the top of free roam's HUD (roam_game.c du_hud): quick chat's choices while its button is held (after
// 0.2 s, so that a tap passed on to the game shows nothing), then the phrase said for 2 s; 1 when there is one.
// roam_web.c's network thread, at its start: above the game's main thread (160), so that the game thread never waits
// on roam_plat_lock while the thread holding it is kept off the core (the Vita, 2026-11-23: waits of 75 to 220 ms).
// It sleeps between its 10 ms ticks and datagrams.
void roam_host_net_thread(void)
{
    s_netThread = sceKernelGetThreadId();
    if (pf::Thread::setCurrentPriority(150))
        pf::logInfo("roam: network thread priority 150");
}

// This thread's time on a CPU (its run clock, us): roam_game.c's slow frames and steps say how much of them waited.
uint64_t roam_host_run_us(void)
{
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0)
        return 0;
    return info.runClocks;
}

// Quick chat's choices while its button is held (after 0.2 s, so that a tap passed on to the game shows nothing): each
// one's button and phrase, for roam_game.c's box on the right of the screen (qc_panel); their count, 0 none.
int roam_host_chat_menu(const char** keys, const char** texts, int max)
{
    if (!optOn(roam_opt("NFSU2_ROAM_SAY")))
        return 0;
    const uint64_t now = pf::ticksUs(), held = win32::controls::quickChatHeldSince();
    if (!held || now - held < 200000)
        return 0;
    win32::controls::Input slots[7];
    win32::controls::quickChatSlots(slots);
    int n = 0;
    for (int i = 0; i < 7 && n < max; ++i)
    {
        const char* text = i == 6 ? "Add friend" : roam_game_phrase(slotPhrase(i));
        if (!text)
            continue;
        keys[n] = inputName(slots[i]);
        texts[n] = text;
        ++n;
    }
    return n;
}

// ---- notices: the page's lines over the game (a player joined free roam, left, went into a race; a lobby nearby)
// nfsu2.xyz's page shows them in a log on the left; here each takes the HUD's top line for NoticeUs, one after the
// other, when quick chat and the races and outruns have nothing to say there. They come from the link's thread.
static const uint64_t NoticeUs = 4000000;
static const int NoticeMax = 6;
static pthread_mutex_t s_noticeLock = PTHREAD_MUTEX_INITIALIZER;
static const uint64_t NoticeStaleUs = 15000000;   // one that waited longer (menus, a shop) is old news
static char s_notices[NoticeMax][96];
static uint64_t s_noticeAt[NoticeMax];
static int s_noticeHead = 0, s_noticeCount = 0;
static uint64_t s_noticeSince = 0;   // when the line on screen now came up (0: none yet)

extern "C" void roam_host_notice(const char* text)
{
    if (!text || !*text || !optOn(roam_opt("NFSU2_ROAM_NOTICES")))
        return;
    char line[96];
    if (text[0] == '\x1e')
    {
        // a hint by its id, worded as the page words it
        if (strcmp(text + 1, "ghost"))
            return;
        snprintf(line, sizeof(line), "New players drive out of here: Ghost mode (Vita Settings) lets you pass through");
    }
    else
        snprintf(line, sizeof(line), "%s", text);
    pthread_mutex_lock(&s_noticeLock);
    if (s_noticeCount == NoticeMax)   // the oldest waiting goes
    {
        s_noticeHead = (s_noticeHead + 1) % NoticeMax;
        --s_noticeCount;
    }
    const int at = (s_noticeHead + s_noticeCount) % NoticeMax;
    snprintf(s_notices[at], sizeof(s_notices[0]), "%s", line);
    s_noticeAt[at] = pf::ticksUs();
    if (!s_noticeCount)
        s_noticeSince = 0;
    ++s_noticeCount;
    pthread_mutex_unlock(&s_noticeLock);
}

static int noticeLine(char* tx, int n, uint64_t now)
{
    pthread_mutex_lock(&s_noticeLock);
    while (s_noticeCount && s_noticeSince && now - s_noticeSince >= NoticeUs)
    {
        s_noticeHead = (s_noticeHead + 1) % NoticeMax;
        s_noticeSince = --s_noticeCount ? now : 0;
    }
    while (s_noticeCount && !s_noticeSince && now - s_noticeAt[s_noticeHead] >= NoticeStaleUs)
    {
        s_noticeHead = (s_noticeHead + 1) % NoticeMax;
        --s_noticeCount;
    }
    int shown = 0;
    if (s_noticeCount)
    {
        if (!s_noticeSince)
            s_noticeSince = now;
        snprintf(tx, size_t(n), "%s", s_notices[s_noticeHead]);
        shown = 1;
    }
    pthread_mutex_unlock(&s_noticeLock);
    return shown;
}

int roam_host_hud_line(char* tx, int n)
{
    if (!optOn(roam_opt("NFSU2_ROAM_SAY")))
        return 0;
    const uint64_t now = pf::ticksUs(), held = win32::controls::quickChatHeldSince();
    if (held && now - held >= 200000 && !roam_game_chat_panel_drawn())   // (the box could not be made: the old line)
    {
        win32::controls::Input slots[7];
        win32::controls::quickChatSlots(slots);
        int at = snprintf(tx, size_t(n), "Quick chat:");
        for (int i = 0; i < 7 && at < n - 1; ++i)
        {
            const int phrase = i < 6 ? slotPhrase(i) : -1;
            const char* text = i == 6 ? "Add friend" : roam_game_phrase(phrase);
            if (!text)
                continue;
            at += snprintf(tx + at, size_t(n - at), "%s %s %s", i ? " /" : "", inputName(slots[i]), text);
        }
        return 1;
    }
    if (s_saidPhrase >= 0 && now - s_saidUs < 2000000)
    {
        const char* text = roam_game_phrase(s_saidPhrase);
        if (text)
        {
            snprintf(tx, size_t(n), "You: %s", text);
            return 1;
        }
    }
    return 0;
}

// the players this one blocks (.ini Blocked: names as they show over cars, joined by commas, up to 32), for the
// switch's hello (revived_link.cpp): the server's free-roam hub then links none of them with us, so neither sees the
// other's car, name, quick chat, outrun or race lobby. Taken at each connection.
extern "C" int roam_host_blocked(char names[][25], int max)
{
    const std::string list = iniValue("Blocked");
    int n = 0;
    size_t at = 0;
    while (at <= list.size() && n < max)
    {
        size_t end = list.find(',', at);
        if (end == std::string::npos)
            end = list.size();
        std::string name;
        for (size_t i = at; i < end; ++i)
            if (list[i] >= ' ' && list[i] <= '~')
                name += list[i];
        trim(name);
        if (name.size() > 24)
            name.resize(24);
        if (!name.empty())
            snprintf(names[n++], 25, "%s", name.c_str());
        at = end + 1;
    }
    return n;
}

// one key of the .ini's [FreeRoam] as the file has it now (stance_vita.c: Vita Settings may change it while playing)
int roam_host_ini_value(const char* key, char* out, int n)
{
    if (!out || n <= 0)
        return 0;
    const std::string v = iniValue(key);
    snprintf(out, size_t(n), "%s", v.c_str());
    return 1;
}

// showoff.c: Select + L presses since the last frame
int roam_host_showoff_presses(void)
{
    const int n = win32::controls::takeShowoffPresses();
    return optOn(roam_opt("NFSU2_ROAM_SHOWOFF")) ? n : 0;
}

// drift.c: the drift button's presses since the last frame, taken in the world only (G_FLOW 6: elsewhere the button
// stays the game's)
int roam_win_committed(uint32_t a);
extern "C" int roam_game_hud_up(void);   // roam_game.c: free roam's HUD up, nothing over it
int roam_host_drift_presses(void)
{
    const uint32_t flow = 0x008654a4u;
    if (nfsu2_guest_base && roam_win_committed(flow) && *reinterpret_cast<uint32_t*>(nfsu2_guest_base + flow) == 6u
        && roam_game_hud_up())
        win32::controls::setDriftLive(pf::ticksUs());
    return win32::controls::takeDriftPresses();
}

// The race lobby panel's keys (roam/mp/mp_vita.c pad_lobby_offer, each frame while free roam's HUD is up): D-pad Left
// held for a new lobby, Down held for Friends only (controls::setLobbyKeys); the presses since the last frame, bit 0
// Left, bit 1 Down; and how far Down's hold towards Friends only is, 0..1 (-1 none), for the quick chat box's bar.
extern "C" void roam_host_lobby_keys(int left, int down)
{
    win32::controls::setLobbyKeys(pf::ticksUs(), left != 0, down != 0);
}

extern "C" int roam_host_lobby_take(void)
{
    return win32::controls::takeLobbyPresses();
}

extern "C" float roam_host_lobby_hold(void)
{
    const uint64_t held = win32::controls::lobbyDownHeldUs();
    return held ? float(double(held) / double(win32::controls::LobbyDownUs)) : -1.0f;
}

// The game's thread, as free roam's frame runs on it (showoff_hooks.cpp: drift mode's music natives change the
// switch only there)
static int s_gameThread = 0;
extern "C" int roam_vita_on_game_thread(void)
{
    return s_gameThread && pf::Thread::currentId() == s_gameThread;
}

// the HUD's top line when nothing else has one (roam_game.c du_hud): the next notice
extern "C" int roam_host_notice_line(char* tx, int n)
{
    return noticeLine(tx, n, pf::ticksUs());
}

int roam_host_say_pick(void)
{
    const int slot = win32::Gamepad::takeSayChord();
    if (slot == 6)
    {
        friendAdd();
        return -1;
    }
    if (slot < 0 || slot > 5)
        return -1;
    const int phrase = slotPhrase(slot);
    if (phrase >= 0)
    {
        s_saidPhrase = phrase;
        s_saidUs = pf::ticksUs();
    }
    return phrase;
}

// roam_game.c bump_page through roam_web.c roam_plat_friend: the noses touch ("on":1) or parted ("on":0)
int roam_host_friend_bump(const char* json)
{
    const uint32_t id = uint32_t(strtoul(jsonField(json, "id").c_str(), nullptr, 10));
    const std::string name = jsonField(json, "name");
    const bool on = jsonField(json, "on") == "1";
    std::string ask;
    pthread_mutex_lock(&s_friendLock);
    if (on)
    {
        if (s_friend.id != id || (s_friend.until && nowUs() >= s_friend.until))
        {
            s_friend = FriendPrompt();
            s_friend.id = id;
            s_friend.name = name;
            s_friend.st = "wait";
            ask = "{\"t\":\"friend\",\"op\":\"can\",\"name\":" + jsonText(name) + "}";
        }
        s_friend.until = 0;
    }
    else if (s_friend.id == id)
        s_friend.until = nowUs() + FriendStayUs;
    pthread_mutex_unlock(&s_friendLock);
    if (!ask.empty())
        revived_link_control_send(ask.c_str());
    return 1;
}

// revived_link.cpp: the server's {"t":"friend",...}
void roam_host_friend(const char* op, const char* name, const char* st, const char* done)
{
    pthread_mutex_lock(&s_friendLock);
    const bool ours = s_friend.id && (sameName(name, s_friend.name) || sameName(name, s_friend.claimed)
                                      || s_friend.st == "wait");
    if (!strcmp(op, "can") && ours && *st)
    {
        s_friend.claimed = name;
        s_friend.st = st;
    }
    else if (!strcmp(op, "add") && ours && *done)
    {
        s_friend.st = !strcmp(done, "asked") ? "asked" : done;
        if (s_friend.until)
            s_friend.until = nowUs() + FriendStayUs;
    }
    else if ((!strcmp(op, "asks") || !strcmp(op, "friends")) && s_friend.id
             && (sameName(name, s_friend.name) || sameName(name, s_friend.claimed)))
        s_friend.st = op;
    pthread_mutex_unlock(&s_friendLock);
    if (!strcmp(op, "asks"))
        roam_log("[roam] friends: %s wants to be friends (nose to nose, then %s)", name, friendKeys());
    else if (!strcmp(op, "friends") || (!strcmp(op, "add") && !strcmp(done, "friends")))
        roam_log("[roam] friends: you and %s are friends now", name);
}

// roam_game.c's name tags: the note after the name of the player nose to nose with us
int roam_host_tag_note(uint32_t id, char* out, int n)
{
    pthread_mutex_lock(&s_friendLock);
    const bool live = s_friend.id == id && (!s_friend.until || nowUs() < s_friend.until);
    const std::string st = live ? s_friend.st : "";
    pthread_mutex_unlock(&s_friendLock);
    if (st.empty())
        return 0;
    char keys[48];
    snprintf(keys, sizeof keys, "%s", friendKeys());
    const char* text = st == "wait"    ? "..."
                     : st == "asked"   ? "friend request sent"
                     : st == "friends" ? "friends"
                     : st == "claim"   ? "friends need claimed names (passphrase)"
                     : st == "full"    ? "friends list full"
                     : st == "theirs" || st == "far" ? "can't add here"
                     : nullptr;
    if (st == "new")
        snprintf(out, size_t(n), "add friend: %s", keys);
    else if (st == "asks")
        snprintf(out, size_t(n), "wants to be friends: %s", keys);
    else if (text)
        snprintf(out, size_t(n), "%s", text);
    else
        snprintf(out, size_t(n), "%s", st.c_str());
    return 1;
}

int roam_win_committed(uint32_t a)
{
    return a >= 0x10000 && win32::MemMap::isGuestRange(a, 1);
}

// ---- sounds: horns between players (horn.c) and friends' engines, nitrous and hydraulics (carsfx.c) ----------------
// free roam has started here (its first frame read the .ini and found it on): only then is there anything to mix
static volatile int s_soundsLive = 0;

// the horn: horn_button held in free roam proper (controls.cpp), Horn not off
int roam_host_horn_held(void)
{
    // (asked each frame free roam's world runs, and only then: the button is the horn's while it is)
    const char* kind = roam_opt("NFSU2_ROAM_HORN");
    if (kind && !strcasecmp(kind, "off"))
        return 0;
    if (!roam_game_hud_up())   // (the World Map, the pause menu: the button is the game's)
        return 0;
    win32::controls::setHornLive(pf::ticksUs());
    return win32::controls::hornHeld() ? 1 : 0;
}

// the game's Master Volume (its Audio options: G_VOLUMES +0, a float 0 .. 1), which every horn follows
float horn_game_master(void)
{
    const uint32_t a = 0x0083aa28u;
    if (!nfsu2_guest_base || !roam_win_committed(a) || !roam_win_committed(a + 3u))
        return 1.0f;
    float v;
    memcpy(&v, nfsu2_guest_base + a, sizeof(v));
    return v >= 0.0f && v <= 1.0f ? v : 1.0f;
}

// a game file by its Windows path in the game's folder ("SOUND\\ENGINE\\X.GIN"); the Vita's card is not case sensitive
int roam_vita_game_path(const char* win, char* out, size_t outsz)
{
    const int n = snprintf(out, outsz, "%s/%s", GameFolder, win ? win : "");
    if (n <= 0 || size_t(n) >= outsz)
        return 0;
    for (char* c = out; *c; ++c)
        if (*c == '\\')
            *c = '/';
    return 1;
}

// src/lib/audio.cpp, on the output thread: the horns and friends' car sounds added to each block of the mix
// (interleaved stereo at 16-bit scale), as nfsu2-web's mixers add them (audio_web.c)
void roam_vita_audio_mix(int32_t* acc, unsigned frames, unsigned rate)
{
    if (!s_soundsLive || !acc || !frames || !rate)
        return;
    static float mix[2 * 1024];
    while (frames)
    {
        const unsigned n = frames < 1024 ? frames : 1024;
        memset(mix, 0, 2 * n * sizeof(mix[0]));
        horn_mix(mix, n, rate);
        carsfx_mix(mix, n, rate);
        for (unsigned i = 0; i < 2 * n; ++i)
            acc[i] += int32_t(mix[i] * 32767.0f);
        acc += 2 * n;
        frames -= n;
    }
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

// A friend's car type's files opened ahead on the file worker (roam_game.c, ROAM_PREFETCH): the ones CarLoader opens
// for its load, so those opens find them kept open (win32::File::prewarm).
extern "C" void roam_win_prefetch_car(const char* type)
{
    if (!type || !*type || strlen(type) > 32 || strchr(type, '\\') || strchr(type, '/') || strstr(type, ".."))
        return;
    static const char* const files[] = { "GEOMETRY.BIN", "TEXTURES.BIN", "VINYLS.BIN", "PARTS_ANIMATIONS.BIN" };
    for (const char* name : files)
    {
        char path[96];
        snprintf(path, sizeof(path), "CARS\\%s\\%s", type, name);
        win32::File::prewarm(path);
    }
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

// A key for the trip (mp_vita.c autotest_tap): the three it presses, through the synthesized keyboard.
void roam_vita_tap(unsigned dik)
{
    static const win32::controls::KeyCode Enter = { 0x0D, 0x1C, false };
    static const win32::controls::KeyCode Escape = { 0x1B, 0x01, false };
    static const win32::controls::KeyCode Left = { 0x25, 0x4B, true };
    switch (dik)
    {
    case 0x1C: win32::Gamepad::tapKey(Enter); break;
    case 0x01: win32::Gamepad::tapKey(Escape); break;
    case 0xCB: win32::Gamepad::tapKey(Left); break;
    default: roam_log("[mprace] no key 0x%02x on the Vita", dik); break;
    }
}

// Whether this game can host a lobby's race (roam_game.c mp_can_host): the game's LAN server, server.dll, translated
// into this build and the same file next to SPEED2.EXE (src/lib/guestmodule.cpp), with its server.cfg from the PC game:
// without it the server stops at "ServerConfig: config load failed" and never listens, so joiners are refused
const uint32_t ServerStateBytes = 0x011651E0 + 32;   // (with room for a heap header of server.dll's CRT)

int roam_vita_can_host(void)
{
    if (!win32::guestmodule::available("server.dll"))
        return 0;
    static int cfg;                 // 0 not asked yet, 1 there, -1 not
    if (!cfg)
    {
        win32::File f("server.cfg", win32::GENERIC_READ, 0, win32::OPEN_EXISTING);
        cfg = static_cast<void*>(f) ? 1 : -1;
        pf::logInfo("server.cfg: %s", cfg > 0 ? "in the game folder" : "not in the game folder: no hosting of LAN "
                    "games (put the server.cfg of the PC game next to SPEED2.EXE)");
    }
    if (cfg < 0)
        return 0;
    // The server's state is one object of 0x11651E0 bytes (17 MB, server.dll 0x94CF4D: operator new at start). Later in
    // free roam, with friends' cars loaded and unloaded, guest memory rarely keeps a range that large (combined-test11:
    // "guest memory exhausted (requested 18239968 bytes)" when a lobby's server started). So it is set aside at the
    // first look, early, and lent to the server each time it starts; without room for it this game does not host.
    static int room;                // 0 not tried yet, 1 set aside, -1 no room
    if (!room)
    {
        room = win32::heap::reserve(ServerStateBytes) ? 1 : -1;
        pf::logInfo("server.dll: %u KB %s for its server state", ServerStateBytes / 1024,
                    room > 0 ? "set aside" : "not available (guest memory too fragmented): this game does not host");
    }
    return room > 0;
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
        // the clocks as they are now, in the game: a plugin (PSVshell, oclockvita, vita_cpu_tweaks) that raises the
        // CPU to 500 MHz does it after the start-up log's line, which shows the port's own 444
        pf::logInfo("roam: clocks now: arm %d MHz, bus %d MHz, gpu %d MHz", scePowerGetArmClockFrequency(),
                    scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency());
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
        roam_vita_can_host();     // (sets the LAN server's memory aside now, before free roam fragments it)
        s_soundsLive = 1;   // horns and friends' car sounds from here on (roam_vita_audio_mix)
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
    showoff_frame(nullptr);   // (its guest calls go through roam_win_call on this frame's CPU too)
    s_gameThread = pf::Thread::currentId();
    drift_frame(nullptr);
    // the trip of a multiplayer race (out of free roam, the game's LAN race, back): its CPU is only passed along,
    // its guest calls go through roam_win_call on this frame's
    static CPU* const s_tripCpu = reinterpret_cast<CPU*>(s_tripRegs);
    mp_vita_port_frame();   // (a racer's quit, from the race's own port)
    mprace_frame(s_tripCpu);
    // the way out while the trip holds the buttons: Start + Select held 2 s (once: give the race up and drive back;
    // again: the controls back at once)
    {
        static uint64_t s_cancelSince;
        static bool s_cancelDone;
        const uint32_t both = win32::controls::bit(win32::controls::Start) | win32::controls::bit(win32::controls::Select);
        const bool held = mprace_owns_input() && (win32::Gamepad::rawInputs() & both) == both;
        const uint64_t now = pf::ticksUs();
        if (!held)
        {
            s_cancelSince = 0;
            s_cancelDone = false;
        }
        else if (!s_cancelSince)
            s_cancelSince = now;
        else if (!s_cancelDone && now - s_cancelSince >= 2000000)
        {
            s_cancelDone = true;
            roam_log("[mprace] f=%ld Start + Select held 2 s: the player calls the trip off", s_frame);
            mprace_player_cancel();
        }
    }
    const bool owns = mprace_owns_input() != 0;
    if (owns != s_padHeld)
    {
        s_padHeld = owns;
        win32::Gamepad::setPadHeld(owns);
        roam_log("[mprace] f=%ld the Vita's buttons %s", s_frame, owns ? "held: the trip drives the menus" : "back to the player");
    }
    s_app = nullptr;
    s_cpu = nullptr;
}

GuestScope::GuestScope(win32::WinApplication* app, x86::CPU& cpu)
    : m_app(s_app), m_cpu(s_cpu)
{
    if (!s_cpu)
    {
        s_app = app;
        s_cpu = &cpu;
    }
}

GuestScope::~GuestScope()
{
    s_app = m_app;
    s_cpu = m_cpu;
}

void hud(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!nfsu2_guest_base || s_cpu)   // (free roam not started, or inside its own frame)
        return;
    static int s_labels = -1;
    if (s_labels < 0)
        s_labels = optOn(roam_opt("NFSU2_ROAM_LABELS")) ? 1 : 0;
    if (s_labels)
        rc::st8(0x008500C9, 1);   // the game's player-names option, which the names follow (G_SHOW_NAMES)
    s_app = app;
    s_cpu = &cpu;
    roam_game_hud(nullptr);
    s_app = nullptr;
    s_cpu = nullptr;
}

}}
