// Widescreen for the Vita's 960x544, always on. The 640x480 canvas keeps the 4:3 scale, centred; HUD
// units move to the screen edges; the player views get a hor+ projection.

#include "widescreen.h"

#include <lib/gamepad.h>
#include <lib/guest.h>
#include <lib/winapp.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <platform/gxm.h>
#include <platform/log.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <native.h>
#include <recomp.h>
#include <set>
#include <string>
#include <unordered_map>

namespace ug2
{

void sub_537480_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_51a5d0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5c7ec0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace widescreen
{

Map placeMap(Place place, double k)
{
    switch (place)
    {
    case Place::Left:
        return { float(k / 320.0), 1.0f };
    case Place::Right:
        return { float(k / 320.0), float(2.0 * k - 1.0) };
    case Place::Full:
        return { float(1.0 / 320.0), 1.0f };
    default:
        return { float(k / 320.0), float(k) };
    }
}

Place placeOf(float x0, float x1)
{
    const float centre = 0.5f * (x0 + x1);
    if (centre < 640.0f / 3.0f)
        return Place::Left;
    if (centre > 1280.0f / 3.0f)
        return Place::Right;
    return Place::Centre;
}

namespace
{

// Zero bytes past .rdata (inside its raw size) that the PATCHES of games/ug2.py point the game's operands at.
const x86::reg32 SlotScale = 0x7e7fe0, SlotOffset = 0x7e7fe4;       // 2D canvas x mapping
const x86::reg32 SlotPictureWidth = 0x7e7fe8;                       // controls picture: 4:3 width in pixels
const x86::reg32 SlotFovX = 0x7e7fec, SlotFovY = 0x7e7ff0;          // projection: x scale, vertical half factor

const x86::reg32 ObjectType = 0x18;
const x86::reg32 TypeImage = 1, TypeColoredImage = 9, TypeMovie = 7;

double s_k = 1.0;               // part of the screen width the 4:3 canvas spans, centred
uint32_t s_width = 0, s_height = 0;     // back buffer
bool s_ready = false;
bool s_hudFlush = false, s_menuFlush = false, s_logFlush = false;
std::string s_package;
x86::reg32 s_context = 0;

struct Box
{
    float x0 = HUGE_VALF, x1 = -HUGE_VALF, y0 = HUGE_VALF, y1 = -HUGE_VALF;
    bool empty() const { return !(x0 <= x1); }
    void add(float x, float y)
    {
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    void add(const Box& b)
    {
        if (!b.empty())
            add(b.x0, b.y0), add(b.x1, b.y1);
    }
};

// HUD units: extents of this flush and of the last flush of the same package.
std::unordered_map<x86::reg32, Box> s_unitNow, s_unitLast;

// Objects whose place their maker set (free roam's panels: separate objects, each piece of a panel would else be
// placed on its own and the panel come apart).
std::unordered_map<x86::reg32, Place> s_setPlace;

bool uiLog()
{
    return pf::cachedStatic([] { return pf::Config::getBool("ui_log", false); });
}

std::string guestString(x86::reg32 address)
{
    std::string out;
    for (int i = 0; address && i < 48; ++i)
    {
        const char c = char(rc::ld8(address + x86::reg32(i)));
        if (!c)
            break;
        out += c >= 32 && c < 127 ? c : '?';
    }
    return out;
}

bool startsWith(const std::string& text, const char* prefix)
{
    for (size_t i = 0; prefix[i]; ++i)
        if (i >= text.size() || std::tolower(static_cast<unsigned char>(text[i])) != prefix[i])
            return false;
    return true;
}

bool isHud(const std::string& package)
{
    return startsWith(package, "hud_") || startsWith(package, "chyron_ig") || startsWith(package, "2phud");
}

// Fixed places from ui_log: the EA Trax pop-up's separate pieces, minimap icons.
bool fixedPlace(const std::string& package, x86::reg32 texture, Place* place)
{
    static const char Minimap[] = "MINIMAP_";
    bool minimap = texture != 0;
    for (x86::reg32 i = 0; minimap && i < sizeof(Minimap) - 1; ++i)
        minimap = char(rc::ld8(texture + 0xc + i)) == Minimap[i];
    if (!minimap && !startsWith(package, "chyron_ig"))
        return false;
    *place = Place::Left;
    return true;
}

void setMap(const Map& map)
{
    rc::stf32(SlotScale, map.scale);
    rc::stf32(SlotOffset, map.offset);
}

// Front touch: display point to 0..1 of the drawn canvas.
bool touchCanvas(float* x, float* y)
{
    const pf::gxm::Rect box = pf::gxm::Device::windowRect(s_width, s_height);
    const Map map = placeMap(Place::Centre, s_k);
    const double clip = 2.0 * (*x * pf::gxm::DisplayWidth - box.x0) / (box.x1 - box.x0) - 1.0;
    const double u = (clip + map.offset) / map.scale / 640.0;
    const double v = (*y * pf::gxm::DisplayHeight - box.y0) / (box.y1 - box.y0);
    *x = float(u);
    *y = float(v);
    return u >= 0.0 && u <= 1.0 && v >= 0.0 && v <= 1.0;
}

void writeSlots(uint32_t width, uint32_t height)
{
    s_width = width;
    s_height = height;
    s_k = width && height ? std::min(1.0, double(height) / double(width) / 0.75) : 1.0;
    setMap(placeMap(Place::Centre, s_k));
    rc::st32(SlotPictureWidth, x86::reg32(std::lround(height * 4.0 / 3.0)));
    rc::stf32(SlotFovX, 1.0f);
    rc::stf32(SlotFovY, 0.5f);
    s_ready = true;
    win32::Gamepad::setTouchMap(touchCanvas);
}

// Canvas extent of an FE render object's polygons.
Box renderBox(x86::reg32 render)
{
    Box box;
    const x86::reg32 head = render + 0x18;
    x86::reg32 node = rc::ld32(head);
    for (int n = 0; node != head && node != 0 && n < 4096; node = rc::ld32(node), ++n)
        for (x86::reg32 v = 0; v < 4; ++v)
            box.add(rc::ldf32(node + 0x10 + 0x10 * v), rc::ldf32(node + 0x14 + 0x10 * v));
    return box;
}

// Top-level group of an FE object (package render contexts).
x86::reg32 topGroup(x86::reg32 object)
{
    x86::reg32 unit = object;
    x86::reg32 context = rc::ld16(object + 0x20);
    for (int depth = 0; s_context && context != 0 && context < 64 && depth < 64; ++depth)
    {
        const x86::reg32 group = rc::ld32(s_context + 0x20 + 0xd0 * context + 0x50);
        if (group == 0 || group == unit)
            break;
        unit = group;
        context = rc::ld16(group + 0x20);
    }
    return unit;
}

const char* placeName(Place place)
{
    return place == Place::Left ? "left" : place == Place::Right ? "right" : place == Place::Full ? "full" : "centre";
}

std::map<std::string, std::string> s_lastLog;      // package -> objects and places of its last logged flush
std::string s_logNow, s_signature;

void logDraw(x86::reg32 render, x86::reg32 object, x86::reg32 unit, const Box& box, const char* how)
{
    const x86::reg32 texture = rc::ld32(render + 0x14);
    const Map map = { rc::ldf32(SlotScale), rc::ldf32(SlotOffset) };
    const pf::gxm::Rect screen = pf::gxm::Device::windowRect(s_width, s_height);
    const double half = 0.5 * (screen.x1 - screen.x0);
    char line[300];
    snprintf(line, sizeof(line), "ui:  obj %08x unit %08x type %u tex %s x %.1f..%.1f y %.1f..%.1f %s px %.0f..%.0f\n",
             unsigned(object), unsigned(unit), unsigned(rc::ld32(object + ObjectType)),
             texture ? guestString(texture + 0xc).c_str() : "-", box.x0, box.x1, box.y0, box.y1, how,
             screen.x0 + (box.x0 * map.scale - map.offset + 1.0) * half,
             screen.x0 + (box.x1 * map.scale - map.offset + 1.0) * half);
    s_logNow += line;
    snprintf(line, sizeof(line), "%08x %08x %s,", unsigned(object), unsigned(unit), how);
    s_signature += line;
}

}

// Racing resolution (thiscall): the display size.
void racingResolution(win32::WinApplication*, x86::CPU& cpu)
{
    const x86::reg32 widthPointer = rc::ld32(cpu.esp + 4), heightPointer = rc::ld32(cpu.esp + 8);
    rc::st32(widthPointer, pf::gxm::DisplayWidth);
    rc::st32(heightPointer, pf::gxm::DisplayHeight);
    cpu.esp += 12;
    const uint32_t width = rc::ld32(widthPointer), height = rc::ld32(heightPointer);
    writeSlots(width, height);
    static bool s_logged = false;
    if (!s_logged)
    {
        s_logged = true;
        pf::logInfo("screen: back buffer %ux%u, 4:3 part of the width %.4f", unsigned(width), unsigned(height), s_k);
    }
}

// FEng package render (thiscall, package argument).
void packageFlush(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 package = rc::ld32(cpu.esp + 4);
    s_package = guestString(package ? rc::ld32(package + 0xc) : 0);
    s_context = rc::ld32(cpu.ecx + 0xc);
    const bool wide = s_ready && s_k < 1.0;
    s_hudFlush = wide && isHud(s_package);
    s_menuFlush = wide && !s_hudFlush;
    s_logFlush = s_ready && uiLog();
    s_logNow.clear();
    s_signature.clear();
    sub_537480_guest(app, cpu);
    if (s_hudFlush)
        s_unitLast.swap(s_unitNow), s_unitNow.clear();
    s_hudFlush = s_menuFlush = s_logFlush = false;
    if (uiLog() && !s_logNow.empty())
    {
        std::string& last = s_lastLog[s_package];
        if (last != s_signature)
        {
            last = s_signature;
            pf::logInfo("ui: package %s", s_package.c_str());
            for (size_t start = 0, end; (end = s_logNow.find('\n', start)) != std::string::npos; start = end + 1)
                pf::logInfo("%s", s_logNow.substr(start, end - start).c_str());
        }
    }
}

// FERenderObject draw (thiscall): the canvas mapping of the object.
void renderObject(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_hudFlush && !s_menuFlush)
    {
        if (s_logFlush)
            logDraw(cpu.ecx, rc::ld32(cpu.ecx + 8), rc::ld32(cpu.ecx + 8), renderBox(cpu.ecx), "4:3");
        sub_51a5d0_guest(app, cpu);
        return;
    }
    const x86::reg32 render = cpu.ecx;
    const x86::reg32 object = rc::ld32(render + 8);
    const x86::reg32 type = rc::ld32(object + ObjectType);
    const Box box = renderBox(render);
    x86::reg32 unit = object;
    const char* how = "centre";
    Place place = Place::Centre;
    bool moved = false;
    if (s_menuFlush)
    {
        if (!box.empty() && (box.x1 <= 0.0f || box.x0 >= 640.0f))
        {
            setMap({ float(s_k / 320.0), 8.0f });
            how = "off";
            moved = true;
        }
    }
    else if (type != TypeMovie && !box.empty())
    {
        unit = topGroup(object);
        s_unitNow[unit].add(box);
        const auto set = s_setPlace.find(object);
        const bool fixed = set != s_setPlace.end() ? (place = set->second, true) : fixedPlace(s_package, rc::ld32(render + 0x14), &place);
        const bool image = type == TypeImage || type == TypeColoredImage;
        if (!fixed && image && box.x0 <= 1.0f && box.x1 >= 639.0f)
            place = Place::Full;
        else if (!fixed)
        {
            auto last = s_unitLast.find(unit);
            const Box& extent = last != s_unitLast.end() && !last->second.empty() ? last->second : box;
            place = placeOf(extent.x0, extent.x1);
        }
        if (place != Place::Centre)
        {
            setMap(placeMap(place, s_k));
            how = placeName(place);
            moved = true;
        }
    }
    if (uiLog())
        logDraw(render, object, unit, box, how);
    sub_51a5d0_guest(app, cpu);
    if (moved)
        setMap(placeMap(Place::Centre, s_k));
}

// Projection (thiscall): hor+ for the player views.
void projection(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 view = rc::ld32(cpu.esp + 4);
    float fovX = 1.0f, fovY = 0.5f;
    if (view)
    {
        const x86::reg32 id = rc::ld32(view + 4), target = rc::ld32(view + 0x58);
        const bool player = id == 1 || id == 2 || id == 4 || id == 5;
        const uint32_t width = target ? rc::ld32(target + 0x14) : 0, height = target ? rc::ld32(target + 0x18) : 0;
        if (player && width && height)
        {
            const double k = std::min(1.0, double(height) / double(width) / 0.75);
            fovX = float(k);
            fovY = float(0.5 / k);
        }
        static std::set<x86::reg32> s_seen;
        if (s_seen.size() < 32 && s_seen.insert(id).second)
            pf::logInfo("screen: view %u, target %ux%u, projection x %.4f, vertical half angle %.4f", unsigned(id),
                        unsigned(width), unsigned(height), fovX, fovY);
    }
    rc::stf32(SlotFovX, fovX);
    rc::stf32(SlotFovY, fovY);
    sub_5c7ec0_guest(app, cpu);
}

}}

namespace ug2 { namespace native
{

void sub_5bf610(win32::WinApplication* app, x86::CPU& cpu) { widescreen::racingResolution(app, cpu); }
void sub_537480(win32::WinApplication* app, x86::CPU& cpu) { widescreen::packageFlush(app, cpu); }
void sub_51a5d0(win32::WinApplication* app, x86::CPU& cpu) { widescreen::renderObject(app, cpu); }
void sub_5c7ec0(win32::WinApplication* app, x86::CPU& cpu) { widescreen::projection(app, cpu); }

}}

// The place of an FE object of free roam's (roam_game.c): 0 centre, 1 left, 2 right, -1 its own extent's again.
extern "C" void ug2_widescreen_place(uint32_t object, int place)
{
    if (place < 0 || place > 2)
        ug2::widescreen::s_setPlace.erase(object);
    else
        ug2::widescreen::s_setPlace[object] = place == 1 ? ug2::widescreen::Place::Left
                                            : place == 2 ? ug2::widescreen::Place::Right : ug2::widescreen::Place::Centre;
}
