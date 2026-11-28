// The PS2 bottom bar: the PS2 bar group inside UI_PC_Help_Bar.fng is filled from the PC bar's own per-screen flags.

#include "psbar.h"
#include "ownscreens.h"
#include "psbarlogic.h"
#include "psbarpackage.h"

#include <lib/memmap.h>
#include <platform/config.h>
#include <platform/switches.h>
#include <platform/log.h>
#include <recomp.h>

#include <cstring>
#include <string>
#include <vector>

namespace ug2 { namespace psbar {

namespace
{

using x86::reg32;

const reg32 BitText = 0x0052D330;       // stdcall (bit): the language text hash of an item bit
const reg32 BitMessage = 0x0052D170;    // stdcall (bit): the message hash an item bit sends
const reg32 FindObject = 0x005379C0;    // cdecl (package name, object hash)
const reg32 SetText = 0x00535FE0;       // thiscall on a text object (string): sets the text, grows its buffer
const reg32 SetFormatted = 0x00537B80;  // cdecl (package name, object hash, format, argument)
const reg32 ShowObject = 0x0050CA50, HideObject = 0x0050CA00;       // cdecl (object), both recursive over groups
const reg32 TableEntries = 0x008383D8, TableCount = 0x008383D0;
const reg32 GameScreenTable = 0x007F7DC8;
const reg32 StringFormat = 0x00786E70, ProfileName = 0x0083A9E0;
const reg32 DataOffset = 0x2C, ObjectFlags = 0x1C, DirtyFlag = 0x400000;
const char* const Bar = "UI_PC_Help_Bar.fng", *const Donor = "UI_OptionsController.fng";
const char* const DonorArchive = "C:\\FRONTEND\\FrontB.lzc";
const reg32 GameMode = 0x00850078;
const reg32 DataX = 0x1C, SwitcherCurrent = 0x3C;
const reg32 BarObject = 0x008384D8, BarShown = 0x6C;

Prompt s_second = { 0, nullptr };
bool s_secondShown = false;
HelpVerdict s_help = { true, "general", "not decided yet" };
reg32 s_buffer = 0;
bool s_missingLogged = false;

InGameBuilt s_inGame;
bool s_panelRow = false;
Layout s_layout = LayoutInGame;
Prompt s_items[ItemCount];
bool s_pairShown[LayoutCount][ItemCount] = {};
reg32 s_switcherSaved = 0;
int s_applied = -1;

std::string readString(reg32 address)
{
    std::string text;
    for (char c; address && (c = char(rc::ld8(address))) != 0 && text.size() < 160; ++address)
        text += c;
    return text;
}

std::string wideString(reg32 address)
{
    std::string text;
    for (unsigned c; address && (c = rc::ld16(address)) != 0 && text.size() < 64; address += 2)
        text += c < 128 ? char(c) : '?';
    return text;
}

reg32 call(win32::WinApplication* app, x86::CPU& cpu, reg32 address, std::initializer_list<reg32> args, reg32 self = 0)
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

std::string tableText(reg32 hash)
{
    const reg32 table = rc::ld32(TableEntries), count = rc::ld32(TableCount);
    for (reg32 i = 0; table && i < count; ++i)
    {
        if (rc::ld32(table + 8 * i) == hash)
            return readString(rc::ld32(table + 8 * i + 4));
    }
    return std::string();
}

bool devLog()
{
#ifdef DEV_TOOLS
    return true;
#else
    return NFSU_DEV_BOOL("pad_log", false);
#endif
}

reg32 find(win32::WinApplication* app, x86::CPU& cpu, reg32 package, reg32 hash)
{
    return call(app, cpu, FindObject, { package, hash });
}

void writeText(win32::WinApplication* app, x86::CPU& cpu, reg32 object, const std::string& text, const char* label)
{
    if (!s_buffer)
        s_buffer = (new win32::MemMap(256))->getBlockStart();
    for (size_t i = 0; i <= text.size() && i < 255; ++i)
        rc::st8(s_buffer + reg32(i), uint8_t(i < text.size() ? text[i] : 0));
    call(app, cpu, SetText, { s_buffer }, object);
    rc::st32(object + ObjectFlags, rc::ld32(object + ObjectFlags) | 2 | DirtyFlag);
    if (devLog())
        pf::logInfo("psbar: %s text '%s' (stored '%s', capacity %u)", label, text.c_str(), wideString(rc::ld32(object + 0x64)).c_str(),
                    unsigned(rc::ld32(object + 0x68)));
}

void hideExtras(win32::WinApplication* app, x86::CPU& cpu, reg32 package)
{
    if (const reg32 object = find(app, cpu, package, PsDpad))
        call(app, cpu, HideObject, { object });
    if (!s_help.available)
    {
        for (reg32 hash : { PsHelpWord, PsHelpIcon })
        {
            if (const reg32 object = find(app, cpu, package, hash))
                call(app, cpu, HideObject, { object });
        }
    }
}

// The screen object of the fill's caller: it keeps itself in esi (edi in one caller), and +4 is the package name given to the fill.
reg32 liveScreen(const x86::CPU& cpu, reg32 nameArg)
{
    for (reg32 candidate : { cpu.esi, cpu.edi })
    {
        if (candidate >= 0x00100000 && candidate < 0x7F000000 && (candidate & 3) == 0 && rc::ld32(candidate + 4) == nameArg)
            return candidate;
    }
    return 0;
}

// The help id from the screen's table entry: ours first, then the game's table (28-byte entries, name first, help id at +8).
reg32 tableHelp(const std::string& screen)
{
    if (screen.empty())
        return 0;
    const reg32 hash = ownscreens::hashOfName(screen);
    reg32 entry = ownscreens::entryOf(hash);
    for (reg32 i = 0; !entry && i < 400; ++i)
    {
        const reg32 row = GameScreenTable + 28 * i, name = rc::ld32(row);
        if (!name)
            break;
        if (ownscreens::hashOfName(readString(name)) == hash)
            entry = row;
    }
    return entry ? rc::ld32(entry + 8) : 0;
}

void secondSlot(win32::WinApplication* app, x86::CPU& cpu, reg32 package, bool show)
{
    const reg32 word = find(app, cpu, package, PsSecondWord), icon = find(app, cpu, package, PsSecondIcon);
    if (!word || !icon)
        return;
    if (show && s_second.token)
    {
        const std::string text = tableText(s_second.text);
        writeText(app, cpu, word, text.empty() ? "?" : text, "second word");
        writeText(app, cpu, icon, std::string("$") + s_second.token + "$", "second icon");
        call(app, cpu, ShowObject, { word });
        call(app, cpu, ShowObject, { icon });
    }
    else
    {
        call(app, cpu, HideObject, { word });
        call(app, cpu, HideObject, { icon });
    }
}

reg32 originalBit(win32::WinApplication* app, x86::CPU& cpu, reg32 address, reg32 bit)
{
    return call(app, cpu, address, { bit });
}

void setX(reg32 object, float x)
{
    const reg32 data = rc::ld32(object + DataOffset);
    if (data && rc::ldf32a(data + DataX) != x)
    {
        rc::stf32a(data + DataX, x);
        rc::st32(object + ObjectFlags, rc::ld32(object + ObjectFlags) | DirtyFlag);
    }
}

void hidePairs(win32::WinApplication* app, x86::CPU& cpu, reg32 package, const bool keep[LayoutCount][ItemCount])
{
    for (int layout = 0; layout < LayoutCount; ++layout)
    {
        for (int item = 0; item < ItemCount; ++item)
        {
            if (!s_pairShown[layout][item] || (keep && keep[layout][item]))
                continue;
            s_pairShown[layout][item] = false;
            for (reg32 hash : { pairIcon(layout, item), pairWord(layout, item) })
            {
                if (const reg32 object = find(app, cpu, package, hash))
                    call(app, cpu, HideObject, { object });
            }
        }
    }
}

bool showPair(win32::WinApplication* app, x86::CPU& cpu, reg32 package, int item, const char* token, const std::string& word)
{
    const Slot& slot = s_inGame.slots[s_layout][item];
    const reg32 icon = slot.baked ? find(app, cpu, package, pairIcon(s_layout, item)) : 0;
    const reg32 text = slot.baked ? find(app, cpu, package, pairWord(s_layout, item)) : 0;
    if (!icon || !text)
        return false;
    writeText(app, cpu, icon, std::string("$") + token + "$", "in-game icon");
    writeText(app, cpu, text, word, "in-game word");
    const float left = pairLeft(slot.x, word.size());
    setX(icon, left);
    setX(text, left + PairGap);
    call(app, cpu, ShowObject, { icon });
    call(app, cpu, ShowObject, { text });
    s_pairShown[s_layout][item] = true;
    return true;
}

// The bar's bank/name texts are the PC switcher's targets; stopped on panel-row screens, where the hidden root shows nothing.
void bank(reg32 bar, bool panelRow)
{
    const Switcher now = { rc::ld32(bar + SwitcherCurrent), s_switcherSaved };
    const Switcher next = switcherFor(panelRow, now);
    if (next.current != now.current)
    {
        rc::st32(bar + SwitcherCurrent, next.current);
        if (devLog())
            pf::logInfo("psbar: bank/name switcher %s (object %08x)", panelRow ? "stopped on a panel-row screen" : "resumed", unsigned(next.current ? next.current : now.current));
    }
    s_switcherSaved = next.saved;
}

// Prompts in the panel's button row; no bar.
void showInGame(win32::WinApplication* app, x86::CPU& cpu, reg32 package)
{
    bool keep[LayoutCount][ItemCount] = {};
    bool used[ItemCount] = {};
    std::string line;
    for (int item = 0; item < ItemCount; ++item)
    {
        if (!s_items[item].token)
            continue;
        const std::string text = tableText(s_items[item].text);
        if (showPair(app, cpu, package, item, s_items[item].token, text.empty() ? "?" : text))
        {
            keep[s_layout][item] = used[item] = true;
            line += " " + std::to_string(item) + ":" + s_items[item].token + " '" + text + "'";
        }
    }
    const int helpItem = s_help.available ? freeItem(s_inGame.slots[s_layout], used) : -1;
    const std::string helpText = helpItem >= 0 ? tableText(HelpText) : std::string();
    if (helpItem >= 0 && showPair(app, cpu, package, helpItem, "JOY_EVENT_FENG_HELP", helpText.empty() ? "Help" : helpText))
    {
        keep[s_layout][helpItem] = true;
        line += " " + std::to_string(helpItem) + ":Help";
    }
    hidePairs(app, cpu, package, keep);
    static std::string s_logged;
    if (devLog() && line != s_logged)
        pf::logInfo("psbar: in-game prompts, layout %s:%s", LayoutScripts[s_layout], line.empty() ? " none" : line.c_str());
    s_logged = line;
}

}

void install()
{
    if (!NFSU_DEV_BOOL("ps2_prompts", true))
        return;
    ownscreens::addReplacement(Bar, [](fepackage::Package& bar)
    {
        fepackage::Package donor;
        Built built;
        if (!ownscreens::loadFromArchive(DonorArchive, Donor, &donor))
        {
            pf::logError("psbar: %s is not in %s; the PC help bar stays as it is", Donor, DonorArchive);
            return false;
        }
        if (!buildBar(bar, donor, &built))
        {
            pf::logError("psbar: building the PS2 bar from %s failed: %s", Donor, built.error.c_str());
            return false;
        }
        pf::logInfo("psbar: PS2 bar group copied from %s (read from %s): %u objects, %u resources merged, %u PC objects hidden", Donor, DonorArchive,
                    unsigned(built.copied), unsigned(built.resourcesAdded), unsigned(built.hiddenPc));
        if (!buildInGame(bar, &s_inGame))
            pf::logError("psbar: no in-game prompts: %s", s_inGame.error.c_str());
        else
            pf::logInfo("psbar: %u in-game prompt pairs baked at the PC items' layout positions", unsigned(s_inGame.pairs));
#ifdef DEV_TOOLS
        for (uint32_t hash : { PcName, PcMoney })
        {
            const std::vector<size_t> ours = bar.find(hash), old = bar.find(hash ^ PcOldSalt);
            const std::vector<uint8_t>* data = ours.size() == 1 ? bar.tagData(ours[0], 0x4153) : nullptr;
            float x = 0, y = 0;
            if (data && data->size() >= 0x24)
            {
                memcpy(&x, &(*data)[0x1C], 4);
                memcpy(&y, &(*data)[0x20], 4);
            }
            pf::logInfo("psbar: bank/name text %08x: %u PS2 object(s) (parent %08x, at %.2f %.2f, Fade_In %d, Fade_Out %d), %u PC original(s) renamed to %08x", unsigned(hash),
                        unsigned(ours.size()), ours.size() == 1 ? unsigned(bar.parent(ours[0])) : 0u, x, y, ours.size() == 1 && bar.hasScript(ours[0], fepackage::nameHash("Fade_In")),
                        ours.size() == 1 && bar.hasScript(ours[0], fepackage::nameHash("Fade_Out")), unsigned(old.size()), unsigned(hash ^ PcOldSalt));
        }
        for (int layout = 0; layout < LayoutCount; ++layout)
        {
            for (int item = 0; item < ItemCount; ++item)
            {
                const Slot& slot = s_inGame.slots[layout][item];
                if (slot.baked)
                    pf::logInfo("psbar: %s item %d at (%.1f, %.1f)", LayoutScripts[layout], item, slot.x, slot.y);
            }
        }
#endif
        return true;
    });
}

void beforeFill(win32::WinApplication* app, x86::CPU& cpu)
{
    static const bool enabled = NFSU_DEV_BOOL("ps2_prompts", true);
    if (!enabled)
        return;
    const reg32 flags = rc::ld32(cpu.esp + 4), screenName = rc::ld32(cpu.esp + 8);
    const std::string screen = screenName ? readString(screenName) : std::string();
    s_second = secondPrompt(screenName ? screen.c_str() : nullptr, flags, [&](uint32_t bit) { return originalBit(app, cpu, BitMessage, bit); },
                            [&](uint32_t bit) { return originalBit(app, cpu, BitText, bit); });
    rc::st32(cpu.esp + 4, 0);
    s_panelRow = panelRowScreen(screen.c_str()) && s_inGame.pairs;
    s_layout = layoutOf(flags);
    if (s_panelRow)
    {
        const reg32 itemFlags = rc::ld32(GameMode) == 0x200 ? flags & ~0x4000u : flags;
        inGamePrompts(itemFlags, [&](uint32_t bit) { return originalBit(app, cpu, BitMessage, bit); },
                      [&](uint32_t bit) { return originalBit(app, cpu, BitText, bit); }, s_items);
    }
    static std::string s_stateKey;
    const std::string stateKey = screen + "|" + std::to_string(flags);
    if (devLog() && stateKey != s_stateKey)
        pf::logInfo("psbar: screen '%s': %s; flags %08x, PC layout %s (%s)", screen.c_str(),
                    s_panelRow ? "listed, no bar, prompts in the panel's button row" : "not listed, the PS2 bar", unsigned(flags), LayoutScripts[s_layout],
                    (flags & LayoutBit) ? "the screen's own" : "none given, bottom row");
    s_stateKey = stateKey;
    const reg32 live = screenName ? liveScreen(cpu, screenName) : 0;
    const reg32 help = live ? rc::ld32(live + 0x28) : tableHelp(screen);
    s_help = helpVerdict(help, help && !tableText(help).empty(), flags, screen.c_str());
    static std::string s_helpKey;
    const std::string helpKey = screen + "|" + std::to_string(help) + "|" + std::to_string(flags & TipBit) + "|" + (hasOwnHelp(screen.c_str()) ? "own" : "-") + "|" + (live ? "live" : "table");
    if (devLog() && helpKey != s_helpKey)
        pf::logInfo("psbar: screen '%s' help id %08x (%s), flags %08x: Help %s, %s help (%s)", screen.c_str(), unsigned(help), live ? "live screen" : "table entry",
                    unsigned(flags), s_help.available ? "shown" : "hidden", s_help.kind, s_help.why);
    s_helpKey = helpKey;
    static std::string s_last;
    static reg32 s_lastFlags = ~0u, s_lastSecond = ~0u;
    if (devLog() && (screen != s_last || flags != s_lastFlags || s_second.text != s_lastSecond))
        pf::logInfo("psbar: fill screen '%s' flags %08x second prompt text %08x token %s", screen.c_str(), unsigned(flags), unsigned(s_second.text),
                    s_second.token ? s_second.token : "none");
    s_last = screen;
    s_lastFlags = flags;
    s_lastSecond = s_second.text;
}

void afterFill(win32::WinApplication* app, x86::CPU& cpu, reg32 bar)
{
    static const bool enabled = NFSU_DEV_BOOL("ps2_prompts", true);
    if (!enabled)
        return;
    const reg32 package = rc::ld32(bar + 4);
    static bool s_firstLogged = false;
    if (!s_firstLogged)
    {
        s_firstLogged = true;
        const ownscreens::ReplacementState state = ownscreens::replacementState(Bar);
        pf::logInfo("psbar: first fill, bar class object %08x (persistent, exists before its package): swap %s, package record %s, FEng instance %08x",
                    unsigned(bar), state.done ? "done" : "NOT done", state.recordOurs ? "uses our chunk" : "does NOT use our chunk", unsigned(state.instance));
    }
    const reg32 root = find(app, cpu, package, PsRoot);
    if (!root)
    {
        if (!s_missingLogged)
            pf::logError("psbar: the PS2 bar group is not in the bar package (%08x); the PC bar is hidden and nothing is drawn", unsigned(package));
        s_missingLogged = true;
        return;
    }
    const bool shown = rc::ld8(bar + BarShown) != 0;
    s_applied = shown ? 1 : 0;
    bank(bar, s_panelRow);
    if (!shown || s_panelRow)
    {
        call(app, cpu, HideObject, { root });
        if (shown)
            showInGame(app, cpu, package);
        else
            hidePairs(app, cpu, package, nullptr);
        return;
    }
    hidePairs(app, cpu, package, nullptr);
    call(app, cpu, ShowObject, { root });
    hideExtras(app, cpu, package);
    if (const reg32 name = find(app, cpu, package, PsName))
    {
        call(app, cpu, SetFormatted, { package, PsName, StringFormat, ProfileName });
        if (devLog())
            pf::logInfo("psbar: name text '%s'", wideString(rc::ld32(name + 0x64)).c_str());
    }
    if (const reg32 help = s_help.available ? find(app, cpu, package, PsHelpWord) : 0)
    {
        const std::string text = tableText(HelpText);
        writeText(app, cpu, help, text.empty() ? "Help" : text, "help word");
    }
    s_secondShown = s_second.token != nullptr;
    secondSlot(app, cpu, package, true);
#ifdef DEV_TOOLS
    static reg32 s_logged = 0;
    const reg32 key = s_second.text ^ 0x9E3779B9;
    if (key != s_logged)
    {
        s_logged = key;
        const reg32 data = rc::ld32(root + DataOffset);
        pf::logInfo("psbar: PS2 group %08x shown x %.2f y %.2f z %.2f flags %08x, second prompt %s", unsigned(root), data ? rc::ldf32(data + 0x1C) : 0.0f,
                    data ? rc::ldf32(data + 0x20) : 0.0f, data ? rc::ldf32(data + 0x24) : 0.0f, unsigned(rc::ld32(root + ObjectFlags)),
                    s_second.token ? s_second.token : "none");
    }
#endif
}

void visibility(win32::WinApplication* app, x86::CPU& cpu, reg32 bar, bool show)
{
    static const bool enabled = NFSU_DEV_BOOL("ps2_prompts", true);
    if (!enabled)
        return;
    const reg32 package = rc::ld32(bar + 4);
    const reg32 root = find(app, cpu, package, PsRoot);
    if (!root)
        return;
    s_applied = show ? 1 : 0;
    bank(bar, s_panelRow);
    if (!show || s_panelRow)
    {
        call(app, cpu, HideObject, { root });
        if (show)
            showInGame(app, cpu, package);
        else
            hidePairs(app, cpu, package, nullptr);
        return;
    }
    call(app, cpu, ShowObject, { root });
    hideExtras(app, cpu, package);
    secondSlot(app, cpu, package, s_secondShown);
}

void frame(win32::WinApplication* app, x86::CPU& cpu)
{
    // A boot race allocates the bar (0xAA filled) but never constructs it; FindObject on it faults.
    static const bool enabled = NFSU_DEV_BOOL("ps2_prompts", true) && !NFSU_DEV_BOOL("boot_race", false);
    const reg32 bar = enabled ? rc::ld32(BarObject) : 0;
    if (!bar || !rc::ld32(bar + 4))
        return;
    const bool shown = rc::ld8(bar + BarShown) != 0;
    if (!visibilityChanged(s_applied, shown))
        return;
    const x86::CPU saved = cpu;
    visibility(app, cpu, bar, shown);
    cpu = saved;
    if (devLog() && !visibilityChanged(s_applied, shown))
        pf::logInfo("psbar: bar %s by an inlined Show/Hide, PS2 bar follows", shown ? "shown" : "hidden");
}

}}
