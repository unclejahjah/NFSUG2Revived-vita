// Front-end state log (game state, boot flow, FEng commands per frame) and scripted pad input (boot_script, host runs).

#include "frontend.h"

#include <lib/controls.h>
#include <lib/file.h>
#include <lib/gamepad.h>
#include <lib/memlog.h>
#include <lib/memmap.h>
#include <lib/window.h>
#include <platform/config.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <recomp.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>

namespace ug2 { namespace frontend {

namespace {

using x86::reg32;
namespace controls = win32::controls;

const reg32 QueryPerformanceCounterSlot = 0x0078311C;  // import slot; the frame timing calls it each main-loop iteration
const reg32 CreateFileASlot = 0x007831D0;      // import slot; stdcall (path, ...)
const reg32 MouseX = 0x008763D4, MouseY = 0x008763D8;      // the game's mouse: 640x480 position, left button
const reg32 MouseButton = 0x008763F1;
const reg32 MouseMoveX = 0x008763E4, MouseMoveY = 0x008763E8;
const reg32 MouseMiddle = 0x008763F7;          // held; only the garage camera orbit 0x4A5D90 reads it
const reg32 GarageHandler = 0x004D8D20;        // message handler of the garage screens (vtable + 8)
const reg32 GarageYaw = 0x274, GarageOrbitSpeed = 0x248;   // floats in the garage screen: degrees, yaw and pitch speed
const reg32 FramesRendered = 0x0086514C;       // incremented at the end of each main-loop iteration by 0x57A050
const reg32 GameState = 0x008654A4;            // 1 front end, 3 leaving it, 4 race (set by 0x57FF10, 0x5800A0, 0x57F210)
const reg32 PendingCall = 0x00865484;          // deferred function the main loop 0x5811C0 runs next
const reg32 ScreenName = 0x008384C8;           // name of the FEng package queued last, set by 0x50C8D0
const reg32 FeManager = 0x008384C4;            // +8 the current screen object
const reg32 BootFlow = 0x00836488;             // boot-flow manager 0x4D9580; +0xC is the current screen node
const reg32 MoviePlayer = 0x008383A4;          // movie player object, zero when none
const reg32 MovieActive = 0x008383AC;          // nonzero while a movie plays
const reg32 MessageHub = 0x008637B0;           // object whose 0x55DC50 (message, 0) notifies the screens
const reg32 SendMessage = 0x0055DC50;          // thiscall (message, 0), ret 8
const reg32 MovieEndMessage = 0xC3960EB9;      // what the movie pump sends when the movie has finished
const reg32 GraphicsDirty = 0x008709D1;        // byte; set by the Display menu, applied by 0x5D2850 (Reset)
const uint32_t WmQuit = 0x0012;
const uint64_t HeartbeatFrames = 600;
const int DefaultHoldMs = 200;

struct Press
{
    uint32_t inputs;         // controls::Input bits
    uint32_t raw;            // DirectInput button bits
    uint64_t startUs;
    uint64_t holdUs;
    uint64_t periodUs;       // 0 for a single press
    uint64_t endUs;          // a repeating press stops at this time, 0 for never
    int touch[2] = { -1, -1 };      // boot_steps front touch in display pixels, -1 for none
    int touchTo[2] = { -1, -1 };    // where a boot_steps drag ends, -1 for a still touch
    int stick[2] = { 0, 0 };        // boot_steps right stick in percent, y down
};

struct Name
{
    const char* text;
    controls::Input input;
};

const Name InputNames[] = {
    { "cross", controls::Cross }, { "circle", controls::Circle }, { "square", controls::Square },
    { "triangle", controls::Triangle }, { "l", controls::L }, { "r", controls::R }, { "select", controls::Select },
    { "start", controls::Start }, { "up", controls::DpadUp }, { "down", controls::DpadDown },
    { "left", controls::DpadLeft }, { "right", controls::DpadRight },
};

struct Step
{
    uint32_t inputs = 0;
    std::string screen;      // wait until a boot-flow or FE screen name contains this
    std::string open;        // wait until a file path opened since the last step contains this
    reg32 state = ~0u;       // wait until the game state is this
    unsigned delay = 30;     // frames to wait once the condition holds
    unsigned hold = 24;      // frames the buttons are held
    int touch[2] = { -1, -1 };
    int touchTo[2] = { -1, -1 };
    int stick[2] = { 0, 0 };
};

std::vector<Step> s_steps;
size_t s_step = 0;
unsigned s_stepPhase = 0;                      // 0 waiting, 1 delaying, 2 holding
uint64_t s_stepFrame = 0;
bool s_stepOpened = false;
std::string s_opened;
std::mutex s_openedLock;
std::vector<Press> s_script;
std::atomic<uint32_t> s_raw{0};
uint64_t s_startUs = 0;
bool s_logState = false;
win32::MethodPtr s_queryPerformanceCounter = nullptr;
win32::MethodPtr s_createFile = nullptr;
int s_mainThread = 0;
reg32 s_frame = ~0u;
double s_moviesSkip = 0;                       // seconds, 0 for off
int s_skipIntro = 0;                           // skip_intro_movies: 0 off, 1 logos, 2 logos and the opening movie
reg32 s_moviePlayer = 0;
std::string s_movieName;
uint64_t s_movieStartUs = 0;
bool s_movieSkipped = false;
int s_quitKind = 0;                            // 0 off, 1 seconds, 2 frames in a state, 3 frames after the last save
double s_quitSeconds = 0;
reg32 s_quitState = 0;
uint64_t s_quitFrames = 0;
uint64_t s_stateSince = 0;
reg32 s_lastState = ~0u;
unsigned s_saveCommits = 0;
uint64_t s_saveFrame = 0;
bool s_quitSent = false;
uint64_t s_garageFrame = 0;
bool s_gfxApplied = false;

struct GraphicsGlobal
{
    const char* name;
    reg32 address;
};

const GraphicsGlobal GraphicsGlobals[] = {
    { "g_CarEnvironmentMapEnable", 0x870CB8 }, { "g_CarEnvironmentMapUpdateData", 0x870CBC },
    { "g_CarShadowEnable", 0x870CC0 }, { "g_CarHeadlightEnable", 0x870CC4 }, { "g_CarLightingEnable", 0x870CC8 },
    { "g_CarDamageEnable", 0x870CCC }, { "g_CrowdEnable", 0x870CD0 }, { "g_RoadReflectionEnable", 0x870CD4 },
    { "g_FogEnable", 0x870CD8 }, { "g_MotionBlurEnable", 0x870CDC }, { "g_LightStreaksEnable", 0x870CE0 },
    { "g_LightGlowEnable", 0x870CE4 }, { "g_AnimatedTextureEnable", 0x870CE8 }, { "g_ParticleSystemEnable", 0x870CEC },
    { "g_DepthOfFieldEnable", 0x870CF0 }, { "g_WorldLodLevel", 0x870CF4 }, { "g_CarLodLevel", 0x870CF8 },
    { "g_OverBrightEnable", 0x870CFC }, { "g_BleachByPassEnable", 0x870D00 }, { "g_TintingEnable", 0x870D04 },
    { "g_FSAALevel", 0x870D08 }, { "g_HorizonFogEnable", 0x870D0C }, { "g_RainEnable", 0x870D10 },
    { "g_TextureFiltering", 0x870D18 }, { "g_RacingResolution", 0x870D1C }, { "g_PerformanceLevel", 0x87099C },
    { "g_VSyncOn", 0x870D24 },
};

std::string guestString(reg32 address)
{
    if (!address || !win32::MemMap::isGuestRange(address, 64))
    {
        char text[16];
        snprintf(text, sizeof(text), "@%08X", unsigned(address));
        return address ? text : "-";
    }
    std::string text;
    for (reg32 i = 0; i < 64 && rc::ld8(address + i); ++i)
    {
        const char c = char(rc::ld8(address + i));
        text += c >= 0x20 && c < 0x7F ? c : '?';
    }
    return text;
}

// Name of the package on top of the FEng manager (its object +8, package +0xE4, name at +0xC).
std::string currentScreen()
{
    const reg32 manager = rc::ld32(FeManager);
    const reg32 screen = manager ? rc::ld32(manager + 8) : 0;
    const reg32 package = screen ? rc::ld32(screen + 0xE4) : 0;
    return package ? guestString(rc::ld32(package + 0xC)) : "-";
}

// Boot-flow screen name, or "-".
std::string bootFlowScreen()
{
    const reg32 manager = rc::ld32(BootFlow);
    if (!manager)
        return "-";
    const reg32 node = rc::ld32(manager + 0xC);
    return node ? guestString(rc::ld32(node + 0xC)) : "-";
}

bool memLogOn()
{
    return pf::Config::getBool("memory_log", false);
}

bool isWorldLoad(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string dir = slash == std::string::npos ? "" : path.substr(0, slash);
    for (char& c : dir)
        c = char(toupper(static_cast<unsigned char>(c)));
    std::string upper = name;
    for (char& c : upper)
        c = char(toupper(static_cast<unsigned char>(c)));
    if (upper == "INGAMEMEMORYFILE.BIN")
        return dir.size() >= 6 && dir.compare(dir.size() - 6, 6, "GLOBAL") == 0;
    return upper.size() >= 8 && upper.compare(0, 3, "L4R") == 0 && upper.compare(upper.size() - 4, 4, ".BUN") == 0
        && dir.size() >= 6 && dir.compare(dir.size() - 6, 6, "TRACKS") == 0;
}

void createFileHook(win32::WinApplication* app, x86::CPU& cpu)
{
    const std::string path = guestString(rc::ld32(cpu.esp + 4));
    if (!s_steps.empty())
    {
        std::lock_guard<std::mutex> guard(s_openedLock);
        if (s_opened.size() < 4096)
            s_opened += path + "|";
    }
    pf::logInfo("fe: open %s at frame %u", path.c_str(), unsigned(rc::ld32(FramesRendered)));
    if (win32::memlog::g_enabled.load(std::memory_order_relaxed) && isWorldLoad(path))
        win32::memlog::event("world load");
    s_createFile(app, cpu);
}

bool parseButton(const std::string& name, Press* press)
{
    for (const Name& n : InputNames)
    {
        if (name == n.text)
        {
            press->inputs |= controls::bit(n.input);
            return true;
        }
    }
    if (name.size() >= 2 && name[0] == 'b' && name.find_first_not_of("0123456789", 1) == std::string::npos
        && atoi(name.c_str() + 1) < 32)
    {
        press->raw |= uint32_t(1) << atoi(name.c_str() + 1);
        return true;
    }
    int x, y, toX = -1, toY = -1;
    char end;
    if (sscanf(name.c_str(), "rs%d_%d%c", &x, &y, &end) == 2)
    {
        press->stick[0] = x;
        press->stick[1] = y;
        return true;
    }
    const int fields = sscanf(name.c_str(), "touch%dx%d-%dx%d%c", &x, &y, &toX, &toY, &end);
    if ((fields == 2 || fields == 4) && x >= 0 && y >= 0 && name.back() != '-')
    {
        press->touch[0] = x;
        press->touch[1] = y;
        press->touchTo[0] = toX;
        press->touchTo[1] = toY;
        return true;
    }
    return false;
}

// Tokens "button[/period][@seconds[~end]][:holdms]", joined by ',' ; buttons may be joined by '+'.
void parseScript(const std::string& text)
{
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t end = text.find(',', pos);
        if (end == std::string::npos)
            end = text.size();
        const std::string original = text.substr(pos, end - pos);
        std::string token = original;
        pos = end + 1;
        if (token.empty())
            continue;
        Press press = {};
        int holdMs = DefaultHoldMs;
        double start = 0, period = 0, until = 0;
        const size_t colon = token.find(':');
        if (colon != std::string::npos)
        {
            holdMs = atoi(token.c_str() + colon + 1);
            token.resize(colon);
        }
        const size_t at = token.find('@');
        if (at != std::string::npos)
        {
            start = atof(token.c_str() + at + 1);
            const size_t tilde = token.find('~', at);
            if (tilde != std::string::npos)
                until = atof(token.c_str() + tilde + 1);
            token.resize(at);
        }
        const size_t slash = token.find('/');
        if (slash != std::string::npos)
        {
            period = atof(token.c_str() + slash + 1);
            token.resize(slash);
        }
        bool valid = !token.empty();
        for (size_t b = 0; valid && b <= token.size();)
        {
            size_t plus = token.find('+', b);
            if (plus == std::string::npos)
                plus = token.size();
            valid = parseButton(token.substr(b, plus - b), &press);
            b = plus + 1;
        }
        if (!valid)
        {
            pf::logError("frontend: boot_script token \"%s\" is not button[+button][/period][@seconds[~end]][:holdms]",
                         original.c_str());
            continue;
        }
        press.startUs = uint64_t(start * 1e6);
        press.holdUs = uint64_t(holdMs) * 1000;
        press.periodUs = uint64_t(period * 1e6);
        press.endUs = uint64_t(until * 1e6);
        s_script.push_back(press);
    }
}

// Tokens "buttons[@screen|@state<N>|@open:<text>][/delay][:hold]" joined by ',' (frames; steps run in order).
void parseSteps(const std::string& text)
{
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t end = text.find(',', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string token = text.substr(pos, end - pos);
        pos = end + 1;
        if (token.empty())
            continue;
        Step step;
        const size_t colon = token.rfind(':');
        if (colon != std::string::npos && token.find_first_not_of("0123456789", colon + 1) == std::string::npos
            && colon + 1 < token.size())
        {
            step.hold = unsigned(atoi(token.c_str() + colon + 1));
            token.resize(colon);
        }
        const size_t slash = token.rfind('/');
        if (slash != std::string::npos && slash + 1 < token.size()
            && token.find_first_not_of("0123456789", slash + 1) == std::string::npos)
        {
            step.delay = unsigned(atoi(token.c_str() + slash + 1));
            token.resize(slash);
        }
        const size_t at = token.find('@');
        std::string condition;
        if (at != std::string::npos)
        {
            condition = token.substr(at + 1);
            token.resize(at);
        }
        if (condition.compare(0, 5, "state") == 0)
            step.state = reg32(atoi(condition.c_str() + 5));
        else if (condition.compare(0, 5, "open:") == 0)
            step.open = condition.substr(5);
        else
            step.screen = condition;
        Press press = {};
        bool valid = !token.empty();
        for (size_t b = 0; valid && b <= token.size();)
        {
            size_t p = token.find('+', b);
            if (p == std::string::npos)
                p = token.size();
            valid = parseButton(token.substr(b, p - b), &press);
            b = p + 1;
        }
        if (!valid)
        {
            pf::logError("frontend: boot_steps token \"%s\" is not buttons[@cond][/delay][:hold]", token.c_str());
            continue;
        }
        step.inputs = press.inputs;
        step.touch[0] = press.touch[0];
        step.touch[1] = press.touch[1];
        step.touchTo[0] = press.touchTo[0];
        step.touchTo[1] = press.touchTo[1];
        step.stick[0] = press.stick[0];
        step.stick[1] = press.stick[1];
        s_steps.push_back(step);
    }
}

// Debug aid: strings of fe_scan_file found twice or more in guest memory (copies in screen objects), logged.
void scanText(reg32 frame)
{
    static std::vector<std::string> strings;
    static std::unordered_map<uint64_t, std::vector<size_t>> index;
    if (strings.empty())
    {
        FILE* f = fopen(pf::Config::getString("fe_scan_file", ""), "rb");
        if (!f)
            return;
        std::string data;
        char buffer[65536];
        for (size_t n; (n = fread(buffer, 1, sizeof(buffer), f)) > 0;)
            data.append(buffer, n);
        fclose(f);
        for (size_t i = 0; i < data.size();)
        {
            size_t e = data.find('\0', i);
            if (e == std::string::npos)
                e = data.size();
            if (e - i >= 4 && e - i < 80 && data[i] >= 'A' && data[i] <= 'z')
            {
                std::string t = data.substr(i, e - i);
                uint64_t key = 0;
                memcpy(&key, t.data(), 4);
                index[key].push_back(strings.size());
                strings.push_back(t);
            }
            i = e + 1;
        }
        pf::logInfo("fe: scan: %u strings from the language file", unsigned(strings.size()));
    }
    const uint8_t* base = win32::g_guestBase;
    std::unordered_map<size_t, unsigned> count;
    std::unordered_map<reg32, size_t> where;
    for (reg32 a = 0x00800000; a < 0x0C000000; ++a)
    {
        if (base[a - 1] != 0 && base[a - 1] != 0x20 && base[a - 1] != 10)
            continue;
        uint64_t key = 0;
        memcpy(&key, base + a, 4);
        const auto it = index.find(key);
        if (it == index.end())
            continue;
        for (size_t id : it->second)
            if (strcmp(reinterpret_cast<const char*>(base + a), strings[id].c_str()) == 0)
            {
                ++count[id];
                where[a] = id;
            }
    }
    std::unordered_map<size_t, unsigned> refs;
    for (reg32 a = 0x00800000; a < 0x0C000000; a += 4)
    {
        uint32_t v;
        memcpy(&v, base + a, 4);
        if (v >= 0x00800000 && v < 0x0C000000)
        {
            const auto it = where.find(v);
            if (it != where.end())
                ++refs[it->second];
        }
    }
    count = refs;
    static std::unordered_map<size_t, unsigned> previous;
    std::string text;
    for (const auto& c : count)
    {
        const auto old = previous.find(c.first);
        const unsigned before = old == previous.end() ? 0 : old->second;
        if (c.second != before && !previous.empty())
            text += "[" + strings[c.first] + "]" + std::to_string(before) + ">" + std::to_string(c.second) + " ";
    }
    for (const auto& c : previous)
        if (!count.count(c.first))
            text += "[" + strings[c.first] + "]" + std::to_string(c.second) + ">0 ";
    previous = count;
    if (!text.empty())
        pf::logInfo("fe: scan frame %u: %s", unsigned(frame), text.c_str());
}

// Debug aid (fe_msg): wraps the message handlers of the live screens and logs the messages they get.
constexpr int MaxHandlers = 96;
win32::WinApplication* s_msgApp = nullptr;
reg32 s_handlerAddress[MaxHandlers];
win32::MethodPtr s_handlerOriginal[MaxHandlers];
int s_handlerCount = 0;
std::unordered_map<reg32, unsigned> s_msgSeen;

void logMessage(int index, x86::CPU& cpu)
{
    const reg32 msg = rc::ld32(cpu.esp + 4);
    unsigned& seen = s_msgSeen[msg ^ (s_handlerAddress[index] * 31)];
    ++seen;
    const reg32 object = rc::ld32(cpu.esp + 8);
    if ((msg == 0x35F8620B || msg == 0xE1FDE1D1 || msg == 0x0C407210) && object > 0x01000000 && object < 0x0C000000)
    {
        char line[400];
        int n = snprintf(line, sizeof(line), "fe: focus object %08X:", unsigned(object));
        for (int i = 0; i < 20; ++i)
            n += snprintf(line + n, sizeof(line) - n, " %X", unsigned(rc::ld32(object + i * 4)));
        for (int i = 0; i < 20; ++i)
        {
            const reg32 w = rc::ld32(object + i * 4);
            if (w > 0x01000000 && w < 0x0C000000 && rc::ld8(w) >= 0x20 && rc::ld8(w) < 0x7F)
                n += snprintf(line + n, sizeof(line) - n, " [%u]=\"%s\"", i, guestString(w).c_str());
        }
        pf::logInfo("%s msg %08X frame %u", line, unsigned(msg), unsigned(rc::ld32(FramesRendered)));
    }
    if (seen <= 6 || seen % 500 == 0)
        pf::logInfo("fe: msg handler %08X screen %08X msg %08X args %08X %08X %08X (#%u) frame %u", unsigned(s_handlerAddress[index]),
                    unsigned(cpu.ecx), unsigned(msg), unsigned(rc::ld32(cpu.esp + 8)), unsigned(rc::ld32(cpu.esp + 12)),
                    unsigned(rc::ld32(cpu.esp + 16)), seen, unsigned(rc::ld32(FramesRendered)));
}

template<int N> void handlerHook(win32::WinApplication* app, x86::CPU& cpu)
{
    logMessage(N, cpu);
    s_handlerOriginal[N](app, cpu);
}

template<int... I> void fillHooks(win32::MethodPtr* table, std::integer_sequence<int, I...>)
{
    ((table[I] = handlerHook<I>), ...);
}

void watchHandlers()
{
    static win32::MethodPtr hooks[MaxHandlers];
    if (!hooks[0])
        fillHooks(hooks, std::make_integer_sequence<int, MaxHandlers>());
    const reg32 hub = MessageHub;
    reg32 node = rc::ld32(hub + 4);
    for (int guard = 0; node && guard < 64; ++guard)
    {
        const reg32 entry = node - 4;
        if (entry == hub)
            break;
        const reg32 object = rc::ld32(entry + 0x14);
        const reg32 vtable = object && win32::MemMap::isGuestRange(object, 4) ? rc::ld32(object) : 0;
        const reg32 handler = vtable && win32::MemMap::isGuestRange(vtable, 16) ? rc::ld32(vtable + 8) : 0;
        node = rc::ld32(entry + 4);
        if (!handler || handler < 0x401000 || handler > 0x780000)
            continue;
        bool known = false;
        for (int i = 0; i < s_handlerCount; ++i)
            known |= s_handlerAddress[i] == handler;
        if (known || s_handlerCount >= MaxHandlers)
            continue;
        s_handlerAddress[s_handlerCount] = handler;
        s_handlerOriginal[s_handlerCount] = s_msgApp->replaceMethod(handler, hooks[s_handlerCount]);
        pf::logInfo("fe: msg watching handler %08X of screen %08X (vtable %08X)", unsigned(handler), unsigned(object),
                    unsigned(vtable));
        ++s_handlerCount;
    }
}

// Debug aid: logs ASCII strings of guest memory that contain fe_grep (case-insensitive).
void grepMemory(reg32 frame)
{
    const std::string pattern = pf::Config::getString("fe_grep", "");
    const uint8_t* base = win32::g_guestBase;
    unsigned shown = 0;
    for (reg32 a = 0x00800000; a < 0x0C000000 && shown < 60; ++a)
    {
        if (base[a] < 0x20 || base[a] > 0x7E || (base[a - 1] >= 0x20 && base[a - 1] <= 0x7E))
            continue;
        reg32 e = a;
        while (e - a < 100 && base[e] >= 0x20 && base[e] <= 0x7E)
            ++e;
        if (e - a < 5 || base[e] != 0)
            continue;
        std::string t(reinterpret_cast<const char*>(base + a), e - a);
        std::string u = t;
        for (char& c : u)
            c = char(tolower(c));
        if (u.find(pattern) != std::string::npos)
        {
            pf::logInfo("fe: grep frame %u %08X %s", unsigned(frame), unsigned(a), t.c_str());
            if (pf::Config::getBool("fe_grep_words", false))
            {
                char line[600];
                int n = snprintf(line, sizeof(line), "fe: words %08X:", unsigned(a - 0x80));
                for (int i = 0; i < 48; ++i)
                    n += snprintf(line + n, sizeof(line) - n, " %X", unsigned(rc::ld32(a - 0x80 + i * 4)));
                pf::logInfo("%s", line);
            }
            ++shown;
        }
    }
}

// Debug aid (fe_gfx_apply=name:value,...): a live Display menu change in GarageMain.
void applyGraphics(reg32 frame, const std::string& cur)
{
    if (s_gfxApplied)
        return;
    if (!s_garageFrame)
    {
        if (cur.find("GarageMain") != std::string::npos)
            s_garageFrame = frame;
        return;
    }
    if (frame - s_garageFrame < uint64_t(pf::Config::getInt("fe_gfx_apply_delay", 300)))
        return;
    s_gfxApplied = true;
    std::string caps;
    for (reg32 a = 0x8764D0; a <= 0x876530; a += 4)
        caps += " " + std::to_string(rc::ld32(a));
    pf::logInfo("fe: gfx option caps 0x8764D0..0x876530:%s", caps.c_str());
    const std::string list = pf::Config::getString("fe_gfx_apply", "");
    for (size_t pos = 0; pos < list.size();)
    {
        size_t end = list.find(',', pos);
        if (end == std::string::npos)
            end = list.size();
        const std::string item = list.substr(pos, end - pos);
        pos = end + 1;
        const size_t colon = item.find(':');
        bool known = false;
        for (const GraphicsGlobal& g : GraphicsGlobals)
        {
            if (colon == std::string::npos || item.compare(0, colon, g.name) != 0 || strlen(g.name) != colon)
                continue;
            known = true;
            const reg32 value = reg32(strtol(item.c_str() + colon + 1, nullptr, 0));
            pf::logInfo("fe: gfx apply %s %u -> %u at frame %u (road reflection menu maximum %u)", g.name,
                        unsigned(rc::ld32(g.address)), unsigned(value), unsigned(frame), unsigned(rc::ld32(0x8764EC)));
            rc::st32(g.address, value);
        }
        if (!known)
            pf::logError("frontend: fe_gfx_apply item \"%s\" is not a g_* name:value", item.c_str());
    }
    rc::st8(GraphicsDirty, 1);
}

// Debug aid (fe_orbit=on): logs the garage camera's yaw.
void logOrbit(reg32 frame)
{
    static float s_yaw = -1.0f;
    reg32 node = rc::ld32(MessageHub + 4);
    for (int guard = 0; node && node - 4 != MessageHub && guard < 64; ++guard)
    {
        const reg32 object = rc::ld32(node - 4 + 0x14);
        node = rc::ld32(node);
        const reg32 vtable = object && win32::MemMap::isGuestRange(object, GarageYaw + 4) ? rc::ld32(object) : 0;
        if (!vtable || !win32::MemMap::isGuestRange(vtable, 16) || rc::ld32(vtable + 8) != GarageHandler)
            continue;
        const float yaw = rc::ldf32(object + GarageYaw);
        float sx, sy;
        win32::Gamepad::getRightStick(&sx, &sy);
        if (yaw != s_yaw || std::fabs(sx) > 0.05f || std::fabs(sy) > 0.05f)
            pf::logInfo("fe: orbit frame %u yaw %.3f, speed %.2f %.2f, stick %.2f,%.2f, real mouse %d,%d move %d,%d buttons %u %u",
                        unsigned(frame), double(yaw), double(rc::ldf32(object + GarageOrbitSpeed)),
                        double(rc::ldf32(object + GarageOrbitSpeed + 4)), double(sx), double(sy),
                        int(rc::ld32(MouseX)), int(rc::ld32(MouseY)), int(rc::ld32(MouseMoveX)),
                        int(rc::ld32(MouseMoveY)), unsigned(rc::ld8(MouseButton)), unsigned(rc::ld8(MouseMiddle)));
        s_yaw = yaw;
        return;
    }
}

// Graphics values when a race starts from the front end.
void logGraphics(reg32 frame)
{
    std::string line;
    char item[64];
    for (const GraphicsGlobal& g : GraphicsGlobals)
    {
        snprintf(item, sizeof(item), " %s %u", g.name + 2, unsigned(rc::ld32(g.address)));
        line += item;
    }
    pf::logInfo("fe: graphics in the race at frame %u:%s", unsigned(frame), line.c_str());
}

// Advances boot_steps by one frame; returns the pad inputs to hold.
uint32_t stepInputs(reg32 frame, reg32 state, const std::string& boot, const std::string& cur, int* touch, float* stick)
{
    if (s_step >= s_steps.size())
        return 0;
    const Step& step = s_steps[s_step];
    if (s_stepPhase == 0)
    {
        bool ok = true;
        if (!step.screen.empty())
            ok = boot.find(step.screen) != std::string::npos || cur.find(step.screen) != std::string::npos;
        if (ok && step.state != ~0u)
            ok = state == step.state;
        if (ok && !step.open.empty())
        {
            std::lock_guard<std::mutex> guard(s_openedLock);
            ok = s_opened.find(step.open) != std::string::npos;
        }
        if (ok)
        {
            s_stepPhase = 1;
            s_stepFrame = frame;
        }
    }
    if (s_stepPhase == 1 && frame - s_stepFrame >= step.delay)
    {
        s_stepPhase = 2;
        s_stepFrame = frame;
        pf::logInfo("fe: step %u press at frame %u, game state %u, screens %s / %s", unsigned(s_step), unsigned(frame),
                    unsigned(state), boot.c_str(), cur.c_str());
    }
    if (s_stepPhase == 2)
    {
        if (frame - s_stepFrame < step.hold)
        {
            touch[0] = step.touch[0];
            touch[1] = step.touch[1];
            stick[0] = float(step.stick[0]) / 100.0f;
            stick[1] = float(step.stick[1]) / 100.0f;
            if (step.touchTo[0] >= 0 && step.hold > 1)
            {
                const int at = int(frame - s_stepFrame), last = int(step.hold) - 1;
                touch[0] += (step.touchTo[0] - step.touch[0]) * at / last;
                touch[1] += (step.touchTo[1] - step.touch[1]) * at / last;
            }
            if (step.touch[0] >= 0 && frame - s_stepFrame + 2 >= step.hold)
                pf::logInfo("fe: touch px %d,%d -> cursor %d,%d (canvas), button %u", step.touch[0], step.touch[1],
                            int(rc::ld32(MouseX)), int(rc::ld32(MouseY)), unsigned(rc::ld8(MouseButton)));
            return step.inputs;
        }
        s_stepPhase = 0;
        ++s_step;
        std::lock_guard<std::mutex> guard(s_openedLock);
        s_opened.clear();
    }
    return 0;
}

// Pad bits of the script at elapsed microseconds.
void scriptAt(uint64_t us, controls::PadSample* pad, uint32_t* raw)
{
    for (const Press& p : s_script)
    {
        const int id = int(&p - s_script.data());
        if (us < p.startUs || (p.endUs && us >= p.endUs))
            continue;
        uint64_t since = us - p.startUs;
        if (p.periodUs)
            since %= p.periodUs;
        if (since < p.holdUs)
        {
            pad->inputs |= p.inputs;
            *raw |= p.raw;
            if (p.touch[0] >= 0 && pad->touchCount < controls::PadSample::MaxTouches)
                pad->touches[pad->touchCount++] = { (float(p.touch[0]) + 0.5f) / float(pf::gxm::DisplayWidth),
                                                    (float(p.touch[1]) + 0.5f) / float(pf::gxm::DisplayHeight), id };
        }
    }
}

reg32 callGuest(win32::WinApplication* app, x86::CPU& cpu, reg32 function, reg32 ecx, const reg32* args, int count)
{
    x86::CPU saved = cpu;
    reg32 esp = cpu.esp;
    for (int i = count - 1; i >= 0; --i)
        rc::st32(esp -= 4, args[i]);
    esp -= 4;
    cpu.esp = esp;
    cpu.ecx = ecx;
    app->dynamic_call(function, cpu);
    const reg32 result = cpu.eax;
    cpu = saved;
    return result;
}

// boot_quit_after: "<seconds>", "state<N>+<frames>" or "save+<frames>".
void parseQuit(const std::string& text)
{
    if (text.empty())
        return;
    if (text.compare(0, 5, "state") == 0 && text.find('+') != std::string::npos)
    {
        s_quitKind = 2;
        s_quitState = reg32(atoi(text.c_str() + 5));
        s_quitFrames = uint64_t(atoll(text.c_str() + text.find('+') + 1));
    }
    else if (text.compare(0, 5, "save+") == 0)
    {
        s_quitKind = 3;
        s_quitFrames = uint64_t(atoll(text.c_str() + 5));
    }
    else if (atof(text.c_str()) > 0)
    {
        s_quitKind = 1;
        s_quitSeconds = atof(text.c_str());
    }
    else
        pf::logError("frontend: boot_quit_after \"%s\" is not <seconds>, state<N>+<frames> or save+<frames>", text.c_str());
}

void checkQuit(uint64_t elapsedUs, reg32 frame, reg32 state)
{
    if (s_quitKind == 0 || s_quitSent)
        return;
    if (state != s_lastState)
    {
        s_lastState = state;
        s_stateSince = frame;
    }
    const unsigned commits = win32::File::saveCommits();
    if (commits != s_saveCommits)
    {
        s_saveCommits = commits;
        s_saveFrame = frame;
    }
    bool quit = false;
    if (s_quitKind == 1)
        quit = double(elapsedUs) / 1e6 >= s_quitSeconds;
    else if (s_quitKind == 2)
        quit = state == s_quitState && frame - s_stateSince >= s_quitFrames;
    else
        quit = s_saveCommits != 0 && frame - s_saveFrame >= s_quitFrames;
    if (!quit)
        return;
    s_quitSent = true;
    pf::logInfo("fe: boot_quit_after reached at frame %u, game state %u, %u save files committed, quitting", unsigned(frame),
                unsigned(state), s_saveCommits);
    win32::Window::postMessage(0, WmQuit, 0, 0);
}

// The start-up movie a name (a path or a bare name, any case) is: 1 a logo or the notice, 2 the opening, else 0.
int introKind(const std::string& name)
{
    size_t start = name.find_last_of("/\\");
    start = start == std::string::npos ? 0 : start + 1;
    std::string base = name.substr(start);
    const size_t dot = base.find('.');
    if (dot != std::string::npos)
        base.resize(dot);
    static const char* const Logos[] = { "ealogo", "THX_logo", "PSA" };
    for (const char* logo : Logos)
    {
        if (strcasecmp(base.c_str(), logo) == 0)
            return 1;
    }
    return strcasecmp(base.c_str(), "FMVOpening") == 0 ? 2 : 0;
}

// Ends a movie after movies_skip seconds, and the start-up movies skip_intro_movies names once they have started.
void checkMovies(win32::WinApplication* app, x86::CPU& cpu, uint64_t nowUs, reg32 frame)
{
    const reg32 player = rc::ld32(MovieActive) ? rc::ld32(MoviePlayer) : 0;
    const std::string name = player ? guestString(rc::ld32(player + 0x24)) : std::string();
    if (player != s_moviePlayer || name != s_movieName)
    {
        s_moviePlayer = player;
        s_movieName = name;
        s_movieStartUs = nowUs;
        s_movieSkipped = false;
        if (player)
            pf::logInfo("fe: movie %s starts at frame %u", name.c_str(), unsigned(frame));
    }
    if (!player || s_movieSkipped)
        return;
    const double playedS = double(nowUs - s_movieStartUs) / 1e6;
    const int intro = s_skipIntro ? introKind(name) : 0;
    // An intro movie ends after a few frames, so that its player is running when the end message arrives.
    const bool skipIntro = intro && intro <= s_skipIntro && playedS >= 0.2;
    if (!skipIntro && (s_moviesSkip <= 0 || playedS < s_moviesSkip))
        return;
    s_movieSkipped = true;
    const reg32 args[2] = { MovieEndMessage, 0 };
    callGuest(app, cpu, SendMessage, MessageHub, args, 2);
    pf::logInfo("fe: movie %s skipped after %.1f s at frame %u", name.c_str(),
                double(nowUs - s_movieStartUs) / 1e6, unsigned(frame));
}

void pollHook(x86::reg32* buttons, int* pov)
{
    RC_USE(pov);
    *buttons |= s_raw.load(std::memory_order_relaxed);
}

void frameHook(win32::WinApplication* app, x86::CPU& cpu)
{
    s_queryPerformanceCounter(app, cpu);
    if (pf::Thread::currentId() != s_mainThread || rc::ld32(FramesRendered) == s_frame)
        return;
    s_frame = rc::ld32(FramesRendered);
    const uint64_t now = pf::ticksUs();
    if (!s_startUs)
        s_startUs = now;
    const uint64_t elapsed = now - s_startUs;
    const reg32 gameState = rc::ld32(GameState);
    const std::string boot = bootFlowScreen(), cur = currentScreen();
    if (!s_script.empty() || !s_steps.empty())
    {
        controls::PadSample pad = {};
        uint32_t raw = 0;
        scriptAt(elapsed, &pad, &raw);
        int touch[2] = { -1, -1 };
        float stick[2] = {};
        pad.inputs |= stepInputs(s_frame, gameState, boot, cur, touch, stick);
        pad.sticks[2] = stick[0];
        pad.sticks[3] = stick[1];
        if (touch[0] >= 0 && pad.touchCount < controls::PadSample::MaxTouches)
            pad.touches[pad.touchCount++] = { (float(touch[0]) + 0.5f) / float(pf::gxm::DisplayWidth),
                                              (float(touch[1]) + 0.5f) / float(pf::gxm::DisplayHeight), 1000 };
        win32::Gamepad::setInjectedPad(pad);
        s_raw.store(raw, std::memory_order_relaxed);
        static int s_touchesLast = 0;
        static bool s_buttonLast = false;
        const bool button = rc::ld8(MouseButton) != 0;
        if (pad.touchCount != s_touchesLast || button != s_buttonLast)
            pf::logInfo("fe: frame %u t=%.2fs touches %d, game mouse button %s at %d,%d", unsigned(s_frame),
                        double(elapsed) / 1e6, pad.touchCount, button ? "down" : "up", int(rc::ld32(MouseX)),
                        int(rc::ld32(MouseY)));
        s_touchesLast = pad.touchCount;
        s_buttonLast = button;
    }
    static const unsigned scanEvery = unsigned(pf::Config::getInt("fe_scan_every", 0));
    if (s_msgApp && s_frame % 30 == 0)
        watchHandlers();
    static const unsigned grepAt = unsigned(pf::Config::getInt("fe_grep_frame", 0));
    if (grepAt && s_frame == grepAt)
        grepMemory(s_frame);
    if (scanEvery && s_frame % scanEvery == 0)
        scanText(s_frame);
    if (s_moviesSkip > 0 || s_skipIntro)
        checkMovies(app, cpu, now, s_frame);
    checkQuit(elapsed, s_frame, gameState);
    static const bool orbit = pf::Config::getBool("fe_orbit", false);
    if (orbit && gameState == 4 && s_frame % 30 == 0)
    {
        const reg32 vehicle = rc::ld32(rc::ld32(0x008900AC) + 4);
        pf::logInfo("fe: car frame %u at %.1f,%.1f,%.1f", unsigned(s_frame), double(rc::ldf32(vehicle + 0x60)),
                    double(rc::ldf32(vehicle + 0x64)), double(rc::ldf32(vehicle + 0x68)));
    }
    if (orbit)
        logOrbit(s_frame);
    static const bool gfxApply = *pf::Config::getString("fe_gfx_apply", "") != 0;
    if (gfxApply)
        applyGraphics(s_frame, cur);
    static reg32 s_gfxState = 0;
    if (gfxApply && gameState != s_gfxState && (s_gfxState = gameState) == 6)
        logGraphics(s_frame);
    if (!s_logState)
        return;
    static reg32 s_state = ~0u, s_screen = ~0u, s_pending = ~0u;
    static std::string s_boot, s_cur;
    const reg32 frame = rc::ld32(FramesRendered);
    const reg32 state = gameState, screen = rc::ld32(ScreenName), pending = rc::ld32(PendingCall);
    const bool changed = state != s_state || screen != s_screen || pending != s_pending || boot != s_boot || cur != s_cur;
    if (changed || frame % HeartbeatFrames == 0)
        pf::logInfo("fe: %s frame %u t=%.1fs game state %u, pending call 0x%08X, screen %s, boot-flow screen %s, FE screen %s",
                    changed ? "state" : "alive", unsigned(frame), double(elapsed) / 1e6, unsigned(state),
                    unsigned(pending), guestString(screen).c_str(), boot.c_str(), cur.c_str());
    if (changed && pf::Config::getBool("fe_dump", false))
    {
        const reg32 m = rc::ld32(FeManager);
        char line[512];
        int n = snprintf(line, sizeof(line), "fe: mgr %08X:", unsigned(m));
        for (int i = 0; m && i < 16; ++i)
            n += snprintf(line + n, sizeof(line) - n, " %08X", unsigned(rc::ld32(m + i * 4)));
        pf::logInfo("%s", line);
        const reg32 sc = m ? rc::ld32(m + 8) : 0;
        const reg32 pkg = sc ? rc::ld32(sc + 0xE4) : 0;
        for (reg32 w : { reg32(20), reg32(21), reg32(14), reg32(16), reg32(27), reg32(28) })
        {
            const reg32 o = pkg ? rc::ld32(pkg + w * 4) : 0;
            if (o < 0x01000000 || o > 0x0A000000)
                continue;
            n = snprintf(line, sizeof(line), "fe: obj[%u] %08X:", unsigned(w), unsigned(o));
            for (int i = 0; i < 24; ++i)
                n += snprintf(line + n, sizeof(line) - n, " %X", unsigned(rc::ld32(o + i * 4)));
            pf::logInfo("%s", line);
        }
        n = snprintf(line, sizeof(line), "fe: pkg %08X:", unsigned(pkg));
        for (int i = 0; pkg && i < 64; ++i)
            n += snprintf(line + n, sizeof(line) - n, " %X", unsigned(rc::ld32(pkg + i * 4)));
        pf::logInfo("%s", line);
        n = snprintf(line, sizeof(line), "fe: scr %08X:", unsigned(sc));
        for (int i = 0; sc && i < 64; ++i)
            n += snprintf(line + n, sizeof(line) - n, " %X", unsigned(rc::ld32(sc + i * 4)));
        pf::logInfo("%s", line);
    }
    s_state = state;
    s_screen = screen;
    s_pending = pending;
    s_boot = boot;
    s_cur = cur;
}


}

uint32_t frameCounter()
{
    return rc::ld32(FramesRendered);
}

void hookFileOpens(win32::WinApplication& app)
{
    if (!s_createFile)
        s_createFile = app.replaceMethod(rc::ld32(CreateFileASlot), createFileHook);
}

void configure(win32::WinApplication& app)
{
    const std::string script = pf::Config::getString("boot_script", "");
    s_logState = pf::Config::getBool("fe_log", true);
    parseSteps(pf::Config::getString("boot_steps", ""));
    if (!script.empty() || !s_steps.empty())
    {
        parseScript(script);
        win32::Gamepad::setPollHook(pollHook);
        pf::logInfo("frontend: boot_script with %u entries, times in seconds from the first frame", unsigned(s_script.size()));
    }
    s_moviesSkip = atof(pf::Config::getString("movies_skip", "0"));
    static const char* const IntroSkips[] = { "off", "logos", "opening" };
    s_skipIntro = 0;   // skip_intro_movies takes the screens out of the boot flow (movies.cpp)
    (void)IntroSkips;
    parseQuit(pf::Config::getString("boot_quit_after", ""));
    if (pf::Config::getBool("fe_msg", false))
        s_msgApp = &app;
    s_mainThread = pf::Thread::currentId();
    const reg32 import = rc::ld32(QueryPerformanceCounterSlot);
    s_queryPerformanceCounter = app.replaceMethod(import, frameHook);
    if ((s_logState && pf::Config::getBool("fe_files", true)) || !s_steps.empty() || memLogOn())
        hookFileOpens(app);
    pf::logInfo("frontend: per-frame hook on QueryPerformanceCounter 0x%08X, state log %s, game state at 0x%08X", unsigned(import),
                s_logState ? "on (fe_log)" : "off", unsigned(GameState));
}

}}
