// PS2-style front-end prompts: our button icons for the text tokens and the PS2 button map (Triangle is Help).

#include "psprompt.h"
#include "helpicons.h"
#include "psmap.h"

#include <lib/memmap.h>
#include <platform/config.h>
#include <platform/log.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace ug2 { namespace psprompt {

namespace
{

using x86::reg32;

const reg32 TextureByHash = 0x004901D0;     // cdecl (hash, default, unloaded) -> texture info
const reg32 AddPoly = 0x00519FC0;           // thiscall on the render object, 12 arguments
const reg32 FontLineHeight = 0x24;
const reg32 PolyTexture = 36 * 4;
const reg32 NewestPoly = 0x1C;              // the render object's newest poly
const reg32 EventCallback = 0x0050C630;     // cdecl (player, joy event, value): sets the FEPad_* flags
const reg32 LoadChunks = 0x00577AB0;        // cdecl (data, bytes)

Context s_context = Outside;
reg32 s_font = 0;
reg32 s_colors = 0;
bool s_enabled = false;
bool s_logTokens = false;
bool s_packLoaded = false;
bool s_tintLoaded = false;
win32::MethodPtr s_eventCallback = nullptr;
std::set<std::string> s_seenTokens;

reg32 bits(float value)
{
    reg32 r;
    memcpy(&r, &value, 4);
    return r;
}

void eventHook(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 event = rc::ld32(cpu.esp + 8);
    rc::st32(cpu.esp + 8, reg32(psmap::remapEvent(int(event))));
    s_eventCallback(app, cpu);
}

reg32 call(win32::WinApplication* app, x86::CPU& cpu, reg32 address, reg32 self, std::initializer_list<reg32> args)
{
    const x86::CPU saved = cpu;
    reg32 sp = cpu.esp;
    for (const reg32* it = args.end(); it != args.begin();)
    {
        --it;
        sp -= 4;
        rc::st32(sp, *it);
    }
    sp -= 4;
    rc::st32(sp, 0);
    cpu.esp = sp;
    cpu.ecx = self;
    app->dynamic_call(address, cpu);
    const reg32 result = cpu.eax;
    cpu = saved;
    return result;
}

std::string guestString(reg32 address)
{
    std::string text;
    for (char c; address && (c = char(rc::ld8(address))) != 0 && text.size() < 80; ++address)
        text += c;
    return text;
}

void loadPack(win32::WinApplication* app, x86::CPU& cpu, bool tint)
{
    (tint ? s_tintLoaded : s_packLoaded) = true;
    const std::vector<uint8_t> pack = tint ? helpicons::buildTintPack() : helpicons::buildPack();
    win32::MemMap* map = new win32::MemMap(reg32(pack.size()));
    const reg32 at = map->getBlockStart();
    for (size_t i = 0; i < pack.size(); ++i)
        rc::st8(at + reg32(i), pack[i]);
    call(app, cpu, LoadChunks, 0, { at, reg32(pack.size()) });
    pf::logInfo("psprompt: icon pack of %u bytes loaded", unsigned(pack.size()));
}

}

void configure(win32::WinApplication& app)
{
    s_enabled = pf::Config::getBool("ps2_prompts", true);
    s_logTokens = pf::Config::getBool("pad_log", false);
    if (s_enabled)
        s_eventCallback = app.replaceMethod(EventCallback, eventHook);
    pf::logInfo("psprompt: PS2 prompts %s", s_enabled ? "on" : "off (ps2_prompts)");
}

reg32 s_lastFont = 0;

reg32 lastFont()
{
    return s_lastFont;
}

Scope enterText(Context context, reg32 font)
{
    if (context == Render || context == Width)
        s_lastFont = font;
    const Scope saved = { s_context, s_font };
    s_context = context;
    s_font = font;
    return saved;
}

void leaveText(const Scope& saved)
{
    s_context = saved.context;
    s_font = saved.font;
}

void beforeTokenSkip(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_enabled || (s_context != Render && s_context != Width))
        return;
    const reg32 text = rc::ld32(cpu.esp + 4);
    if (rc::ld16(text) != '$')
        return;
    std::string token;
    for (reg32 c, at = text + 2; (c = rc::ld16(at)) != '$' && c != 0 && token.size() < 64; at += 2)
        token += char(c >= 'a' && c <= 'z' ? c - 32 : c);
    psmap::Icon icon = psmap::iconForToken(token.c_str());
    const bool tint = icon == psmap::IconNone && (icon = psmap::iconForBindToken(token.c_str())) != psmap::IconNone;
    if (s_logTokens && s_seenTokens.insert(token).second)
        pf::logInfo("psprompt: token %s icon %d", token.c_str(), int(icon));
    if (icon == psmap::IconNone)
        return;
    if (tint ? !s_tintLoaded : !s_packLoaded)
        loadPack(app, cpu, tint);
    const reg32 texture = call(app, cpu, TextureByHash, 0, { tint ? psmap::tintAtlasHash() : psmap::atlasHash(), 0, 0 });
    if (!texture)
        return;
    const psmap::Cell cell = psmap::cellOf(icon);
    const reg32 frame = cpu.esp + 12;       // the caller's stack before it pushed the call's arguments
    const float size = rc::ldf32(s_font + FontLineHeight);
    static unsigned s_traced = 0;
    if (s_logTokens && token.compare(0, 5, "BIND_") == 0 && s_traced++ < 40)
        pf::logInfo("psprompt: %s %s size %.2f width so far %.2f x %.2f font %08x", s_context == Width ? "width" : "draw", token.c_str(),
                    size, rc::ldf32(frame + 0x28), rc::ldf32(frame + 0x10), unsigned(s_font));
    const psmap::IconBox box = psmap::boxOf(icon, size);
    if (s_context == Width)
    {
        rc::stf32(frame + 0x28, rc::ldf32(frame + 0x28) + box.width);
        return;
    }
    const float x = rc::ldf32(frame + 0x10), y = rc::ldf32(frame + 0x18);
    const reg32 flags = rc::ld32(frame + 0x14), textColor = rc::ld32(frame + 0x38);
    const reg32 renderObject = rc::ld32(frame + 0x5C), renderInfo = rc::ld32(frame + 0x60);
    const bool depth = ((flags >> 8) & 1) != 0;
    reg32 z = 0;
    if (depth)
        z = rc::ld32(rc::ld32(rc::ld32(renderObject + 8) + 0x2C) + 0x24);
    if (!s_colors)
        s_colors = (new win32::MemMap(16))->getBlockStart();
    for (int i = 0; i < 4; ++i)
        rc::st32(s_colors + 4 * i, (textColor & 0xFF000000) | (tint ? (textColor >> 1) & 0x7F7F7F : 0x808080));
    call(app, cpu, AddPoly, renderObject,
                            { bits(x), bits(y + box.top), bits(x + box.width), bits(y + box.top + box.height), z, bits(cell.u0), bits(cell.v0), bits(cell.u1), bits(cell.v1),
                              s_colors, renderInfo, depth ? 1u : 0u });
    const reg32 poly = rc::ld32(renderObject + NewestPoly);
    if (poly)
        rc::st32(poly + PolyTexture, texture);
    rc::stf32(frame + 0x10, x + box.width);
}

}}
