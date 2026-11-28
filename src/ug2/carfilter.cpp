// Car filter row: each car select package is baked when the loader registers it (ownscreens replacement).

#include "carfilter.h"
#include "carfilterpackage.h"
#include "ownscreens.h"

#include <platform/config.h>
#include <platform/switches.h>
#include <platform/log.h>
#include <recomp.h>

#include <map>
#include <string>

namespace ug2 { namespace carfilter {

namespace
{

using x86::reg32;

std::map<std::string, Baked> s_baked;

#ifdef DEV_TOOLS
const reg32 FindObject = 0x005379C0;    // cdecl (package name, object hash)
const reg32 ObjectFlags = 0x1C;

reg32 find(win32::WinApplication* app, x86::CPU& cpu, reg32 package, reg32 hash)
{
    const x86::CPU saved = cpu;
    reg32 sp = cpu.esp;
    rc::st32(sp -= 4, hash);
    rc::st32(sp -= 4, package);
    rc::st32(sp -= 4, 0);
    cpu.esp = sp;
    app->dynamic_call(FindObject, cpu);
    const reg32 result = cpu.eax;
    cpu = saved;
    return result;
}

std::string readString(reg32 address)
{
    std::string text;
    for (char c; address && (c = char(rc::ld8(address))) != 0 && text.size() < 80; ++address)
        text += c;
    return text;
}
#endif

}

void install()
{
    if (!NFSU_DEV_BOOL("ps2_prompts", true))
        return;
    for (const char* screen : Screens)
    {
        const std::string name = screen;
        ownscreens::addReplacement(name, [name](fepackage::Package& package)
        {
            Baked baked;
            if (!bake(package, &baked))
            {
                pf::logError("carfilter: %s keeps the PC arrows: %s", name.c_str(), baked.error.c_str());
                return false;
            }
            s_baked[name] = baked;
#ifdef DEV_TOOLS
            pf::logInfo("carfilter: %s baked: PC arrows hidden, L1 icon %08x at (%.1f, %.1f), R1 icon %08x at (%.1f, %.1f), scale %.3f", name.c_str(),
                        unsigned(baked.leftIcon), baked.left.x, baked.left.y, unsigned(baked.rightIcon), baked.right.x, baked.right.y, baked.scale);
#endif
            return true;
        });
    }
}

void afterFill(win32::WinApplication* app, x86::CPU& cpu, reg32 screenName)
{
#ifdef DEV_TOOLS
    static std::string s_last, s_logged;
    const std::string screen = screenName ? readString(screenName) : std::string();
    if (screen != s_last)
        s_logged.clear();
    s_last = screen;
    const auto found = s_baked.find(screen);
    if (found == s_baked.end() || s_logged == screen)
        return;
    const reg32 left = find(app, cpu, screenName, found->second.leftIcon), right = find(app, cpu, screenName, found->second.rightIcon);
    if (!left && !right)
        return;
    s_logged = screen;
    const reg32 leftFlags = left ? rc::ld32(left + ObjectFlags) : 0, rightFlags = right ? rc::ld32(right + ObjectFlags) : 0;
    const bool shown = left && right && !(leftFlags & fepackage::FlagHidden) && !(rightFlags & fepackage::FlagHidden);
    pf::logInfo("carfilter: %s %s the L1/R1 icons (objects %08x %08x, flags %08x %08x)", screen.c_str(), shown ? "shows" : "does NOT show",
                unsigned(left), unsigned(right), unsigned(leftFlags), unsigned(rightFlags));
#else
    (void)app;
    (void)cpu;
    (void)screenName;
#endif
}

}}
